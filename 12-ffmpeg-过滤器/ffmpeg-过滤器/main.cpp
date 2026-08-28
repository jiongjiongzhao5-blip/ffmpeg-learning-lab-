#include <iostream>
#include <string>
#include <memory>

// -----------------------------------------------------------------------------
// 1. FFmpeg 头文件引入（纯 C 接口）
// -----------------------------------------------------------------------------
// 必须使用 extern "C" 包裹，防止 C++ 编译器对函数名进行 Name Mangling（名称改编），
// 否则链接器会找不到 FFmpeg 库中对应的符号（因为 FFmpeg 是按照 C 标准编译的）。
extern "C" {
#include <libavfilter/avfilter.h>      // 滤镜核心：AVFilterGraph, AVFilter
#include <libavfilter/buffersink.h>    // 输出汇（Buffersink）：从滤镜图取出处理后的帧
#include <libavfilter/buffersrc.h>     // 输入源（Buffersrc）：向滤镜图喂入原始帧
#include <libavutil/opt.h>             // AVOptions：用于设置滤镜上下文的各种参数
#include <libavutil/pixdesc.h>         // 像素格式描述：辅助调试或格式转换
}

// -----------------------------------------------------------------------------
// 2. RAII 资源管理（智能指针 + 自定义删除器）
// -----------------------------------------------------------------------------
// FFmpeg 的 AVFilterGraph 必须通过 avfilter_graph_alloc() 分配，
// 且通过 avfilter_graph_free() 释放（内部会递归释放所有包含的上下文和过滤器）。
// 下面的删除器利用 std::unique_ptr 实现自动内存管理，防止忘记释放导致内存泄漏。
struct FilterGraphDeleter {
    // 重载函数调用运算符，std::unique_ptr 在析构时会调用此函数
    void operator()(AVFilterGraph* graph) const {
        // avfilter_graph_free 接受二级指针，内部会置空原指针，且本身是空安全的（传入 NULL 不会崩溃）
        if (graph) {
            avfilter_graph_free(&graph);
        }
    }
};
// 定义智能指针类型，当变量超出作用域时，自动调用 FilterGraphDeleter
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;

// -----------------------------------------------------------------------------
// 3. 视频滤镜管道封装类
// -----------------------------------------------------------------------------
// 职责：封装完整的 FFmpeg 滤镜图生命周期，提供“初始化 -> 喂帧 -> 取帧”的标准接口。
// 注意：本类目前并非线程安全，调用者需确保外部加锁，或保证单线程顺序访问。
class VideoFilterPipeline {
public:
    VideoFilterPipeline() = default;
    ~VideoFilterPipeline() = default;

    // 禁止拷贝（因为 unique_ptr 不可拷贝），防止资源被多次释放
    VideoFilterPipeline(const VideoFilterPipeline&) = delete;
    VideoFilterPipeline& operator=(const VideoFilterPipeline&) = delete;

    // -------------------------------------------------------------------------
    // 初始化滤镜图
    // @param in_width     输入视频宽度
    // @param in_height    输入视频高度
    // @param in_pix_fmt   输入像素格式（例如 AV_PIX_FMT_YUV420P）
    // @param time_base    输入帧的时间基（例如 1/90000），决定了 pts（显示时间戳）的单位
    // @return             0 表示成功，负数表示 FFmpeg 标准错误码
    // -------------------------------------------------------------------------
    int Init(int in_width, int in_height, AVPixelFormat in_pix_fmt, AVRational time_base) {
        // 3.1 分配滤镜图顶层结构体
        graph_.reset(avfilter_graph_alloc());
        if (!graph_) {
            return AVERROR(ENOMEM); // 内存分配失败
        }

        // 3.2 获取内置滤镜（通过名称查找）
        // "buffer" 是 FFmpeg 内置的输入源滤镜，专门用来向滤镜图注入外部数据。
        // "buffersink" 是输出汇滤镜，用来从滤镜图提取处理后的数据。
        const AVFilter* buffersrc = avfilter_get_by_name("buffer");
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            // 这种情况极少发生，通常是因为 FFmpeg 编译时禁用了相关组件
            return AVERROR_FILTER_NOT_FOUND;
        }

        // 3.3 创建输入源滤镜上下文（buffersrc）
        // 注意：buffersrc 的参数非常关键，必须通过字符串格式指定。
        // 格式：video_size=宽x高:pix_fmt=像素格式枚举值:time_base=分子/分母:pixel_aspect=像素宽高比
        char args[256];
        snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
            in_width, in_height, in_pix_fmt, time_base.num, time_base.den);

        // avfilter_graph_create_filter 内部会分配上下文，并将其自动挂载到 graph_ 上。
        // 注意：这里 buffersrc_ctx_ 是类成员指针，指向 graph_ 内部管理的对象。
        // 只要 graph_ 存在，该指针就是有效的；但如果 Init 失败导致 graph_ 重置，该指针会悬空，
        // 因此后续任何失败分支都应确保将指针置 nullptr（详见末尾析构逻辑）。
        int ret = avfilter_graph_create_filter(&buffersrc_ctx_, buffersrc, "in",
            args, nullptr, graph_.get());
        if (ret < 0) return ret;

        // 3.4 创建输出汇滤镜上下文（buffersink）
        ret = avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out",
            nullptr, nullptr, graph_.get());
        if (ret < 0) return ret;

        // 定义允许的像素格式列表（以 AV_PIX_FMT_NONE 结尾）
        enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

        // 计算整个数组占用的字节数
        size_t size = sizeof(pix_fmts);

        // 通过 av_opt_set_bin 将二进制数据直接写入选项
        // 第三个参数需要强制转换为 const uint8_t*
        int ret2 = av_opt_set_bin(buffersink_ctx_, "pix_fmts",
            (const uint8_t*)pix_fmts, size,
            AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) return ret2;

        // 3.6 构建图的端点描述符（AVFilterInOut）
        // 这是 FFmpeg 滤镜图解析中最容易令人困惑的地方！
        // 核心概念：滤镜图有两个“对外接口” —— 输入端点（对应 buffersrc）和 输出端点（对应 buffersink）。
        // 但在 avfilter_graph_parse_ptr 的函数签名中：
        //   int avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters,
        //                                AVFilterInOut **inputs,   <- 代表图的输入端点（连接滤镜链的输出端）
        //                                AVFilterInOut **outputs,  <- 代表图的输出端点（连接滤镜链的输入端）
        //                                void *log_ctx);
        // 这里的逻辑是反转的：参数中的 inputs 实际上是要连接到缓冲汇（buffersink）的末端。
        // 参数中的 outputs 实际上是要连接到缓冲源（buffersrc）的始端。
        // 所以我们将 buffersrc_ctx_ 填入 outputs，将 buffersink_ctx_ 填入 inputs。
        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            return AVERROR(ENOMEM);
        }

        // 配置输出端点（指向源）：名称为 "in"，与滤镜描述中未命名输入的默认标签匹配
        outputs->name = av_strdup("in");   // 名称必须与滤镜描述中的 [in] 对应，若描述中未命名，则默认为 "in"
        outputs->filter_ctx = buffersrc_ctx_;    // 关联到缓冲源
        outputs->pad_idx = 0;                 // 使用第 0 个垫（pad）
        outputs->next = nullptr;           // 链式端点（本例只有一个）

        // 配置输入端点（指向汇）：名称为 "out"，与滤镜描述中未命名输出的默认标签匹配
        inputs->name = av_strdup("out");
        inputs->filter_ctx = buffersink_ctx_;   // 关联到缓冲汇
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        // 3.7 解析并构建滤镜链
        // 滤镜描述字符串：
        //   scale=1280:720:force_original_aspect_ratio=decrease -> 将画面缩放到 1280x720，保持原始宽高比，若尺寸不足则缩小（decrease）
        //   pad=1280:720:(ow-iw)/2:(oh-ih)/2:black           -> 用黑色填充剩余的空白区域，实现居中显示（无变形）
        //   drawtext=text='RECORDING %{pts\\:hms}':x=20:y=20:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.5
        //      -> 绘制文本水印，显示当前录制时长（PTS 转换为 HMS 格式）。
        //        注意：转义符 \\: 是因为字符串中的 : 会被滤镜解析器视为分隔符，需要双重转义（C++ 字符串转义 + FFmpeg 过滤图转义）。
        const char* filter_desc =
            "scale=1280:720:force_original_aspect_ratio=decrease,"
            "pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,"
            "drawtext=text='RECORDING %{pts\\:hms}':x=20:y=20:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.5";

        // 解析并连接。注意：传入的 inputs 和 outputs 指针在函数内部会被消耗（置空），
        // 因此传入后我们不能再使用它们，最后调用 avfilter_inout_free 释放内存即可。
        ret = avfilter_graph_parse_ptr(graph_.get(), filter_desc, &inputs, &outputs, nullptr);
        // 无论解析成功与否，必须释放 endpoints 结构体，避免内存泄漏。
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (ret < 0) return ret;

        // 3.8 提交配置并触发格式/参数协商（最关键的一步）
        // 此函数会检查所有滤镜是否支持连接的格式，进行必要的缓冲分配，并准备处理帧。
        // 如果不调用此函数，后续的 SendFrame/ReceiveFrame 必然会失败。
        return avfilter_graph_config(graph_.get(), nullptr);
    }

    // -------------------------------------------------------------------------
    // 向滤镜图发送原始帧（输入）
    // @param frame 待处理的 AVFrame（原始未压缩图像数据）。
    //              当 frame == nullptr 时，表示冲刷（EOF），通知滤镜图后续不再有输入。
    // @return      0 表示成功，AVERROR(EAGAIN) 表示需要先接收输出帧才能继续输入，
    //              AVERROR_EOF 表示已冲刷完毕。
    // -------------------------------------------------------------------------
    int SendFrame(AVFrame* frame) {
        // AV_BUFFERSRC_FLAG_KEEP_REF: 保持对帧的引用计数。这意味着 av_buffersrc_add_frame 会内部增加 ref，
        // 外部调用者仍然可以安全地释放自己的引用（调用 av_frame_free）。
        // 如果不加此标志，FFmpeg 可能会直接接管帧，外部再释放会导致 double-free。
        return av_buffersrc_add_frame_flags(buffersrc_ctx_, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    }

    // -------------------------------------------------------------------------
    // 从滤镜图接收处理后的帧（输出）
    // @param out_frame 调用者预先分配的 AVFrame 指针（需保证 av_frame_alloc 过），
    //                  函数内部会将数据填充到该结构体中。
    // @return          0 表示成功取出一帧；
    //                  AVERROR(EAGAIN) 表示当前没有输出帧（需要继续输入更多帧）；
    //                  AVERROR_EOF 表示滤镜图已处理完所有输入，没有更多输出了；
    //                  其他负数表示具体错误。
    // -------------------------------------------------------------------------
    int ReceiveFrame(AVFrame* out_frame) {
        // 从 buffersink 取出帧。注意：取出后 out_frame 会引用滤镜图内部的数据缓冲区。
        // 调用者务必在不再使用时调用 av_frame_unref 或 av_frame_free 释放引用。
        return av_buffersink_get_frame(buffersink_ctx_, out_frame);
    }

private:
    // 智能指针管理滤镜图，析构时自动调用 avfilter_graph_free
    FilterGraphPtr graph_{ nullptr };

    // 裸指针指向滤镜图中的上下文（由 graph_ 管理生命周期，不负责释放）
    // 重要：如果 Init 中途失败，但 graph_ 已被重置（reset），这些指针将悬空。
    // 虽然当前写法在失败时直接 return，未显式置空，但 C++ 对象析构时这些指针不会自动置空。
    // 严格的安全做法是在 Init 的每个失败分支中都置 nullptr，或者依赖 graph_ 的存在性判断。
    // 本示例为了简洁保留此写法，实际生产环境建议增加 initialized_ 标志位或使用 weak_ptr 辅助。
    AVFilterContext* buffersrc_ctx_{ nullptr };
    AVFilterContext* buffersink_ctx_{ nullptr };
};