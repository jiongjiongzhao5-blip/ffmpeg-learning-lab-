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

// ==========================================
// 基础常量与枚举定义
// ==========================================
constexpr int MAX_QUEUE_SIZE = 15 * 1024 * 1024;
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
constexpr double AV_NOSYNC_THRESHOLD = 10.0;
constexpr int SAMPLE_CORRECTION_PERCENT_MAX = 10;
constexpr int AUDIO_DIFF_AVG_NB = 20;

enum SyncType {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK
};

static AVPacket* flush_pkt = nullptr;

// ==========================================
// 时钟模块 (Clock)
// ==========================================
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

    void set(double new_pts, int new_serial) {
        double time = av_gettime_relative() / 1000000.0;
        pts = new_pts;
        last_updated = time;
        pts_drift = new_pts - time;
        serial = new_serial;
    }

    double get() {
        if (paused) return pts;
        if (std::isnan(pts)) return NAN;
        double time = av_gettime_relative() / 1000000.0;
        return pts_drift + time - (time - last_updated) * (1.0 - speed);
    }

    void set_paused(bool p) {
        if (paused != p) {
            paused = p;
            if (!p) {
                set(get(), serial);
            }
        }
    }

    // [11-13/14] 外部时钟校准机制
    void sync_to_slave(PlayerClock& slave) {
        double slave_time = slave.get();
        double master_time = get();
        if (!std::isnan(slave_time) && (std::isnan(master_time) || std::fabs(master_time - slave_time) > AV_NOSYNC_THRESHOLD)) {
            set(slave_time, slave.serial);
        }
    }

public:
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    bool paused;
};

// ==========================================
// 线程安全包队列 (PacketQueue)
// ==========================================
struct PacketItem {
    AVPacket* pkt;
    int serial;
};

class PacketQueue {
public:
    PacketQueue() : abort_request(false), serial(0), size(0), nb_packets(0) {}
    ~PacketQueue() { flush(); }

    void start() {
        std::lock_guard<std::mutex> lock(mtx);
        abort_request = false;
        put_flush_packet();
    }

    void abort() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            abort_request = true;
        }
        cv.notify_all();
    }

    void put_flush_packet() {
        serial++;
        put_internal(flush_pkt);
    }

    bool put(AVPacket* pkt) {
        std::lock_guard<std::mutex> lock(mtx);
        if (abort_request) return false;
        return put_internal(pkt);
    }

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
            cv.wait(lock);
        }
    }

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

    int get_size() const { return size; }
    int get_nb_packets() const { return nb_packets; }
    int get_serial() const { return serial; }

private:
    bool put_internal(AVPacket* pkt) {
        AVPacket* new_pkt = nullptr;
        if (pkt != flush_pkt) {
            new_pkt = av_packet_alloc();
            if (!new_pkt) return false;
            av_packet_ref(new_pkt, pkt);
        }
        else {
            new_pkt = flush_pkt;
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
    int serial;
    int size;
    int nb_packets;
};

// ==========================================
// 线程安全帧队列 (FrameQueue)
// ==========================================
struct FrameItem {
    AVFrame* frame = nullptr;
    double pts = 0.0;
    double duration = 0.0;
    int serial = 0;
};

class FrameQueue {
public:
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

    void abort() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            abort_request = true;
        }
        cv.notify_all();
    }

    FrameItem* peek_writable() {
        std::unique_lock<std::mutex> lock(mtx);
        while (size >= max_size && !abort_request) {
            cv.wait(lock);
        }
        if (abort_request) return nullptr;
        return &queue[windex];
    }

    void push() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            windex = (windex + 1) % max_size;
            size++;
        }
        cv.notify_all();
    }

    FrameItem* peek_readable() {
        std::unique_lock<std::mutex> lock(mtx);
        while (size - rindex_shown <= 0 && !abort_request) {
            cv.wait(lock);
        }
        if (abort_request) return nullptr;
        return &queue[(rindex + rindex_shown) % max_size];
    }

    FrameItem* peek_last() {
        return &queue[rindex];
    }

    void next() {
        std::lock_guard<std::mutex> lock(mtx);
        if (keep_last && !rindex_shown) {
            rindex_shown = 1;
            return;
        }
        av_frame_unref(queue[rindex].frame);
        rindex = (rindex + 1) % max_size;
        size--;
        cv.notify_all();
    }

    int nb_remaining() {
        std::lock_guard<std::mutex> lock(mtx);
        return size - rindex_shown;
    }

private:
    std::vector<FrameItem> queue;
    int max_size;
    bool keep_last;
    int rindex;
    int windex;
    int size;
    int rindex_shown;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> abort_request;
};

// ==========================================
// 播放器主内核
// ==========================================
struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
};

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
        av_channel_layout_default(&audio_tgt.ch_layout, 2);
        audio_tgt.freq = 44100;
        audio_tgt.fmt = AV_SAMPLE_FMT_S16;
        audio_tgt.frame_size = av_get_bytes_per_sample(audio_tgt.fmt) * audio_tgt.ch_layout.nb_channels;
        audio_tgt.bytes_per_sec = audio_tgt.freq * audio_tgt.frame_size;
        audio_resample_buf.resize(256 * 1024);
    }

    ~ModernPlayer() { stop(); }

    bool open(const std::string& url) {
        file_url = url;
        abort_request = false;

        fmt_ctx = avformat_alloc_context();
        if (avformat_open_input(&fmt_ctx, file_url.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) return false;

        audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

        if (audio_stream_idx >= 0) open_codec_context(audio_stream_idx);
        if (video_stream_idx >= 0) open_codec_context(video_stream_idx);

        if (audio_stream_idx >= 0) {
            init_sdl3_audio();
        }

        audio_pq.start();
        video_pq.start();

        read_th = std::thread(&ModernPlayer::read_thread, this);
        if (audio_stream_idx >= 0) audio_decode_th = std::thread(&ModernPlayer::audio_decode_thread, this);
        if (video_stream_idx >= 0) video_decode_th = std::thread(&ModernPlayer::video_decode_thread, this);

        return true;
    }

    void stop() {
        abort_request = true;
        audio_pq.abort();
        video_pq.abort();
        audio_fq.abort();
        video_fq.abort();

        if (read_th.joinable()) read_th.join();
        if (audio_decode_th.joinable()) audio_decode_th.join();
        if (video_decode_th.joinable()) video_decode_th.join();

        // SDL3 音频流销毁
        if (audio_stream) {
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = nullptr;
        }

        if (swr_ctx) { swr_free(&swr_ctx); }
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); }
        if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); }
        if (fmt_ctx) { avformat_close_input(&fmt_ctx); }

        av_channel_layout_uninit(&audio_tgt.ch_layout);
    }

    // [11-15] 播放/暂停控制
    void toggle_pause() {
        set_pause(!paused);
        step = false;
    }

    void set_pause(bool p) {
        if (paused != p) {
            paused = p;
            if (!p) {
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

    // [11-16] 逐帧播放
    void step_to_next_frame() {
        if (paused) {
            set_pause(false);
        }
        step = true;
    }

    // [11-16] 音量调节与静音 (SDL3 浮点音量 0.0f - 1.0f)
    void adjust_volume(float delta) {
        audio_volume = std::clamp(audio_volume + delta, 0.0f, 1.0f);
    }

    void toggle_mute() {
        muted = !muted;
    }

    // [11-13/14] 同步基准切换
    void set_sync_type(SyncType type) {
        sync_type = type;
    }

    void request_seek(int64_t target_pts_sec) {
        seek_pos = target_pts_sec * AV_TIME_BASE;
        seek_req = true;
    }

    double get_master_clock() {
        switch (sync_type) {
        case AV_SYNC_VIDEO_MASTER: return vidclk.get();
        case AV_SYNC_AUDIO_MASTER: return audclk.get();
        case AV_SYNC_EXTERNAL_CLOCK:
        default: return extclk.get();
        }
    }

    // [11-10] 视频主刷新与渲染循环 (适配 SDL3)
    void video_render_loop(SDL_Renderer* renderer, SDL_Texture*& texture) {
        frame_timer = av_gettime_relative() / 1000000.0;

        while (!abort_request) {
            if (video_stream_idx < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (video_fq.nb_remaining() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            FrameItem* vp = video_fq.peek_readable();
            if (!vp) continue;

            if (vp->serial != video_pq.get_serial()) {
                video_fq.next();
                continue;
            }

            // [11-15] 暂停时一直保持渲染最后一帧
            if (paused) {
                render_frame(renderer, texture, video_fq.peek_last()->frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            FrameItem* lastvp = video_fq.peek_last();
            double duration = vp_duration(lastvp, vp);
            double delay = compute_target_delay(duration);

            double time = av_gettime_relative() / 1000000.0;
            if (time < frame_timer + delay) {
                double remaining_time = (frame_timer + delay) - time;
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(remaining_time * 1000000)));
                continue;
            }

            frame_timer += delay;
            if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX) {
                frame_timer = time;
            }

            vidclk.set(vp->pts, vp->serial);
            extclk.sync_to_slave(vidclk);

            // [11-16] 逐帧模式下禁用丢帧
            if (!step && video_fq.nb_remaining() > 1) {
                if (time > frame_timer + duration) {
                    video_fq.next();
                    continue;
                }
            }

            render_frame(renderer, texture, vp->frame);
            video_fq.next();

            // [11-16] 逐帧显示完成后立刻重新暂停
            if (step) {
                set_pause(true);
                step = false;
            }
        }
    }

private:
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

    // SDL3 采用 AudioStream 进行设备绑定与回调处理
    void init_sdl3_audio() {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16;
        spec.channels = audio_tgt.ch_layout.nb_channels;
        spec.freq = audio_tgt.freq;

        // 打开默认播放设备的音频流
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &ModernPlayer::sdl3_audio_callback_static, this);
        if (audio_stream) {
            SDL_ResumeAudioStreamDevice(audio_stream);
        }
        else {
            std::cerr << "Failed to open SDL3 audio device stream: " << SDL_GetError() << std::endl;
        }
    }

    void read_thread() {
        AVPacket* pkt = av_packet_alloc();
        while (!abort_request) {
            if (seek_req) {
                int ret = avformat_seek_file(fmt_ctx, -1, INT64_MIN, seek_pos, INT64_MAX, 0);
                if (ret >= 0) {
                    audio_pq.flush();
                    video_pq.flush();
                    audio_pq.put_flush_packet();
                    video_pq.put_flush_packet();
                }
                seek_req = false;
            }

            if (audio_pq.get_size() + video_pq.get_size() > MAX_QUEUE_SIZE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            int ret = av_read_frame(fmt_ctx, pkt);
            if (ret < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

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
                    double pts = (frame->pts == AV_NOPTS_VALUE) ? 0.0 : frame->pts * av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

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

    // SDL3 音频回调静态转接函数
    static void sdl3_audio_callback_static(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
        static_cast<ModernPlayer*>(userdata)->sdl3_audio_callback(stream, additional_amount);
    }

    // [11-16] SDL3 音频回调处理与音量调节 (SDL_MixAudio)
    void sdl3_audio_callback(SDL_AudioStream* stream, int additional_amount) {
        int len = additional_amount;
        std::vector<uint8_t> mix_buf(len);
        int mix_index = 0;

        while (len > 0) {
            if (audio_buf_index >= audio_buf_size) {
                audio_buf_size = audio_decode_frame();
                audio_buf_index = 0;
                if (audio_buf_size < 0) {
                    audio_buf_size = 1024;
                    std::memset(audio_resample_buf.data(), 0, audio_buf_size);
                }
            }

            int len1 = audio_buf_size - audio_buf_index;
            if (len1 > len) len1 = len;

            if (muted || paused) {
                std::memset(mix_buf.data() + mix_index, 0, len1);
            }
            else if (audio_volume >= 0.99f) {
                std::memcpy(mix_buf.data() + mix_index, audio_resample_buf.data() + audio_buf_index, len1);
            }
            else {
                std::memset(mix_buf.data() + mix_index, 0, len1);
                // SDL3: SDL_MixAudio(dst, src, format, len, volume) volume 范围为 0.0f - 1.0f
                SDL_MixAudio(mix_buf.data() + mix_index, audio_resample_buf.data() + audio_buf_index, SDL_AUDIO_S16, len1, audio_volume);
            }

            len -= len1;
            mix_index += len1;
            audio_buf_index += len1;
        }

        // 送入 SDL3 音频流
        SDL_PutAudioStreamData(stream, mix_buf.data(), additional_amount);

        double hw_buf_delay = static_cast<double>(audio_buf_size - audio_buf_index) / audio_tgt.bytes_per_sec;
        audclk.set(audio_clock - hw_buf_delay, audio_pq.get_serial());
        extclk.sync_to_slave(audclk);
    }

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

    int audio_decode_frame() {
        if (paused) return -1;

        FrameItem* af = audio_fq.peek_readable();
        if (!af) return -1;

        if (af->serial != audio_pq.get_serial()) {
            audio_fq.next();
            return -1;
        }

        if (!swr_ctx) {
            swr_alloc_set_opts2(&swr_ctx,
                &audio_tgt.ch_layout, audio_tgt.fmt, audio_tgt.freq,
                &af->frame->ch_layout, (enum AVSampleFormat)af->frame->format, af->frame->sample_rate,
                0, nullptr);
            swr_init(swr_ctx);
        }

        int wanted_nb_samples = synchronize_audio(af->frame->nb_samples);
        if (wanted_nb_samples != af->frame->nb_samples) {
            swr_set_compensation(swr_ctx,
                (wanted_nb_samples - af->frame->nb_samples) * audio_tgt.freq / af->frame->sample_rate,
                wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate);
        }

        int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx, af->frame->sample_rate) + af->frame->nb_samples,
            audio_tgt.freq, af->frame->sample_rate, AV_ROUND_UP);
        uint8_t* out_data = audio_resample_buf.data();

        int converted_samples = swr_convert(swr_ctx, &out_data, out_samples,
            (const uint8_t**)af->frame->extended_data, af->frame->nb_samples);

        audio_clock = af->pts + static_cast<double>(af->frame->nb_samples) / af->frame->sample_rate;
        int resampled_data_size = converted_samples * audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(audio_tgt.fmt);

        audio_fq.next();
        return resampled_data_size;
    }

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

    // [11-10] 完整图像格式转换与渲染 (SwsScale / SDL3 规范)
    void render_frame(SDL_Renderer* renderer, SDL_Texture*& texture, AVFrame* frame) {
        if (!frame || frame->width <= 0 || frame->height <= 0) return;

        int w = frame->width & ~1;
        int h = frame->height & ~1;

        if (frame->format == AV_PIX_FMT_YUV420P && frame->linesize[0] > 0) {
            if (!texture) {
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
            }
            SDL_UpdateYUVTexture(texture, nullptr,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);
        }
        else {
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

        SDL_RenderClear(renderer);
        // SDL3: 旋转翻转渲染统一采用 SDL_RenderTextureRotated
        SDL_FlipMode flip = (frame->linesize[0] < 0 && frame->format == AV_PIX_FMT_YUV420P) ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE;
        SDL_RenderTextureRotated(renderer, texture, nullptr, nullptr, 0.0, nullptr, flip);
        SDL_RenderPresent(renderer);
    }

private:
    std::string file_url;
    std::atomic<bool> abort_request;
    std::atomic<bool> paused;
    bool last_paused;
    std::atomic<bool> step;
    std::atomic<bool> seek_req;
    int64_t seek_pos;

    SyncType sync_type;
    std::atomic<float> audio_volume;
    std::atomic<bool> muted;

    int audio_stream_idx;
    int video_stream_idx;

    AVFormatContext* fmt_ctx;
    AVCodecContext* audio_codec_ctx;
    AVCodecContext* video_codec_ctx;

    SwrContext* swr_ctx;
    SwsContext* sws_ctx;

    PacketQueue audio_pq;
    PacketQueue video_pq;
    FrameQueue audio_fq;
    FrameQueue video_fq;

    PlayerClock audclk;
    PlayerClock vidclk;
    PlayerClock extclk;

    AudioParams audio_tgt;
    SDL_AudioStream* audio_stream; // SDL3 音频流对象
    std::vector<uint8_t> audio_resample_buf;
    int audio_buf_size;
    int audio_buf_index;
    double audio_clock;
    double audio_diff_cum;
    int audio_diff_avg_count;
    double frame_timer;

    std::thread read_th;
    std::thread audio_decode_th;
    std::thread video_decode_th;
};

// ==========================================
// 主函数与 SDL3 键盘事件驱动
// ==========================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./modern_player_sdl3 <media_file_path>" << std::endl;
        return -1;
    }

    flush_pkt = av_packet_alloc();
    flush_pkt->data = (uint8_t*)flush_pkt;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "Could not initialize SDL3: " << SDL_GetError() << std::endl;
        return -1;
    }

    // SDL3 创建窗口不再需要传 x, y
    SDL_Window* window = SDL_CreateWindow("Modern A/V Player Core Engine (SDL3 Full)",
        1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return -1;
    }

    // SDL3 创建渲染器简化接口
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_Texture* texture = nullptr;

    ModernPlayer player;
    if (!player.open(argv[1])) {
        return -1;
    }

    std::thread render_th([&]() {
        player.video_render_loop(renderer, texture);
        });

    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) { // SDL3 事件类型升级
                running = false;
                break;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.key) { // SDL3 使用 event.key.key
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_SPACE: // [11-15] 空格键：播放/暂停
                    player.toggle_pause();
                    break;
                case SDLK_S:     // [11-16] S键：逐帧播放
                    player.step_to_next_frame();
                    break;
                case SDLK_M:     // [11-16] M键：静音切换
                    player.toggle_mute();
                    break;
                case SDLK_UP:    // [11-16] 方向键上：音量+
                    player.adjust_volume(0.05f);
                    break;
                case SDLK_DOWN:  // [11-16] 方向键下：音量-
                    player.adjust_volume(-0.05f);
                    break;
                case SDLK_RIGHT: // 快进 10s
                    player.request_seek(10);
                    break;
                case SDLK_LEFT:  // 快退 10s
                    player.request_seek(-10);
                    break;
                case SDLK_1:     // [11-13/14] 切换为音频主时钟
                    player.set_sync_type(AV_SYNC_AUDIO_MASTER);
                    break;
                case SDLK_2:     // [11-13/14] 切换为视频主时钟
                    player.set_sync_type(AV_SYNC_VIDEO_MASTER);
                    break;
                case SDLK_3:     // [11-13/14] 切换为外部主时钟
                    player.set_sync_type(AV_SYNC_EXTERNAL_CLOCK);
                    break;
                default:
                    break;
                }
            }
        }
        SDL_Delay(10);
    }

    player.stop();
    if (render_th.joinable()) render_th.join();

    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    av_packet_free(&flush_pkt);
    return 0;
}