# FFmpeg 实用工具集 – Visual Studio 版

欢迎使用这套基于 FFmpeg 的 C++ 命令行工具集。  
它们涵盖了媒体文件解复用、音频提取与重采样、H.264 码流解析、封装格式转换和 MP4 优化等常见音视频处理任务，**全部无需编解码，处理快速且无损**（音频重采样除外）。

---

## 依赖与环境

- **FFmpeg 开发库**（≥ 4.0，部分工具需 ≥ 7.0 以支持 `AVChannelLayout`）  
  核心组件：`libavformat`、`libavcodec`、`libavutil`、`libswresample`（仅工具5需要）  
  - 推荐下载预编译版本（如 [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) 或 [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases)）的 **Shared** 或 **Dev** 包。
- **Visual Studio**：2017 / 2019 / 2022 均可，支持 C++ 桌面开发工作负载。
- **操作系统**：Windows 10/11（也支持 Linux/macOS，但本指南侧重 Windows + VS）。

---

## Visual Studio 项目通用配置

每个工具都是一个独立的控制台应用程序，请为每个工具单独创建项目，或在一个解决方案中添加多个项目。配置步骤如下：

### 1. 添加 FFmpeg 头文件路径
- 在项目属性 → **C/C++** → **常规** → **附加包含目录** 中添加 FFmpeg 的 `include` 文件夹路径。  
  例如：`C:\ffmpeg\include`

### 2. 添加 FFmpeg 库文件路径
- 在项目属性 → **链接器** → **常规** → **附加库目录** 中添加 FFmpeg 的 `lib` 文件夹路径。  
  例如：`C:\ffmpeg\lib`

### 3. 指定需要链接的库文件
- 在项目属性 → **链接器** → **输入** → **附加依赖项** 中添加对应的 `.lib` 文件（注意区分 Debug/Release 和静态/动态版本）：
  - 工具1、2、4、6：  
    `avformat.lib`、`avcodec.lib`、`avutil.lib`
  - 工具5（音频解码器）：  
    `avformat.lib`、`avcodec.lib`、`swresample.lib`、`avutil.lib`
  - 工具3（纯 C++，不依赖 FFmpeg）：无需任何 FFmpeg 库。

> **提示**：如果使用动态链接库（DLL），编译后需将对应的 DLL（如 `avformat-*.dll`）放在可执行文件同目录或系统 PATH 中。

---

## 工具列表（含详细 README 链接）

### 1. 媒体文件解复用分析器 (`demuxer_analyzer`)
**作用**：打开任意媒体文件，遍历所有音视频数据包，统计数量与总大小，并打印前 5 个视频包的详细信息。  
**用法**：  
```bash
demuxer_analyzer.exe <input_media_file>
```
**详细说明**：[README_demuxer.md](README_demuxer.md)

---

### 2. AAC ADTS 提取器 (`aac_extractor`)
**作用**：从媒体文件中提取 AAC 音频流，添加 ADTS 头，输出 `.aac` 文件。  
**用法**：  
```bash
aac_extractor.exe <input.mp4/flv/...> <output.aac>
```
**详细说明**：[README_aac_extractor.md](README_aac_extractor.md)

---

### 3. H.264 Annex‑B 解析器 (`annexb_parser`)
**作用**：解析 H.264 裸流中的 NALU，显示类型、重要性、起始码长度等。  
**特点**：零拷贝，无外部依赖。  
**用法**：直接运行，内置测试数据。  
**详细说明**：[README_annexb_parser.md](README_annexb_parser.md)

---

### 4. FLV 到 Annex‑B 提取器 (`flv_to_annexb`)
**作用**：从 FLV 提取 H.264 裸流并转为 Annex‑B 格式（加起始码）。  
**用法**：  
```bash
flv_to_annexb.exe <input.flv> <output.h264>
```
**详细说明**：[README_flv_to_annexb.md](README_flv_to_annexb.md)

---

### 5. 音频解码与重采样器 (`audio_decoder`)
**作用**：解码音频流，重采样为 44.1kHz / 立体声 / S16 PCM，输出 `.pcm`。  
**用法**：  
```bash
audio_decoder.exe <input.mp4/flv/...> <output.pcm>
```
**播放验证**：
```bash
ffplay -ar 44100 -ac 2 -f s16le output.pcm
```
**详细说明**：[README_audio_decoder.md](README_audio_decoder.md)

---

### 6. MP4 Faststart 优化器 (`mp4_faststart`)
**作用**：将 MP4 的 `moov` 移到文件开头，实现边下边播。  
**用法**：  
```bash
mp4_faststart.exe <input.mp4> <output_faststart.mp4>
```
**详细说明**：[README_mp4_faststart.md](README_mp4_faststart.md)

---



### 7.README – 视频解码器（YUV420P 提取器）

这是一个**轻量级、高效的视频解码工具**。
它能够将任意媒体文件（MP4、FLV、MKV、AVI 等）中的 **视频流解码为原始 YUV420P 格式**，输出标准的 `.yuv` 文件，可用于视频分析、编码测试或作为后续处理（如滤镜、转码）的输入。

```
.exe <input.mp4> <output.yuv>
```



------

### 8.从内存解码音频 —— 自定义 AVIO 示例程序

这是一个**C++ 命令行工具**，它利用 **FFmpeg** 库，从**内存缓冲区**中读取音频数据（而不是从磁盘文件），解码为 **PCM 原始音频**，并保存到文件。

**典型应用场景**：

- 音频文件是**加密**的，需要先解密到内存再解码；
- 音频数据来自**网络流**（如 HTTP 响应体）或**数据库**，不想落地为临时文件；
- 需要自定义数据源（如嵌入式系统、自定义协议）但想复用 FFmpeg 的解码能力。

```
.exe <输入音频文件> <输出PCM文件>
```



------



## Visual Studio 解决方案组织建议

1. 创建一个空解决方案，然后为每个工具添加一个 **控制台应用** 项目。
2. 将对应工具的 `main.cpp` 复制到项目源文件目录。
3. 每个项目单独配置上述 FFmpeg 路径和附加依赖项（工具3可跳过）。
4. 设置输出目录为同一文件夹，方便共享 FFmpeg DLL。

## 贡献与扩展

欢迎根据需求修改源码，每个工具的单独 README 中都附有“扩展建议”章节，可指导你增加新功能。  
如果你希望将所有工具合并为一个多功能程序，也可以通过命令行参数选择功能。

---

如有问题，请查阅各工具的详细 README 或 FFmpeg 官方文档。