#include <iostream>      // 标准输入输出流，用于错误和信息输出
#include <fstream>       // 文件流，用于输出 YUV 原始数据
#include <memory>        // 智能指针，用于 RAII 资源管理
#include <string>        // 字符串操作

// 使用 extern "C" 包含 FFmpeg 的 C 语言头文件，防止 C++ 名称修饰导致链接失败
extern "C" {
#include <libavformat/avformat.h>   // 解封装（容器）相关 API
#include <libavcodec/avcodec.h>     // 编解码相关 API
#include <libavutil/imgutils.h>     // 图像工具，如像素格式信息
}

// ============================================================================
// 1. RAII 资源管理辅助结构
//    使用 std::unique_ptr + 自定义 Deleter，确保 FFmpeg 资源自动释放，避免内存泄漏
// ============================================================================

// 释放 AVFormatContext（解封装上下文）
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);  // 关闭输入并释放上下文
    }
};

// 释放 AVCodecContext（解码器上下文）
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};

// 释放 AVPacket（数据包）
struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

// 释放 AVFrame（解码后的帧）
struct AVFrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};

// 定义独特的智能指针类型，方便使用
using UniqueAVFormatContext = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;

// ============================================================================
// 2. 错误日志辅助函数
//    将 FFmpeg 错误码转换为可读字符串并输出到标准错误
// ============================================================================
void log_av_error(const std::string& prefix, int err_code) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };   // FFmpeg 提供的错误缓冲区大小
    av_strerror(err_code, err_buf, sizeof(err_buf)); // 将错误码转为描述文字
    std::cerr << prefix << ": " << err_buf << " (" << err_code << ")" << std::endl;
}

// ============================================================================
// 3. 写入 YUV420P 帧的核心函数
//    注意：严格按照每行的 linesize 逐行写入，而非连续写入 data 指针，
//    因为 linesize 可能大于实际宽度（内存对齐填充），直接写整个 data 会产生绿边/花屏。
//    参数：frame - 解码后的 AVFrame，必须为 YUV420P 格式；
//          out_file - 输出文件流（二进制模式）
// ============================================================================
void write_yuv420p_frame(const AVFrame* frame, std::ofstream& out_file) {
    // Y 分量（亮度）：每行 width 个字节，共 height 行
    for (int i = 0; i < frame->height; ++i) {
        out_file.write(reinterpret_cast<const char*>(frame->data[0] + i * frame->linesize[0]),
            frame->width);
    }
    // U 分量（色度蓝色差）：宽高均为 Y 的一半，行数为 height/2
    for (int i = 0; i < frame->height / 2; ++i) {
        out_file.write(reinterpret_cast<const char*>(frame->data[1] + i * frame->linesize[1]),
            frame->width / 2);
    }
    // V 分量（色度红色差）：同样宽高减半
    for (int i = 0; i < frame->height / 2; ++i) {
        out_file.write(reinterpret_cast<const char*>(frame->data[2] + i * frame->linesize[2]),
            frame->width / 2);
    }
}

// ============================================================================
// 4. 解码单个数据包（或冲刷解码器）的函数
//    负责向解码器发送 packet，并循环接收所有输出的帧，写入文件。
//    参数：
//       dec_ctx - 解码器上下文
//       pkt     - 输入的 AVPacket（若为 nullptr 则表示冲刷解码器）
//       frame   - 用于接收解码帧的 AVFrame（复用）
//       out_file- 输出文件流
//    返回值：0 表示成功，负数表示错误。
// ============================================================================
int decode_packet(AVCodecContext* dec_ctx, const AVPacket* pkt,
    AVFrame* frame, std::ofstream& out_file) {
    // 将 packet 送入解码器（若 pkt 为 nullptr，则发送空包以冲刷内部缓冲）
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        log_av_error("Error sending packet to decoder", ret);
        return ret;
    }

    // 循环接收解码出的帧，直到解码器返回 EAGAIN（需要更多数据）或 EOF（已排空）
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // 正常情况：需要更多输入或已无更多输出
            return 0;
        }
        else if (ret < 0) {
            // 其他错误
            log_av_error("Error receiving frame from decoder", ret);
            return ret;
        }

        // 成功解码出一帧，检查像素格式（只处理 YUV420P）
        if (frame->format == AV_PIX_FMT_YUV420P) {
            write_yuv420p_frame(frame, out_file);
        }
        else {
            // 实际工程中可添加格式转换，这里忽略非 YUV420P 的帧
            std::cerr << "Warning: frame format is not YUV420P, skipped." << std::endl;
        }
        // 释放当前帧的引用计数，以便 av_frame_alloc 分配的 frame 下次复用
        av_frame_unref(frame);
    }
    return 0;
}

// ============================================================================
// 5. 主函数
//    流程：打开输入文件 -> 查找视频流 -> 初始化解码器 -> 循环读取包并解码 ->
//    冲刷解码器 -> 完成。
//    命令行参数：<输入媒体文件> <输出 YUV 文件>
// ============================================================================
int main(int argc, char* argv[]) {
    // 检查命令行参数
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input_media_file> <output_yuv_file>" << std::endl;
        return -1;
    }

    const char* input_path = argv[1];
    const char* output_path = argv[2];

    // ---------- 步骤1：打开输入文件并获取格式上下文 ----------
    AVFormatContext* raw_fmt_ctx = nullptr;
    // avformat_open_input 会探测文件格式并分配 AVFormatContext
    if (int ret = avformat_open_input(&raw_fmt_ctx, input_path, nullptr, nullptr); ret < 0) {
        log_av_error("Cannot open input file", ret);
        return -1;
    }
    // 使用智能指针自动管理生命周期
    UniqueAVFormatContext fmt_ctx(raw_fmt_ctx);

    // 读取媒体文件的部分数据，获取流信息（如编码参数、时长等）
    if (int ret = avformat_find_stream_info(fmt_ctx.get(), nullptr); ret < 0) {
        log_av_error("Cannot find stream information", ret);
        return -1;
    }

    // ---------- 步骤2：查找最佳视频流并初始化解码器 ----------
    const AVCodec* decoder = nullptr;
    // av_find_best_stream 自动选择最合适的视频流，并返回对应的解码器
    int video_stream_idx = av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO,
        -1, -1, &decoder, 0);
    if (video_stream_idx < 0 || !decoder) {
        std::cerr << "Cannot find valid video stream/decoder" << std::endl;
        return -1;
    }

    // 分配解码器上下文
    UniqueAVCodecContext dec_ctx(avcodec_alloc_context3(decoder));
    if (!dec_ctx) {
        std::cerr << "Failed to allocate codec context" << std::endl;
        return -1;
    }

    // 将流的编码参数（如宽高、像素格式、比特率等）拷贝到解码器上下文中
    if (int ret = avcodec_parameters_to_context(dec_ctx.get(),
        fmt_ctx->streams[video_stream_idx]->codecpar); ret < 0) {
        log_av_error("Failed to copy codec parameters to context", ret);
        return -1;
    }

    // 打开解码器（真正初始化内部状态）
    if (int ret = avcodec_open2(dec_ctx.get(), decoder, nullptr); ret < 0) {
        log_av_error("Failed to open decoder", ret);
        return -1;
    }

    // ---------- 步骤3：打开输出文件，分配 Packet 和 Frame ----------
    // 以二进制模式打开输出文件，用于写入原始 YUV 数据
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "Failed to open output file: " << output_path << std::endl;
        return -1;
    }

    // 分配 AVPacket 和 AVFrame（使用 unique_ptr 自动释放）
    UniqueAVPacket pkt(av_packet_alloc());
    UniqueAVFrame  frame(av_frame_alloc());
    if (!pkt || !frame) {
        std::cerr << "Failed to allocate packet or frame" << std::endl;
        return -1;
    }

    // ---------- 步骤4：主循环——读取并解码所有 packet ----------
    // av_read_frame 从文件中读取一个 packet（可能包含音频或视频数据）
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        // 只处理视频流的数据包
        if (pkt->stream_index == video_stream_idx) {
            // 调用解码函数处理当前 packet
            decode_packet(dec_ctx.get(), pkt.get(), frame.get(), out_file);
        }
        // 释放 packet 内部引用，以便复用 pkt 对象（av_packet_alloc 分配的对象可以重复使用）
        av_packet_unref(pkt.get());
    }

    // ---------- 步骤5：冲刷解码器（Flush） ----------
    // 当所有 packet 都送入解码器后，可能还有缓存的帧未输出，需要发送 nullptr 来冲刷。
    // 此时 decode_packet 内部会不断调用 avcodec_receive_frame 直到 EAGAIN/EOF。
    decode_packet(dec_ctx.get(), nullptr, frame.get(), out_file);

    std::cout << "Decoding finished successfully." << std::endl;
    return 0;
}