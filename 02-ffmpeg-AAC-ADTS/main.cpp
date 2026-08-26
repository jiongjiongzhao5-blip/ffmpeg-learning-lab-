#include <iostream>      // 标准输入输出，用于打印信息
#include <memory>        // 提供 std::unique_ptr，用于 RAII 管理资源
#include <string>        // std::string 字符串操作
#include <system_error>  // 虽然未直接使用，但保留以支持可能的系统错误处理

// FFmpeg 是 C 库，需要用 extern "C" 包裹才能被 C++ 正确链接
extern "C" {
#include <libavformat/avformat.h>   // 封装格式处理（解封装、读取流等）
#include <libavcodec/avcodec.h>     // 编解码相关（参数、ID 等）
#include <libavcodec/bsf.h>         // Bitstream Filter（比特流过滤器），用于添加 ADTS 头
#include <libavutil/error.h>        // 错误处理工具（av_strerror 等）
}

// -------------------- RAII 资源管理辅助器 --------------------
// FFmpeg 的很多资源都需要手动释放，我们利用 C++ 的 RAII 思想，
// 通过 unique_ptr + 自定义 Deleter 自动释放资源，避免内存泄漏。

struct FFmpegDeleter {
    // 针对 AVFormatContext 的释放函数（关闭输入文件并释放上下文）
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);   // 内部会调用 avformat_free_context 并置空指针
    }
    // 针对 AVBSFContext 的释放函数（释放比特流过滤器上下文）
    void operator()(AVBSFContext* bsf) const {
        if (bsf) av_bsf_free(&bsf);
    }
    // 针对 AVPacket 的释放函数（释放包内部缓冲并回收包结构）
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

// 定义三种智能指针类型，方便使用
using ScopedFormatContext = std::unique_ptr<AVFormatContext, FFmpegDeleter>;
using ScopedBSFContext = std::unique_ptr<AVBSFContext, FFmpegDeleter>;
using ScopedPacket = std::unique_ptr<AVPacket, FFmpegDeleter>;

/**
 * 将 FFmpeg 错误码转换为可读的字符串（C++ 风格）
 * @param errnum 负数的错误码（FFmpeg 中错误码通常为负值）
 * @return 错误描述字符串
 */
static std::string av_err2str_cpp(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(errnum, errbuf, sizeof(errbuf));  // 将错误码转为人类可读信息
    return std::string(errbuf);
}

// -------------------- 核心工程类 --------------------
/**
 * AacAdtsExtractor 类：从多媒体文件中提取 AAC 音频流，并封装为标准 ADTS 格式的 .aac 文件。
 * 主要流程：打开输入 -> 查找 AAC 流 -> 创建 aac_adts 过滤器 -> 逐包读取、过滤、写入输出文件。
 */
class AacAdtsExtractor {
public:
    /**
     * 静态方法，执行提取操作
     * @param input_path  输入文件路径（如 .mp4, .flv, .mkv 等）
     * @param output_path 输出 .aac 文件路径（包含 ADTS 头）
     * @return 成功返回 true，否则 false
     */
    static bool extract_to_adts_file(const std::string& input_path, const std::string& output_path) {
        // ---------- 步骤1：打开输入媒体文件 ----------
        AVFormatContext* raw_fmt_ctx = nullptr;
        // avformat_open_input 会自动分配 AVFormatContext，并读取文件头信息
        int ret = avformat_open_input(&raw_fmt_ctx, input_path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            std::cerr << "Open input failed: " << av_err2str_cpp(ret) << "\n";
            return false;
        }
        // 使用智能指针管理格式上下文，离开作用域自动调用 avformat_close_input
        ScopedFormatContext fmt_ctx(raw_fmt_ctx);

        // 查找流信息，填充 streams 中的元数据（如编码参数、时长等）
        ret = avformat_find_stream_info(fmt_ctx.get(), nullptr);
        if (ret < 0) {
            std::cerr << "Find stream info failed: " << av_err2str_cpp(ret) << "\n";
            return false;
        }

        // ---------- 步骤2：定位 AAC 音频流 ----------
        int audio_stream_idx = -1;
        // 遍历所有流，根据 codec_id 判断是否为 AAC
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
            if (fmt_ctx->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC) {
                audio_stream_idx = static_cast<int>(i);
                break;
            }
        }
        if (audio_stream_idx < 0) {
            std::cerr << "No AAC audio stream found in source\n";
            return false;
        }

        // 获取该音频流的编码参数，其中包含 extradata（AudioSpecificConfig），
        // 这些信息在后续添加 ADTS 头时会被使用。
        AVCodecParameters* audio_codecpar = fmt_ctx->streams[audio_stream_idx]->codecpar;

        // ---------- 步骤3：创建并初始化 ADTS 比特流过滤器 ----------
        // "aac_adts" 是 FFmpeg 提供的过滤器，它能为每个 AAC 帧自动计算并添加上 7 字节的 ADTS 头
        const AVBitStreamFilter* bsf = av_bsf_get_by_name("aac_adts");
        if (!bsf) {
            std::cerr << "Cannot find 'aac_adts' BSF\n";
            return false;
        }

        AVBSFContext* raw_bsf_ctx = nullptr;
        // 分配过滤器上下文
        ret = av_bsf_alloc(bsf, &raw_bsf_ctx);
        if (ret < 0) {
            std::cerr << "Alloc BSF failed: " << av_err2str_cpp(ret) << "\n";
            return false;
        }
        ScopedBSFContext bsf_ctx(raw_bsf_ctx); // 智能管理

        // 将输入流的编码参数复制给过滤器的 par_in（输入参数），
        // 这样过滤器就能知道采样率、声道数、profile 等信息，从而正确构造 ADTS 头。
        ret = avcodec_parameters_copy(bsf_ctx->par_in, audio_codecpar);
        if (ret < 0) {
            std::cerr << "Copy codecpar to BSF failed: " << av_err2str_cpp(ret) << "\n";
            return false;
        }

        // 初始化过滤器（会根据 par_in 内部状态准备好工作环境）
        ret = av_bsf_init(bsf_ctx.get());
        if (ret < 0) {
            std::cerr << "BSF Init failed: " << av_err2str_cpp(ret) << "\n";
            return false;
        }

        // ---------- 步骤4：打开输出文件（C 标准 IO） ----------
        FILE* out_fp = fopen(output_path.c_str(), "wb");
        if (!out_fp) {
            std::cerr << "Failed to open output file: " << output_path << "\n";
            return false;
        }
        // 使用 unique_ptr 配合 fclose 确保文件在作用域结束时正确关闭
        std::unique_ptr<FILE, decltype(&fclose)> fp_guard(out_fp, fclose);

        // ---------- 步骤5：循环解封装、过滤、写入 ----------
        // 分配两个 AVPacket：一个用于从容器读取原始包，另一个用于接收过滤后的包（带有 ADTS 头）
        ScopedPacket pkt(av_packet_alloc());      // 原始包
        ScopedPacket bsf_pkt(av_packet_alloc());  // 过滤后包

        // 循环读取所有数据包，直到 av_read_frame 返回错误（通常是 EOF）
        while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
            // 只处理音频流（其它流如视频、字幕直接忽略）
            if (pkt->stream_index == audio_stream_idx) {
                // 将原始包送入过滤器（注意：av_bsf_send_packet 内部会引用 pkt 的数据，
                // 调用后 pkt 不再拥有该数据，因此需要后续调用 av_packet_unref 释放）
                ret = av_bsf_send_packet(bsf_ctx.get(), pkt.get());
                if (ret < 0) {
                    std::cerr << "Send packet to BSF failed: " << av_err2str_cpp(ret) << "\n";
                    // 发生错误时仍需释放 pkt 内部资源
                    av_packet_unref(pkt.get());
                    break;
                }

                // 从过滤器接收处理后的包（可能一次发送产生多个输出包，所以用 while 循环）
                while (av_bsf_receive_packet(bsf_ctx.get(), bsf_pkt.get()) == 0) {
                    // 将过滤后的完整 AAC 帧（含 ADTS 头）直接写入输出文件
                    fwrite(bsf_pkt->data, 1, bsf_pkt->size, out_fp);
                    // 释放 bsf_pkt 内部数据，以便下次复用
                    av_packet_unref(bsf_pkt.get());
                }
            }
            // 释放原始包的内部资源（av_read_frame 分配的 buffer 等）
            av_packet_unref(pkt.get());
        }

        // ---------- 步骤6：冲刷过滤器 ----------
        // 发送空包（nullptr）表示没有更多输入，要求过滤器输出所有缓存的包
        av_bsf_send_packet(bsf_ctx.get(), nullptr);
        // 循环接收剩余的包（过滤器可能因为分包或缓存而保留了一些帧）
        while (av_bsf_receive_packet(bsf_ctx.get(), bsf_pkt.get()) == 0) {
            fwrite(bsf_pkt->data, 1, bsf_pkt->size, out_fp);
            av_packet_unref(bsf_pkt.get());
        }

        std::cout << "Successfully extracted AAC to ADTS: " << output_path << "\n";
        return true;
    }
};

// ---------- 主程序 ----------
int main(int argc, char* argv[]) {
    // 检查命令行参数数量，至少需要输入文件和输出文件
    if (argc < 3) {
        std::cout << "Usage: ./aac_extractor <input.mp4/flv> <output.aac>\n";
        return 0;
    }

    // 调用核心提取函数
    bool success = AacAdtsExtractor::extract_to_adts_file(argv[1], argv[2]);
    return success ? 0 : 1;
}