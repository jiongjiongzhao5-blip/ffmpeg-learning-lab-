// ============================================================================
// 文件名: flv_demux.cpp
// 功能: 从 FLV 容器文件中提取 H.264 视频裸流，并转换为 Annex-B 格式
//       (即每个 NAL 单元前添加起始码 0x00 0x00 0x00 0x01)
// 依赖: FFmpeg 库 (libavformat, libavcodec)
// 编译示例: g++ -o flv_demux flv_demux.cpp -lavformat -lavcodec -lstdc++ -std=c++11
// 使用: ./flv_demux <输入.flv> <输出.h264>
// ============================================================================

// 标准库头文件
#include <iostream>   // 用于标准输入输出 (std::cout, std::cerr)
#include <fstream>    // 用于文件操作 (std::ofstream)
#include <string>     // 用于 std::string 类型

// FFmpeg 是 C 语言库，使用 extern "C" 防止 C++ 名称修饰 (name mangling)
extern "C" {
#include <libavformat/avformat.h>   // 封装格式处理 (AVFormatContext, avformat_*)
#include <libavcodec/bsf.h>         // 比特流过滤器 (AVBSFContext, av_bsf_*)
}

// ----------------------------------------------------------------------------
// 函数: DemuxFlvToAnnexB
// 参数: input_path  - 输入的 FLV 文件路径
//       output_path - 输出的 H.264 裸流文件路径 (Annex-B 格式)
// 返回值: true  成功; false 失败
// 功能: 打开 FLV 文件，找到 H.264 视频流，通过比特流过滤器 (BSF) 将
//       存储格式 (AVCC，带 extradata) 转换为 Annex-B (带起始码)，
//       并将所有视频包写入输出文件。
// 说明: 本函数展示了现代 C++ 与 FFmpeg C API 的混合使用，
//       以及资源管理（RAII 与手动释放结合）。
// ----------------------------------------------------------------------------
bool DemuxFlvToAnnexB(const std::string& input_path, const std::string& output_path) {
    // ---------- 1. 核心 FFmpeg 对象声明 ----------
    AVFormatContext* ifmt_ctx = nullptr;   // 输入文件的封装格式上下文
    const AVBitStreamFilter* bsf = nullptr; // 比特流过滤器对象 (只读)
    AVBSFContext* bsf_ctx = nullptr;       // 比特流过滤器上下文 (实例)

    // ---------- 2. 打开输出文件 (二进制模式) ----------
    // 使用 RAII 风格的 ofstream，在析构时自动关闭文件，避免忘记关闭。
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "Failed to open output file: " << output_path << std::endl;
        return false;
    }

    // ---------- 3. 打开输入文件并探测封装格式 ----------
    // avformat_open_input 会读取文件头，识别封装格式 (FLV, MP4, ...)
    // 并填充 AVFormatContext 的基本信息。
    if (avformat_open_input(&ifmt_ctx, input_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Could not open input file." << std::endl;
        return false;
    }

    // avformat_find_stream_info 会读取一部分媒体数据，获取流信息
    // (如编码参数、时长、码率等)。对于 FLV 文件，这一步会解析出
    // 视频流的 codecpar (编码参数)。
    if (avformat_find_stream_info(ifmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to retrieve stream information." << std::endl;
        avformat_close_input(&ifmt_ctx);   // 失败时需释放资源
        return false;
    }

    // ---------- 4. 查找最佳视频流 ----------
    // av_find_best_stream 自动选择最合适的视频流（通常只有一个）。
    // 参数: 上下文, 媒体类型, 指定流索引(-1自动), 相关流(-1), 解码器参数(可选), 标志)
    int video_stream_idx = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO,
        -1, -1, nullptr, 0);
    if (video_stream_idx < 0) {
        std::cerr << "No video stream found in FLV." << std::endl;
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // 获取视频流指针
    AVStream* video_stream = ifmt_ctx->streams[video_stream_idx];

    // 检查编码类型是否为 H.264 (AVC)
    if (video_stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        std::cerr << "Video codec is not H.264/AVC." << std::endl;
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // ---------- 5. 初始化 H.264 MP4 到 Annex-B 的比特流过滤器 ----------
    // 背景知识:
    //   - FLV/MP4 通常采用 "AVCC" 格式存储 H.264 数据，即每个 NAL 单元前有
    //     长度字段 (4 字节大端)，而 extradata 中包含 SPS 和 PPS。
    //   - Annex-B 格式则是每个 NAL 单元前有起始码 (0x00 0x00 0x00 0x01 或
    //     0x00 0x00 0x01)，且 SPS/PPS 通常作为单独 NAL 单元发送。
    //   - FFmpeg 提供的 "h264_mp4toannexb" 过滤器能够自动完成转换：
    //       ① 从 extradata 中提取 SPS/PPS，并在输出流开头生成包含这些 NAL 的
    //          起始码数据包。
    //       ② 将每个视频包中的长度前缀替换为起始码。
    //       ③ 必要情况下对 NAL 单元进行拆分/组合以符合标准。

    // 5a. 通过名字获取比特流过滤器对象
    bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf) {
        std::cerr << "BSF h264_mp4toannexb not found." << std::endl;
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // 5b. 为过滤器分配上下文
    if (av_bsf_alloc(bsf, &bsf_ctx) < 0) {
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // 5c. 将视频流的编码参数复制到过滤器的输入参数中
    //     过滤器需要知道 extradata (包含 SPS/PPS) 以及编码相关信息。
    avcodec_parameters_copy(bsf_ctx->par_in, video_stream->codecpar);

    // 5d. 初始化过滤器上下文 (进行内部校验和准备)
    if (av_bsf_init(bsf_ctx) < 0) {
        std::cerr << "Failed to init BSF context." << std::endl;
        av_bsf_free(&bsf_ctx);
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // ---------- 6. 分配数据包对象 (AVPacket) ----------
    // AVPacket 用于存储从文件中读取的压缩数据包，以及在过滤器中传递的数据。
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "Failed to allocate AVPacket." << std::endl;
        av_bsf_free(&bsf_ctx);
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // ---------- 7. 循环读取所有数据包并进行过滤 ----------
    // av_read_frame 按时间顺序从输入文件读取下一个数据包 (可能是视频或音频)。
    // 当返回 >= 0 表示成功读取到 pkt，当返回 < 0 表示读完了或出错。
    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        // 只处理视频流的数据包，其他流 (音频、字幕等) 忽略
        if (pkt->stream_index == video_stream_idx) {
            // 7a. 将原始数据包送入比特流过滤器
            //     注意: av_bsf_send_packet 会将 pkt 的所有权转移给过滤器，
            //     所以调用后不能再使用 pkt，但 pkt 内部数据被引用，我们仍需
            //     在最后调用 av_packet_unref 来释放引用。
            //     如果返回 0 表示成功送入。
            if (av_bsf_send_packet(bsf_ctx, pkt) == 0) {
                // 7b. 循环接收过滤后的数据包
                //     过滤器可能将一个输入包拆成多个输出包 (例如分割 NAL 单元)，
                //     所以需要用 while 不断接收直到返回错误 (EAGAIN 表示等待更多输入，
                //     AVERROR_EOF 表示结束，其他错误表示失败)。
                while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                    // 此时 pkt->data 已经是 Annex-B 格式的 H.264 裸数据，
                    // 可以直接写入输出文件。
                    out_file.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
                    // 释放当前包引用 (减引用计数)
                    av_packet_unref(pkt);
                }
            }
        }
        // 释放原始 pkt 的引用 (如果还未释放)
        av_packet_unref(pkt);
    }

    // ---------- 8. 刷新过滤器缓冲区 ----------
    // 有些过滤器可能在输入结束后仍有缓存的输出数据 (例如最后一个 NAL 单元)。
    // 通过发送一个空包 (nullptr) 来通知过滤器输入结束。
    av_bsf_send_packet(bsf_ctx, nullptr);
    // 继续接收剩余的过滤输出包
    while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
        out_file.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        av_packet_unref(pkt);
    }

    // ---------- 9. 资源清理 ----------
    // 注意释放顺序: 先释放数据包，然后过滤器，最后关闭输入上下文。
    av_packet_free(&pkt);           // 释放 pkt 对象并置为 nullptr
    av_bsf_free(&bsf_ctx);          // 释放过滤器上下文
    avformat_close_input(&ifmt_ctx);// 关闭输入文件并释放上下文

    // ofstream 析构时会自动关闭，但我们显式关闭以明确
    out_file.close();

    // 成功完成
    std::cout << "Demuxing and BSF Annex-B extraction succeeded." << std::endl;
    return true;
}

// ----------------------------------------------------------------------------
// 主函数: 程序入口
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 检查命令行参数数量，需要提供输入和输出文件路径
    if (argc < 3) {
        std::cout << "Usage: ./flv_demux <input.flv> <output.h264>" << std::endl;
        return -1;
    }

    // 调用核心提取函数，根据返回值决定程序退出状态
    // 0 表示成功，非零表示失败
    return DemuxFlvToAnnexB(argv[1], argv[2]) ? 0 : -1;
}