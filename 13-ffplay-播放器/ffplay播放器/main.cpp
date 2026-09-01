/**
 * ============================================================================
 * 工业级现代化音视频播放器引擎 (Modern C++17 + FFmpeg 8.x/9.x + SDL3)
 * ============================================================================
 *
 * [11-01] 架构设计意义与背景：
 * - ffplay.c 是现代众多工业级/开源播放器（如 Bilibili ijkplayer）的核心原型。
 * - 核心骨架：解复用读线程 -> 线程安全队列 -> 音视频解码线程 -> 环形帧队列 -> 渲染/回调输出。
 * - 本工程采用 C++17、现代线程同步模型、RAII、原子变量、FFmpeg 8.x 与 SDL3 规范，
 *   彻底淘汰废弃 API，实现完整的工业级播放器。
 */

 /* ==================== 标准库与外部库头文件 ==================== */
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>

/* FFmpeg 与 SDL3 的 C 接口（extern "C" 确保 C++ 正确链接） */
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <SDL3/SDL.h>
}

/* ==================== 全局常量定义 ==================== */
// 包队列最大字节数（避免内存爆炸）
constexpr int MAX_QUEUE_SIZE = 15 * 1024 * 1024;
// 音视频同步阈值：最小/最大偏差（秒）
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
// 重复帧阈值
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
// 不强制同步阈值（超过此值认为时钟差异过大，不做同步）
constexpr double AV_NOSYNC_THRESHOLD = 10.0;
// 音频采样数校正最大百分比（用于动态调整采样数实现同步）
constexpr int SAMPLE_CORRECTION_PERCENT_MAX = 10;
// 音频差异平均样本数（滑动平均）
constexpr int AUDIO_DIFF_AVG_NB = 20;

/**
 * 同步主时钟类型：
 * - AV_SYNC_AUDIO_MASTER：以音频为基准（最常用，保证音频流畅）
 * - AV_SYNC_VIDEO_MASTER：以视频为基准
 * - AV_SYNC_EXTERNAL_CLOCK：外部系统时钟为基准
 */
enum SyncType {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK
};

// 特殊“冲刷包”指针，用于队列中的清除标记
static AVPacket* flush_pkt = nullptr;

/* ==========================================
 * 时钟模块 (PlayerClock)
 * 管理单一时间线（音频、视频或外部时钟），支持暂停、倍速。
 * ========================================== */
class PlayerClock {
public:
    PlayerClock() {
        speed = 1.0;
        paused = false;
        pts = NAN;
        pts_drift = NAN;
        last_updated = 0.0;
        serial = 0;
    }

    /**
     * 设置当前时钟值（通常在新帧到来时调用）
     * @param new_pts    新的 PTS（秒）
     * @param new_serial 流序列号（用于丢弃旧数据）
     */
    void set(double new_pts, int new_serial) {
        double time = av_gettime_relative() / 1000000.0;  // 当前系统时间（秒）
        pts = new_pts;
        last_updated = time;
        pts_drift = new_pts - time;  // 漂移 = PTS - 系统时间
        serial = new_serial;
    }

    /**
     * 获取当前时钟值（考虑流逝的时间，若暂停则返回固定 PTS）
     * @return 当前时钟值（秒），若无效则返回 NAN
     */
    double get() {
        if (paused) return pts;
        if (std::isnan(pts)) return NAN;
        double time = av_gettime_relative() / 1000000.0;
        // 公式：pts_drift + time - (time - last_updated) * (1.0 - speed)
        // 当 speed=1.0 时，简化为 pts_drift + time，即原始 PTS 跟随系统时间推进
        return pts_drift + time - (time - last_updated) * (1.0 - speed);
    }

    /**
     * 设置暂停状态
     * @param p true 暂停，false 恢复
     */
    void set_paused(bool p) {
        if (paused != p) {
            paused = p;
            if (!p) {
                // 恢复时重新校准当前时间，避免跳跃
                set(get(), serial);
            }
        }
    }

    /**
     * 将当前时钟同步到从时钟（用于外部时钟跟随主时钟）
     * @param slave 从时钟（如外部时钟跟随音频或视频）
     */
    void sync_to_slave(PlayerClock& slave) {
        double slave_time = slave.get();
        double master_time = get();
        // 若从时钟有效且与主时钟差异超过阈值，则强制追齐
        if (!std::isnan(slave_time) && (std::isnan(master_time) || std::fabs(master_time - slave_time) > AV_NOSYNC_THRESHOLD)) {
            set(slave_time, slave.serial);
        }
    }

public:
    double pts;            // 当前 PTS（秒）
    double pts_drift;      // PTS 与系统时间差值
    double last_updated;   // 最近一次 set() 时的系统时间
    double speed;          // 播放倍速（未完整实现）
    int serial;            // 序列号，用于区分不同流重置
    bool paused;           // 是否暂停
};

/* ==========================================
 * 线程安全包队列 (PacketQueue)
 * 用于存放解复用出来的 AVPacket，支持多线程读/写。
 * ========================================== */

struct PacketItem {
    AVPacket* pkt;   // 包指针（可能为 flush_pkt）
    int serial;      // 序列号
};

class PacketQueue {
public:
    PacketQueue() : abort_request(false), serial(0), size(0), nb_packets(0) {}
    ~PacketQueue() { flush(); }

    /**
     * 启动队列（放入一个冲刷包，重置序列号）
     */
    void start() {
        std::lock_guard<std::mutex> lock(mtx);
        abort_request = false;
        put_flush_packet();
    }

    /**
     * 中止队列，唤醒所有等待线程
     */
    void abort() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            abort_request = true;
        }
        cv.notify_all();
    }

    /**
     * 放入一个冲刷包，用于清理解码器缓存并重新同步
     */
    void put_flush_packet() {
        serial++;
        put_internal(flush_pkt);
    }

    /**
     * 放入一个普通 AVPacket（会深拷贝）
     * @param pkt 源包（不会被释放，调用者需自行处理）
     * @return 是否成功
     */
    bool put(AVPacket* pkt) {
        std::lock_guard<std::mutex> lock(mtx);
        if (abort_request) return false;
        return put_internal(pkt);
    }

    /**
     * 取出一个包（阻塞直到有包或中止）
     * @param out_pkt    输出包指针（调用者需 av_packet_free）
     * @param out_serial 输出序列号
     * @return 成功返回 true，中止返回 false
     */
    bool get(AVPacket** out_pkt, int& out_serial) {
        std::unique_lock<std::mutex> lock(mtx);
        while (true) {
            if (abort_request) return false;
            if (!queue.empty()) {
                PacketItem item = queue.front();
                queue.pop();
                nb_packets--;
                if (item.pkt != flush_pkt) {
                    size -= item.pkt->size + sizeof(PacketItem);
                }
                *out_pkt = item.pkt;
                out_serial = item.serial;
                return true;
            }
            cv.wait(lock);  // 无包时等待
        }
    }

    /**
     * 清空队列中所有包（释放普通包，保留冲刷包标记）
     */
    void flush() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!queue.empty()) {
            PacketItem item = queue.front();
            queue.pop();
            if (item.pkt && item.pkt != flush_pkt) {
                av_packet_free(&item.pkt);
            }
        }
        size = 0;
        nb_packets = 0;
    }

    /* 查询接口 */
    int get_size() const { return size; }
    int get_nb_packets() const { return nb_packets; }
    int get_serial() const { return serial; }

private:
    /**
     * 内部放入逻辑：深拷贝包或直接使用冲刷包
     */
    bool put_internal(AVPacket* pkt) {
        AVPacket* new_pkt = nullptr;
        if (pkt != flush_pkt) {
            new_pkt = av_packet_alloc();
            if (!new_pkt) return false;
            av_packet_ref(new_pkt, pkt);   // 深拷贝
        }
        else {
            new_pkt = flush_pkt;           // 冲刷包直接使用全局指针
        }
        queue.push({ new_pkt, serial });
        nb_packets++;
        if (pkt != flush_pkt) {
            size += new_pkt->size + sizeof(PacketItem);
        }
        cv.notify_one();
        return true;
    }

    std::queue<PacketItem> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> abort_request;
    int serial;          // 当前序列号（每次冲刷递增）
    int size;            // 队列中包总字节数
    int nb_packets;      // 包数量
};

/* ==========================================
 * 线程安全帧队列 (FrameQueue)
 * 存放解码后的 AVFrame（视频或音频），环形缓冲区，支持保持最后一帧。
 * ========================================== */

struct FrameItem {
    AVFrame* frame = nullptr;
    double pts = 0.0;
    double duration = 0.0;  // 音频帧持续时间，视频帧可忽略
    int serial = 0;
};

class FrameQueue {
public:
    /**
     * @param max_capacity     环形缓冲区容量
     * @param keep_last_frame  是否保留最后一帧（用于暂停时显示）
     */
    FrameQueue(int max_capacity, bool keep_last_frame = true)
        : max_size(max_capacity), keep_last(keep_last_frame), rindex(0), windex(0),
        size(0), rindex_shown(0), abort_request(false) {
        queue.resize(max_size);
        for (int i = 0; i < max_size; ++i) {
            queue[i].frame = av_frame_alloc();
        }
    }

    ~FrameQueue() {
        abort();
        for (int i = 0; i < max_size; ++i) {
            if (queue[i].frame) {
                av_frame_free(&queue[i].frame);
            }
        }
    }

    /**
     * 中止队列，唤醒等待线程
     */
    void abort() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            abort_request = true;
        }
        cv.notify_all();
    }

    /**
     * 获取可写的帧槽（阻塞直到有空位或中止）
     * @return 帧槽指针，若中止则返回 nullptr
     */
    FrameItem* peek_writable() {
        std::unique_lock<std::mutex> lock(mtx);
        while (size >= max_size && !abort_request) {
            cv.wait(lock);
        }
        if (abort_request) return nullptr;
        return &queue[windex];
    }

    /**
     * 将当前写位置推进（表示已写入一帧）
     */
    void push() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            windex = (windex + 1) % max_size;
            size++;
        }
        cv.notify_all();
    }

    /**
     * 获取可读的帧槽（阻塞直到有帧或中止）
     * @return 帧槽指针，若中止则返回 nullptr
     */
    FrameItem* peek_readable() {
        std::unique_lock<std::mutex> lock(mtx);
        while (size - rindex_shown <= 0 && !abort_request) {
            cv.wait(lock);
        }
        if (abort_request) return nullptr;
        return &queue[(rindex + rindex_shown) % max_size];
    }

    /**
     * 获取最后一帧（用于渲染暂停画面）
     */
    FrameItem* peek_last() {
        return &queue[rindex];
    }

    /**
     * 消费当前可读帧，推进读位置
     */
    void next() {
        std::lock_guard<std::mutex> lock(mtx);
        if (keep_last && !rindex_shown) {
            rindex_shown = 1;   // 第一帧保留，不释放
            return;
        }
        av_frame_unref(queue[rindex].frame);  // 释放帧内容
        rindex = (rindex + 1) % max_size;
        size--;
        cv.notify_all();
    }

    /**
     * 返回剩余可读帧数（不包括保留的最后一帧）
     */
    int nb_remaining() {
        std::lock_guard<std::mutex> lock(mtx);
        return size - rindex_shown;
    }

private:
    std::vector<FrameItem> queue;
    int max_size;
    bool keep_last;
    int rindex;           // 读索引（最后返回的帧）
    int windex;           // 写索引
    int size;             // 当前帧总数（包括保留帧）
    int rindex_shown;     // 是否已显示第一帧（用于 keep_last）
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> abort_request;
};

/* ==========================================
 * 音频参数结构 (AudioParams)
 * 描述目标音频格式（重采样后的格式）
 * ========================================== */
struct AudioParams {
    int freq;                           // 采样率
    AVChannelLayout ch_layout;          // 声道布局
    enum AVSampleFormat fmt;            // 采样格式
    int frame_size;                     // 每帧字节数（单声道样本字节数）
    int bytes_per_sec;                  // 每秒字节数
};

/* ==========================================
 * 播放器主内核类 (ModernPlayer)
 * 协调所有线程、队列、时钟、SDL3 渲染和音频输出。
 * ========================================== */
class ModernPlayer {
public:
    ModernPlayer()
        : audio_fq(9, true), video_fq(3, true),
        abort_request(false), paused(false), last_paused(false), step(false),
        seek_req(false), sync_type(AV_SYNC_AUDIO_MASTER),
        audio_volume(1.0f), muted(false),
        audio_stream_idx(-1), video_stream_idx(-1),
        fmt_ctx(nullptr), audio_codec_ctx(nullptr), video_codec_ctx(nullptr),
        swr_ctx(nullptr), sws_ctx(nullptr),
        audio_buf_size(0), audio_buf_index(0), audio_clock(0.0),
        audio_diff_cum(0.0), audio_diff_avg_count(0), frame_timer(0.0),
        audio_stream(nullptr) {
        // 设置目标音频格式：立体声 44100Hz S16
        av_channel_layout_default(&audio_tgt.ch_layout, 2);
        audio_tgt.freq = 44100;
        audio_tgt.fmt = AV_SAMPLE_FMT_S16;
        audio_tgt.frame_size = av_get_bytes_per_sample(audio_tgt.fmt) * audio_tgt.ch_layout.nb_channels;
        audio_tgt.bytes_per_sec = audio_tgt.freq * audio_tgt.frame_size;
        audio_resample_buf.resize(256 * 1024);
    }

    ~ModernPlayer() { stop(); }

    /**
     * 打开媒体文件，启动所有线程
     * @param url 文件路径或网络 URL
     * @return 成功 true
     */
    bool open(const std::string& url) {
        file_url = url;
        abort_request = false;

        // ---------- 解复用器初始化 ----------
        fmt_ctx = avformat_alloc_context();
        if (avformat_open_input(&fmt_ctx, file_url.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) return false;

        // 寻找最佳音视频流
        audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

        // 打开解码器上下文
        if (audio_stream_idx >= 0) open_codec_context(audio_stream_idx);
        if (video_stream_idx >= 0) open_codec_context(video_stream_idx);

        // ---------- SDL3 音频设备初始化 ----------
        if (audio_stream_idx >= 0) {
            init_sdl3_audio();
        }

        // 启动包队列（放入冲刷包）
        audio_pq.start();
        video_pq.start();

        // 启动三个核心线程
        read_th = std::thread(&ModernPlayer::read_thread, this);
        if (audio_stream_idx >= 0) audio_decode_th = std::thread(&ModernPlayer::audio_decode_thread, this);
        if (video_stream_idx >= 0) video_decode_th = std::thread(&ModernPlayer::video_decode_thread, this);

        return true;
    }

    /**
     * 停止播放，等待所有线程退出，释放资源
     */
    void stop() {
        abort_request = true;
        audio_pq.abort();
        video_pq.abort();
        audio_fq.abort();
        video_fq.abort();

        if (read_th.joinable()) read_th.join();
        if (audio_decode_th.joinable()) audio_decode_th.join();
        if (video_decode_th.joinable()) video_decode_th.join();

        // 销毁 SDL3 音频流
        if (audio_stream) {
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = nullptr;
        }

        // 释放 FFmpeg 资源
        if (swr_ctx) { swr_free(&swr_ctx); }
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); }
        if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); }
        if (fmt_ctx) { avformat_close_input(&fmt_ctx); }

        av_channel_layout_uninit(&audio_tgt.ch_layout);
    }

    /* ==================== 控制接口 ==================== */

    /** 切换播放/暂停 */
    void toggle_pause() {
        set_pause(!paused);
        step = false;
    }

    /** 设置暂停状态 */
    void set_pause(bool p) {
        if (paused != p) {
            paused = p;
            if (!p) {
                // 恢复时调整视频帧计时器，避免跳跃
                double time = av_gettime_relative() / 1000000.0;
                frame_timer += time - vidclk.last_updated;
                if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
            }
            else {
                if (audio_stream) SDL_PauseAudioStreamDevice(audio_stream);
            }
            audclk.set_paused(p);
            vidclk.set_paused(p);
            extclk.set_paused(p);
        }
    }

    /** 逐帧播放（暂停时按 S 键调用） */
    void step_to_next_frame() {
        if (paused) {
            set_pause(false);
        }
        step = true;
    }

    /** 音量调节（±0.05） */
    void adjust_volume(float delta) {
        audio_volume = std::clamp(audio_volume + delta, 0.0f, 1.0f);
    }

    /** 切换静音 */
    void toggle_mute() {
        muted = !muted;
    }

    /** 切换同步主时钟类型 */
    void set_sync_type(SyncType type) {
        sync_type = type;
    }

    /** 跳转（相对当前时间偏移，秒） */
    void request_seek(int64_t target_pts_sec) {
        seek_pos = target_pts_sec * AV_TIME_BASE;
        seek_req = true;
    }

    /**
     * 获取主时钟值（根据当前同步类型）
     */
    double get_master_clock() {
        switch (sync_type) {
        case AV_SYNC_VIDEO_MASTER: return vidclk.get();
        case AV_SYNC_AUDIO_MASTER: return audclk.get();
        case AV_SYNC_EXTERNAL_CLOCK:
        default: return extclk.get();
        }
    }

    /* ==================== 视频渲染循环 ==================== */

    /**
     * 视频渲染主循环（运行在独立线程）
     * @param renderer SDL3 渲染器
     * @param texture  SDL3 纹理（内部创建/更新）
     */
    void video_render_loop(SDL_Renderer* renderer, SDL_Texture*& texture) {
        frame_timer = av_gettime_relative() / 1000000.0;

        while (!abort_request) {
            if (video_stream_idx < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 无帧可读则等待
            if (video_fq.nb_remaining() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            FrameItem* vp = video_fq.peek_readable();
            if (!vp) continue;

            // 若序列号不匹配（冲刷后旧帧），跳过
            if (vp->serial != video_pq.get_serial()) {
                video_fq.next();
                continue;
            }

            // ---------- 暂停状态：持续显示最后一帧 ----------
            if (paused) {
                render_frame(renderer, texture, video_fq.peek_last()->frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            // ---------- 计算显示延迟 ----------
            FrameItem* lastvp = video_fq.peek_last();
            double duration = vp_duration(lastvp, vp);    // 当前帧应持续的时间
            double delay = compute_target_delay(duration); // 根据同步调整后的延迟

            double time = av_gettime_relative() / 1000000.0;
            // 若未到显示时间，则睡眠等待
            if (time < frame_timer + delay) {
                double remaining_time = (frame_timer + delay) - time;
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(remaining_time * 1000000)));
                continue;
            }

            // 更新帧计时器
            frame_timer += delay;
            if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX) {
                frame_timer = time;  // 防止累积误差过大
            }

            // 更新视频时钟并同步外部时钟
            vidclk.set(vp->pts, vp->serial);
            extclk.sync_to_slave(vidclk);

            // ---------- 丢帧策略（逐帧模式下禁止丢帧） ----------
            if (!step && video_fq.nb_remaining() > 1) {
                if (time > frame_timer + duration) {
                    video_fq.next();
                    continue;
                }
            }

            // 渲染当前帧
            render_frame(renderer, texture, vp->frame);
            video_fq.next();

            // 逐帧模式下，显示完一帧后立即暂停
            if (step) {
                set_pause(true);
                step = false;
            }
        }
    }

private:
    /* ==================== 内部辅助函数 ==================== */

    /**
     * 打开指定流索引的解码器上下文
     */
    void open_codec_context(int stream_idx) {
        AVStream* st = fmt_ctx->streams[stream_idx];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, st->codecpar);
        codec_ctx->pkt_timebase = st->time_base;
        avcodec_open2(codec_ctx, codec, nullptr);

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_codec_ctx = codec_ctx;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_codec_ctx = codec_ctx;
        }
    }

    /**
     * 初始化 SDL3 音频流（替代传统的音频回调）
     */
    void init_sdl3_audio() {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16;
        spec.channels = audio_tgt.ch_layout.nb_channels;
        spec.freq = audio_tgt.freq;

        // 打开默认播放设备的音频流，并绑定回调函数
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &ModernPlayer::sdl3_audio_callback_static, this);
        if (audio_stream) {
            SDL_ResumeAudioStreamDevice(audio_stream);
        }
        else {
            std::cerr << "Failed to open SDL3 audio device stream: " << SDL_GetError() << std::endl;
        }
    }

    /* ==================== 线程函数 ==================== */

    /**
     * 解复用读线程：从文件中读取 AVPacket 放入对应队列
     */
    void read_thread() {
        AVPacket* pkt = av_packet_alloc();
        while (!abort_request) {
            // 处理跳转请求
            if (seek_req) {
                int ret = avformat_seek_file(fmt_ctx, -1, INT64_MIN, seek_pos, INT64_MAX, 0);
                if (ret >= 0) {
                    // 冲刷所有队列，使解码器重新同步
                    audio_pq.flush();
                    video_pq.flush();
                    audio_pq.put_flush_packet();
                    video_pq.put_flush_packet();
                }
                seek_req = false;
            }

            // 队列满时等待
            if (audio_pq.get_size() + video_pq.get_size() > MAX_QUEUE_SIZE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            int ret = av_read_frame(fmt_ctx, pkt);
            if (ret < 0) {
                // 读不到包时休眠，避免忙等
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 按流类型分发到对应包队列
            if (pkt->stream_index == audio_stream_idx) {
                audio_pq.put(pkt);
            }
            else if (pkt->stream_index == video_stream_idx) {
                video_pq.put(pkt);
            }
            else {
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
    }

    /**
     * 视频解码线程：从视频包队列取包，解码后放入视频帧队列
     */
    void video_decode_thread() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = nullptr;
        int pkt_serial = 0;

        while (!abort_request) {
            if (!video_pq.get(&pkt, pkt_serial)) break;

            if (pkt == flush_pkt) {
                avcodec_flush_buffers(video_codec_ctx);
                continue;
            }

            if (avcodec_send_packet(video_codec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(video_codec_ctx, frame) == 0) {
                    // 计算 PTS（秒）
                    double pts = (frame->pts == AV_NOPTS_VALUE) ? 0.0 : frame->pts * av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

                    // 简单丢帧策略：若 PTS 远落后于主时钟且队列中还有包，则丢弃该帧
                    double diff = pts - get_master_clock();
                    if (!std::isnan(diff) && std::fabs(diff) < AV_NOSYNC_THRESHOLD && diff < 0 && video_pq.get_nb_packets() > 0) {
                        av_frame_unref(frame);
                        continue;
                    }

                    FrameItem* item = video_fq.peek_writable();
                    if (!item) break;

                    av_frame_move_ref(item->frame, frame);
                    item->pts = pts;
                    item->serial = pkt_serial;
                    video_fq.push();
                }
            }
            av_packet_free(&pkt);
        }
        av_frame_free(&frame);
    }

    /**
     * 音频解码线程：从音频包队列取包，解码后放入音频帧队列
     */
    void audio_decode_thread() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = nullptr;
        int pkt_serial = 0;

        while (!abort_request) {
            if (!audio_pq.get(&pkt, pkt_serial)) break;

            if (pkt == flush_pkt) {
                avcodec_flush_buffers(audio_codec_ctx);
                continue;
            }

            if (avcodec_send_packet(audio_codec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(audio_codec_ctx, frame) == 0) {
                    AVRational tb = { 1, frame->sample_rate };
                    double pts = (frame->pts == AV_NOPTS_VALUE) ? 0.0 : frame->pts * av_q2d(tb);

                    FrameItem* item = audio_fq.peek_writable();
                    if (!item) break;

                    av_frame_move_ref(item->frame, frame);
                    item->pts = pts;
                    item->duration = av_q2d({ frame->nb_samples, frame->sample_rate });
                    item->serial = pkt_serial;
                    audio_fq.push();
                }
            }
            av_packet_free(&pkt);
        }
        av_frame_free(&frame);
    }

    /* ==================== 音频输出相关 ==================== */

    /**
     * SDL3 音频回调静态包装（将用户数据转为 this 指针）
     */
    static void sdl3_audio_callback_static(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
        static_cast<ModernPlayer*>(userdata)->sdl3_audio_callback(stream, additional_amount);
    }

    /**
     * SDL3 音频回调核心逻辑：从音频帧队列取出帧，重采样，混音，送入 SDL 流
     */
    void sdl3_audio_callback(SDL_AudioStream* stream, int additional_amount) {
        int len = additional_amount;
        std::vector<uint8_t> mix_buf(len);
        int mix_index = 0;

        while (len > 0) {
            // 若当前音频缓冲区已用完，解码新数据
            if (audio_buf_index >= audio_buf_size) {
                audio_buf_size = audio_decode_frame();
                audio_buf_index = 0;
                if (audio_buf_size < 0) {
                    // 没有可用数据，填充静音
                    audio_buf_size = 1024;
                    std::memset(audio_resample_buf.data(), 0, audio_buf_size);
                }
            }

            int len1 = audio_buf_size - audio_buf_index;
            if (len1 > len) len1 = len;

            // 混音处理（静音/音量调节）
            if (muted || paused) {
                std::memset(mix_buf.data() + mix_index, 0, len1);
            }
            else if (audio_volume >= 0.99f) {
                std::memcpy(mix_buf.data() + mix_index, audio_resample_buf.data() + audio_buf_index, len1);
            }
            else {
                std::memset(mix_buf.data() + mix_index, 0, len1);
                // SDL3 混音函数：音量范围 0.0~1.0
                SDL_MixAudio(mix_buf.data() + mix_index, audio_resample_buf.data() + audio_buf_index, SDL_AUDIO_S16, len1, audio_volume);
            }

            len -= len1;
            mix_index += len1;
            audio_buf_index += len1;
        }

        // 将混音后的数据送入 SDL3 音频流
        SDL_PutAudioStreamData(stream, mix_buf.data(), additional_amount);

        // 更新音频时钟（减去硬件缓冲延迟）
        double hw_buf_delay = static_cast<double>(audio_buf_size - audio_buf_index) / audio_tgt.bytes_per_sec;
        audclk.set(audio_clock - hw_buf_delay, audio_pq.get_serial());
        extclk.sync_to_slave(audclk);
    }

    /**
     * 从音频帧队列取一帧，进行重采样，返回重采样后数据大小
     * @return 数据字节数，负值表示无数据
     */
    int audio_decode_frame() {
        if (paused) return -1;

        FrameItem* af = audio_fq.peek_readable();
        if (!af) return -1;

        if (af->serial != audio_pq.get_serial()) {
            audio_fq.next();
            return -1;
        }

        // 创建/更新重采样器上下文
        if (!swr_ctx) {
            swr_alloc_set_opts2(&swr_ctx,
                &audio_tgt.ch_layout, audio_tgt.fmt, audio_tgt.freq,
                &af->frame->ch_layout, (enum AVSampleFormat)af->frame->format, af->frame->sample_rate,
                0, nullptr);
            swr_init(swr_ctx);
        }

        // 根据主时钟偏差调整输出采样数（实现音频同步）
        int wanted_nb_samples = synchronize_audio(af->frame->nb_samples);
        if (wanted_nb_samples != af->frame->nb_samples) {
            swr_set_compensation(swr_ctx,
                (wanted_nb_samples - af->frame->nb_samples) * audio_tgt.freq / af->frame->sample_rate,
                wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate);
        }

        // 计算输出样本数
        int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx, af->frame->sample_rate) + af->frame->nb_samples,
            audio_tgt.freq, af->frame->sample_rate, AV_ROUND_UP);
        uint8_t* out_data = audio_resample_buf.data();

        // 执行重采样
        int converted_samples = swr_convert(swr_ctx, &out_data, out_samples,
            (const uint8_t**)af->frame->extended_data, af->frame->nb_samples);

        // 更新音频时钟
        audio_clock = af->pts + static_cast<double>(af->frame->nb_samples) / af->frame->sample_rate;
        int resampled_data_size = converted_samples * audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(audio_tgt.fmt);

        audio_fq.next();
        return resampled_data_size;
    }

    /**
     * 音频同步校正：根据主时钟偏差调整采样数
     */
    int synchronize_audio(int nb_samples) {
        if (sync_type == AV_SYNC_AUDIO_MASTER) return nb_samples;

        double diff = audclk.get() - get_master_clock();
        if (!std::isnan(diff) && std::fabs(diff) < AV_NOSYNC_THRESHOLD) {
            audio_diff_cum = diff + 0.9 * audio_diff_cum;
            if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                audio_diff_avg_count++;
            }
            else {
                double avg_diff = audio_diff_cum * (1.0 - 0.9);
                if (std::fabs(avg_diff) >= 0.04) {
                    int wanted_nb_samples = nb_samples + static_cast<int>(diff * audio_tgt.freq);
                    int min_nb_samples = nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    int max_nb_samples = nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100;
                    return std::clamp(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
            }
        }
        else {
            audio_diff_cum = 0;
            audio_diff_avg_count = 0;
        }
        return nb_samples;
    }

    /* ==================== 视频同步辅助 ==================== */

    /**
     * 计算两帧之间的显示时长
     */
    double vp_duration(FrameItem* lastvp, FrameItem* vp) {
        if (lastvp->serial == vp->serial) {
            double duration = vp->pts - lastvp->pts;
            if (std::isnan(duration) || duration <= 0 || duration > 1.0) {
                return vp->duration > 0 ? vp->duration : 0.04;
            }
            return duration;
        }
        return 0.0;
    }

    /**
     * 根据主时钟偏差计算目标显示延迟
     */
    double compute_target_delay(double delay) {
        double diff = vidclk.get() - get_master_clock();
        double sync_threshold = std::max(AV_SYNC_THRESHOLD_MIN, std::min(AV_SYNC_THRESHOLD_MAX, delay));

        if (!std::isnan(diff) && std::fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold) {
                delay = std::max(0.0, delay + diff);
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = delay + diff;
            }
            else if (diff >= sync_threshold) {
                delay = 2 * delay;
            }
        }
        return delay;
    }

    /* ==================== 视频渲染 ==================== */

    /**
     * 将 AVFrame 转换为 SDL 纹理并渲染
     * 支持 YUV420P 直接纹理更新，其他格式通过 sws_scale 转为 BGRA
     */
    void render_frame(SDL_Renderer* renderer, SDL_Texture*& texture, AVFrame* frame) {
        if (!frame || frame->width <= 0 || frame->height <= 0) return;

        int w = frame->width & ~1;
        int h = frame->height & ~1;

        if (frame->format == AV_PIX_FMT_YUV420P && frame->linesize[0] > 0) {
            // 直接使用 YUV 纹理
            if (!texture) {
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
            }
            SDL_UpdateYUVTexture(texture, nullptr,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);
        }
        else {
            // 非 YUV420P，转换为 BGRA 纹理
            if (!texture) {
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            }

            sws_ctx = sws_getCachedContext(sws_ctx,
                frame->width, frame->height, (enum AVPixelFormat)frame->format,
                w, h, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            void* pixels = nullptr;
            int pitch = 0;
            if (SDL_LockTexture(texture, nullptr, &pixels, &pitch)) {
                uint8_t* dst_data[4] = { (uint8_t*)pixels, nullptr, nullptr, nullptr };
                int dst_linesize[4] = { pitch, 0, 0, 0 };

                sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
                SDL_UnlockTexture(texture);
            }
        }

        // 渲染纹理（支持垂直翻转，某些 YUV 格式需要）
        SDL_RenderClear(renderer);
        SDL_FlipMode flip = (frame->linesize[0] < 0 && frame->format == AV_PIX_FMT_YUV420P) ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE;
        SDL_RenderTextureRotated(renderer, texture, nullptr, nullptr, 0.0, nullptr, flip);
        SDL_RenderPresent(renderer);
    }

    /* ==================== 成员变量 ==================== */

    // 状态控制
    std::string file_url;
    std::atomic<bool> abort_request;
    std::atomic<bool> paused;
    bool last_paused;
    std::atomic<bool> step;          // 逐帧模式标志
    std::atomic<bool> seek_req;
    int64_t seek_pos;

    SyncType sync_type;
    std::atomic<float> audio_volume;
    std::atomic<bool> muted;

    // 流索引
    int audio_stream_idx;
    int video_stream_idx;

    // FFmpeg 上下文
    AVFormatContext* fmt_ctx;
    AVCodecContext* audio_codec_ctx;
    AVCodecContext* video_codec_ctx;

    // 重采样/转换上下文
    SwrContext* swr_ctx;
    SwsContext* sws_ctx;

    // 包/帧队列
    PacketQueue audio_pq;
    PacketQueue video_pq;
    FrameQueue audio_fq;
    FrameQueue video_fq;

    // 时钟
    PlayerClock audclk;
    PlayerClock vidclk;
    PlayerClock extclk;

    // 音频参数与缓冲
    AudioParams audio_tgt;
    SDL_AudioStream* audio_stream;      // SDL3 音频流对象
    std::vector<uint8_t> audio_resample_buf;
    int audio_buf_size;
    int audio_buf_index;
    double audio_clock;
    double audio_diff_cum;
    int audio_diff_avg_count;
    double frame_timer;                // 视频渲染计时器

    // 线程
    std::thread read_th;
    std::thread audio_decode_th;
    std::thread video_decode_th;
};

/* ==========================================
 * 主函数与 SDL3 事件循环
 * ========================================== */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./modern_player_sdl3 <media_file_path>" << std::endl;
        return -1;
    }

    // 初始化特殊冲刷包
    flush_pkt = av_packet_alloc();
    flush_pkt->data = (uint8_t*)flush_pkt;  // 标记为冲刷包

    // 初始化 SDL3（视频 + 音频）
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "Could not initialize SDL3: " << SDL_GetError() << std::endl;
        return -1;
    }

    // 创建窗口（SDL3 无需指定 x,y）
    SDL_Window* window = SDL_CreateWindow("Modern A/V Player Core Engine (SDL3 Full)",
        1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return -1;
    }

    // 创建渲染器（SDL3 简化接口）
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_Texture* texture = nullptr;

    // 创建播放器实例并打开文件
    ModernPlayer player;
    if (!player.open(argv[1])) {
        return -1;
    }

    // 启动视频渲染线程
    std::thread render_th([&]() {
        player.video_render_loop(renderer, texture);
        });

    // ---------- 主事件循环 ----------
    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
                break;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.key) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_SPACE:          // 播放/暂停
                    player.toggle_pause();
                    break;
                case SDLK_S:              // 逐帧
                    player.step_to_next_frame();
                    break;
                case SDLK_M:              // 静音切换
                    player.toggle_mute();
                    break;
                case SDLK_UP:             // 音量 +
                    player.adjust_volume(0.05f);
                    break;
                case SDLK_DOWN:           // 音量 -
                    player.adjust_volume(-0.05f);
                    break;
                case SDLK_RIGHT:          // 快进 10 秒
                    player.request_seek(10);
                    break;
                case SDLK_LEFT:           // 快退 10 秒
                    player.request_seek(-10);
                    break;
                case SDLK_1:              // 切换为音频主时钟
                    player.set_sync_type(AV_SYNC_AUDIO_MASTER);
                    break;
                case SDLK_2:              // 切换为视频主时钟
                    player.set_sync_type(AV_SYNC_VIDEO_MASTER);
                    break;
                case SDLK_3:              // 切换为外部时钟
                    player.set_sync_type(AV_SYNC_EXTERNAL_CLOCK);
                    break;
                default:
                    break;
                }
            }
        }
        SDL_Delay(10);
    }

    // 停止播放器并等待渲染线程结束
    player.stop();
    if (render_th.joinable()) render_th.join();

    // 清理 SDL 资源
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // 释放冲刷包
    av_packet_free(&flush_pkt);
    return 0;
}