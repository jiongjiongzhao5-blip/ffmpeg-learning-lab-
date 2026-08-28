/**
 * ============================================================================
 * audio_mix.cpp —— 两路 AAC 混音（C++17 + FFmpeg 8/9）
 *
 * 功能：将两个 AAC 音频文件混音（混合）成一个立体声输出文件（M4A 容器）。
 *
 * 滤镜图结构（对应 main.cpp 的知识点）：
 *    abuffer(in0)  \
 *                   amix(inputs=2, weights) ---> abuffersink(out)
 *    abuffer(in1)  /
 *
 * 用法:
 *   ./audio_mix in0.aac in1.aac out.m4a
 *
 * 注意：输入必须是 AAC（每帧 1024 采样），amix 输出帧数与之匹配，
 * 才能直接喂给 AAC 编码器。若输入采样率/帧长不一致，生产里要在滤镜图里
 * 再加 aresample（重采样）来处理 —— 这里为讲清主流程先省略。
 * ============================================================================
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <memory>

 // 所有 FFmpeg 头文件必须以 extern "C" 包裹，因为它们是 C 库
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>
}

// ---- RAII 封装（和 video_tool 一样）----
// 使用 unique_ptr 配合自定义删除器，自动管理 FFmpeg 对象生命周期，防止内存泄漏
struct CtxDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FmtDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct GraphDeleter { void operator()(AVFilterGraph* p) const { avfilter_graph_free(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PktDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CtxDeleter>;
using FmtPtr = std::unique_ptr<AVFormatContext, FmtDeleter>;
using GraphPtr = std::unique_ptr<AVFilterGraph, GraphDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PktPtr = std::unique_ptr<AVPacket, PktDeleter>;

// 一个音频输入：包含格式上下文（容器）和解码器上下文
struct AudioInput {
    FmtPtr fmt_ctx;          // 输入文件格式上下文
    CodecPtr dec_ctx;        // 音频解码器上下文
    int stream_index = -1;   // 音频流在容器中的索引
};

/**
 * 打开输入文件并准备解码器
 *
 * 现代 FFmpeg 推荐使用 codecpar（codec parameters）来传递流参数给解码器，
 * 而不再使用已被废弃的 AVStream::codec 字段。
 *
 * @param in        AudioInput 结构体引用（输出）
 * @param filename  输入文件名
 * @return 0 成功，负数失败
 */
static int open_input(AudioInput& in, const char* filename) {
    AVFormatContext* fc = nullptr;
    // 打开文件并探测格式
    if (avformat_open_input(&fc, filename, nullptr, nullptr) < 0) {
        fprintf(stderr, "打不开 %s\n", filename); return -1;
    }
    in.fmt_ctx.reset(fc);  // 交给 unique_ptr 管理

    // 读取流信息（如时长、码率等）
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        fprintf(stderr, "找不到流信息\n"); return -1;
    }

    // 寻找最佳音频流，同时自动匹配解码器
    const AVCodec* codec = nullptr;
    in.stream_index = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (in.stream_index < 0) {
        fprintf(stderr, "没有音频流\n"); return -1;
    }

    // 为解码器分配上下文
    in.dec_ctx.reset(avcodec_alloc_context3(codec));
    // 关键：从 codecpar 复制参数到解码器上下文（替代旧的 stream->codec）
    if (avcodec_parameters_to_context(in.dec_ctx.get(),
        fc->streams[in.stream_index]->codecpar) < 0)
        return -1;
    // 打开解码器
    if (avcodec_open2(in.dec_ctx.get(), codec, nullptr) < 0) {
        fprintf(stderr, "打开解码器失败\n"); return -1;
    }
    return 0;
}

/**
 * 构建 FFmpeg 滤镜图（filter graph），用于混音
 *
 * 图结构：
 *   abuffer(in0) ──┐
 *                   ├── amix ── abuffersink(out)
 *   abuffer(in1) ──┘
 *
 * 其中 abuffer 是“源”滤镜，接收外部输入帧；
 * amix 是混音滤镜，这里配置为两输入，权重 1:0.3，时长取最长；
 * abuffersink 是“槽”滤镜，用于从中拉取处理后的帧。
 *
 * @param out_graph 输出的滤镜图指针（需用 unique_ptr 接管）
 * @param src0      输出第一个 abuffer 上下文指针
 * @param src1      输出第二个 abuffer 上下文指针
 * @param sink      输出 abuffersink 上下文指针
 * @param in0       输入0 的解码器上下文（用于获取输入参数）
 * @param in1       输入1 的解码器上下文
 * @return 0 成功，负数失败
 */
static int init_filter_graph(AVFilterGraph** out_graph,
    AVFilterContext** src0, AVFilterContext** src1,
    AVFilterContext** sink,
    const AudioInput& in0, const AudioInput& in1) {
    // 设置输出格式：固定 48kHz、FLTP 平面浮点、立体声
    const int out_sample_rate = 48000;
    const enum AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLTP;
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);  // 默认立体声布局

    // 分配滤镜图对象
    GraphPtr graph(avfilter_graph_alloc());
    const AVFilter* f_src = avfilter_get_by_name("abuffer");
    const AVFilter* f_sink = avfilter_get_by_name("abuffersink");
    if (!graph || !f_src || !f_sink) {
        fprintf(stderr, "取滤镜失败\n"); return -1;
    }

    // ---- 创建两个 abuffer 源滤镜 ----
    // 每个 abuffer 需要传入参数：时基、采样率、采样格式、声道布局
    for (int k = 0; k < 2; k++) {
        AVCodecContext* c = (k == 0 ? in0 : in1).dec_ctx.get();
        char args[256];
        // 使用 PRIx64 宏打印 64 位十六进制（声道布局掩码）
        snprintf(args, sizeof(args),
            "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%" PRIx64,
            c->sample_rate, c->sample_rate,
            av_get_sample_fmt_name(c->sample_fmt), c->ch_layout.u.mask);
        AVFilterContext* s = nullptr;
        // avfilter_graph_create_filter 会创建滤镜并将其添加到图中
        if (avfilter_graph_create_filter(&s, f_src, k == 0 ? "in0" : "in1",
            args, nullptr, graph.get()) < 0) {
            fprintf(stderr, "创建 abuffer 失败\n"); return -1;
        }
        if (k == 0) *src0 = s; else *src1 = s;
    }

    // ---- 创建 abuffersink 输出滤镜 ----
    // abuffersink 是图的终点，需要设置期望的输出格式约束
    if (avfilter_graph_create_filter(sink, f_sink, "out", nullptr, nullptr, graph.get()) < 0) {
        fprintf(stderr, "创建 abuffersink 失败\n"); return -1;
    }
    // 约束输出格式：只接受 FLTP、48kHz、立体声（掩码 0x3）
    const int64_t fmts[] = { AV_SAMPLE_FMT_FLTP, -1 };
    const int64_t rates[] = { 48000, -1 };
    const int64_t layouts[] = { (int64_t)out_ch_layout.u.mask, -1 };
    // av_opt_set_int_list 用于设置滤镜的 option（此处是 sink 的格式约束）
    av_opt_set_bin(*sink, "sample_fmts", (const uint8_t*)fmts, sizeof(fmts), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_bin(*sink, "sample_rates", (const uint8_t*)rates, sizeof(rates), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_bin(*sink, "channel_layouts", (const uint8_t*)layouts, sizeof(layouts), AV_OPT_SEARCH_CHILDREN);

    // ---- 描述滤镜连接关系 ----
    // 滤镜描述字符串： [in0][in1]amix=inputs=2:weights='1 0.3':duration=longest[out]
    // 含义：两个输入流进入 amix，权重分别 1 和 0.3，时长为最长输入，输出标签 out。
    const char* descr = "[in0][in1]amix=inputs=2:weights='1 0.3':duration=longest[out]";

    // 构建输入/输出端点（AVFilterInOut）用于连接
    // outputs 表示滤镜图的“出口”们（即各输入源滤镜的输出 pad）
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* outputs1 = avfilter_inout_alloc();
    outputs->name = av_strdup("in0");
    outputs->filter_ctx = *src0;
    outputs->pad_idx = 0;
    outputs1->name = av_strdup("in1");
    outputs1->filter_ctx = *src1;
    outputs1->pad_idx = 0;
    outputs->next = outputs1;

    // inputs 表示滤镜图的“入口”们（即最终输出滤镜的输入 pad）
    AVFilterInOut* inputs = avfilter_inout_alloc();
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *sink;
    inputs->pad_idx = 0;

    // 解析滤镜描述并连接到图中
    int ret = avfilter_graph_parse_ptr(graph.get(), descr, &inputs, &outputs, nullptr);
    if (ret >= 0) {
        // 配置整个图（分配缓冲区、检查一致性等）
        ret = avfilter_graph_config(graph.get(), nullptr);
    }

    // 释放临时 InOut 结构
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0) {
        fprintf(stderr, "构建滤镜图失败: %d\n", ret);
        return -1;
    }

    // 将图的所有权转交给调用者（用 unique_ptr 接管）
    *out_graph = graph.release();
    return 0;
}

/**
 * 主程序入口
 */
int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "用法: %s in0.aac in1.aac out.m4a\n", argv[0]);
        return 1;
    }

    // ---- 打开两个输入 ----
    AudioInput in0, in1;
    if (open_input(in0, argv[1]) || open_input(in1, argv[2])) return 1;

    // ---- 构建滤镜图 ----
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src0 = nullptr, * src1 = nullptr, * sink = nullptr;
    if (init_filter_graph(&graph, &src0, &src1, &sink, in0, in1) < 0) return 1;
    // 用 unique_ptr 管理 graph 的生命周期（但注意 src0/src1/sink 是图中的引用，不用单独释放）
    GraphPtr graph_holder(graph);

    // ---- 打开输出文件（M4A 容器 + AAC 编码）----
    AVFormatContext* ofmt = nullptr;
    // 根据输出文件名自动猜测格式（m4a 对应 mov/mp4 格式）
    if (avformat_alloc_output_context2(&ofmt, nullptr, nullptr, argv[3]) < 0) return 1;
    // 创建输出流
    AVStream* ost = avformat_new_stream(ofmt, nullptr);

    // 查找 AAC 编码器
    const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    CodecPtr enc(avcodec_alloc_context3(aac));
    // 设置编码参数：48kHz, FLTP, 立体声, 比特率 128kbps
    enc->sample_rate = 48000;
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&enc->ch_layout, 2);
    enc->bit_rate = 128000;

    // 重要：M4A/MP4 容器要求 AAC 不带 ADTS 头，而使用 Global Header
    // 这个标志必须在 avcodec_open2 之前设置
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 打开编码器
    if (avcodec_open2(enc.get(), aac, nullptr) < 0) {
        fprintf(stderr, "打开编码器失败\n"); return 1;
    }
    // 将编码器参数复制到输出流 codecpar
    if (avcodec_parameters_from_context(ost->codecpar, enc.get()) < 0) return 1;
    // 设置流时基（采样率倒数）
    ost->time_base = AVRational{ 1, 48000 };

    // 打开输出文件（写模式）
    if (avio_open(&ofmt->pb, argv[3], AVIO_FLAG_WRITE) < 0) return 1;
    // 写容器头部（包含流信息等）
    if (avformat_write_header(ofmt, nullptr) < 0) {
        fprintf(stderr, "写头失败\n"); return 1;
    }

    // ---- 主循环：读取输入包 → 解码 → 推入滤镜图 → 拉取混合帧 → 编码写入 ----
    PktPtr pkt0(av_packet_alloc()), pkt1(av_packet_alloc());
    FramePtr dec_frame(av_frame_alloc());   // 解码输出帧（复用）
    FramePtr mix_frame(av_frame_alloc());   // 混音输出帧（复用）
    int eof0 = 0, eof1 = 0;                 // EOF 标志
    int64_t pts0 = 0, pts1 = 0;             // 每个输入流的时间戳（按采样数递增，时基为 1/采样率）

    while (!eof0 || !eof1) {
        // ---- 轮流处理两个输入 ----
        for (int k = 0; k < 2; k++) {
            AudioInput& in = (k == 0) ? in0 : in1;
            int& eof = (k == 0) ? eof0 : eof1;
            AVFilterContext* src = (k == 0) ? src0 : src1;
            if (eof) continue;

            AVPacket* pkt = (k == 0) ? pkt0.get() : pkt1.get();
            int ret = av_read_frame(in.fmt_ctx.get(), pkt);
            if (ret < 0) {
                // 文件读完了，向滤镜图发送 EOF（NULL 帧）
                av_buffersrc_add_frame_flags(src, nullptr, AV_BUFFERSRC_FLAG_PUSH);
                eof = 1;
                continue;
            }

            // 只处理音频流（忽略可能存在的其他流）
            if (pkt->stream_index == in.stream_index) {
                // 送包给解码器
                if (avcodec_send_packet(in.dec_ctx.get(), pkt) >= 0) {
                    // 循环接收解码后的帧（一个包可能包含多个帧，但 AAC 一般一个包一帧）
                    while (avcodec_receive_frame(in.dec_ctx.get(), dec_frame.get()) >= 0) {
                        // 有些容器不带精确的 pts，这里我们手动生成 pts
                        // 按采样数递增（AAC 每帧 1024 采样）
                        dec_frame->pts = (k == 0 ? pts0 : pts1);
                        if (k == 0) pts0 += dec_frame->nb_samples;
                        else        pts1 += dec_frame->nb_samples;

                        // 将解码后的帧推入滤镜图
                        av_buffersrc_add_frame_flags(src, dec_frame.get(),
                            AV_BUFFERSRC_FLAG_PUSH);
                        // 清理 frame 引用，以便下次复用
                        av_frame_unref(dec_frame.get());
                    }
                }
            }
            av_packet_unref(pkt);  // 释放包引用
        }

        // ---- 从滤镜图拉取混合后的帧 ----
        int r = av_buffersink_get_frame(sink, mix_frame.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            // 滤镜图暂无输出或已经结束，继续循环等待更多输入
            continue;
        }
        if (r < 0) {
            fprintf(stderr, "buffersink 取帧失败\n");
            break;
        }

        // ---- 将混合帧编码并写入输出 ----
        if (avcodec_send_frame(enc.get(), mix_frame.get()) >= 0) {
            while (avcodec_receive_packet(enc.get(), pkt0.get()) >= 0) {
                // 将编码包的时间戳从编码器时基转换到流时基
                av_packet_rescale_ts(pkt0.get(), enc->time_base, ost->time_base);
                pkt0->stream_index = 0;
                // 交错写入（保证时间戳顺序）
                av_interleaved_write_frame(ofmt, pkt0.get());
                av_packet_unref(pkt0.get());
            }
        }
        av_frame_unref(mix_frame.get());
    }

    // ---- 冲刷编码器（发送 NULL 帧）----
    avcodec_send_frame(enc.get(), nullptr);
    while (avcodec_receive_packet(enc.get(), pkt0.get()) >= 0) {
        av_packet_rescale_ts(pkt0.get(), enc->time_base, ost->time_base);
        pkt0->stream_index = 0;
        av_interleaved_write_frame(ofmt, pkt0.get());
        av_packet_unref(pkt0.get());
    }

    // ---- 写文件尾并清理 ----
    av_write_trailer(ofmt);
    avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);
    // graph_holder 自动释放 graph
    printf("混音完成\n");
    return 0;
}