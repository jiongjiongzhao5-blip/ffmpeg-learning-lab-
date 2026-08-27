/**
 * @file mp4_faststart.cpp
 * @brief 使用 FFmpeg 库将 MP4 文件重新封装为 "faststart" 格式（moov 原子位于文件开头）
 *
 * 功能说明：
 * - 读取输入 MP4 文件，分析其流信息（视频/音频轨道参数）
 * - 创建新的 MP4 输出文件，并设置 `movflags=faststart` 选项
 * - 逐包复制原始帧数据（remux），不进行编码/解码，保持原始质量
 * - 最终生成的文件支持流式播放（moov 在 mdat 之前，适用于 HTTP 渐进式下载）
 *
 * 用法：./mp4_faststart <input.mp4> <output_faststart.mp4>
 *
 * 依赖：FFmpeg 库（libavformat, libavcodec, libavutil）
 */

#include <iostream>
#include <string>
#include <memory>       // std::unique_ptr, 自定义删除器

 // 使用 extern "C" 包含 FFmpeg 的 C 头文件，避免 C++ 名称修饰问题
extern "C" {
#include <libavformat/avformat.h>   // 封装格式处理（解复用、复用）
#include <libavcodec/avcodec.h>     // 编解码器相关（获取编解码器名称）
#include <libavutil/dict.h>         // 字典操作（用于设置选项）
}

/**
 * @brief 自定义删除器，用于 std::unique_ptr 管理 AVFormatContext*
 *
 * 根据上下文是输入还是输出，采取不同的释放方式：
 * - 如果是输入（ctx->iformat 非空），调用 avformat_close_input()
 * - 如果是输出（ctx->oformat 非空），则先关闭 IO 上下文（若需要），再调用 avformat_free_context()
 * 此设计实现 RAII，确保资源自动释放，避免内存泄漏。
 */
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (!ctx) return;                         // 空指针直接返回

        if (ctx->iformat) {
            // 输入上下文：关闭并释放所有内部资源
            avformat_close_input(&ctx);
        }
        else {
            // 输出上下文：需要先关闭 IO 上下文（如果格式不是 NOFILE 且 pb 存在）
            if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) {
                avio_closep(&ctx->pb);           // 关闭并释放 AVIOContext
            }
            avformat_free_context(ctx);           // 释放 AVFormatContext 本身
        }
    }
};

// 定义 RAII 类型别名，方便使用
using UniqueAVFormatContext = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

/**
 * @brief 主函数
 * @param argc 命令行参数个数
 * @param argv 参数数组
 * @return 0 成功，非0失败
 */
int main(int argc, char* argv[]) {
    // 检查命令行参数，至少需要输入文件和输出文件
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input_mp4> <output_faststart_mp4>\n";
        return -1;
    }

    const char* in_filename = argv[1];
    const char* out_filename = argv[2];

    // ========== 步骤1：打开输入文件并探测流信息 ==========
    AVFormatContext* raw_in_ctx = nullptr;
    // avformat_open_input: 打开媒体文件并读取头部，探测封装格式
    if (avformat_open_input(&raw_in_ctx, in_filename, nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input file.\n";
        return -1;
    }
    // 使用 RAII 包装，确保后续异常或提前返回时自动释放
    UniqueAVFormatContext in_ctx(raw_in_ctx);

    // avformat_find_stream_info: 读取一部分数据，获取流参数信息（如编码器、分辨率等）
    if (avformat_find_stream_info(in_ctx.get(), nullptr) < 0) {
        std::cerr << "Failed to retrieve input stream information.\n";
        return -1;
    }

    // 打印各轨道信息（仅用于调试和展示）
    std::cout << "=== MP4 Track Analysis (libavformat) ===" << std::endl;
    for (unsigned int i = 0; i < in_ctx->nb_streams; i++) {
        AVStream* stream = in_ctx->streams[i];
        AVCodecParameters* codec_par = stream->codecpar;   // 编解码参数

        // 判断媒体类型
        const char* type_str = (codec_par->codec_type == AVMEDIA_TYPE_VIDEO) ? "Video" :
            (codec_par->codec_type == AVMEDIA_TYPE_AUDIO) ? "Audio" : "Other";
        std::cout << "Track #" << i << " (" << type_str << "):" << std::endl;
        // avcodec_get_name: 根据 codec_id 获取可读的编解码器名称
        std::cout << "  Codec: " << avcodec_get_name(codec_par->codec_id) << std::endl;
        // 时间基（用于时间戳转换）
        std::cout << "  Timebase: " << stream->time_base.num << "/" << stream->time_base.den << std::endl;

        if (codec_par->codec_type == AVMEDIA_TYPE_VIDEO) {
            std::cout << "  Resolution: " << codec_par->width << "x" << codec_par->height << std::endl;
        }
        else if (codec_par->codec_type == AVMEDIA_TYPE_AUDIO) {
            std::cout << "  Sample Rate: " << codec_par->sample_rate << " Hz" << std::endl;
            // ch_layout 存储声道布局，nb_channels 为声道数
            std::cout << "  Channels: " << codec_par->ch_layout.nb_channels << std::endl;
        }
    }

    // ========== 步骤2：初始化输出上下文 ==========
    AVFormatContext* raw_out_ctx = nullptr;
    // avformat_alloc_output_context2: 根据输出格式（"mp4"）分配输出上下文
    if (avformat_alloc_output_context2(&raw_out_ctx, nullptr, "mp4", out_filename) < 0) {
        std::cerr << "Failed to create output context.\n";
        return -1;
    }
    UniqueAVFormatContext out_ctx(raw_out_ctx);

    // ========== 步骤3：复制输入流参数到输出流 ==========
    // 遍历所有输入流，为每个流在输出上下文中创建对应的流，并复制编解码参数
    for (unsigned int i = 0; i < in_ctx->nb_streams; i++) {
        AVStream* in_stream = in_ctx->streams[i];
        // avformat_new_stream: 在输出上下文中创建新流
        AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
        if (!out_stream) {
            std::cerr << "Failed to allocate output stream.\n";
            return -1;
        }
        // avcodec_parameters_copy: 复制编解码参数（如编码类型、分辨率、采样率等）
        if (avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
            std::cerr << "Failed to copy codec parameters.\n";
            return -1;
        }
        // 清空 codec_tag，避免某些封装对标签的不兼容问题（让 FFmpeg 自动选择）
        out_stream->codecpar->codec_tag = 0;
    }

    // ========== 步骤4：打开输出文件（IO 层） ==========
    // 如果输出格式需要文件 IO（非 AVFMT_NOFILE 标志），则打开文件
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        // avio_open: 打开输出文件（写模式）
        if (avio_open(&out_ctx->pb, out_filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Failed to open output file IO.\n";
            return -1;
        }
    }

    // ========== 步骤5：设置 Faststart 选项并写入头部 ==========
    // 关键：设置 movflags=faststart，使得在写入 trailer 时重新将 moov 原子移动到文件开头
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0);   // 0 表示不覆盖已有值

    // avformat_write_header: 写入文件头（对于 MP4，此时写入 ftyp 和预留 moov 空间，实际上 moov 在 trailer 时才会生成）
    // 传入 opts，将 faststart 选项传递给复用器
    if (avformat_write_header(out_ctx.get(), &opts) < 0) {
        std::cerr << "Error occurred when opening output file.\n";
        av_dict_free(&opts);   // 释放选项字典
        return -1;
    }
    av_dict_free(&opts);       // 释放字典（已使用完毕）

    // ========== 步骤6：读取、重写时间戳、写入帧数据（Remux 循环） ==========
    AVPacket* pkt = av_packet_alloc();   // 分配一个数据包对象
    if (!pkt) {
        std::cerr << "Failed to allocate packet.\n";
        return -1;
    }

    // 循环读取输入帧，直到读取失败（文件结束或出错）
    while (av_read_frame(in_ctx.get(), pkt) >= 0) {
        // 根据包中的流索引获取输入流和对应的输出流
        AVStream* in_stream = in_ctx->streams[pkt->stream_index];
        AVStream* out_stream = out_ctx->streams[pkt->stream_index];

        // 将包的时间戳从输入流的时间基转换为输出流的时间基
        // av_packet_rescale_ts 会修改 pkt 中的 pts, dts, duration 字段
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);

        // 将包的位置信息置为 -1，表示不保留原始文件位置（输出文件位置由复用器管理）
        pkt->pos = -1;

        // av_interleaved_write_frame: 交错的写入数据包，确保音视频交错良好
        if (av_interleaved_write_frame(out_ctx.get(), pkt) < 0) {
            std::cerr << "Error muxing packet.\n";
            break;
        }
        // 释放包内部数据，但保留 pkt 本身以便下一次循环重用
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);   // 释放包对象

    // ========== 步骤7：写入文件尾部（trailer） ==========
    // av_write_trailer: 写入文件结束标记，同时，由于设置了 faststart，
    // 此函数内部会将 moov 原子从文件末尾移动到开头，并更新 mdat 的偏移量。
    // 这正是实现 faststart 的关键所在。
    av_write_trailer(out_ctx.get());

    std::cout << "\nFast-start remux completed successfully.\n";

    // 程序正常结束，RAII 会自动释放输入和输出上下文（通过 unique_ptr 的析构）
    return 0;
}