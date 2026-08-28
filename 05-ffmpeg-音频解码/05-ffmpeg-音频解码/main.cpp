// ============================================================================
// 文件名: audio_decoder_pipeline.cpp
// 功能: 使用 FFmpeg 库解码 FLV 文件中的音频流，并重采样为固定格式的 PCM 数据。
//       输出为原始 PCM 文件（无头信息），可被 ffplay 等工具直接播放。
// 适用 FFmpeg 版本: 7.x / 8.x / 9.x（使用现代 API，如 AVChannelLayout）
// 设计核心: 采用 RAII（资源获取即初始化）管理 FFmpeg 资源，避免内存泄漏。
// ============================================================================

// ---------------------- 包含必要的 C++ 标准库头文件 ------------------------
#include <iostream>   // 用于控制台输出信息（std::cout, std::cerr）
#include <fstream>    // 用于文件读写（std::ofstream 写 PCM 文件）
#include <memory>     // 提供 std::unique_ptr，用于自动释放资源
#include <string>     // 使用 std::string 处理文件路径字符串
#include <vector>     // 使用 std::vector 作为动态缓冲区，存放重采样后的 PCM 数据

// ----- 包含 FFmpeg 的 C 语言头文件（必须用 extern "C" 防止 C++ 名称修饰）-----
extern "C" {
#include <libavformat/avformat.h>      // 解封装（Demuxer）：读取容器格式（如 FLV、MP4）并分离音频/视频流
#include <libavcodec/avcodec.h>        // 编解码器：将压缩数据（如 AAC、MP3）解码为原始 PCM
#include <libswresample/swresample.h>  // 重采样（Resampler）：转换采样率、声道数、采样格式
#include <libavutil/channel_layout.h>  // 声道布局结构体（AVChannelLayout），现代 API 替代旧的 uint64_t 掩码
#include <libavutil/samplefmt.h>       // 采样格式枚举（如 AV_SAMPLE_FMT_S16、AV_SAMPLE_FMT_FLTP）
#include <libavutil/opt.h>             // 选项设置（本代码未使用，但包含以防后续扩展）
}

// =========================================================
// 1. RAII 辅助工具：利用 C++ 析构函数自动释放 FFmpeg 资源
//    原理：FFmpeg 的资源（如 AVFormatContext）需要用专门的函数释放，
//    如果忘记调用，就会造成内存泄漏。我们将这些释放函数封装成“删除器”（Deleter），
//    然后与 std::unique_ptr 组合，当智能指针生命周期结束时，自动调用删除器。
// =========================================================

// 删除器：负责释放 AVFormatContext
// avformat_close_input 会关闭文件并释放所有内部缓冲，传入指针的指针（&ctx）以便置空。
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);  // 只有非空才释放，避免野指针
    }
};

// 删除器：负责释放 AVCodecContext
// avcodec_free_context 会释放解码器上下文及其内部缓存。
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};

// 删除器：负责释放 AVPacket
// av_packet_free 会释放结构体本身以及内部的 buffer（引用计数减一并释放）。
struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

// 删除器：负责释放 AVFrame
// av_frame_free 释放帧结构体及其数据 buffer。
struct AVFrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};

// 删除器：负责释放 SwrContext（重采样上下文）
// swr_free 释放重采样器内部所有缓存和状态。
struct SwrContextDeleter {
    void operator()(SwrContext* swr) const {
        if (swr) swr_free(&swr);
    }
};

// 定义智能指针类型别名，方便使用
// 以后我们可以这样写：UniqueAVFormatContext fmt_ctx; 它会自动管理资源。
using UniqueAVFormatContext = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;
using UniqueSwrContext = std::unique_ptr<SwrContext, SwrContextDeleter>;

// ---------- 辅助函数：将 FFmpeg 错误码转换为可读的错误信息并打印 ----------
void log_ffmpeg_error(int err, const std::string& msg) {
    // FFmpeg 提供了一个宏 AV_ERROR_MAX_STRING_SIZE（通常为64）作为错误字符串缓冲区大小
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    // av_make_error_string 将错误码格式化到缓冲区中，返回指针（可直接使用）
    av_make_error_string(err_buf, sizeof(err_buf), err);
    // 打印：自定义消息 + FFmpeg 生成的错误描述 + 原始错误码（方便查找文档）
    std::cerr << "[Error] " << msg << ": " << err_buf << " (" << err << ")" << std::endl;
}

// =========================================================
// 2. 音频解码与重采样核心 Pipeline 类
//    这个类封装了所有操作，外部只需调用 init() 和 process() 即可。
//    内部成员都是智能指针，不需要手动 delete，安全可靠。
// =========================================================
class AudioDecoderPipeline {
public:
    // ---------- 输出参数固定为最通用的格式 ----------
    // 为什么要固定？因为很多播放器（如 ffplay、VLC）默认支持 44.1kHz 立体声 S16。
    // 而且我们做重采样就是为了统一格式，方便后续处理（如播放、分析）。
    static constexpr int OUT_SAMPLE_RATE = 44100;          // 44.1 kHz（CD 音质标准）
    static constexpr AVSampleFormat OUT_SAMPLE_FMT = AV_SAMPLE_FMT_S16; // 有符号 16 位整型，交错存储（左右声道交替）

    // 构造函数：默认即可，所有成员变量在初始化列表中自动初始化为空智能指针
    AudioDecoderPipeline() = default;

    // ---------- 初始化函数：打开文件、查找音频流、准备解码器 ----------
    // 参数：输入 FLV 文件路径（也可以是其他容器，但这里以 FLV 为例）
    // 返回值：成功 true，失败 false
    bool init(const std::string& input_flv_path) {
        // ---- 步骤1：打开输入文件（解封装器） ----
        // avformat_open_input 的作用：
        //   - 根据文件名自动探测文件格式（FLV、MP4、AVI等）
        //   - 分配并填充 AVFormatContext 结构体，包含文件中的所有流信息（但此时还未读取详细参数）
        //   - 第三个参数为输入格式（传 NULL 自动探测），第四个为字典选项（一般填 NULL）
        AVFormatContext* raw_fmt_ctx = nullptr;
        int ret = avformat_open_input(&raw_fmt_ctx, input_flv_path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            log_ffmpeg_error(ret, "Failed to open input FLV");
            return false;  // 打开失败，例如文件不存在或权限问题
        }
        // 立即将原始指针交给 unique_ptr 托管，即使后面发生异常，也能自动释放
        fmt_ctx_.reset(raw_fmt_ctx);

        // ---- 步骤2：读取流信息（获取详细参数） ----
        // avformat_find_stream_info 会从文件中读取一部分数据包，解析出每个流的编码参数
        // （如音频的采样率、声道数、编码格式等），这些信息存放在 fmt_ctx->streams 中。
        // 如果不调用此函数，后面就无法正确初始化解码器。
        if ((ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr)) < 0) {
            log_ffmpeg_error(ret, "Failed to find stream info");
            return false;
        }

        // ---- 步骤3：查找音频流 ----
        // 一个文件可能包含多个音频流（如不同语言），av_find_best_stream 会智能选择最佳的一个。
        // 参数：上下文、媒体类型（音频）、-1 表示不指定流索引、-1 表示不指定相关流、返回解码器（不需要则 NULL）、标志（0）
        // 返回值：流索引（>=0）或负错误码。
        audio_stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_idx_ < 0) {
            std::cerr << "No audio stream found in input file!" << std::endl;
            return false;
        }

        // 获取该音频流的指针（后面大量用到）
        AVStream* audio_stream = fmt_ctx_->streams[audio_stream_idx_];

        // ---- 步骤4：查找解码器 ----
        // 每个流都有一个 codecpar（编解码器参数），其中 codec_id 是枚举值，表示编码类型（如 AAC、MP3）。
        // 我们需要根据这个 ID 找到对应的解码器实现（AVCodec）。
        const AVCodec* decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (!decoder) {
            std::cerr << "Unsupported codec ID: " << audio_stream->codecpar->codec_id << std::endl;
            return false;  // 例如 AC-3 编码未编译进 FFmpeg
        }

        // ---- 步骤5：分配解码器上下文 ----
        // avcodec_alloc_context3 分配 AVCodecContext 结构体，但此时还未与解码器关联参数。
        codec_ctx_.reset(avcodec_alloc_context3(decoder));
        if (!codec_ctx_) {
            std::cerr << "Failed to allocate codec context" << std::endl;
            return false;
        }

        // ---- 步骤6：将流参数拷贝到解码器上下文 ----
        // 解码器需要知道码率、采样率、声道布局等，这些都在 codecpar 中。
        // avcodec_parameters_to_context 负责复制这些参数，之后才能正确解码。
        if ((ret = avcodec_parameters_to_context(codec_ctx_.get(), audio_stream->codecpar)) < 0) {
            log_ffmpeg_error(ret, "Failed to copy codec parameters to context");
            return false;
        }

        // ---- 步骤7：正式打开解码器 ----
        // avcodec_open2 会初始化解码器内部状态，分配所需内存。
        // 第三个参数是选项字典（一般填 NULL）。
        if ((ret = avcodec_open2(codec_ctx_.get(), decoder, nullptr)) < 0) {
            log_ffmpeg_error(ret, "Failed to open codec");
            return false;
        }

        // ---- 步骤8：分配 Packet 和 Frame 对象 ----
        // AVPacket 用于存放从文件中读取的压缩数据（如 AAC 帧）。
        // AVFrame 用于存放解码后的原始 PCM 数据（未重采样前）。
        // av_packet_alloc() / av_frame_alloc() 分配结构体并初始化内部指针。
        pkt_.reset(av_packet_alloc());
        frame_.reset(av_frame_alloc());

        // ---- 打印输入音频信息，方便调试 ----
        std::cout << "[Pipeline] Initialized successfully. Codec: " << decoder->name
            << ", In Format: " << av_get_sample_fmt_name(codec_ctx_->sample_fmt)
            << ", Channels: " << codec_ctx_->ch_layout.nb_channels
            << ", Sample Rate: " << codec_ctx_->sample_rate << "Hz" << std::endl;
        return true;
    }

    // ---------- 主处理函数：循环读包 -> 解码 -> 重采样 -> 写文件 ----------
    // 参数：输出 PCM 文件路径
    void process(const std::string& output_pcm_path) {
        // 以二进制方式打开输出文件（因为 PCM 是纯音频数据，无文本格式）
        std::ofstream pcm_out(output_pcm_path, std::ios::binary);
        if (!pcm_out.is_open()) {
            std::cerr << "Failed to open output PCM file: " << output_pcm_path << std::endl;
            return;
        }

        // ---- 步骤9：循环从文件中读取数据包 ----
        // av_read_frame 每次读取一个数据包（可能包含完整音频帧或视频帧），
        // 它内部会处理文件缓冲，返回 0 表示成功，负数表示出错或 EOF。
        while (av_read_frame(fmt_ctx_.get(), pkt_.get()) >= 0) {
            // 判断这个包属于哪个流（因为我们只处理音频，忽略视频或其他流）
            if (pkt_->stream_index == audio_stream_idx_) {
                // 调用私有方法解码这个包，并重采样写入文件
                decode_and_resample(pkt_.get(), pcm_out);
            }
            // 重要！每次读完一个包后必须释放其内部数据引用，否则会造成内存泄漏
            // av_packet_unref 会减少内部 buffer 的引用计数，如果计数归零则释放 buffer。
            // 注意：这不会释放 AVPacket 结构体本身（因为我们复用同一个 pkt_ 对象）。
            av_packet_unref(pkt_.get());
        }

        // ---- 步骤10：冲刷解码器（Flush） ----
        // 解码器内部可能缓存了一些尚未输出的帧（例如因为解码器需要参考后续帧）。
        // 当所有数据包都发送完毕后，必须发送一个 NULL 包给解码器，告诉它“没有更多数据了”，
        // 让它把缓存的帧都吐出来。这个过程叫做“冲刷”。
        std::cout << "[Pipeline] Demuxing completed. Flushing decoder..." << std::endl;
        decode_and_resample(nullptr, pcm_out);  // 传入 nullptr 表示冲刷模式

        std::cout << "[Pipeline] Audio processing finished. Output: " << output_pcm_path << std::endl;
    }

private:
    // ---------- 延迟初始化重采样器 ----------
    // 为什么不在 init() 中初始化？因为某些编码格式（如 AAC）的音频参数可能
    // 直到解码出第一帧才能完全确定（例如声道布局在流信息中未明确）。
    // 所以我们利用实际解码出的第一帧的 AVFrame 来初始化重采样器，确保参数准确。
    // 参数 frame：已经解码好的 AVFrame（包含输入音频的实际参数）
    void init_resampler_if_needed(const AVFrame* frame) {
        if (swr_ctx_) return;  // 如果已经初始化过，直接返回

        // ---- 设置输出声道布局为立体声（2 声道） ----
        // AVChannelLayout 是现代 FFmpeg 用于描述声道布局的结构体，支持更多声道配置。
        // 我们固定为 2 声道立体声（左前、右前）。
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, 2);  // 使用默认布局（立体声）

        // ---- 创建重采样器上下文并设置参数 ----
        SwrContext* raw_swr = nullptr;
        // swr_alloc_set_opts2 是较新版本的 API，使用 AVChannelLayout 代替旧的 uint64_t 掩码。
        // 参数依次为：
        //   - &raw_swr: 输出指针（如果为 NULL 则自动分配）
        //   - &out_ch_layout: 输出声道布局
        //   - OUT_SAMPLE_FMT: 输出采样格式（S16）
        //   - OUT_SAMPLE_RATE: 输出采样率
        //   - &frame->ch_layout: 输入声道布局（从解码出的帧中获取）
        //   - frame->format: 输入采样格式（注意是 int，需要强制转换为 AVSampleFormat）
        //   - frame->sample_rate: 输入采样率
        //   - 0: 日志级别（0 表示默认）
        //   - nullptr: 可选选项字典
        int ret = swr_alloc_set_opts2(
            &raw_swr,
            &out_ch_layout,
            OUT_SAMPLE_FMT,
            OUT_SAMPLE_RATE,
            &frame->ch_layout,
            static_cast<AVSampleFormat>(frame->format),
            frame->sample_rate,
            0,
            nullptr
        );

        // ---- 检查是否成功分配并初始化 ----
        // 如果 ret < 0 或者 raw_swr 为 NULL，或者 swr_init 返回失败，则重采样器不可用。
        // swr_init 会执行内部查表、分配缓存等操作。
        if (ret < 0 || !raw_swr || swr_init(raw_swr) < 0) {
            log_ffmpeg_error(ret, "Failed to initialize audio resampler");
            if (raw_swr) swr_free(&raw_swr);  // 清理部分分配的资源
            av_channel_layout_uninit(&out_ch_layout);  // 释放输出布局内部动态内存（若有）
            return;  // 此时 swr_ctx_ 保持为空，后续重采样操作会被跳过
        }
        // 将原始指针交给 unique_ptr 托管
        swr_ctx_.reset(raw_swr);
        // 释放临时布局结构体（内部可能分配了字符串等，需调用 uninit）
        av_channel_layout_uninit(&out_ch_layout);
    }

    // ---------- 解码一个数据包并重采样写入文件 ----------
    // 参数 pkt：输入的压缩数据包（可以为 nullptr，表示冲刷模式）
    // 参数 pcm_out：输出文件流引用
    void decode_and_resample(const AVPacket* pkt, std::ofstream& pcm_out) {
        // ---- 步骤A：将数据包送入解码器 ----
        // avcodec_send_packet 将压缩包提交给解码器。
        // 如果 pkt == nullptr，解码器会进入冲刷模式，开始输出缓存的帧。
        int ret = avcodec_send_packet(codec_ctx_.get(), pkt);
        if (ret < 0) {
            log_ffmpeg_error(ret, "avcodec_send_packet failed");
            return;  // 发送失败，可能是数据包损坏
        }

        // ---- 步骤B：循环从解码器中接收解码后的帧 ----
        // avcodec_receive_frame 会尝试从解码器输出一个解码好的 AVFrame。
        // 需要循环调用，因为一个数据包可能产生多个帧（如某些音频格式），或者解码器内部缓冲了多帧。
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_.get(), frame_.get());
            if (ret == AVERROR(EAGAIN)) {
                // EAGAIN 表示解码器需要更多数据才能输出帧，此时应跳出循环，等待下一个包。
                break;
            }
            else if (ret == AVERROR_EOF) {
                // EOF 表示所有数据已输出完毕，冲刷完成。
                break;
            }
            else if (ret < 0) {
                // 其他错误，打印并退出循环
                log_ffmpeg_error(ret, "avcodec_receive_frame error");
                break;
            }

            // ---- 步骤C：成功收到一个解码帧，进行重采样 ----
            // 第一次收到帧时，重采样器尚未初始化，调用此函数进行延迟初始化。
            init_resampler_if_needed(frame_.get());

            // 如果重采样器已成功初始化，则执行转换
            if (swr_ctx_) {
                // ---- C1：计算输出缓冲区大小 ----
                // 重采样器内部可能有延迟（例如多相滤波器会缓存一些样本），
                // swr_get_delay 返回当前缓存的样本数（以输入采样率为单位）。
                // 我们需要把输入帧的样本数加上延迟，再按输出采样率缩放，得到输出样本数。
                int64_t delay = swr_get_delay(swr_ctx_.get(), frame_->sample_rate);
                // av_rescale_rnd 进行分数缩放，AV_ROUND_UP 表示向上取整，确保不遗漏任何样本。
                int out_samples = av_rescale_rnd(
                    delay + frame_->nb_samples,   // 输入总样本数（含延迟）
                    OUT_SAMPLE_RATE,              // 输出采样率
                    frame_->sample_rate,          // 输入采样率
                    AV_ROUND_UP
                );

                // ---- C2：分配输出缓冲区 ----
                // av_samples_get_buffer_size 计算存储给定样本数所需的字节数。
                // 参数：行大小（可 NULL）、声道数（2）、样本数、采样格式、对齐（1 表示默认）。
                // 对于交错格式（S16），所有声道的数据连续存放，所以缓冲区是一块连续内存。
                size_t out_buf_size = av_samples_get_buffer_size(nullptr, 2, out_samples, OUT_SAMPLE_FMT, 1);
                std::vector<uint8_t> out_buffer(out_buf_size);  // 创建足够大的 vector

                // swr_convert 要求输出数据以指针数组形式传入（每个声道一个指针），
                // 但对于交错格式，只需第一个指针指向整个缓冲区即可。
                uint8_t* out_data[1] = { out_buffer.data() };

                // ---- C3：执行重采样转换 ----
                // swr_convert 将 frame 中的数据转换为目标格式，并返回实际转换出的样本数。
                // 参数：上下文、输出指针数组、输出最大样本数、输入指针数组（const 修饰）、输入样本数。
                // 注意：frame_->data 是 uint8_t**，需要转换为 const uint8_t**。
                int converted_samples = swr_convert(
                    swr_ctx_.get(),
                    out_data,                 // 输出数据指针数组
                    out_samples,              // 输出缓冲区能容纳的最大样本数
                    const_cast<const uint8_t**>(frame_->data), // 输入数据（强制去除 const，因为 API 设计如此）
                    frame_->nb_samples        // 输入样本数
                );

                // ---- C4：写入输出文件 ----
                if (converted_samples > 0) {
                    // 计算实际转换出的字节数（可能少于之前分配的大小）
                    int real_bytes = av_samples_get_buffer_size(nullptr, 2, converted_samples, OUT_SAMPLE_FMT, 1);
                    // 将 vector 中的原始数据写入文件（二进制方式）
                    pcm_out.write(reinterpret_cast<char*>(out_buffer.data()), real_bytes);
                }
            }

            // ---- 步骤D：释放帧内部引用 ----
            // 每个帧内部可能引用着 buffer，调用 av_frame_unref 减少引用计数，
            // 这样当所有引用都释放后，buffer 才会被释放。
            // 注意：frame_ 对象本身被复用，所以不能调用 av_frame_free。
            av_frame_unref(frame_.get());
        }
    }

    // ---------- 成员变量 ----------
    int audio_stream_idx_ = -1;             // 音频流在 fmt_ctx_ 中的索引（-1 表示未找到）
    UniqueAVFormatContext fmt_ctx_;         // 解封装上下文（管理整个文件）
    UniqueAVCodecContext  codec_ctx_;       // 解码器上下文
    UniqueAVPacket        pkt_;             // 当前读取的压缩数据包（每次循环复用）
    UniqueAVFrame         frame_;           // 解码后的原始音频帧（每次复用）
    UniqueSwrContext      swr_ctx_;         // 重采样器（延迟初始化）
};

// =========================================================
// 3. 主函数：解析命令行参数，实例化并启动 Pipeline
// =========================================================
int main(int argc, char* argv[]) {
    // 支持命令行参数：第一个参数是输入 FLV 文件名，第二个是输出 PCM 文件名
    // 如果未指定，使用默认值 "input.flv" 和 "output.pcm"
    std::string input_file = (argc > 1) ? argv[1] : "cctv1.flv";
    std::string output_file = (argc > 2) ? argv[2] : "output.pcm";

    // 创建 Pipeline 对象
    AudioDecoderPipeline pipeline;
    // 初始化（打开文件、查找流、准备解码器）
    if (!pipeline.init(input_file)) {
        return -1;  // 初始化失败则退出
    }

    // 执行解码和重采样流程
    pipeline.process(output_file);

    // 提示用户如何验证生成的 PCM 文件（使用 ffplay）
    std::cout << "\n[Success] To verify PCM audio playback in terminal, run:\n"
        << "ffplay -ar 44100 -ac 2 -f s16le " << output_file << std::endl;
    return 0;
}