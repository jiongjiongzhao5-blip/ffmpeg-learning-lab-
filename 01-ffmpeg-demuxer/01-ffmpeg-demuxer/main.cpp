#include <iostream>      // 标准输入输出流，用于控制台打印
#include <iomanip>       // 格式化输出，如 setw、setprecision
#include <string>        // 字符串操作
#include <memory>        // 智能指针 unique_ptr，用于 RAII 资源管理

// FFmpeg 是 C 语言库，必须用 extern "C" 包裹，防止 C++ 名称修饰
extern "C" {
#include <libavformat/avformat.h>   // 解复用（封装格式）相关 API
#include <libavcodec/avcodec.h>     // 编解码相关（此处仅用到部分定义）
#include <libavutil/avutil.h>       // 通用工具，如错误码、时间基
#include <libavutil/timestamp.h>    // 时间戳处理（本代码未直接使用，但保留）
}


void print_ffmpeg_error(int errnum, const std::string& prefix) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};  // 错误信息缓冲区
    av_strerror(errnum, errbuf, sizeof(errbuf));  // 将错误码转为可读字符串
    std::cerr << prefix << ": " << errbuf << " (code: " << errnum << ")\n";
}

int main(int argc, char* argv[]) {
    // 检查命令行参数：至少需要传入一个媒体文件路径
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input_media_file>\n";
        return -1;
    }
    const char* input_url = argv[1];   // 输入文件路径

    // 1. 打开输入流并分配解封装上下文
    // AVFormatContext 是解封装的核心结构，包含媒体文件的封装信息、流列表等
    AVFormatContext* fmt_ctx = nullptr;
    // avformat_open_input 会内部自动分配 AVFormatContext，并打开文件/网络流
    int ret = avformat_open_input(&fmt_ctx, input_url, nullptr, nullptr);
    if (ret < 0) {
        print_ffmpeg_error(ret, "avformat_open_input failed");
        return -1;
    }

    // 使用 RAII 确保资源自动释放
    // unique_ptr 自定义删除器，在 fmt_guard 析构时调用 avformat_close_input 释放上下文
    // avformat_close_input 会将指针置为 NULL，我们传入的删除器会检查并调用
    auto fmt_guard = std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>(
        fmt_ctx, [](AVFormatContext* ctx) {
            if (ctx) {
                avformat_close_input(&ctx);  // 释放上下文并置空指针
            }
        }
    );
    // 此时 fmt_ctx 已经托管给 unique_ptr，但原始指针仍可用（不要手动 delete）

    // 2. 探测流信息
    // avformat_find_stream_info 读取一部分数据来获取流编码参数、时长等信息
    // 第二个参数为选项字典，传 nullptr 表示默认
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        print_ffmpeg_error(ret, "avformat_find_stream_info failed");
        return -1;
    }

    // 打印媒体整体元数据信息，包括容器格式、时长、流数量、编码类型等（输出到 stderr）
    // 参数：上下文，输出索引（0），输入URL，是否输出到 stdout（0表示stderr）
    av_dump_format(fmt_ctx, 0, input_url, 0);

    // 3. 寻找最佳视音频流
    // av_find_best_stream 是推荐的方法，可自动选择最合适的流，避免硬编码流索引
    // 参数：上下文，媒体类型，要排除的流索引（-1表示无），相关流（-1），输出解码器（nullptr不关心），flag（0默认）
    // 返回值：流索引，若找不到则返回负错误码（如 AVERROR_STREAM_NOT_FOUND）
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    std::cout << "\n===========================================\n";
    std::cout << "Video Stream Index: " << video_stream_idx << "\n";
    std::cout << "Audio Stream Index: " << audio_stream_idx << "\n";
    std::cout << "=============================================\n\n";

    // 4. 分配 AVPacket
    // AVPacket 用于存储解复用后的压缩数据包（编码数据），包含数据缓冲、时间戳、标志等
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "Failed to allocate AVPacket\n";
        return -1;
    }
    // 使用 unique_ptr 自动管理 AVPacket 生命周期，删除器调用 av_packet_free
    auto pkt_guard = std::unique_ptr<AVPacket, void(*)(AVPacket*)>(
        pkt, [](AVPacket* p) { av_packet_free(&p); }
    );

    // 统计变量
    int64_t video_pkt_count = 0, audio_pkt_count = 0;   // 视频/音频包个数
    int64_t video_bytes = 0, audio_bytes = 0;           // 视频/音频总字节数

    // 5. 循环读取所有 Packet（解复用）
    // av_read_frame 每次读取一个 packet，返回 0 表示成功，负错误码表示出错或文件结束（AVERROR_EOF）
    while ((ret = av_read_frame(fmt_ctx, pkt)) >= 0) {
        // 获取当前 packet 所属的流（AVStream）
        AVStream* stream = fmt_ctx->streams[pkt->stream_index];

        // 根据流索引判断是视频还是音频
        if (pkt->stream_index == video_stream_idx) {
            video_pkt_count++;
            video_bytes += pkt->size;   // pkt->size 是数据缓冲区大小

            // 仅打印前 5 个视频帧的关键信息，用于调试或观察
            if (video_pkt_count <= 5) {
                // 计算 PTS（显示时间戳）的秒数：pts * time_base
                // AV_NOPTS_VALUE 表示无效时间戳，需判断
                double pts_sec = (pkt->pts != AV_NOPTS_VALUE) ? (pkt->pts * av_q2d(stream->time_base)) : -1.0;
                double dts_sec = (pkt->dts != AV_NOPTS_VALUE) ? (pkt->dts * av_q2d(stream->time_base)) : -1.0;
                bool is_key = (pkt->flags & AV_PKT_FLAG_KEY);  // 是否为关键帧

                std::cout << "[Video PKT] #" << std::setw(3) << video_pkt_count
                          << " | KeyFrame: " << (is_key ? "YES" : " NO")
                          << " | Size: " << std::setw(6) << pkt->size << "B"
                          << " | PTS: " << std::fixed << std::setprecision(3) << pts_sec << "s"
                          << " | DTS: " << std::fixed << std::setprecision(3) << dts_sec << "s\n";
            }
        } else if (pkt->stream_index == audio_stream_idx) {
            audio_pkt_count++;
            audio_bytes += pkt->size;
        }
        // 其他流（如字幕、数据等）我们忽略，不统计

        // 重要：av_read_frame 内部为 pkt 分配了引用计数缓冲，必须调用 av_packet_unref 释放当前包
        // 否则内存泄漏，且后续 av_read_frame 会覆盖旧数据导致未定义行为
        av_packet_unref(pkt);
    }

    // 循环退出后检查：如果是文件结束（EOF）则正常，否则打印错误
    if (ret != AVERROR_EOF && ret < 0) {
        print_ffmpeg_error(ret, "av_read_frame read error");
    }

    // 6. 输出统计报告
    std::cout << "\n============================================\n";
    std::cout << "Video Packets : " << video_pkt_count << " | Total Size: " << video_bytes / 1024.0 << " KB\n";
    std::cout << "Audio Packets : " << audio_pkt_count << " | Total Size: " << audio_bytes / 1024.0 << " KB\n";
    std::cout << "=============================================\n";

    // 返回 0 表示成功
    return 0;
}
