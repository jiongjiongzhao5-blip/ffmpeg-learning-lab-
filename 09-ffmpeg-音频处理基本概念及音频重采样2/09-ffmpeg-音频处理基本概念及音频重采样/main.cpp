// ============================================================================
// 文件名: AudioResamplePipeline.hpp
// 描述:  基于 FFmpeg 5.0+ (现代 API) 实现的高性能音频重采样管道。
//        核心设计思路：输入任意格式 (采样率/声道/格式) -> 重采样 -> 内部 FIFO
//        缓存 -> 输出固定大小的帧 (便于后续编码或处理)。
// 作者:   (根据代码推断)
// 日期:   (根据上下文)
// ============================================================================
#include <iostream>
#include <vector>
#include <memory>
#include <cstdint>

/*
 * FFmpeg 是纯 C 语言库，在 C++ 中包含时必须使用 extern "C" 包裹，
 * 防止 C++ 编译器对函数名进行 Name Mangling (名称修饰)，
 * 否则链接器会找不到 FFmpeg 中导出的符号。
 */
extern "C" {
#include <libavutil/opt.h>                // FFmpeg 选项框架 (此处未显式使用，但 swr_alloc 依赖其上下文)
#include <libavutil/channel_layout.h>     // 现代声道布局 API (AVChannelLayout)，替代已弃用的 uint64_t 掩码
#include <libavutil/samplefmt.h>          // 采样格式枚举 (AVSampleFormat，如 AV_SAMPLE_FMT_FLTP)
#include <libavutil/audio_fifo.h>         // 音频 FIFO 缓冲区，用于缓存重采样后的数据，以解决输入/输出采样点数量不匹配的问题
#include <libavutil/frame.h>              // AVFrame 结构体，用于承载音频数据及元数据 (PTS, 采样率等)
#include <libswresample/swresample.h>     // 音频重采样库，核心功能：采样率转换、声道重排/混音、格式转换
}

/**
 * @class AudioResamplePipeline
 * @brief 音频重采样管道类，RAII 风格管理 FFmpeg 资源。
 *
 * @details
 * 1. **构造函数**：初始化 SwrContext (重采样器) 和 AVAudioFifo (缓冲区)。
 * 2. **PushFrame**：接收输入 AVFrame，进行重采样，将结果压入 FIFO。
 *    - 支持传入 nullptr 来执行 Flush (冲刷重采样器内部的缓存的延迟采样)。
 *    - 自动处理输入 PTS 到输出 PTS 的时间基转换。
 * 3. **PopFrame**：从 FIFO 中取出固定大小 (out_frame_size_) 的数据，封装成 AVFrame。
 * 4. **线程安全**：此类非线程安全，调用者需在外部加锁。
 * 5. **拷贝控制**：拷贝构造和赋值操作被显式删除 (Rule of Five)，防止浅拷贝导致资源双删。
 */
class AudioResamplePipeline {
public:
    /**
     * @brief 构造函数：配置并初始化重采样器及缓存 FIFO。
     *
     * @param in_layout       输入声道布局 (例如 AVChannelLayout 的立体声、5.1 环绕声等)
     * @param in_sample_rate  输入采样率 (Hz，如 44100)
     * @param in_fmt          输入采样格式 (如 AV_SAMPLE_FMT_S16P 平面有符号16位)
     * @param out_layout      输出声道布局
     * @param out_sample_rate 输出采样率 (Hz，如 48000)
     * @param out_fmt         输出采样格式 (如 AV_SAMPLE_FMT_FLTP 平面浮点)
     * @param out_frame_size  输出帧大小 (采样点数)。该值由上层业务决定 (例如 AAC 编码每帧 1024 个样本点)。
     *
     * @throws std::runtime_error 当 SwrContext 初始化失败或 FIFO 内存分配失败时抛出。
     */
    AudioResamplePipeline(const AVChannelLayout& in_layout, int in_sample_rate, AVSampleFormat in_fmt,
        const AVChannelLayout& out_layout, int out_sample_rate, AVSampleFormat out_fmt,
        int out_frame_size)
        : out_sample_rate_(out_sample_rate), out_frame_size_(out_frame_size), out_fmt_(out_fmt) {

        /*
         * 【深拷贝声道布局】
         * 参数传入的是 const 引用，可能指向临时对象或外部短暂对象。
         * 类内部需要持久保存这两个布局信息 (用于 PopFrame 构建 AVFrame 和析构释放)。
         * av_channel_layout_copy 会深层拷贝内部数据 (如自定义声道映射表)，必须配对调用 uninit。
         */
        av_channel_layout_copy(&in_layout_, &in_layout);
        av_channel_layout_copy(&out_layout_, &out_layout);

        /*
         * 【创建重采样器上下文 (SwrContext)】
         * 使用现代 API: swr_alloc_set_opts2，替代已弃用的 swr_alloc_set_opts (后者使用 uint64_t 掩码)。
         * 第 1 个参数: 传入 SwrContext 指针的地址 (若为 NULL 则自动分配)。
         * 第 2-4 参数: 输出参数 (布局, 格式, 采样率)。
         * 第 5-7 参数: 输入参数 (布局, 格式, 采样率)。
         * 第 8 参数:  日志级别 (0 表示默认)。
         * 第 9 参数:  日志上下文 (nullptr)。
         * 返回值: 成功返回 0，失败返回负错误码。
         */
        int ret = swr_alloc_set_opts2(&swr_ctx_,
            &out_layout_, out_fmt_, out_sample_rate_,
            &in_layout_, in_fmt, in_sample_rate,
            0, nullptr);
        if (ret < 0 || swr_init(swr_ctx_) < 0) {
            // swr_init 会分配内部缓存和滤波器表。如果参数组合不合理 (例如采样率转换比率过大)，此处会失败。
            throw std::runtime_error("SwrContext initialization failed.");
        }

        /*
         * 【分配音频 FIFO】
         * 为什么需要 FIFO？
         * 因为重采样过程是异步的：输入 1024 个采样点，输出可能是 960 或 1100 个采样点 (取决于采样率转换比)。
         * 重采样器不能保证每次 PushFrame 恰好生成 out_frame_size_ 个样本。
         * 因此需要用 FIFO 来攒数据，凑够 out_frame_size_ 个样本再 Pop。
         *
         * av_audio_fifo_alloc 参数:
         *   1. 采样格式 (用于计算每个样本占用的字节数)。
         *   2. 声道数 (nb_channels)。
         *   3. 初始分配的采样点数容量。此处申请 2 * out_frame_size_ 是为了应对瞬时峰值，
         *      防止一次性写入过多数据导致频繁扩容 (虽然 FIFO 会自动扩容，但预分配提升性能)。
         */
        fifo_ = av_audio_fifo_alloc(out_fmt_, out_layout_.nb_channels, out_frame_size_ * 2);
        if (!fifo_) {
            throw std::runtime_error("av_audio_fifo_alloc failed.");
        }
    }

    /**
     * @brief 析构函数：释放所有 FFmpeg 申请的堆内存。
     *
     * @note 释放顺序需要注意：先释放 FIFO，再释放重采样器，最后反初始化声道布局。
     *       swr_free 会将传入的指针置为 NULL，这是 FFmpeg 的标准安全做法。
     */
    ~AudioResamplePipeline() {
        if (fifo_) av_audio_fifo_free(fifo_);   // 释放 FIFO 内部数据及自身结构
        if (swr_ctx_) swr_free(&swr_ctx_);       // swr_free 内部会调用 swr_close，并释放所有缓存
        av_channel_layout_uninit(&in_layout_);   // 释放深拷贝的声道布局内部动态内存 (如自定义映射表)
        av_channel_layout_uninit(&out_layout_);
    }

    // 【禁用拷贝构造和赋值运算符】
    // 因为该类管理了 FFmpeg 原始指针 (swr_ctx_, fifo_)，若执行浅拷贝会导致两个对象指向同一内存，
    // 析构时造成 double-free (双重释放)。强制使用移动语义或直接传引用。
    AudioResamplePipeline(const AudioResamplePipeline&) = delete;
    AudioResamplePipeline& operator=(const AudioResamplePipeline&) = delete;

    /**
     * @brief 向管道推入一帧原始音频数据。
     *
     * @param in_frame 输入 AVFrame 指针。
     *                 - 正常情况: 指向包含音频数据的帧。
     *                 - Flush 模式: 传入 nullptr，通知重采样器输出内部所有缓存的延迟数据 (在文件末尾很有用)。
     * @return true  操作成功 (即使没有输出任何采样点，只要无错误即返回 true)。
     * @return false 发生内存分配失败或 swr_convert 内部错误。
     *
     * @details 【详细执行流程】
     * 1. **PTS 处理 (时间戳)**：
     *    - 首次接收有效帧时，利用 av_rescale_q 将输入 PTS (基于输入采样率) 转换为输出采样率下的 PTS。
     *    - 注意：这里假设输入 AVFrame 的 time_base 为 {1, sample_rate} (即 PTS 以采样点数计数)。
     *    - next_out_pts_ 将用于 PopFrame 时给输出帧赋值，并严格递增。
     * 2. **计算输出缓冲区大小**：
     *    - swr_get_delay 获取重采样器内部缓存的采样数 (由于采样率转换的相位补偿)。
     *    - 使用 av_rescale_rnd (带四舍五入) 估算本次转换可能产生的最大输出采样数。
     * 3. **分配临时缓冲区**：
     *    - av_samples_alloc_array_and_samples 会同时分配指针数组 (针对平面格式) 和数据内存。
     *    - 对于交错格式 (packed)，extended_data[0] 指向全部数据；对于平面格式 (planar)，每通道单独一行。
     * 4. **执行重采样**：
     *    - swr_convert 处理数据转换。如果 in_frame 为 nullptr，则 in_data 为 nullptr, in_samples 为 0，
     *      此时 swr_convert 会利用内部缓存的延迟数据生成最后的输出样本 (Flush 行为)。
     * 5. **写入 FIFO**：
     *    - 将转换后的数据拷贝到 FIFO 中 (av_audio_fifo_write 会执行内存拷贝)。
     * 6. **清理**：
     *    - 释放临时缓冲区。注意释放方式：先释放数据区 (converted_data[0])，再释放指针数组 (converted_data)。
     */
    bool PushFrame(const AVFrame* in_frame) {
        // 提取输入数据和采样数。若为 nullptr，则视为 Flush 请求，传入空指针和 0 采样点。
        uint8_t** in_data = in_frame ? in_frame->extended_data : nullptr;
        int in_samples = in_frame ? in_frame->nb_samples : 0;

        // ===== 第一部分：PTS (Presentation Time Stamp) 初始重映射 =====
        // 仅在第一个有效帧时计算一次。此实现假设后续输入帧的 PTS 是连续的，或者调用者不关心 next_in_pts_。
        // 注意：next_in_pts_ 在此类中仅用作 "是否已经初始化" 的标志位，并未被后续逻辑真正使用。
        if (in_frame && next_in_pts_ == AV_NOPTS_VALUE && in_frame->pts != AV_NOPTS_VALUE) {
            next_in_pts_ = in_frame->pts; // 记录一下，实际未被使用 (可能用于丢包检测，此处仅作占位)
            // av_rescale_q: 将 PTS 从输入时间基 {1, in_frame->sample_rate} 转换到输出时间基 {1, out_sample_rate_}。
            // 例如：输入 44100Hz, PTS=44100 (代表 1秒)，输出 48000Hz，则 PTS 变为 48000。
            next_out_pts_ = av_rescale_q(in_frame->pts,
                { 1, in_frame->sample_rate },
                { 1, out_sample_rate_ });
        }

        // ===== 第二部分：计算本次重采样可能产生的最大输出采样点数 =====
        /*
         * swr_get_delay 获取重采样器内部暂存的延迟采样数。
         * 为什么会有延迟？因为在采样率转换 (如 44100 -> 48000) 中，重采样滤波器需要累积一定的历史样本来插值。
         * 参数传入输入采样率，返回值换算基于输入采样率的时间基。为了统一，传入当前帧的采样率或输出采样率。
         * 此处采用条件判断：如果是 Flush (in_frame 为空)，传入 out_sample_rate_ 更准确。
         */
        int64_t delay = swr_get_delay(swr_ctx_, in_frame ? in_frame->sample_rate : out_sample_rate_);

        /*
         * av_rescale_rnd: 计算 (delay + in_samples) * (out_rate / in_rate) 并向上取整 (AV_ROUND_UP)。
         * 为什么向上取整？因为重采样输出数量不是整数比，多分配几个字节防止缓冲区溢出，确保安全。
         * 类比：44.1kHz 转 48kHz，输入 1024 样本，输出可能是 1114.5，向上取整为 1115。
         */
        int max_out_samples = av_rescale_rnd(delay + in_samples,
            out_sample_rate_,
            in_frame ? in_frame->sample_rate : out_sample_rate_,
            AV_ROUND_UP);

        if (max_out_samples <= 0) return true; // 如果估算为 0，则无需处理，直接返回成功。

        // ===== 第三部分：分配临时输出缓冲区 =====
        uint8_t** converted_data = nullptr;
        int linesize = 0; // 行大小 (对于音频通常指每通道的字节跨度，此处未使用但 API 要求传入)

        /*
         * av_samples_alloc_array_and_samples:
         * 1. 分配指针数组 (对于平面格式，指针数 = 声道数；对于打包格式，指针数 = 1)。
         * 2. 分配实际的数据内存，并让指针数组指向对应的内存区域。
         * 3. 返回 0 成功，负值失败。
         * 参数: 指针数组地址, 行大小地址, 通道数, 采样点数, 采样格式, 对齐 (0 表示默认对齐)。
         */
        if (av_samples_alloc_array_and_samples(&converted_data, &linesize,
            out_layout_.nb_channels,
            max_out_samples, out_fmt_, 0) < 0) {
            return false;
        }

        // ===== 第四部分：执行重采样转换 =====
        /*
         * swr_convert 核心转换函数：
         * 参数 1: SwrContext。
         * 参数 2: 输出缓冲区 (uint8_t** 类型，对应 converted_data)。
         * 参数 3: 输出缓冲区最大可容纳的采样点数 (此处为 max_out_samples)。
         * 参数 4: 输入缓冲区 (const uint8_t**，需要强转)。
         * 参数 5: 输入的采样点数。
         * 返回值: 实际输出的采样点数 (>=0)，负值表示错误。
         *
         * 注意：对于 Flush (in_data == nullptr)，此处会触发重采样器输出内部缓存的样本。
         * swr_convert 可能会消耗少于 in_samples 的样本 (内部缓冲区满了)，但此处未做循环处理。
         * 对于大多数流式场景，输入数据量适中，单次调用即可。若输入超大，则需循环调用。
         */
        int real_out_samples = swr_convert(swr_ctx_, converted_data, max_out_samples,
            (const uint8_t**)in_data, in_samples);

        // ===== 第五部分：将有效数据写入 FIFO =====
        if (real_out_samples > 0) {
            /*
             * av_audio_fifo_write:
             * 将 converted_data 中的数据拷贝到 FIFO 内部缓冲区。
             * 参数 (void**) 强制转换，因为 FIFO 不关心具体指针类型，只负责内存搬运。
             * 写入后，FIFO 内部 size 增加 real_out_samples。
             */
            av_audio_fifo_write(fifo_, (void**)converted_data, real_out_samples);
        }

        // ===== 第六部分：释放临时缓冲区 =====
        /*
         * 由于 converted_data 是由 av_samples_alloc_array_and_samples 分配的，
         * 其内存布局为：[指针数组] -> [实际数据块]。
         * 因此必须分两步释放：
         * 1. av_freep(&converted_data[0]) 释放实际数据块，并将 converted_data[0] 置 NULL。
         * 2. av_freep(&converted_data) 释放指针数组本身。
         * 不可直接 av_free(converted_data)，那样只会释放指针数组，导致数据块内存泄漏。
         */
        av_freep(&converted_data[0]);
        av_freep(&converted_data);

        // real_out_samples >= 0 表示无错误，即使为 0 (没有新输出) 也算成功。
        return real_out_samples >= 0;
    }

    /**
     * @brief 从 FIFO 中弹出一个固定大小的音频帧。
     *
     * @return std::unique_ptr<AVFrame, void(*)(AVFrame*)>
     *         包含有效 AVFrame 的智能指针，若 FIFO 内数据不足 out_frame_size_ 则返回空指针。
     *
     * @details 【执行流程】
     * 1. 检查 FIFO 中的有效采样点数是否 >= out_frame_size_。
     * 2. 分配 AVFrame 结构体，设置全部元数据 (采样率、格式、声道布局、采样点数)。
     * 3. 调用 av_frame_get_buffer 为帧分配数据内存 (此操作会根据声道数和格式计算缓冲区大小)。
     * 4. 调用 av_audio_fifo_read 从 FIFO 中读取数据到 frame->extended_data 指向的内存。
     *    - 读取会消耗 FIFO 中的数据 (读取后 FIFO 内部指针前移)。
     * 5. 设置 PTS 为之前计算好的 next_out_pts_，然后 next_out_pts_ 加上 out_frame_size_。
     *    - 因为输出采样率是恒定的，且输出帧大小固定，所以 PTS 严格等差递增。
     * 6. 返回带有自定义删除器的 unique_ptr，确保 AVFrame 被正确释放 (av_frame_free)。
     */
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> PopFrame() {
        // 检查 FIFO 中累计的采样点数是否足够凑够一帧。
        // 这里使用 < 而不是 <=，因为等于刚好够时应该读取。
        if (av_audio_fifo_size(fifo_) < out_frame_size_) {
            // 返回空智能指针。删除器设为空 lambda，防止对空指针误操作。
            return { nullptr, [](AVFrame*) {} };
        }

        // --- 1. 分配 AVFrame 结构体 (栈上只是一个指针，实际结构在堆上) ---
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return { nullptr, [](AVFrame*) {} };
        }

        // --- 2. 填充帧元数据 ---
        frame->nb_samples = out_frame_size_;
        frame->format = out_fmt_;
        frame->sample_rate = out_sample_rate_;
        // 深拷贝输出声道布局到帧中 (frame 析构时会自动 uninit，不影响外部)。
        av_channel_layout_copy(&frame->ch_layout, &out_layout_);

        // --- 3. 为帧分配音频数据缓冲区 ---
        /*
         * av_frame_get_buffer:
         * 根据 frame 中设定的 nb_samples, format, ch_layout 计算所需 buffer 大小并分配内存。
         * 分配后，frame->extended_data 和 frame->data 将指向有效的内存区域。
         * 参数 0 表示默认对齐 (32 字节对齐，有利于 SIMD 优化)。
         * 返回值 < 0 表示失败 (如内存不足)。
         */
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return { nullptr, [](AVFrame*) {} };
        }

        // --- 4. 从 FIFO 读取数据到帧缓冲区 ---
        /*
         * av_audio_fifo_read:
         * 从 FIFO 中读取 out_frame_size_ 个采样点到 frame->extended_data。
         * 对于平面格式，frame->extended_data[0] 对应通道 0，[1] 对应通道 1 ...
         * 对于打包格式，frame->extended_data[0] 包含所有通道的交错数据。
         * 注意强制转换为 void**，因为 FIFO 内部操作的是无类型指针。
         * 该函数会从 FIFO 中移除读取的数据。
         */
        av_audio_fifo_read(fifo_, (void**)frame->extended_data, out_frame_size_);

        // --- 5. 设置 PTS 并更新内部计数器 ---
        frame->pts = next_out_pts_;
        /*
         * 输出采样率固定，帧大小固定，因此下一个输出帧的 PTS 必须严格增加 out_frame_size_。
         * 这样保证了音频数据的时间轴连续性，避免编码器或播放器出现时间戳跳跃。
         */
        next_out_pts_ += out_frame_size_;

        // --- 6. 返回智能指针 ---
        /*
         * 自定义删除器: 使用 av_frame_free 释放帧及其内部缓冲。
         * av_frame_free 会检查指针是否为 NULL，并将指针置 NULL，非常安全。
         */
        return { frame, [](AVFrame* f) { av_frame_free(&f); } };
    }

private:
    // ===================== FFmpeg 核心对象 =====================
    SwrContext* swr_ctx_ = nullptr;      // 重采样上下文。持有内部滤波器状态、缓存、缓冲区等。
    AVAudioFifo* fifo_ = nullptr;        // 音频 FIFO。解决重采样输出数据量不固定与用户期望固定帧大小之间的矛盾。

    // ===================== 声道布局 (深拷贝副本) =====================
    AVChannelLayout in_layout_{};        // 输入声道布局。用于构造时初始化，以及可能的调试。
    AVChannelLayout out_layout_{};       // 输出声道布局。PopFrame 时需要用来填充 AVFrame。

    // ===================== 输出参数 (缓存) =====================
    int out_sample_rate_;                // 输出采样率 (Hz)
    int out_frame_size_;                 // 输出帧大小 (采样点数，固定)
    AVSampleFormat out_fmt_;             // 输出采样格式 (如 FLTP, S16 等)

    // ===================== PTS (时间戳) 状态 =====================
    int64_t next_in_pts_ = AV_NOPTS_VALUE;  // 仅用于记录首次输入 PTS，此处作为首次初始化标志 (实际未参与计算)。
    int64_t next_out_pts_ = 0;               // 下一个输出帧的 PTS 值。PopFrame 时累加 out_frame_size_。
};