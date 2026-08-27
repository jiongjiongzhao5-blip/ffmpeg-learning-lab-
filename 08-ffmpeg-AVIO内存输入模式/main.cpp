#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <cstring>

/**
 * FFmpeg 库是用 C 语言编写的，在 C++ 中引用其头文件时必须用 extern "C" 包裹，
 * 以防止 C++ 编译器对函数名进行 name mangling（名称修饰），否则链接时会找不到符号。
 */
extern "C" {
#include <libavformat/avformat.h>   // 容器格式（封装/解封装）相关 API
#include <libavformat/avio.h>       // 输入/输出抽象层（AVIO）API
#include <libavcodec/avcodec.h>     // 编解码器 API
#include <libavutil/mem.h>          // 内存管理辅助（如 av_malloc）
#include <libavutil/samplefmt.h>    // 采样格式相关定义（如 planar/packed 判断）
}

// ============================================================================
// 1. 自定义内存缓冲区上下文
//    FFmpeg 的 AVIOContext 允许用户提供自定义的读/写/定位回调函数。
//    我们通过这个结构体在回调之间传递内存数据的状态。
// ============================================================================
struct MemoryBufferContext {
    const uint8_t* data = nullptr;   // 指向内存数据的起始地址（只读，因为我们是解码输入）
    size_t size = 0;                 // 数据总长度（字节）
    size_t pos = 0;                  // 当前读取位置（字节偏移），由 read 和 seek 回调维护
};

// ============================================================================
// 2. 自定义读取回调（read_packet）
//    FFmpeg 在需要从输入源读取数据时会调用此函数。
//    它的行为类似于 fread()，但数据源是内存。
// ============================================================================
static int read_packet_callback(void* opaque, uint8_t* buf, int buf_size) {
    // opaque 是在 avio_alloc_context 时传入的 userdata，我们传入 MemoryBufferContext 指针
    auto* mem_ctx = static_cast<MemoryBufferContext*>(opaque);
    if (!mem_ctx || mem_ctx->pos >= mem_ctx->size) {
        // 数据已经读完或上下文无效，返回 AVERROR_EOF（-541478725，即 "End of file"）
        return AVERROR_EOF;
    }

    // 计算剩余可读字节数
    size_t remaining = mem_ctx->size - mem_ctx->pos;
    // 确定本次实际要读取的字节数：不能超过请求的 buf_size，也不能超过剩余数据量
    size_t to_read = (remaining < static_cast<size_t>(buf_size)) ? remaining : static_cast<size_t>(buf_size);

    // 将数据从内存拷贝到 FFmpeg 提供的缓冲区中
    std::memcpy(buf, mem_ctx->data + mem_ctx->pos, to_read);
    // 更新读取位置
    mem_ctx->pos += to_read;

    // 返回实际读取的字节数（FFmpeg 期望返回正数表示成功，0 表示 EOF，负值表示错误）
    return static_cast<int>(to_read);
}

// ============================================================================
// 3. 自定义 Seek 回调（seek）
//    FFmpeg 可能需要随机访问（例如 seek 到某个时间点），或者查询流的总大小。
//    我们支持标准 SEEK_SET/CUR/END 以及 FFmpeg 特有的 AVSEEK_SIZE。
// ============================================================================
static int64_t seek_callback(void* opaque, int64_t offset, int whence) {
    auto* mem_ctx = static_cast<MemoryBufferContext*>(opaque);
    if (!mem_ctx) return -1;

    // ---- 特殊处理：AVSEEK_SIZE ----
    // FFmpeg 在初始化或某些操作时会调用 whence == AVSEEK_SIZE 来获取输入源的总大小，
    // 这有助于估算时长或分配缓冲区。我们必须返回整个数据块的大小。
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(mem_ctx->size);
    }

    // ---- 标准定位 ----
    int64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET: // 绝对位置
        new_pos = offset;
        break;
    case SEEK_CUR: // 相对当前位置
        new_pos = static_cast<int64_t>(mem_ctx->pos) + offset;
        break;
    case SEEK_END: // 相对文件末尾
        new_pos = static_cast<int64_t>(mem_ctx->size) + offset;
        break;
    default:
        // 其他值（如 AVSEEK_FORCE 等）我们不支持，返回 -1 表示错误
        return -1;
    }

    // 检查新位置是否在 [0, size] 范围内（位置可以等于 size，表示 EOF）
    if (new_pos < 0 || static_cast<size_t>(new_pos) > mem_ctx->size) {
        return -1;
    }

    mem_ctx->pos = static_cast<size_t>(new_pos);
    // 返回新的绝对位置（FFmpeg 期望成功返回新位置）
    return new_pos;
}

// ============================================================================
// 4. RAII 资源管理类（智能指针 Deleter）
//    使用 std::unique_ptr 管理 FFmpeg 对象，避免手动调用 free/close 带来的遗漏或顺序错误。
//    注意：这些 Deleter 只负责释放对象，不处理 AVIOContext（因为它不是标准分配的对象）。
// ============================================================================

/** 释放 AVFormatContext：avformat_close_input 会关闭所有流并释放上下文 */
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx); // 传入指针的地址，以便内部置空
    }
};

/** 释放 AVCodecContext：avcodec_free_context 会释放内部所有资源 */
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};

/** 释放 AVPacket：av_packet_free 会释放 packet 本身及其内部缓冲区（如果有） */
struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

/** 释放 AVFrame：av_frame_free 会释放 frame 及其内部数据引用（但不会释放外部 data） */
struct AVFrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};

// ============================================================================
// 5. 写入 PCM 帧到文件
//    音频解码后得到 AVFrame，其中包含一个或多个采样点。
//    写入时需要考虑：
//      - 采样格式（S16, S32, FLT 等）决定每个样点字节数。
//      - 声道数（channels）决定每个采样点包含几个声道值。
//      - 平面（planar）还是交错（packed）布局决定数据在内存中的组织方式。
//    我们将所有数据统一转为交错格式输出，因为大多数 PCM 播放器期望交错格式。
// ============================================================================
void write_pcm_frame(const AVCodecContext* dec_ctx, const AVFrame* frame, std::ofstream& out_file) {
    // 获取每个采样点（单个声道的一个值）的字节数，例如 AV_SAMPLE_FMT_S16 返回 2
    int data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
    // 获取声道数（FFmpeg 新版用 ch_layout.nb_channels，旧版用 channels，这里使用新式）
    int channels = dec_ctx->ch_layout.nb_channels;

    // 判断是否为平面格式（Planar）：如果为真，则每个声道的数据独立存放在 frame->data[ch] 中
    if (av_sample_fmt_is_planar(dec_ctx->sample_fmt)) {
        // 平面格式写入为交错输出（即 L0 R0 L1 R1 ...），
        // 因为输出 PCM 文件通常要求交错格式。
        for (int i = 0; i < frame->nb_samples; ++i) {          // 遍历每个采样点
            for (int ch = 0; ch < channels; ++ch) {            // 遍历每个声道
                // frame->data[ch] 指向第 ch 声道的起始地址，偏移 i * data_size 得到第 i 个样点
                out_file.write(reinterpret_cast<const char*>(frame->data[ch] + data_size * i), data_size);
            }
        }
    }
    else {
        // 交错格式（Packed）：所有声道的数据已经按 L0 R0 L1 R1 ... 排列在 frame->data[0] 中
        // 直接一次性写入全部字节即可
        out_file.write(reinterpret_cast<const char*>(frame->data[0]),
            frame->nb_samples * channels * data_size);
    }
}

// ============================================================================
// 6. 解码一个数据包（或冲刷解码器）
//    这是典型的 FFmpeg 解码循环封装。
//    新版本 FFmpeg（>= 3.0）采用 send_packet / receive_frame 异步模型。
//    调用者传入一个 AVPacket（压缩数据），解码器可能输出 0 个、1 个或多个 AVFrame。
//    当 pkt == nullptr 时，表示冲刷（flush）解码器，强制输出内部缓存的帧。
// ============================================================================
int decode_packet(AVCodecContext* dec_ctx, const AVPacket* pkt, AVFrame* frame, std::ofstream& out_file) {
    // --- 第一步：将数据包送入解码器 ---
    // avcodec_send_packet 将压缩数据包送入解码器的输入队列。
    // 如果 pkt 为 nullptr，则发送一个“冲刷”标记，让解码器返回所有缓存的帧。
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        // 返回负值表示错误（例如数据损坏、格式不支持等）
        return ret;
    }

    // --- 第二步：循环接收解码后的帧 ---
    // avcodec_receive_frame 会从解码器的输出队列中取出一帧。
    // 返回值：
    //   - 0 表示成功取得一帧
    //   - AVERROR(EAGAIN) 表示目前没有帧可输出，需要送入更多数据包
    //   - AVERROR_EOF 表示所有帧已输出完毕（冲刷结束）
    //   - 其他负值表示错误
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // 正常情况：没有更多帧可读，退出循环，但上层函数应返回成功（0）
            return 0;
        }
        else if (ret < 0) {
            // 遇到其他错误，直接返回错误码
            return ret;
        }

        // 成功解码出一帧，写入 PCM 数据
        write_pcm_frame(dec_ctx, frame, out_file);
        // 重要：调用 av_frame_unref 释放 frame 对内部缓冲区的引用，
        // 以便下一轮 receive_frame 可以覆盖 frame 的内容。
        // 如果没有调用，avcodec_receive_frame 会报错（因为 frame 仍被引用）。
        av_frame_unref(frame);
    }
    return 0;
}

// ============================================================================
// 7. 主函数
//    演示完整流程：从磁盘读文件到内存 → 自定义 AVIO → 解封装 → 解码 → 输出 PCM。
//    这种模式适用于加密媒体、网络流、内存缓冲等无法直接提供文件路径的场景。
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input_audio_file> <output_pcm_file>" << std::endl;
        return -1;
    }

    // ==================== (1) 将输入文件全部读入内存 ====================
    // 我们模拟“内存数据源”，例如从网络接收或解密后的数据。
    std::ifstream in_file(argv[1], std::ios::binary | std::ios::ate); // ate 使文件指针在末尾
    if (!in_file.is_open()) {
        std::cerr << "Failed to open input file: " << argv[1] << std::endl;
        return -1;
    }
    std::streamsize file_size = in_file.tellg(); // 通过当前位置获取文件大小
    in_file.seekg(0, std::ios::beg);             // 回到文件开头准备读取

    std::vector<uint8_t> memory_stream(file_size); // 分配连续内存
    if (!in_file.read(reinterpret_cast<char*>(memory_stream.data()), file_size)) {
        std::cerr << "Failed to read input file into memory." << std::endl;
        return -1;
    }
    in_file.close();

    // 初始化我们的内存上下文，data 指向 vector 的底层数组，size 为总长度
    MemoryBufferContext mem_ctx{ memory_stream.data(), memory_stream.size(), 0 };

    // ==================== (2) 创建自定义 AVIO 上下文 ====================
    // AVIOContext 是 FFmpeg 的输入/输出抽象层，它内部有一个缓冲区，并调用用户提供的回调。
    // 我们要让 FFmpeg 从内存读取，因此使用 avio_alloc_context 构造一个只读的 AVIO 对象。
    constexpr int io_buf_size = 4096; // 内部缓冲区大小，通常 4KB 即可，也可以更大以提高性能
    uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(io_buf_size));
    if (!io_buffer) {
        std::cerr << "av_malloc failed for io_buffer." << std::endl;
        return -1;
    }

    // avio_alloc_context 参数：
    //   1. 内部缓冲区指针（由 av_malloc 分配，会被 AVIOContext 管理）
    //   2. 缓冲区大小
    //   3. write_flag：0 表示只读，非 0 表示可写（但我们不需要写）
    //   4. opaque：用户数据，会作为第一个参数传递给回调函数
    //   5. read_packet：读回调（必须）
    //   6. write_packet：写回调（我们不需要，传 nullptr）
    //   7. seek：定位回调（可选，但为了支持 AVSEEK_SIZE 和随机访问，必须提供）
    AVIOContext* avio_ctx = avio_alloc_context(
        io_buffer, io_buf_size,
        0,                  // 只读
        &mem_ctx,           // 用户数据
        read_packet_callback,
        nullptr,            // 没有写回调
        seek_callback       // 提供 seek 回调，让 FFmpeg 可以 seek 和查询大小
    );
    if (!avio_ctx) {
        av_free(io_buffer);   // 如果分配失败，需手动释放刚分配的缓冲区
        std::cerr << "avio_alloc_context failed." << std::endl;
        return -1;
    }

    // ==================== (3) 创建格式上下文并绑定 AVIO ====================
    // AVFormatContext 负责解封装（demuxing），它需要一个输入源（通常通过 URL 或文件描述符）。
    // 我们通过将它的 pb 字段设置为自定义 AVIOContext，让它从内存读取。
    AVFormatContext* raw_fmt_ctx = avformat_alloc_context();
    if (!raw_fmt_ctx) {
        // 如果分配失败，需要释放之前创建的 AVIOContext（avio_context_free 会自动释放内部 io_buffer）
        avio_context_free(&avio_ctx);
        std::cerr << "avformat_alloc_context failed." << std::endl;
        return -1;
    }
    raw_fmt_ctx->pb = avio_ctx;            // 绑定自定义 IO
    raw_fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO; // 重要标志：告知 FFmpeg 我们使用了自定义 IO，
    // 这样 avformat_close_input 在释放时不会尝试关闭 pb（因为 pb 不是它分配的文件描述符）。
    // 如果没有这个标志，可能导致双重释放或访问无效内存。

// 打开输入：avformat_open_input 会读取文件头，识别容器格式。
// 第二个参数是 URL，因为我们使用了自定义 IO，可以传入 nullptr（但某些格式可能需要扩展名，这里无所谓）。
// 第三个参数为 fmt（指定格式），传入 nullptr 让 FFmpeg 自动探测。
// 第四个参数为 options，通常为 nullptr。
    if (avformat_open_input(&raw_fmt_ctx, nullptr, nullptr, nullptr) < 0) {
        std::cerr << "avformat_open_input failed." << std::endl;
        // 注意：avformat_open_input 失败时，如果 raw_fmt_ctx 被内部修改过，需要释放，
        // 但此处 raw_fmt_ctx 尚未被 unique_ptr 接管，我们手动释放并释放 avio_ctx。
        avformat_free_context(raw_fmt_ctx);   // 如果 open 失败，raw_fmt_ctx 可能部分初始化，用 avformat_free_context 释放
        avio_context_free(&avio_ctx);
        return -1;
    }

    // 使用 unique_ptr 管理格式上下文，确保 main 结束时自动调用 avformat_close_input
    // 注意：avformat_close_input 会释放内部所有流和 avio_ctx（如果是由它打开的），但由于我们绑定了自定义 pb，
    // 且设置了 AVFMT_FLAG_CUSTOM_IO，它不会释放 pb，需要我们自己释放。
    std::unique_ptr<AVFormatContext, AVFormatContextDeleter> fmt_ctx(raw_fmt_ctx);

    // 读取部分数据以获取流信息（如编码器参数、时长等）。
    // avformat_find_stream_info 会读取一些数据包来分析流，对于某些格式（如 MP3）是必需的。
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        std::cerr << "avformat_find_stream_info failed." << std::endl;
        return -1;
    }

    // ==================== (4) 查找音频流并初始化解码器 ====================
    // av_find_best_stream 遍历所有流，选择最佳音频流（根据默认规则，或通过指定参数）
    // 它会返回流索引，并可选地返回对应的解码器（AVCodec *）。
    const AVCodec* decoder = nullptr;
    int audio_stream_idx = av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_AUDIO,
        -1, -1, &decoder, 0);
    if (audio_stream_idx < 0 || !decoder) {
        std::cerr << "Could not find audio stream or decoder." << std::endl;
        return -1;
    }

    // 分配解码器上下文（AVCodecContext）
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> dec_ctx(
        avcodec_alloc_context3(decoder)
    );
    if (!dec_ctx) {
        std::cerr << "avcodec_alloc_context3 failed." << std::endl;
        return -1;
    }

    // 将流中的编码参数（codecpar）复制到解码器上下文。
    // codecpar 是在 avformat_find_stream_info 时从容器中解析出的编码信息（如采样率、声道数、比特率等）。
    // 解码器需要这些参数才能正确初始化。
    avcodec_parameters_to_context(dec_ctx.get(), fmt_ctx->streams[audio_stream_idx]->codecpar);

    // 打开解码器：avcodec_open2 会初始化编码器实例，分配内部资源。
    if (avcodec_open2(dec_ctx.get(), decoder, nullptr) < 0) {
        std::cerr << "avcodec_open2 failed." << std::endl;
        return -1;
    }

    // ==================== (5) 打开输出 PCM 文件 ====================
    std::ofstream out_file(argv[2], std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "Failed to open output file: " << argv[2] << std::endl;
        return -1;
    }

    // ==================== (6) 分配数据包和帧对象 ====================
    // AVPacket 存放压缩数据（编码后的音频帧），AVFrame 存放解码后的原始数据（PCM）。
    // 为了复用内存，它们在循环外分配。
    std::unique_ptr<AVPacket, AVPacketDeleter> pkt(av_packet_alloc());
    std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
    if (!pkt || !frame) {
        std::cerr << "Failed to allocate packet or frame." << std::endl;
        return -1;
    }

    // ==================== (7) 主解码循环 ====================
    // av_read_frame 从容器中读取一个数据包（解复用），返回 0 表示成功，负数表示 EOF 或错误。
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        // 只处理音频流的数据包（可能包含视频、字幕等，我们忽略）
        if (pkt->stream_index == audio_stream_idx) {
            int ret = decode_packet(dec_ctx.get(), pkt.get(), frame.get(), out_file);
            if (ret < 0) {
                std::cerr << "Error decoding packet: " << ret << std::endl;
                break;
            }
        }
        // 释放当前 packet 内部引用，以便 av_read_frame 可以填充下一个包。
        // 注意：av_packet_unref 会减少引用计数并释放缓冲区，但不会释放 pkt 结构本身。
        av_packet_unref(pkt.get());
    }

    // ==================== (8) 冲刷解码器 ====================
    // 有些编码格式（如 AAC）在最后一帧之后可能还有缓存的帧（如延时帧），
    // 发送 nullptr 表示不再有数据包，解码器会输出所有剩余的帧。
    int ret = decode_packet(dec_ctx.get(), nullptr, frame.get(), out_file);
    if (ret < 0) {
        std::cerr << "Error flushing decoder: " << ret << std::endl;
    }

    // ==================== (9) 清理资源 ====================
    // 由于使用了 unique_ptr，fmt_ctx 和 dec_ctx 会在离开作用域时自动释放。
    // 但 AVIOContext (avio_ctx) 是我们手动分配的，并且 fmt_ctx 的析构不会释放它，
    // 因为设置了 AVFMT_FLAG_CUSTOM_IO。所以我们必须显式释放 avio_ctx。
    // 注意释放顺序：先关闭格式上下文（确保不再使用 pb），再释放 AVIOContext。
    // 我们手动 reset fmt_ctx 提前触发释放，然后再释放 avio_ctx。
    fmt_ctx.reset();   // 调用 avformat_close_input，释放所有内部资源，但保留 pb（因为自定义标志）
    // 释放 AVIOContext：它会自动调用 av_free 释放内部 io_buffer。
    avio_context_free(&avio_ctx);

    std::cout << "AVIO memory decoding finished successfully." << std::endl;
    return 0;
}