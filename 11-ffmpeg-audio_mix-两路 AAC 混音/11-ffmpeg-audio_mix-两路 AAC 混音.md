# Audio Mixer — 两路 AAC 混音工具

## 用途

`audio_mix` 是一个基于 FFmpeg 的命令行工具，用于将两个 **AAC 音频文件**混合（混音）成一个立体声输出文件（M4A 容器）。  
典型场景：将背景音乐与人声、两条音轨合并，或调节两条音轨的音量比例。

## 功能特点

- 使用 FFmpeg 的 `amix` 滤镜进行高质量混音。
- 支持设置两条音轨的混音权重（即音量比例）。
- 输出固定为 **48kHz 立体声 FLTP** 格式，AAC 编码，封装为 M4A。
- 自动处理时间戳，支持输入时长不一致（`duration=longest`，输出时长取较长者）。
- 使用现代 FFmpeg API（`codecpar`、`channel_layout` 等），兼容 FFmpeg 8/9。
- RAII 风格内存管理，防止泄漏。

## 依赖

- **FFmpeg** 开发库（libavcodec, libavformat, libavfilter, libavutil）
- **C++17** 兼容编译器（如 GCC 8+、Clang 7+）

## 编译

使用 g++ 编译：

```bash
g++ -std=c++17 -o audio_mix audio_mix.cpp \
    -lavcodec -lavformat -lavfilter -lavutil -lm
```