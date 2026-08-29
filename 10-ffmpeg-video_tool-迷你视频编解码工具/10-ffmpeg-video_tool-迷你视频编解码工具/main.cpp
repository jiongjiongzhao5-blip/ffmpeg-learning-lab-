/**
 * ============================================================================
 * video_tool.cpp —— 迷你视频工具（C++17 + FFmpeg 8/9）
 *
 * 本工具整合了课堂 4 个视频处理核心知识点：
 *   ① NAL 单元类型分析（PDF-1 的基础）
 *   ② 软解与硬解（d3d11va、qsv、cuda、vaapi 等）
 *   ③ 使用 swscale 将解码帧转为 YUV420P 并写出（工程化做法，避免手工处理 linesize）
 *   ④ 软编（libx264/libx265）与硬编（h264_nvenc/hevc_nvenc）
 *
 * 编译（Linux）：
 *   g++ -std=c++17 video_tool.cpp -o video_tool \
 *       $(pkg-config --cflags --libs libavcodec libavutil libswscale libavformat)
 *
 * 用法示例：
 *   ./video_tool decode in.h265                       # 软解裸流并打印 NAL 类型
 *   ./video_tool decode in.mp4 --out out.yuv          # 解容器并输出 YUV
 *   ./video_tool decode in.h264 --hw d3d11va          # Windows 下使用 D3D11VA 硬解
 *   ./video_tool encode in.yuv out.h265 --codec libx265 --preset medium
 *   ./video_tool encode in.yuv out.h264 --codec h264_nvenc --preset p5
 * ============================================================================
 *
 * 核心设计理念：
 *   - 资源管理：所有 FFmpeg 对象均使用 std::unique_ptr + 自定义删除器，实现 RAII，
 *     确保异常安全，杜绝内存泄漏。
 *   - 解码流程：严格遵循 avcodec_send_packet() / avcodec_receive_frame() 的现代 API，
 *     同时支持裸流（需解析器）和容器（av_read_frame）。
 *   - 硬解支持：通过 get_format 回调协商硬件像素格式，用 av_hwdevice_ctx_create
 *     创建设备，再通过 av_hwframe_transfer_data 将显存帧拷回内存（代价较高，但便于后续处理）。
 *   - 颜色空间转换：统一使用 swscale 转换为 YUV420P，消除 linesize 对齐带来的手工错误。
 *   - 编码器配置：将“preset/tune/rc”等私有参数通过 av_opt_set 写入 priv_data，
 *     区分软编（x264/x265）与硬编（NVENC）的不同参数体系。
 * ============================================================================ */

#define _CRT_SECURE_NO_WARNINGS   // 防止 Visual Studio 对 fopen 等函数产生安全警告

// ---------- 标准 C++ 头文件 ----------
#include <cstdio>      // fopen, fread, fwrite, printf, perror
#include <cstring>     // memmove, strcmp
#include <cstdlib>     // atoi
#include <cinttypes>   // PRId64 宏（用于 printf 打印 int64_t 类型的 pts/dts）
#include <string>      // std::string
#include <memory>      // std::unique_ptr, std::shared_ptr
#include <vector>      // std::vector

// ---------- FFmpeg 头文件（必须放在 extern "C" 块中，防止 C++ 名字修饰）----------
extern "C" {
#include <libavcodec/avcodec.h>      // 编解码核心：AVCodec, AVCodecContext, AVPacket, avcodec_*
#include <libavutil/frame.h>         // AVFrame 及 av_frame_alloc/free
#include <libavutil/imgutils.h>      // 图像辅助：av_image_alloc, av_image_fill_arrays, av_image_get_buffer_size
#include <libavutil/opt.h>           // av_opt_set：用于设置编码器私有选项
#include <libavutil/hwcontext.h>     // 硬件加速上下文：av_hwdevice_*, av_hwframe_transfer_data
#include <libavutil/time.h>          // av_gettime_relative（高精度计时）
#include <libavutil/pixdesc.h>       // av_get_pix_fmt_name（像素格式名称）
#include <libavformat/avformat.h>    // 容器格式：AVFormatContext, avformat_*, av_read_frame
#include <libswscale/swscale.h>      // 图像缩放/转换：SwsContext, sws_*
}

/* ============================================================================
 * 一、RAII 包装器 —— 让 FFmpeg 的 C 对象自动释放
 *   FFmpeg 的每个对象都有专用的释放函数（如 avcodec_free_context, av_frame_free 等）。
 *   在 C++ 中，最安全的做法是使用 std::unique_ptr 并传入自定义删除器。
 *   这样，当 unique_ptr 生命周期结束（离开作用域或被 reset）时，删除器自动调用释放函数。
 *   这是 C++ 资源管理（RAII）的标准模式，极大地降低了内存泄漏风险。
 * ============================================================================ */

// 删除器：用于 AVCodecContext* —— 调用 avcodec_free_context，它会释放内部所有附加数据。
struct AvCodecContextDeleter {
    void operator()(AVCodecContext* p) const {
        avcodec_free_context(&p);   // 传入指针的地址，函数内部会将 p 置为 NULL
    }
};

// 删除器：用于 AVFrame* —— av_frame_free 会释放帧数据（如果 refcount 为 0）
struct AvFrameDeleter {
    void operator()(AVFrame* p) const {
        av_frame_free(&p);
    }
};

// 删除器：用于 AVPacket* —— av_packet_free 会释放内部缓冲区（若 refcount 为 0）
struct AvPacketDeleter {
    void operator()(AVPacket* p) const {
        av_packet_free(&p);
    }
};

// 删除器：用于 AVBufferRef* —— av_buffer_unref 减少引用计数，计数归零则释放底层缓冲区
struct AvBufferRefDeleter {
    void operator()(AVBufferRef* p) const {
        av_buffer_unref(&p);
    }
};

// 删除器：用于 AVCodecParserContext* —— av_parser_close 关闭解析器
struct AvParserDeleter {
    void operator()(AVCodecParserContext* p) const {
        av_parser_close(p);
    }
};

// 删除器：用于 SwsContext* —— sws_freeContext 释放转换上下文
struct SwsContextDeleter {
    void operator()(SwsContext* p) const {
        sws_freeContext(p);
    }
};

// 删除器：用于 AVFormatContext* —— avformat_close_input 关闭输入并释放上下文
struct AvFormatCtxDeleter {
    void operator()(AVFormatContext* p) const {
        avformat_close_input(&p);
    }
};

// 为方便书写，定义智能指针类型别名
using CodecCtxPtr   = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using FramePtr      = std::unique_ptr<AVFrame, AvFrameDeleter>;
using PacketPtr     = std::unique_ptr<AVPacket, AvPacketDeleter>;
using BufferRefPtr  = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;
using ParserPtr     = std::unique_ptr<AVCodecParserContext, AvParserDeleter>;
using SwsPtr        = std::unique_ptr<SwsContext, SwsContextDeleter>;
using FormatCtxPtr  = std::unique_ptr<AVFormatContext, AvFormatCtxDeleter>;

/**
 * 工具函数：将 FFmpeg 错误码（通常为负值）转换为人类可读的字符串。
 * 生产环境中遇到错误，打印此字符串能快速定位问题，而不是仅看到一个数字。
 * 原理：av_strerror 内部查找错误码对应的描述文本。
 */
static std::string ff_err_str(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };  // FFmpeg 定义的最大错误字符串长度（通常 64 或更大）
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

/* ============================================================================
 * 二、NAL 单元类型分析（对应 PDF-1 的核心知识）
 *   裸流（Annex-B）的结构：开始码（0x000001 或 0x00000001）+ NAL 单元数据 + 下一个开始码...
 *   NAL 头部字节的位定义：
 *     - H.264: 第一个字节的低 5 位 (bits 0~4) 为 nal_unit_type。
 *     - H.265: 第一个字节的 bit 1~6 (即 (byte & 0x7E) >> 1) 为 nal_unit_type。
 *   实际用途：判断帧类型（是否为 IDR 关键帧），验证编码器 GOP 设置是否生效，
 *   或者在 RTP 接收时判断是否可随机访问。
 * ============================================================================ */

/**
 * H.264 NAL 类型名称表（仅列出常用类型，完整定义见标准 Table 7-1）
 */
static const char* nal_name_h264(int t) {
    switch (t) {
    case 1: return "P";          // 非参考 P 片（Non-IDR picture）
    case 2: return "B";          // B 片
    case 5: return "IDR";        // 瞬时解码刷新（关键帧，随机访问点）
    case 6: return "SEI";        // 补充增强信息（如帧打包信息、用户注册数据等）
    case 7: return "SPS";        // 序列参数集（含分辨率、档次、级别等）
    case 8: return "PPS";        // 图像参数集（含熵编码模式、片组等）
    case 9: return "AUD";        // 访问单元分隔符（用于分割不同访问单元）
    default: return "OTHER";
    }
}

/**
 * H.265 (HEVC) NAL 类型名称表（部分关键类型，完整见标准 Table 7-1）
 */
static const char* nal_name_h265(int t) {
    switch (t) {
    case 0:  return "TRAIL_N";   // 非参考帧的尾部片（Trailing, non-reference）
    case 1:  return "TRAIL_R";   // 参考帧的尾部片（Trailing, reference）
    case 19: return "IDR_W_DLP"; // IDR 帧，允许存在可解码的前导图片（Decodable Leading Pictures）
    case 20: return "IDR_N_LP";  // IDR 帧，无前导图片（最常用）
    case 21: return "CRA(OpenGOP)"; // 清洁随机访问（开放 GOP 起点，可参考之前帧）
    case 32: return "VPS";       // 视频参数集（定义时间层、操作点等）
    case 33: return "SPS";       // 序列参数集
    case 34: return "PPS";       // 图像参数集
    case 39: return "SEI";       // 补充增强信息
    case 40: return "AUD";       // 访问单元分隔符
    default: return "OTHER";
    }
}

/**
 * 打印一个 NAL 单元的类型、大小和头部字节。
 * @param data     指向 NAL 单元数据（不含开始码）的指针
 * @param size     该 NAL 单元的总字节数（含头部）
 * @param codec_id 用于区分 H.264 / H.265
 *
 * 注意：此函数仅在裸流解析时调用，容器模式下不调用，因为容器已封装好帧信息。
 */
static void print_nal_type(const uint8_t* data, size_t size, enum AVCodecID codec_id) {
    if (size < 1) return;  // 不可能发生，但防御性检查
    int nal_type;
    if (codec_id == AV_CODEC_ID_H264) {
        nal_type = data[0] & 0x1F;          // 取低 5 位
    } else { // 假定为 H.265
        nal_type = (data[0] & 0x7E) >> 1;   // 取中间 6 位（bit1~bit6）
    }
    const char* name = (codec_id == AV_CODEC_ID_H264) ? nal_name_h264(nal_type)
                                                      : nal_name_h265(nal_type);
    printf("      nal header=0x%02X  type=%-2d  size=%-6zu  (%s)\n",
           data[0], nal_type, size, name);
}

/**
 * 判断当前位置是否为一个有效的 Annex-B 开始码（3 字节或 4 字节）。
 * @param p         指向内存的指针
 * @param remaining 剩余字节数，防止越界
 * @return true 表示找到一个开始码
 *
 * 注意：开始码必须为 0x000001 或 0x00000001，不会出现其他变体。
 */
static bool is_start_code(const uint8_t* p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

/* ============================================================================
 * 三、解码会话结构体（DecodeSession）
 *   包含解码所需的所有状态和上下文。所有资源均以智能指针管理，无需手动释放。
 *   hw_name 非空时表示启用硬件加速，否则为纯软件解码。
 *   outfile 非空时会将解码帧转换为 YUV420P 并写入该文件。
 * ============================================================================ */
struct DecodeSession {
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;   // 当前解码器 ID（如 AV_CODEC_ID_H264）
    std::string hw_name;                          // 硬件加速设备名称，如 "d3d11va"，空表示软解

    ParserPtr  parser;          // 裸流解析器（仅当处理裸流时需要，容器模式不需要）
    CodecCtxPtr dec_ctx;        // 解码器上下文（包含解码器实例、参数、状态等）
    BufferRefPtr hw_device_ctx; // 硬件设备上下文（代表 GPU 设备，例如 Direct3D 设备）
    enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE; // 解码器输出的硬件像素格式（如 AV_PIX_FMT_D3D11）

    FILE* outfile = nullptr;    // 若需要写出 YUV，则此文件指针非空
    SwsPtr sws;                 // 图像转换上下文（将任意像素格式转为 YUV420P），可复用
    int frame_count = 0;        // 统计成功解码的帧数
};

/* ============================================================================
 * 四、硬件解码初始化
 *   本函数完成硬件设备创建和格式协商，必须在 avcodec_open2 之前调用。
 *   步骤：
 *     1. 通过名字查找 AVHWDeviceType（如 "d3d11va" -> AV_HWDEVICE_TYPE_D3D11VA）
 *     2. 遍历解码器支持的硬件配置列表（avcodec_get_hw_config），找到与设备类型匹配的像素格式。
 *     3. 设置解码器的 get_format 回调，用于在打开时向解码器告知我们期望的硬件格式。
 *     4. 创建实际的硬件设备上下文（av_hwdevice_ctx_create），并挂到解码器上下文中。
 * ============================================================================ */

/**
 * get_format 回调函数：解码器在 avcodec_open2 期间会调用此回调，
 * 传入它自身支持的像素格式列表（pix_fmts），我们需要从中选出我们想要的硬件格式。
 * 若找到，返回该格式；否则返回 AV_PIX_FMT_NONE，导致解码器打开失败。
 *
 * 为什么需要回调？因为不同的硬件平台（D3D11, VAAPI, CUDA）对应的像素格式不同，
 * 解码器自身不知道用户想用哪种，必须由用户通过回调告知。
 *
 * ctx->opaque 指向 DecodeSession，我们在 open_decoder 中预先设置。
 */
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    auto* self = static_cast<DecodeSession*>(ctx->opaque);   // 取回会话指针
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == self->hw_pix_fmt) {
            return *p;   // 匹配到我们想要的硬件格式
        }
    }
    // 若列表中没有，返回 AV_PIX_FMT_NONE 会触发 avcodec_open2 失败
    return AV_PIX_FMT_NONE;
}

/**
 * 初始化硬件解码器。
 * @param s       解码会话（输出参数，hw_pix_fmt 和 hw_device_ctx 被设置）
 * @param ctx     解码器上下文（已分配，但尚未打开）
 * @param hw_name 设备名称字符串
 * @return 0 成功，负数失败
 */
static int init_hw_decoder(DecodeSession& s, AVCodecContext* ctx, const char* hw_name) {
    // ① 将名称转换为 AVHWDeviceType 枚举
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name(hw_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "[HW] 不支持的设备: %s (可用 cuda/d3d11va/dxva2/qsv/vaapi/videotoolbox)\n", hw_name);
        return -1;
    }

    // ② 获取解码器支持的硬件配置（每个配置对应一种像素格式和设备类型）
    //    循环遍历，找到与 type 匹配且支持 HW_DEVICE_CTX 方法的配置。
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(ctx->codec, i);
        if (!cfg) break;  // 没有更多配置
        // 检查该配置是否允许通过设备上下文方式使用，且设备类型匹配
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
            s.hw_pix_fmt = cfg->pix_fmt;   // 记录硬件像素格式，如 AV_PIX_FMT_D3D11
            break;
        }
    }
    if (s.hw_pix_fmt == AV_PIX_FMT_NONE) {
        fprintf(stderr, "[HW] %s 不支持解码器 %s\n", hw_name, ctx->codec->name);
        return -1;
    }

    // ③ 设置回调函数，解码器打开时会调用它来协商输出格式
    ctx->get_format = get_hw_format;

    // ④ 创建硬件设备上下文（这一步会真正初始化 GPU 资源）
    AVBufferRef* dev_ctx = nullptr;
    // av_hwdevice_ctx_create 参数：设备上下文指针、设备类型、设备名称（通常为 NULL）、选项、标志
    if (av_hwdevice_ctx_create(&dev_ctx, type, nullptr, nullptr, 0) < 0) {
        fprintf(stderr, "[HW] 创建设备失败\n");
        return -1;
    }
    s.hw_device_ctx.reset(dev_ctx);               // 使用 RAII 持有设备引用
    ctx->hw_device_ctx = av_buffer_ref(dev_ctx);  // 解码器也持有一个引用（引用计数+1）
    // 注意：av_buffer_ref 可能返回 NULL（内存不足），此处简化错误处理，生产中应检查。
    return 0;
}

/* ============================================================================
 * 五、解码一个包（decode_packet）
 *   标准 send/receive 流程：
 *     1. 调用 avcodec_send_packet() 送压缩数据。
 *     2. 循环调用 avcodec_receive_frame() 取解码后的帧，直到返回 EAGAIN 或 EOF。
 *     3. 对硬件帧，用 av_hwframe_transfer_data 将其从 GPU 拷贝到 CPU 内存（否则无法使用 swscale）。
 *     4. 若指定了 outfile，则使用 swscale 将帧转为 YUV420P 并写入文件。
 *   注意：一个压缩包可能解出 0 帧、1 帧或多帧（如 B 帧重排序），故需循环接收。
 * ============================================================================ */

static int decode_packet(DecodeSession& s, AVPacket* pkt) {
    // ① 将压缩数据包送入解码器
    int ret = avcodec_send_packet(s.dec_ctx.get(), pkt);
    if (ret < 0) {
        // 正常情况下不应该发生 EAGAIN，因为我们会先收完所有帧再送下一包。
        // 但若发生，可打印错误。
        fprintf(stderr, "[DEC] send_packet 失败: %s\n", ff_err_str(ret).c_str());
        return ret;
    }

    // ② 循环接收帧
    while (ret >= 0) {
        FramePtr frame(av_frame_alloc());   // 分配空帧
        if (!frame) return AVERROR(ENOMEM);

        ret = avcodec_receive_frame(s.dec_ctx.get(), frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // EAGAIN：解码器需要更多数据才能输出下一帧，正常返回。
            // EOF：已排空所有缓存帧，正常返回。
            return 0;
        }
        if (ret < 0) {
            fprintf(stderr, "[DEC] receive_frame 失败: %s\n", ff_err_str(ret).c_str());
            return ret;
        }
        s.frame_count++;

        // ---- 若帧是硬件格式，将其拷贝到 CPU 内存 ----
        FramePtr sw_frame;   // 存放拷贝后的内存帧
        const AVFrame* out = frame.get();   // out 最终指向实际使用的帧
        if (s.hw_pix_fmt != AV_PIX_FMT_NONE && frame->format == s.hw_pix_fmt) {
            // 帧来自硬件解码器，format 为硬件像素格式（如 AV_PIX_FMT_D3D11）
            sw_frame.reset(av_frame_alloc());
            // av_hwframe_transfer_data 执行 GPU→CPU 拷贝，会自动分配内存帧所需的缓冲区。
            // 注意：此操作会阻塞并消耗大量 PCIe 带宽，不适合实时低延迟场景。
            if (av_hwframe_transfer_data(sw_frame.get(), frame.get(), 0) < 0) {
                fprintf(stderr, "[HW] 拷贝帧到内存失败\n");
                return -1;
            }
            // 拷贝后，sw_frame 的 format 会变为某种软件格式（如 NV12 或 YUV420P）
            out = sw_frame.get();
        }

        // ---- 打印帧信息（帧序号、尺寸、像素格式名称、PTS） ----
        // 注意：PTS 可能为 AV_NOPTS_VALUE（表示未设置），此时打印一个很大的负数。
        printf("[DEC] #%-4d %dx%d  fmt=%-12s  pts=%" PRId64 "\n",
               s.frame_count, out->width, out->height,
               av_get_pix_fmt_name((AVPixelFormat)out->format), out->pts);

        // ---- 若需要写出 YUV，则使用 swscale 转换为 YUV420P ----
        if (s.outfile) {
            // 如果 sws 上下文尚未创建，则根据源格式和目标格式创建
            if (!s.sws) {
                s.sws.reset(sws_getContext(
                    out->width, out->height, (AVPixelFormat)out->format,   // 源图像参数
                    out->width, out->height, AV_PIX_FMT_YUV420P,           // 目标参数（固定 YUV420P）
                    SWS_BILINEAR, nullptr, nullptr, nullptr                // 缩放算法，BILINEAR 速度较快
                ));
                if (!s.sws) return AVERROR(ENOMEM);
            }

            // 为目标帧分配内存（YUV420P 是三平面格式，每行按 32 字节对齐，提升 SIMD 效率）
            uint8_t* dst_data[4] = { nullptr };
            int dst_linesize[4] = { 0 };
            // av_image_alloc 会为所有平面分配连续的内存块，并设置各个平面的 data 指针和 linesize。
            if (av_image_alloc(dst_data, dst_linesize, out->width, out->height,
                               AV_PIX_FMT_YUV420P, 32) < 0) {
                return AVERROR(ENOMEM);
            }

            // 执行颜色空间转换和缩放（如果有必要）
            // sws_scale 会自动处理源 linesize 可能因对齐而大于实际宽度的情况。
            sws_scale(s.sws.get(),
                      out->data, out->linesize, 0, out->height,
                      dst_data, dst_linesize);

            // YUV420P 布局：Y 平面 w*h 字节，U 平面 (w/2)*(h/2)，V 平面 (w/2)*(h/2)
            // 依次写入文件
            fwrite(dst_data[0], 1, out->width * out->height, s.outfile);
            fwrite(dst_data[1], 1, (out->width / 2) * (out->height / 2), s.outfile);
            fwrite(dst_data[2], 1, (out->width / 2) * (out->height / 2), s.outfile);

            // 释放分配的内存（av_image_alloc 分配的是连续内存，释放第一个指针即可）
            av_freep(&dst_data[0]);
        }
    }
    return 0;
}

/* ============================================================================
 * 六、打开解码器（open_decoder）
 *   根据 codec_id 查找解码器，分配上下文，配置硬件（若需要），然后打开。
 *   此阶段不处理数据，仅准备解码环境。
 * ============================================================================ */
static int open_decoder(DecodeSession& s) {
    // ① 通过 ID 查找解码器（例如 AV_CODEC_ID_H264 对应 libx264 解码器）
    const AVCodec* codec = avcodec_find_decoder(s.codec_id);
    if (!codec) {
        fprintf(stderr, "找不到解码器 id=%d\n", s.codec_id);
        return -1;
    }
    // ② 分配解码器上下文（此时上下文尚未关联具体参数）
    s.dec_ctx.reset(avcodec_alloc_context3(codec));
    if (!s.dec_ctx) return AVERROR(ENOMEM);
    s.dec_ctx->opaque = &s;   // 存储会话指针，供回调函数使用

    // ③ 如果启用了硬件加速，则初始化硬件设备（必须在 open2 之前）
    if (!s.hw_name.empty()) {
        if (init_hw_decoder(s, s.dec_ctx.get(), s.hw_name.c_str()) < 0) return -1;
    }

    // ④ 打开解码器（此时会调用 get_format 回调进行格式协商）
    if (avcodec_open2(s.dec_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开解码器失败\n");
        return -1;
    }
    return 0;
}

/* ============================================================================
 * 七、解码裸流（.h264/.h265）
 *   裸流没有容器封装，必须通过解析器（AVCodecParserContext）从字节流中分割出 NAL 单元。
 *   解析器内部会识别开始码，并返回完整的 NAL 数据。
 *   本函数采用“环形缓冲区”策略：每次从文件读取 1MB，解析器消费数据，剩余不足阈值时
 *   将剩余数据移到缓冲区头部再补充新数据，避免数据丢失。
 * ============================================================================ */
static int decode_raw_stream(DecodeSession& s, const std::string& input) {
    FILE* in = fopen(input.c_str(), "rb");
    if (!in) { perror("open input"); return -1; }

    // 缓冲区大小 1MB，额外加上 FFmpeg 要求的填充区（AV_INPUT_BUFFER_PADDING_SIZE）
    // 原因：某些解码器在优化时会一次读取超出数据末尾的几个字节（如 SIMD 指令），
    // 如果没有填充区可能访问非法内存。FFmpeg 要求至少预留 AV_INPUT_BUFFER_PADDING_SIZE 字节。
    const size_t BUF_SIZE = 1 << 20;                 // 1 MB
    const size_t REFILL_THRESH = 4096;               // 当剩余数据少于 4KB 时重新填充
    std::vector<uint8_t> inbuf(BUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    PacketPtr pkt(av_packet_alloc());

    uint8_t* data = inbuf.data();
    size_t data_size = fread(inbuf.data(), 1, BUF_SIZE, in);   // 首次读取

    while (data_size > 0) {
        // 解析器从 data 中切出一个完整的 NAL 单元，填充到 pkt 中。
        // av_parser_parse2 返回已消费的字节数（consumed），同时设置 pkt->data 和 pkt->size。
        int ret = av_parser_parse2(s.parser.get(), s.dec_ctx.get(),
                                   &pkt->data, &pkt->size,       // 输出包数据
                                   data, (int)data_size,        // 输入数据
                                   AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0); // pts/dts/pos（不关心）
        if (ret < 0) {
            fprintf(stderr, "parse 失败\n");
            break;
        }
        data += ret;         // 前进已消费的字节
        data_size -= ret;

        if (pkt->size) {
            // 解析器返回的包数据包含开始码，我们需要跳过开始码才能读取 NAL 头部。
            // 开始码长度可能是 3 或 4 字节。
            size_t off = 0;
            if (pkt->size >= 4 && pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 0 && pkt->data[3] == 1)
                off = 4;
            else if (pkt->size >= 3 && pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1)
                off = 3;
            // 打印 NAL 信息，供调试使用
            printf("[NAL] pkt size=%d%s\n", pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? "  (关键帧)" : "");
            print_nal_type(pkt->data + off, pkt->size - off, s.codec_id);
            // 解码这个 NAL 单元
            decode_packet(s, pkt.get());
        }

        // 如果剩余数据太少，则移动未消费的数据到缓冲区开头，再读入新数据。
        // 这样解析器可以连续处理，避免因数据不足而无法切出完整 NAL。
        if (data_size < REFILL_THRESH) {
            memmove(inbuf.data(), data, data_size);   // 将剩余数据搬到开头
            data = inbuf.data();                      // 重置 data 指针
            size_t n = fread(inbuf.data() + data_size, 1, BUF_SIZE - data_size, in);
            if (n > 0) data_size += n;
        }
    }
    fclose(in);

    // ---- 冲刷解码器：传入空包，让解码器输出所有缓存帧（例如被延迟的 B 帧） ----
    pkt->data = nullptr;
    pkt->size = 0;
    decode_packet(s, pkt.get());
    return 0;
}

/* ============================================================================
 * 八、解码容器（mp4/ts/flv 等）
 *   容器有封装格式，可以直接通过 av_read_frame 读取打包好的数据包（已带 PTS/DTS 和 stream_index）。
 *   解码器参数从 AVStream->codecpar 中获取，不再使用废弃的 stream->codec。
 *   此函数只处理视频流，忽略音频流。
 * ============================================================================ */
static int decode_container(DecodeSession& s, const std::string& input) {
    // ① 打开输入文件并探测格式
    FormatCtxPtr fmt_ctx;
    {
        AVFormatContext* fc = nullptr;
        // avformat_open_input 会读取文件头部，自动识别封装格式（如 MP4、TS）
        if (avformat_open_input(&fc, input.c_str(), nullptr, nullptr) < 0) {
            fprintf(stderr, "打不开 %s（不是容器？试试裸流模式）\n", input.c_str());
            return -1;
        }
        fmt_ctx.reset(fc);
    }
    // ② 查找流信息（avformat_find_stream_info 会读取一部分数据来获取准确的编解码器参数）
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        fprintf(stderr, "找不到流信息\n");
        return -1;
    }

    // ③ 找到最佳的视频流（自动选择，同时返回对应的解码器）
    const AVCodec* codec = nullptr;
    int video_index = av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_index < 0) {
        fprintf(stderr, "没有视频流\n");
        return -1;
    }
    AVStream* st = fmt_ctx->streams[video_index];

    // ④ 使用 codecpar 初始化解码器上下文（现代方式）
    s.codec_id = st->codecpar->codec_id;
    s.dec_ctx.reset(avcodec_alloc_context3(codec));
    s.dec_ctx->opaque = &s;
    // 将容器中的参数（如 extradata、分辨率、像素格式等）复制到解码器上下文
    if (avcodec_parameters_to_context(s.dec_ctx.get(), st->codecpar) < 0) {
        fprintf(stderr, "参数初始化失败\n");
        return -1;
    }
    // 若启用硬件，则初始化硬件设备
    if (!s.hw_name.empty() && init_hw_decoder(s, s.dec_ctx.get(), s.hw_name.c_str()) < 0) return -1;
    // 打开解码器
    if (avcodec_open2(s.dec_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开解码器失败\n");
        return -1;
    }

    // ⑤ 循环读取包（av_read_frame 返回下一个可用的包）
    PacketPtr pkt(av_packet_alloc());
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == video_index) {
            decode_packet(s, pkt.get());
        }
        av_packet_unref(pkt.get());   // 释放当前包引用，以备下一次循环
    }
    // 冲刷解码器
    pkt->data = nullptr;
    pkt->size = 0;
    decode_packet(s, pkt.get());
    return 0;
}

/* ============================================================================
 * 九、编码 YUV → H.264/H.265（软编/硬编）
 *   编码流程同样采用 send_frame / receive_packet 模式。
 *   关键设计：
 *     - time_base 统一设为 1/fps，这样每帧 pts 递增 1，避免了过去不同编码器 time_base 不一致的混乱。
 *     - 编码器私有参数（如 preset、tune、rc）不能直接通过 AVCodecContext 字段设置，
 *       必须通过 av_opt_set 写入 priv_data（因为 FFmpeg 对私有参数采用选项机制）。
 *     - 区分 NVENC（硬编）和 libx264/x265（软编），它们的参数名和取值范围不同。
 *     - 编码完成后需要冲刷编码器（send_frame(NULL)）以输出所有缓存帧。
 * ============================================================================ */
static int run_encode(const std::string& in_yuv, const std::string& out_file,
                      const std::string& codec_name, const std::string& preset,
                      int width, int height, int fps) {
    // ① 根据编码器名称查找编码器（如 "libx264"、"h264_nvenc"）
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if (!codec) {
        fprintf(stderr, "找不到编码器 %s\n", codec_name.c_str());
        return -1;
    }
    CodecCtxPtr enc_ctx(avcodec_alloc_context3(codec));

    // ---- 公共编码参数（所有编码器通用） ----
    enc_ctx->width          = width;
    enc_ctx->height         = height;
    enc_ctx->time_base      = AVRational{ 1, fps };   // 时间基：每帧间隔为 1/fps 秒
    enc_ctx->framerate      = AVRational{ fps, 1 };   // 帧率（用于编码器内部速率控制）
    enc_ctx->gop_size       = fps * 2;                // GOP 大小：每 2 秒一个 I 帧
    enc_ctx->max_b_frames   = 2;                      // 最大连续 B 帧数（2 个 P 帧之间最多 2 个 B）
    enc_ctx->pix_fmt        = AV_PIX_FMT_YUV420P;     // 输入 YUV 格式（编码器通常接受 YUV420P）
    enc_ctx->bit_rate       = 3'000'000;              // 目标码率 3 Mbps（单位：比特每秒）

    // ---- 编码器私有参数（通过 av_opt_set 设置） ----
    // 为什么不能直接赋值 enc_ctx->preset？因为 preset 不是公共字段，不同编码器实现方式不同，
    // FFmpeg 统一用“选项”机制管理私有参数，必须通过 av_opt_set 写入 priv_data。
    bool is_nvenc = (codec_name.find("nvenc") != std::string::npos);
    if (is_nvenc) {
        // NVIDIA NVENC 硬编：preset 取值 p1~p7（p1 最快，p7 画质最好）
        // tuning: hq（高质量）, rc: vbr（可变码率）, cq: 恒定质量（类似 x264 的 CRF）
        av_opt_set(enc_ctx->priv_data, "preset", preset.empty() ? "p5" : preset.c_str(), 0);
        av_opt_set(enc_ctx->priv_data, "tuning", "hq", 0);
        av_opt_set(enc_ctx->priv_data, "rc", "vbr", 0);
        av_opt_set(enc_ctx->priv_data, "cq", "23", 0);   // 23 是常用值，数值越小质量越好
    } else {
        // 软编 libx264 / libx265：preset 可取 ultrafast/medium/veryslow 等
        av_opt_set(enc_ctx->priv_data, "preset", preset.empty() ? "medium" : preset.c_str(), 0);
        // 对于 H.265，libx265 的私有参数通过 x265-params 传递（键值对以冒号分隔）
        if (codec->id == AV_CODEC_ID_H265) {
            // 设置 keyint（GOP 大小）、min-keyint、bframes、以及关闭 open-gop（防止产生 CRA 帧，
            // 因为某些播放器不支持 CRA，强制 all-IDR 提高兼容性）。
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
    frame->width  = enc_ctx->width;
    frame->height = enc_ctx->height;
    // 为帧分配数据空间（内部会创建 data[0]~data[3] 并分配内存，按 32 字节对齐）
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        fprintf(stderr, "分配帧内存失败\n");
        return -1;
    }
    // 计算一帧 YUV 数据的总字节数（用于从文件中读取）
    int frame_bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format),
                                               frame->width, frame->height, 1);
    std::vector<uint8_t> yuv_buf(frame_bytes);
    PacketPtr pkt(av_packet_alloc());

    int64_t begin = av_gettime_relative() / 1000;   // 记录开始时间（毫秒）
    int64_t pts = 0;

    // 循环读取每一帧 YUV
    for (;;) {
        size_t n = fread(yuv_buf.data(), 1, frame_bytes, in);
        if (n <= 0) break;   // 文件结束或读取失败

        // av_frame_make_writable 确保帧数据可写（可能因内部引用计数而共享，但这里新分配的帧可写）
        if (av_frame_make_writable(frame.get()) < 0) break;

        // 将读取的平面数据填充到 AVFrame 的 data 和 linesize 中。
        // av_image_fill_arrays 根据像素格式自动设置每个平面的指针和行跨度。
        if (av_image_fill_arrays(frame->data, frame->linesize, yuv_buf.data(),
                                 static_cast<AVPixelFormat>(frame->format),
                                 frame->width, frame->height, 1) < 0) break;
        frame->pts = pts++;   // pts 递增（由于 time_base=1/fps，实际时间 = pts/fps 秒）

        // send 一帧到编码器
        if (avcodec_send_frame(enc_ctx.get(), frame.get()) < 0) {
            fprintf(stderr, "send_frame 失败\n");
            break;
        }
        // 接收编码后的包（一个帧可能输出多个包，如分片情况）
        while (avcodec_receive_packet(enc_ctx.get(), pkt.get()) >= 0) {
            printf("[ENC] pts=%" PRId64 " dts=%" PRId64 " size=%d\n", pkt->pts, pkt->dts, pkt->size);
            fwrite(pkt->data, 1, pkt->size, out);
            av_packet_unref(pkt.get());   // 释放包引用
        }
    }

    // ---- 冲刷编码器：传入 NULL 帧，让编码器输出剩余缓存帧（如由于 B 帧延迟） ----
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
 * 十、主函数：命令行参数解析与分发
 *   支持 decode 和 encode 两个子命令。
 *   参数解析较为简单，仅支持固定选项，适合演示用途。
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
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--hw" && i + 1 < argc) hw_name = argv[++i];
            else if (a == "--out" && i + 1 < argc) out_yuv = argv[++i];
            else if (!input.empty() && i == argc) {} // 忽略多余
            else if (a[0] != '-') input = a;
        }
        if (input.empty()) {
            print_usage();
            return 0;
        }

        DecodeSession s;
        s.hw_name = hw_name;
        FILE* out = out_yuv.empty() ? nullptr : fopen(out_yuv.c_str(), "wb");
        s.outfile = out;

        // 通过文件扩展名简单判断是否为裸流（生产环境更可靠的方式是使用 av_probe_input_buffer）
        bool is_raw = false;
        if (input.size() >= 4) {
            std::string tail4 = input.substr(input.size() - 4);
            std::string tail5 = input.size() >= 5 ? input.substr(input.size() - 5) : "";
            is_raw = (tail4 == ".264" || tail4 == ".265" || tail5 == ".h264" || tail5 == ".h265");
        }
        int ret;
        if (is_raw) {
            // 裸流：根据文件名中的 "264" 或 "265" 决定解码器 ID（启发式，可能不准确但可用）
            s.codec_id = (input.find("264") != std::string::npos &&
                          input.find("265") == std::string::npos) ? AV_CODEC_ID_H264 : AV_CODEC_ID_H265;
            s.parser.reset(av_parser_init(s.codec_id));   // 创建解析器
            if (!s.parser) {
                fprintf(stderr, "找不到解析器\n");
                return -1;
            }
            if (open_decoder(s) < 0) return -1;
            ret = decode_raw_stream(s, input);
        } else {
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