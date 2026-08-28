/**
 * ============================================================================
 * video_tool.cpp —— 迷你视频工具（C++17 + FFmpeg 8/9）
 *
 * 它把你课堂 4 个视频文件的知识点合并成一个生产形态的小工具：
 *   ① NAL 单元类型分析（PDF-1 的知识）
 *   ② 软解 / 硬解（d3d11va、qsv、cuda、vaapi...）
 *   ③ 解码帧用 swscale 转 YUV420P 写出（生产做法，替代老代码手写 fwrite）
 *   ④ 软编(libx264/libx265) / 硬编(h264_nvenc/hevc_nvenc)
 *
 * 编译（Linux）：
 *   g++ -std=c++17 video_tool.cpp -o video_tool \
 *       $(pkg-config --cflags --libs libavcodec libavutil libswscale libavformat)
 *
 * 用法：
 *   ./video_tool decode in.h265                       # 软解 + 打印 NAL 类型
 *   ./video_tool decode in.mp4  --out out.yuv         # 容器解码，写出 YUV
 *   ./video_tool decode in.h264 --hw d3d11va          # Windows 硬解
 *   ./video_tool encode in.yuv out.h265 --codec libx265 --preset medium
 *   ./video_tool encode in.yuv out.h264 --codec h264_nvenc --preset p5
 * ============================================================================
 */
#define _CRT_SECURE_NO_WARNINGS
 // 标准库头文件
#include <cstdio>      // 标准 I/O (fopen, fread, fwrite, printf...)
#include <cstring>     // memmove, strcmp
#include <cstdlib>     // atoi
#include <cinttypes>   // PRId64 宏，用于打印 int64_t
#include <string>      // std::string
#include <memory>      // std::unique_ptr, std::shared_ptr
#include <vector>      // std::vector

// FFmpeg 是 C 库，在 C++ 里必须用 extern "C" 包起来，否则 C++ 编译器会做名字修饰（name mangling），
// 导致链接时找不到 FFmpeg 的符号。所有 FFmpeg 头文件都必须在 extern "C" 块内包含。
extern "C" {
#include <libavcodec/avcodec.h>      // 编解码器核心 API (avcodec_*, AVCodecContext, AVPacket...)
#include <libavutil/frame.h>         // AVFrame 结构及操作
#include <libavutil/imgutils.h>      // 图像工具：av_image_alloc, av_image_fill_arrays
#include <libavutil/opt.h>           // 选项设置：av_opt_set（用于编码器私有参数）
#include <libavutil/hwcontext.h>     // 硬件加速上下文：av_hwdevice_*, av_hwframe_transfer_data
#include <libavutil/time.h>          // 时间工具：av_gettime_relative（用于性能测量）
#include <libavutil/pixdesc.h>       // 像素格式描述：av_get_pix_fmt_name
#include <libavformat/avformat.h>    // 容器格式 API：avformat_*, AVFormatContext
#include <libswscale/swscale.h>      // 图像缩放/格式转换：sws_* 系列
}

/* ============================================================================
 * 一、FFmpeg 对象的 RAII 包装
 *   FFmpeg 的 C 接口需要手动调用 av*_free 或 av*_unref 来释放资源，稍有不慎就会内存泄漏。
 *   C++17 最常规的工程做法：使用 std::unique_ptr 搭配自定义 deleter（删除器）。
 *   这样当 unique_ptr 离开作用域时，会自动调用 deleter 释放 FFmpeg 对象。
 *   这极大地提高了代码的健壮性和可读性，避免了手动管理资源的繁琐和错误。
 * ============================================================================ */

 // 自定义删除器（deleter），用于 AVCodecContext*
struct AvCodecContextDeleter {
    void operator()(AVCodecContext* p) const {
        avcodec_free_context(&p);   // 内部会释放所有关联资源（extradata, 内部缓冲区等）
    }
};
// 以下删除器类似，分别对应各自的释放函数
struct AvFrameDeleter {
    void operator()(AVFrame* p) const {
        av_frame_free(&p);          // 释放帧及内部数据（若未引用则释放）
    }
};
struct AvPacketDeleter {
    void operator()(AVPacket* p) const {
        av_packet_free(&p);         // 释放包及内部缓冲区（若未引用则释放）
    }
};
struct AvBufferRefDeleter {
    void operator()(AVBufferRef* p) const {
        av_buffer_unref(&p);        // 减少引用计数，计数到 0 时释放底层缓冲区
    }
};
struct AvParserDeleter {
    void operator()(AVCodecParserContext* p) const {
        av_parser_close(p);         // 关闭解析器，释放内部状态
    }
};
struct SwsContextDeleter {
    void operator()(SwsContext* p) const {
        sws_freeContext(p);         // 释放图像转换上下文
    }
};
struct AvFormatCtxDeleter {
    void operator()(AVFormatContext* p) const {
        avformat_close_input(&p);   // 关闭输入文件并释放 AVFormatContext
    }
};

// 类型别名，方便使用
using CodecCtxPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;
using ParserPtr = std::unique_ptr<AVCodecParserContext, AvParserDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using FormatCtxPtr = std::unique_ptr<AVFormatContext, AvFormatCtxDeleter>;

/**
 * 工具函数：将 FFmpeg 错误码（负数）转换为人类可读的字符串。
 * 生产环境中，遇到错误时打印这个字符串比只打印数字更有意义。
 */
static std::string ff_err_str(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };  // FFmpeg 定义的最大错误字符串长度
    av_strerror(errnum, buf, sizeof(buf));     // 填充错误描述
    return std::string(buf);
}

/* ============================================================================
 * 二、NAL 类型分析（对应 PDF-1 的核心知识）
 *   裸流(H.264/H.265 Annex-B) 的结构 = 开始码 (0x000001 或 0x00000001) + 一个 NALU + 开始码 + ...
 *   判断 NAL 类型：
 *     - H264:  NAL 单元第一个字节的低 5 位 (data[0] & 0x1F)
 *     - H265:  NAL 单元第一个字节的第 2~7 位 ((data[0] & 0x7E) >> 1)
 *   生产中的真实用途：RTP/摄像头裸流里判断"这一包是不是关键帧"（IDR）、
 *   以及调试时确认 SPS/PPS/IDR 是否周期出现，从而验证编码器 GOP 设置是否生效。
 * ============================================================================ */

 /**
  * H.264 NAL 单元类型名称（只列出常见的）
  */
static const char* nal_name_h264(int t) {
    switch (t) {
    case 1: return "P";          // P 帧分片（非参考帧）
    case 2: return "B";          // B 帧分片
    case 5: return "IDR";        // 关键帧（Instantaneous Decoding Refresh）
    case 6: return "SEI";        // 补充增强信息（如用户自定义数据）
    case 7: return "SPS";        // 序列参数集（编码参数，如分辨率、档次）
    case 8: return "PPS";        // 图像参数集（如熵编码模式、片组）
    case 9: return "AUD";        // 访问单元分隔符
    default: return "OTHER";
    }
}

/**
 * H.265 (HEVC) NAL 单元类型名称（部分关键类型）
 * 注意：H.265 的 IDR 分两种（IDR_W_DLP 和 IDR_N_LP），CRA 是开放 GOP 的起始点。
 */
static const char* nal_name_h265(int t) {
    switch (t) {
    case 0:  return "TRAIL_N";   // 非参考帧的尾部图片
    case 1:  return "TRAIL_R";   // 参考帧的尾部图片
    case 19: return "IDR_W_DLP"; // IDR 帧，可解码前导图片
    case 20: return "IDR_N_LP";  // IDR 帧，无前导图片
    case 21: return "CRA(OpenGOP)"; // 清洁随机访问（开放 GOP 起始）
    case 32: return "VPS";       // 视频参数集（比 SPS 更高层）
    case 33: return "SPS";       // 序列参数集
    case 34: return "PPS";       // 图像参数集
    case 39: return "SEI";       // 补充增强信息
    case 40: return "AUD";       // 访问单元分隔符
    default: return "OTHER";
    }
}

/**
 * 打印一个 NAL 单元的类型信息。
 * @param data  指向 NAL 单元数据起始的指针（不含开始码，即开始码后的第一个字节）
 * @param size  该 NAL 单元的总大小（包括头部）
 * @param codec_id  解码器 ID，用于区分 H.264 还是 H.265
 */
static void print_nal_type(const uint8_t* data, size_t size, enum AVCodecID codec_id) {
    if (size < 1) return;  // NAL 单元至少要有 1 字节头部
    int nal_type;
    if (codec_id == AV_CODEC_ID_H264) {
        nal_type = data[0] & 0x1F;          // H.264：取低 5 位
    }
    else { // 假定为 H.265
        nal_type = (data[0] & 0x7E) >> 1;   // H.265：取第 2~7 位
    }
    const char* name = (codec_id == AV_CODEC_ID_H264) ? nal_name_h264(nal_type)
        : nal_name_h265(nal_type);
    printf("      nal header=0x%02X  type=%-2d  size=%-6zu  (%s)\n",
        data[0], nal_type, size, name);
}

/**
 * 判断某位置是否为一个有效的 Annex-B 开始码。
 * Annex-B 规定开始码可以是 3 字节 (0x000001) 或 4 字节 (0x00000001)。
 * @param p         指向当前检查位置的指针
 * @param remaining 该位置之后剩余的数据长度（防止越界）
 * @return true 如果该位置是一个开始码
 */
static bool is_start_code(const uint8_t* p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

/* ============================================================================
 * 三、解码会话结构（DecodeSession）
 *   封装了解码所需的所有上下文和状态。采用 RAII 管理资源，所有成员都是智能指针或原始 FILE*。
 *   hw_name 为空表示纯软解；非空表示指定硬件加速设备（如 "d3d11va"）。
 * ============================================================================ */
struct DecodeSession {
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;   // 解码器 ID，如 AV_CODEC_ID_H264
    std::string hw_name;                          // 硬件加速设备名，空表示软件解码
    ParserPtr  parser;          // 裸流解析器（只有处理裸流时需要；容器解码不需要）
    CodecCtxPtr dec_ctx;        // 解码器上下文（包含解码器状态、参数、私有选项等）
    BufferRefPtr hw_device_ctx; // 硬件设备上下文（代表 GPU 设备，RAII 自动释放）
    enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE; // 解码器输出的硬件像素格式（如 AV_PIX_FMT_D3D11）
    FILE* outfile = nullptr;    // 要写 YUV 时的目标文件指针（不为空则写出）
    SwsPtr sws;                 // 图像格式转换上下文（用于将解码帧转为 YUV420P，可复用）
    int frame_count = 0;        // 已解码帧数（主要用于打印统计）
};

/* ============================================================================
 * 四、硬件解码初始化
 *   对应课堂 main4.c 的知识点。
 *   生产注意：命令行里直接用 `-hwaccel d3d11va` 会自动回退软解更省心；
 *   但你在 C++ 里自己写，就需要走下面这套 API，更加灵活但也更复杂。
 * ============================================================================ */

 /**
  * get_format 回调函数：解码器在 avcodec_open2 时询问"输出帧使用哪种像素格式"。
  * 我们从解码器支持的格式列表（pix_fmts）中，挑出我们之前通过硬件初始化确定的硬件格式。
  * 如果找到，则告诉解码器使用该格式；否则返回 AV_PIX_FMT_NONE，导致打开失败。
  * 注意：此回调函数在解码器打开时调用一次或多次，用于协商格式。
  *       ctx->opaque 是我们提前设置的用户数据指针（指向 DecodeSession）。
  */
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    auto* self = static_cast<DecodeSession*>(ctx->opaque);  // 从 opaque 取回会话指针
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == self->hw_pix_fmt) {
            return *p;   // 命中硬件格式，告诉解码器就用这个
        }
    }
    // 如果列表里没有我们想要的硬件格式，则拒绝（返回 AV_PIX_FMT_NONE 会导致打开失败）
    return AV_PIX_FMT_NONE;
}

/**
 * 初始化硬件解码器。
 * @param s       解码会话（输出参数，会设置 hw_pix_fmt 和 hw_device_ctx）
 * @param ctx     解码器上下文（已分配但未打开）
 * @param hw_name 硬件加速设备名称，如 "d3d11va"、"cuda"、"vaapi" 等
 * @return 0 成功，负数表示失败
 */
static int init_hw_decoder(DecodeSession& s, AVCodecContext* ctx, const char* hw_name) {
    // ① 把名字（如 "d3d11va"）翻译成 AVHWDeviceType 枚举
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name(hw_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "[HW] 不支持的设备: %s (可用 cuda/d3d11va/dxva2/qsv/vaapi/videotoolbox)\n", hw_name);
        return -1;
    }

    // ② 遍历解码器支持的硬件配置，找到与设备类型匹配的像素格式。
    //    注意：硬解出来的帧是硬件格式（如 AV_PIX_FMT_D3D11、AV_PIX_FMT_CUDA），不是普通 yuv420p。
    //    AVCodecHWConfig 结构体描述了解码器支持的硬件配置。
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(ctx->codec, i);
        if (!cfg) break;  // 没有更多配置
        // 检查此配置是否通过设备上下文方式支持，并且设备类型匹配
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
            s.hw_pix_fmt = cfg->pix_fmt;   // 记录硬件像素格式
            break;
        }
    }
    if (s.hw_pix_fmt == AV_PIX_FMT_NONE) {
        fprintf(stderr, "[HW] %s 不支持解码器 %s\n", hw_name, ctx->codec->name);
        return -1;
    }

    // ③ 告诉解码器输出这个硬件格式（通过 get_format 回调，在 open2 之前设置）
    ctx->get_format = get_hw_format;   // 设置回调函数

    // ④ 创建硬件设备上下文（相当于"打开显卡"），并挂到解码器上。
    AVBufferRef* dev_ctx = nullptr;
    // av_hwdevice_ctx_create 会打开设备，如 d3d11va 需要初始化 Direct3D 设备。
    // 参数：设备上下文指针、设备类型、设备名称（可空）、选项（可空）、flags
    if (av_hwdevice_ctx_create(&dev_ctx, type, nullptr, nullptr, 0) < 0) {
        fprintf(stderr, "[HW] 创建设备失败\n");
        return -1;
    }
    s.hw_device_ctx.reset(dev_ctx);               // RAII 持有设备上下文
    ctx->hw_device_ctx = av_buffer_ref(dev_ctx);  // 解码器再持有一个引用（引用计数+1）
    // 注意：av_buffer_ref 返回新引用，失败会返回 NULL，但这里简化错误处理。
    return 0;
}

/* ============================================================================
 * 五、解码一个包 → 若干帧（send/receive 标准模式，对应 main1.c）
 *   知识点：解码出的帧可能是"硬件帧"(在显存)或"内存帧"。
 *   硬解帧要用 av_hwframe_transfer_data 拷回内存 —— 这一步 GPU→CPU 拷贝，
 *   拷贝很贵，纯硬解播放/硬转码时尽量不拷（课堂 main4.c 专门测过这个耗时）。
 *   本工具为了统一写出 YUV，都会拷回 CPU。
 * ============================================================================ */

 /**
  * 解码一个 AVPacket，并处理所有输出的帧。
  * @param s   解码会话（包含解码器上下文、输出文件等）
  * @param pkt 输入包（可能为 NULL，用于冲刷解码器）
  * @return 0 成功，负数表示失败
  */
static int decode_packet(DecodeSession& s, AVPacket* pkt) {
    // ① 将压缩数据包送入解码器
    int ret = avcodec_send_packet(s.dec_ctx.get(), pkt);
    if (ret < 0) {
        // 注意：如果 ret == AVERROR(EAGAIN) 表示解码器缓冲区满，需要先 receive；
        // 但这里标准用法是交替 send/receive，通常不会出现 EAGAIN，除非代码逻辑错误。
        fprintf(stderr, "[DEC] send_packet 失败: %s\n", ff_err_str(ret).c_str());
        return ret;
    }

    // ② 循环接收解码后的帧（一个包可能解出 0 帧、1 帧或多帧，如 B 帧重排序后）
    while (ret >= 0) {
        FramePtr frame(av_frame_alloc());  // 分配一个空的 AVFrame
        if (!frame) return AVERROR(ENOMEM);

        ret = avcodec_receive_frame(s.dec_ctx.get(), frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // EAGAIN：当前包已消费完，需要继续 send 下一个包
            // EOF：解码器已排空，没有更多帧
            return 0;   // 正常情况，不是错误
        }
        if (ret < 0) {
            fprintf(stderr, "[DEC] receive_frame 失败: %s\n", ff_err_str(ret).c_str());
            return ret;
        }
        s.frame_count++;

        // ---- 如果帧是硬件格式，则将其拷回内存 ----
        FramePtr sw_frame;   // 用于存放拷贝后的内存帧
        const AVFrame* out = frame.get();   // out 指向最终用于输出的帧
        if (s.hw_pix_fmt != AV_PIX_FMT_NONE && frame->format == s.hw_pix_fmt) {
            // 帧是硬件格式，需要拷贝到 CPU 内存
            sw_frame.reset(av_frame_alloc());
            // av_hwframe_transfer_data 执行 GPU→CPU 拷贝，需要分配内存帧的空间。
            // 内部会自动分配相应的缓冲区。
            if (av_hwframe_transfer_data(sw_frame.get(), frame.get(), 0) < 0) {
                fprintf(stderr, "[HW] 拷贝帧到内存失败\n");
                return -1;
            }
            // 拷贝后，sw_frame 的格式会是某种软件格式（如 AV_PIX_FMT_NV12 或 YUV420P）
            out = sw_frame.get();   // 后续处理都使用这个内存帧
        }

        // ---- 打印帧信息（相当于课堂老代码的 print_video_format） ----
        // 输出帧序号、宽高、像素格式名称、PTS（显示时间戳）
        printf("[DEC] #%-4d %dx%d  fmt=%-12s  pts=%" PRId64 "\n",
            s.frame_count, out->width, out->height,
            av_get_pix_fmt_name((AVPixelFormat)out->format), out->pts);

        // ---- 需要写 YUV 时：统一用 swscale 转成 YUV420P ----
        // 课堂 main1.c 用手工 fwrite + linesize 逐行拷贝，极易写错（比如 linesize 对齐问题）。
        // 生产里用 swscale，对齐问题它全包了（还能顺便做缩放、颜色空间转换）。
        if (s.outfile) {
            // 如果 sws 尚未创建，则根据源格式和目标格式创建转换上下文
            if (!s.sws) {
                s.sws.reset(sws_getContext(
                    out->width, out->height, (AVPixelFormat)out->format,   // 源格式
                    out->width, out->height, AV_PIX_FMT_YUV420P,           // 目标格式（固定 YUV420P）
                    SWS_BILINEAR, nullptr, nullptr, nullptr                // 缩放算法（BILINEAR 速度快）
                ));
                if (!s.sws) return AVERROR(ENOMEM);
            }

            // 为输出帧分配内存（YUV420P 平面布局，每行 32 字节对齐，有利于 SIMD）
            uint8_t* dst_data[4] = { nullptr };
            int dst_linesize[4] = { 0 };
            if (av_image_alloc(dst_data, dst_linesize, out->width, out->height,
                AV_PIX_FMT_YUV420P, 32) < 0) {
                return AVERROR(ENOMEM);
            }

            // 执行转换：sws_scale 将 out 中的数据转换到 dst_data 中
            sws_scale(s.sws.get(),
                out->data, out->linesize, 0, out->height,
                dst_data, dst_linesize);

            // YUV420P 布局：Y 平面 w*h 字节，U 和 V 平面各 (w/2)*(h/2) 字节
            // 依次写出三个平面
            fwrite(dst_data[0], 1, out->width * out->height, s.outfile);
            fwrite(dst_data[1], 1, (out->width / 2) * (out->height / 2), s.outfile);
            fwrite(dst_data[2], 1, (out->width / 2) * (out->height / 2), s.outfile);

            av_freep(&dst_data[0]);   // av_image_alloc 分配的一块连续内存，释放第一个指针即可
        }
    }
    return 0;
}

/* ============================================================================
 * 六、打开解码器（共用：裸流和容器都要用）
 *   根据 codec_id 查找解码器，分配上下文，配置硬件（若需要），然后打开。
 * ============================================================================ */
static int open_decoder(DecodeSession& s) {
    // 根据 codec_id 找到解码器（如 AV_CODEC_ID_H264 → libx264 解码器）
    const AVCodec* codec = avcodec_find_decoder(s.codec_id);
    if (!codec) {
        fprintf(stderr, "找不到解码器 id=%d\n", s.codec_id);
        return -1;
    }
    // 分配解码器上下文
    s.dec_ctx.reset(avcodec_alloc_context3(codec));
    if (!s.dec_ctx) return AVERROR(ENOMEM);
    s.dec_ctx->opaque = &s;   // 存储会话指针，供回调函数使用

    // 如果指定了硬件加速，必须在 avcodec_open2 之前初始化硬件设备
    if (!s.hw_name.empty()) {
        if (init_hw_decoder(s, s.dec_ctx.get(), s.hw_name.c_str()) < 0) return -1;
    }

    // 打开解码器（此时会调用 get_format 回调来协商像素格式）
    if (avcodec_open2(s.dec_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开解码器失败\n");
        return -1;
    }
    return 0;
}

/* ============================================================================
 * 七、解码裸流（.h264/.h265）
 *   裸流没有容器，必须用 av_parser_parse2 把字节流切成"一个 NALU 一个 AVPacket"。
 *   这是摄像头/网络拉流场景的标准做法。
 * ============================================================================ */
static int decode_raw_stream(DecodeSession& s, const std::string& input) {
    FILE* in = fopen(input.c_str(), "rb");
    if (!in) { perror("open input"); return -1; }

    // 缓冲区大小 1MB，加上 FFmpeg 要求的额外 padding（防止解码器越界读）
    const size_t BUF_SIZE = 1 << 20;                 // 1 MB
    const size_t REFILL_THRESH = 4096;               // 剩余数据小于 4KB 时重新填充
    // AV_INPUT_BUFFER_PADDING_SIZE 是 FFmpeg 要求的，在缓冲区末尾预留一些空间，
    // 有些优化过的解码器会一次多读几个字节，如果不预留可能读到非法内存。
    std::vector<uint8_t> inbuf(BUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    PacketPtr pkt(av_packet_alloc());

    uint8_t* data = inbuf.data();
    size_t data_size = fread(inbuf.data(), 1, BUF_SIZE, in);   // 首次读取

    while (data_size > 0) {
        // 让解析器从 data 中切出一个完整的 NAL 单元，并填充到 pkt 中
        // av_parser_parse2 返回已消费的字节数，并将 pkt->data 和 pkt->size 设置为切出的包数据。
        int ret = av_parser_parse2(s.parser.get(), s.dec_ctx.get(),
            &pkt->data, &pkt->size,       // 输出：包数据指针和大小
            data, (int)data_size,        // 输入数据
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0); // pts/dts 等（这里不关心）
        if (ret < 0) {
            fprintf(stderr, "parse 失败\n");
            break;
        }
        data += ret;         // 跳过已消费的字节
        data_size -= ret;

        if (pkt->size) {
            // 解析器返回的包通常自带开始码，我们需要跳过开始码再读取 NAL 头。
            // 开始码长度可能是 3 或 4 字节。
            size_t off = 0;
            if (pkt->size >= 4 && pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 0 && pkt->data[3] == 1)
                off = 4;
            else if (pkt->size >= 3 && pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1)
                off = 3;
            // 打印 NAL 信息（类型、大小等）
            printf("[NAL] pkt size=%d%s\n", pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? "  (关键帧)" : "");
            print_nal_type(pkt->data + off, pkt->size - off, s.codec_id);
            // 解码这个 NAL 单元
            decode_packet(s, pkt.get());
        }

        // 如果剩余数据不足阈值，则把未消费的数据移到缓冲区开头，然后从文件中补读更多数据
        // 这样可以保证解析器有足够的数据继续切包
        if (data_size < REFILL_THRESH) {
            memmove(inbuf.data(), data, data_size);   // 将剩余数据搬到开头
            data = inbuf.data();                      // 更新 data 指针
            size_t n = fread(inbuf.data() + data_size, 1, BUF_SIZE - data_size, in);
            if (n > 0) data_size += n;
        }
    }
    fclose(in);

    // ---- 冲刷解码器：传空包，让解码器吐出内部缓存的最后一帧 ----
    // 清空 pkt 内容，设 data=NULL, size=0
    pkt->data = nullptr;
    pkt->size = 0;
    decode_packet(s, pkt.get());
    return 0;
}

/* ============================================================================
 * 八、解码容器（mp4/ts/flv...）
 *   容器用 av_read_frame 直接拿包，用 codecpar 初始化解码器参数。
 *   （老代码里 stream->codec 已被废弃，这是现代写法，使用 codecpar）。
 * ============================================================================ */
static int decode_container(DecodeSession& s, const std::string& input) {
    // ① 打开输入文件并读取头部信息
    FormatCtxPtr fmt_ctx;
    {
        AVFormatContext* fc = nullptr;
        // avformat_open_input 会探测文件格式并初始化 AVFormatContext
        if (avformat_open_input(&fc, input.c_str(), nullptr, nullptr) < 0) {
            fprintf(stderr, "打不开 %s（不是容器？试试裸流模式）\n", input.c_str());
            return -1;
        }
        fmt_ctx.reset(fc);
    }
    // ② 查找流信息（如读取包来获取编解码器参数、时长等）
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        fprintf(stderr, "找不到流信息\n");
        return -1;
    }

    // ③ 找到最佳的视频流（av_find_best_stream 自动选择）
    const AVCodec* codec = nullptr;
    int video_index = av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_index < 0) {
        fprintf(stderr, "没有视频流\n");
        return -1;
    }
    AVStream* st = fmt_ctx->streams[video_index];

    // ④ 初始化解码器上下文
    s.codec_id = st->codecpar->codec_id;
    s.dec_ctx.reset(avcodec_alloc_context3(codec));
    s.dec_ctx->opaque = &s;
    // 使用容器中的编解码器参数（codecpar）填充解码器上下文
    if (avcodec_parameters_to_context(s.dec_ctx.get(), st->codecpar) < 0) {
        fprintf(stderr, "参数初始化失败\n");
        return -1;
    }
    // 若需要硬件加速，初始化硬件
    if (!s.hw_name.empty() && init_hw_decoder(s, s.dec_ctx.get(), s.hw_name.c_str()) < 0) return -1;
    // 打开解码器
    if (avcodec_open2(s.dec_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开解码器失败\n");
        return -1;
    }

    // ⑤ 循环读取包，送给解码器
    PacketPtr pkt(av_packet_alloc());
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        // 只处理视频流，丢弃音频或其他流
        if (pkt->stream_index == video_index) {
            decode_packet(s, pkt.get());
        }
        av_packet_unref(pkt.get());   // 释放包内部引用（容器包的数据可能来自缓冲区）
    }
    // 冲刷解码器
    pkt->data = nullptr;
    pkt->size = 0;
    decode_packet(s, pkt.get());
    return 0;
}

/* ============================================================================
 * 九、编码 YUV → H.264/H.265（软编/硬编）
 *   对应 main2.c / main3.c 的知识点：
 *   - time_base 建议统一设 1/fps，这样每帧 pts 递增 1 即可
 *     （老代码 libx264 用 1/1000、pts+40，nvenc 又用 1/fps —— 两个体系，
 *       现在统一 1/fps 最不容易错，也方便理解）
 *   - preset/tune/profile 是"编码器私有参数"，必须走 priv_data 通道，
 *     直接设 AVCodecContext 字段（如 preset）对大多数编码器不生效。
 *   - NVENC 的 preset 是 p1~p7（不是 x264 的 medium/veryslow！），
 *     且参数名也不同，必须使用正确的选项名（"preset", "tuning", "rc" 等）。
 * ============================================================================ */
static int run_encode(const std::string& in_yuv, const std::string& out_file,
    const std::string& codec_name, const std::string& preset,
    int width, int height, int fps) {
    // ① 根据名称查找编码器（如 "libx264"、"h264_nvenc"）
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if (!codec) {
        fprintf(stderr, "找不到编码器 %s\n", codec_name.c_str());
        return -1;
    }
    CodecCtxPtr enc_ctx(avcodec_alloc_context3(codec));

    // ---- 公共参数（所有编码器通用）----
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->time_base = AVRational{ 1, fps };   // 时间基为 1/fps 秒，即每帧间隔 1/fps 秒
    enc_ctx->framerate = AVRational{ fps, 1 };   // 帧率
    enc_ctx->gop_size = fps * 2;              // GOP 大小：每 2 秒一个 I 帧（关键帧间隔）
    enc_ctx->max_b_frames = 2;                   // 两个参考帧之间最多 2 个 B 帧
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;   // 输入 YUV 格式（编码器接受 YUV420P）
    enc_ctx->bit_rate = 3'000'000;            // 目标码率 3 Mbps（注意单位是 bps，不是 kbps）

    // ---- 编码器私有参数：走 priv_data 通道 ----
    bool is_nvenc = (codec_name.find("nvenc") != std::string::npos);
    if (is_nvenc) {
        // NVIDIA 硬编：preset 取值 p1~p7（p1 最快画质最差，p7 最慢画质最好）
        // 额外参数：tuning（hq 高质量）、rc（vbr 可变码率）、cq（恒定质量，类似 CRF）
        av_opt_set(enc_ctx->priv_data, "preset", preset.empty() ? "p5" : preset.c_str(), 0);
        av_opt_set(enc_ctx->priv_data, "tuning", "hq", 0);
        av_opt_set(enc_ctx->priv_data, "rc", "vbr", 0);
        av_opt_set(enc_ctx->priv_data, "cq", "23", 0);   // 23 是常见质量值，越小质量越好
    }
    else {
        // x264 / x265 软编：preset 可以是 ultrafast/medium/veryslow 等
        av_opt_set(enc_ctx->priv_data, "preset", preset.empty() ? "medium" : preset.c_str(), 0);
        // 注意：tune=zerolatency 适合直播场景，但会降低压缩效率，这里不设置
        if (codec->id == AV_CODEC_ID_H265) {
            // libx265 专属参数通过 x265-params 传递，格式为冒号分隔的 key=value 列表
            // 这里强制 open-gop=0 避免使用 CRA 帧，使得所有 IDR 都是真正的随机访问点，
            // 提高兼容性（某些播放器不支持 CRA）。
            av_opt_set(enc_ctx->priv_data, "x265-params",
                "keyint=50:min-keyint=25:bframes=2:open-gop=0", 0);
        }
    }

    // 打开编码器
    if (avcodec_open2(enc_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开编码器失败\n");
        return -1;
    }

    // 打开输入 YUV 文件和输出码流文件
    FILE* in = fopen(in_yuv.c_str(), "rb");
    FILE* out = fopen(out_file.c_str(), "wb");
    if (!in || !out) {
        perror("open file");
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }

    // ---- 准备帧缓冲区 ----
    FramePtr frame(av_frame_alloc());
    frame->format = enc_ctx->pix_fmt;
    frame->width = enc_ctx->width;
    frame->height = enc_ctx->height;
    // 为帧分配数据缓冲区（内部会分配 data[0]~data[3] 对应的平面内存）
    if (av_frame_get_buffer(frame.get(), 32) < 0) {   // 32 字节对齐
        fprintf(stderr, "分配帧内存失败\n");
        return -1;
    }
    int frame_bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, 1);
    std::vector<uint8_t> yuv_buf(frame_bytes);   // 用于读取 YUV 文件的一帧数据
    PacketPtr pkt(av_packet_alloc());

    // 记录开始时间（毫秒）
    int64_t begin = av_gettime_relative() / 1000;
    int64_t pts = 0;

    // 循环读取每一帧 YUV 数据
    for (;;) {
        size_t n = fread(yuv_buf.data(), 1, frame_bytes, in);
        if (n <= 0) break;   // 文件结束

        // av_frame_make_writable：确保帧数据可写（可能因为参考帧等原因被只读引用）
        if (av_frame_make_writable(frame.get()) < 0) break;

        // 将读取的 YUV 数据填充到 frame 的 data 和 linesize 中
        if (av_image_fill_arrays(frame->data, frame->linesize, yuv_buf.data(),
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, 1) < 0) break;
        frame->pts = pts++;   // 每帧 pts 递增 1（由于 time_base=1/fps，所以实际时间 = pts/fps 秒）

        // ---- send/receive 编码 ----
        if (avcodec_send_frame(enc_ctx.get(), frame.get()) < 0) {
            fprintf(stderr, "send_frame 失败\n");
            break;
        }
        // 循环接收编码后的包（一个帧可能产生多个包，如切片）
        while (avcodec_receive_packet(enc_ctx.get(), pkt.get()) >= 0) {
            printf("[ENC] pts=%" PRId64 " dts=%" PRId64 " size=%d\n", pkt->pts, pkt->dts, pkt->size);
            fwrite(pkt->data, 1, pkt->size, out);
            av_packet_unref(pkt.get());   // 释放包引用
        }
    }

    // ---- 冲刷编码器：传入 NULL 帧，让编码器输出所有缓存的帧（如 B 帧延迟） ----
    avcodec_send_frame(enc_ctx.get(), nullptr);
    while (avcodec_receive_packet(enc_ctx.get(), pkt.get()) >= 0) {
        fwrite(pkt->data, 1, pkt->size, out);
        av_packet_unref(pkt.get());
    }

    int64_t end = av_gettime_relative() / 1000;
    printf("[ENC] %s(preset=%s) 编码耗时 %lld ms\n", codec_name.c_str(), preset.c_str(), end - begin);

    fclose(in);
    fclose(out);
    return 0;
}

/* ============================================================================
 * 十、主入口：解析命令行参数
 *   支持 decode 和 encode 两种子命令，解析参数并调用相应函数。
 * ============================================================================ */
static void print_usage() {
    printf(
        "用法:\n"
        "  video_tool decode <input> [--hw 设备] [--out out.yuv]\n"
        "      input 可以是 .h264/.h265 裸流，也可以是 mp4/ts 等容器\n"
        "      --hw 可选: d3d11va / dxva2 / qsv / cuda / vaapi / videotoolbox\n"
        "  video_tool encode <in.yuv> <out> --codec 编码器 [--preset 档位]\n"
        "      [--width W --height H --fps F]\n"
        "      编码器: libx264 / libx265 / h264_nvenc / hevc_nvenc\n"
    );
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    std::string cmd = argv[1];

    if (cmd == "decode") {
        std::string input, hw_name, out_yuv;
        // 简单解析参数：支持 --hw 和 --out
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--hw" && i + 1 < argc) hw_name = argv[++i];
            else if (a == "--out" && i + 1 < argc) out_yuv = argv[++i];
            else if (!input.empty() && i == argc) {} // 忽略多余
            else if (a[0] != '-') input = a;          // 非选项参数视为输入文件名
        }
        if (input.empty()) {
            print_usage();
            return 0;
        }

        DecodeSession s;
        s.hw_name = hw_name;
        FILE* out = out_yuv.empty() ? nullptr : fopen(out_yuv.c_str(), "wb");
        s.outfile = out;

        // 判断是裸流还是容器：简单的扩展名判断（生产环境可用 av_probe_input_buffer 探测）
        bool is_raw = false;
        if (input.size() >= 4) {
            std::string tail4 = input.substr(input.size() - 4);
            std::string tail5 = input.size() >= 5 ? input.substr(input.size() - 5) : "";
            is_raw = (tail4 == ".264" || tail4 == ".265" || tail5 == ".h264" || tail5 == ".h265");
        }
        int ret;
        if (is_raw) {
            // 裸流：根据文件名判断编码格式（简单策略）
            s.codec_id = (input.find("264") != std::string::npos &&
                input.find("265") == std::string::npos) ? AV_CODEC_ID_H264 : AV_CODEC_ID_H265;
            s.parser.reset(av_parser_init(s.codec_id));   // 为裸流创建解析器
            if (!s.parser) {
                fprintf(stderr, "找不到解析器\n");
                return -1;
            }
            if (open_decoder(s) < 0) return -1;
            ret = decode_raw_stream(s, input);
        }
        else {
            // 容器模式
            ret = decode_container(s, input);
        }
        if (out) fclose(out);
        printf("总共解码 %d 帧\n", s.frame_count);
        return ret;
    }

    if (cmd == "encode") {
        std::string in_yuv, out_file, codec_name, preset;
        int width = 1280, height = 720, fps = 25;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--codec" && i + 1 < argc) codec_name = argv[++i];
            else if (a == "--preset" && i + 1 < argc) preset = argv[++i];
            else if (a == "--width" && i + 1 < argc) width = atoi(argv[++i]);
            else if (a == "--height" && i + 1 < argc) height = atoi(argv[++i]);
            else if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]);
            else if (in_yuv.empty()) in_yuv = a;
            else if (out_file.empty()) out_file = a;
        }
        if (in_yuv.empty() || out_file.empty() || codec_name.empty()) {
            print_usage();
            return 0;
        }
        return run_encode(in_yuv, out_file, codec_name, preset, width, height, fps);
    }

    print_usage();
    return 0;
}