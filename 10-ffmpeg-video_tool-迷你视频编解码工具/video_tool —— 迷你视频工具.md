## 代码功能概述

这份 `video_tool.cpp` 是一个基于 FFmpeg 的轻量级视频处理命令行工具，用 C++17 编写，涵盖了课堂上关于视频编解码的四个核心知识点，并以生产工程的方式实现。它主要提供以下能力：

- **解码**：支持 H.264 / H.265 裸流（Annex‑B）和常见容器格式（MP4、TS 等）的解码，可输出解码后的 YUV420P 原始帧。
- **硬解加速**：支持多种硬件加速后端（如 D3D11VA、CUDA、VAAPI、QSV 等），在解码时自动将硬件帧拷贝回内存以便后续处理。
- **NAL 单元分析**：在解码裸流时，会逐 NAL 打印其类型（SPS、PPS、IDR、B/P 帧等），有助于理解码流结构和调试。
- **编码**：支持软编码（libx264 / libx265）和硬编码（NVIDIA NVENC），可将 YUV420P 原始帧编码为 H.264 / H.265 码流，并可设置编码预设（preset）和参数。
- **工程化设计**：使用 RAII 管理 FFmpeg 资源，统一使用 `swscale` 进行像素格式转换，避免手动内存操作，代码健壮且易读。

该工具非常适合用于学习 FFmpeg API、调试视频流、验证编码参数，或者作为更复杂视频处理项目的骨架。

------

## README 文档（中文）

# video_tool —— 迷你视频编解码工具

基于 FFmpeg 的 C++17 命令行工具，支持 H.264/H.265 的软/硬解码、裸流 NAL 分析、YUV 导出以及软/硬编码。

## 特性

- ✅ 解码 H.264/H.265 裸流（.h264/.h265）或容器文件（.mp4/.ts 等）
- ✅ 可选硬件加速解码（d3d11va / cuda / vaapi / qsv / videotoolbox）
- ✅ 解码时自动将帧转为 YUV420P 并写出（.yuv）
- ✅ 裸流模式下逐 NAL 单元打印类型（SPS/PPS/IDR/SEI 等）
- ✅ 编码 YUV420P 为 H.264/H.265，支持软编（libx264/libx265）和硬编（h264_nvenc/hevc_nvenc）
- ✅ 支持编码预设（preset）调整速度/质量平衡
- ✅ RAII 资源管理，无内存泄漏

## 依赖

- C++17 编译器（g++ 或 clang）
- FFmpeg 库（libavcodec, libavutil, libswscale, libavformat），推荐 FFmpeg 8/9 版本

## 编译

Linux / macOS 下使用 pkg-config：

```bash
g++ -std=c++17 video_tool.cpp -o video_tool \
    $(pkg-config --cflags --libs libavcodec libavutil libswscale libavformat)
```





Windows 下可自行配置包含路径和链接库，或使用 MSYS2 / vcpkg 安装 FFmpeg 后编译。

## 用法

### 解码

bash

```
# 软解裸流并打印 NAL 信息
./video_tool decode in.h265

# 解码容器文件并输出 YUV
./video_tool decode in.mp4 --out out.yuv

# 使用硬件加速解码（例如 Windows 下的 d3d11va）
./video_tool decode in.h264 --hw d3d11va --out out.yuv
```



支持的硬件设备名：`d3d11va`, `dxva2`, `cuda`, `qsv`, `vaapi`, `videotoolbox`。

### 编码

bash

```
# 软编码 H.265，使用 medium 预设
./video_tool encode in.yuv out.h265 --codec libx265 --preset medium

# NVIDIA 硬编码 H.264，使用 p5 预设（平衡）
./video_tool encode in.yuv out.h264 --codec h264_nvenc --preset p5

# 自定义分辨率、帧率（默认 1280x720, 25fps）
./video_tool encode in.yuv out.h264 --codec libx264 --width 1920 --height 1080 --fps 30
```



支持的编码器：`libx264`, `libx265`, `h264_nvenc`, `hevc_nvenc`。

**预设说明**：

- 软编预设：`ultrafast` ~ `veryslow`（x264/x265 通用）
- NVENC 预设：`p1`（最快）~ `p7`（最慢/质量最高）

## 示例输出

解码裸流时的 NAL 打印示例：

text

```
[NAL] pkt size=157  (关键帧)
      nal header=0x26  type=19  size=156  (IDR_W_DLP)
[NAL] pkt size=45
      nal header=0x42  type=33  size=44  (SPS)
...
[DEC] #1    1920x1080  fmt=yuv420p    pts=0
```



编码时输出每帧的 PTS 和包大小。

## 注意事项

- 输入 YUV 必须是 YUV420P 格式（planar，无交错）。
- 硬解需要系统支持相应驱动（NVIDIA 显卡需安装驱动，Windows 需 Direct3D 支持等）。
- 硬解帧会通过 `av_hwframe_transfer_data` 拷回 CPU，避免直接操作显存。
- 编码器的私有参数（如 preset）使用 `av_opt_set` 设置，而非直接修改 `AVCodecContext` 成员。

## 项目结构

单文件实现，代码注释详尽，适合学习 FFmpeg API 和 C++ RAII 用法。