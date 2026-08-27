# 从内存解码音频 —— 自定义 AVIO 示例程序

## 1. 这是什么？

这是一个**C++ 命令行工具**，它利用 **FFmpeg** 库，从**内存缓冲区**中读取音频数据（而不是从磁盘文件），解码为 **PCM 原始音频**，并保存到文件。

**典型应用场景**：
- 音频文件是**加密**的，需要先解密到内存再解码；
- 音频数据来自**网络流**（如 HTTP 响应体）或**数据库**，不想落地为临时文件；
- 需要自定义数据源（如嵌入式系统、自定义协议）但想复用 FFmpeg 的解码能力。

## 2. 核心原理（通俗版）

FFmpeg 默认从文件或网络 URL 读取数据（通过 `avformat_open_input` 传入路径）。但它的设计非常灵活，允许我们**替换底层 I/O 操作**——通过 `AVIOContext` 提供自定义的 `读` 和 `跳转` 函数，让 FFmpeg 以为自己在操作一个文件，实际上数据来自我们指定的内存区域。

**工作流程**：
1. 将输入音频文件（如 `.mp3`, `.aac`, `.m4a` 等）**完整读入内存**（一个 `std::vector<uint8_t>`）。
2. 创建 `AVIOContext`，绑定三个自定义回调：
   - **读回调**：从内存当前位置拷贝数据到 FFmpeg 缓冲区；
   - **定位回调**：支持跳转到任意位置（如 `SEEK_SET`），以及返回数据总大小（`AVSEEK_SIZE`）。
3. 创建 `AVFormatContext`，将其 `pb` 字段设为该 `AVIOContext`，并设置 `AVFMT_FLAG_CUSTOM_IO` 标志，让 FFmpeg 知道我们在自己管理 IO。
4. 调用 `avformat_open_input`（此时它通过我们的回调读取文件头）→ `avformat_find_stream_info` 分析流信息 → 找到音频流 → 初始化解码器。
5. 循环调用 `av_read_frame` 读取压缩数据包，送入解码器，将解码后的 PCM 帧写入输出文件。
6. 冲刷解码器，释放资源。

**优势**：不依赖临时文件，解密后即可直接解码；支持任意来源的连续内存数据。

## 3. 编译要求

- **FFmpeg 库**（开发版）：需要 `libavformat`, `libavcodec`, `libavutil`, `libavdevice`（可选）等。
  - 安装方式（Ubuntu/Debian）：`sudo apt install libavformat-dev libavcodec-dev libavutil-dev`
  - 或从源码编译 FFmpeg（推荐启用 `--enable-shared`）。
- **编译器**：支持 C++11 或更高（如 GCC, Clang）。
- **CMake**（推荐）或直接 g++ 命令行。

### 示例编译（使用 g++）

```bash
g++ -std=c++11 -o audio_decoder_mem main.cpp \
    -lavformat -lavcodec -lavutil -lstdc++ -pthread
```

（若 FFmpeg 安装在非标准路径，需添加 `-I` 和 `-L`）

### 使用 CMake（推荐）

创建一个 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.10)
project(AudioDecoderMem)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)

add_executable(audio_decoder_mem main.cpp)
target_include_directories(audio_decoder_mem PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(audio_decoder_mem ${FFMPEG_LIBRARIES})
```

然后：

```bash
mkdir build && cd build
cmake .. && make
```

## 4. 使用方法

```bash
./audio_decoder_mem <输入音频文件> <输出PCM文件>
```

- **输入音频文件**：任意 FFmpeg 支持的音频格式（MP3, AAC, WAV, FLAC, OGG 等）。
- **输出 PCM 文件**：解码后的原始 PCM 数据，格式为 **16-bit 小端**（通常）、与输入音频相同的采样率和声道数，**交错格式**（L R L R ...）。

### 示例

假设你有一个 `song.m4a`，想解码为 `song.pcm`：

```bash
./audio_decoder_mem song.m4a song.pcm
```

之后可以用 `ffplay` 验证：

```bash
ffplay -f s16le -ar 44100 -ac 2 song.pcm
```

（请根据实际采样率和声道数调整 `-ar` 和 `-ac` 参数，程序输出时不改变这些参数）

## 5. 程序行为详解

- **读入内存**：整个输入文件一次性加载到 `std::vector`，适合大小适中的文件（如几 MB 到几百 MB）。若文件极大（> 1GB），可考虑流式分块读取，但本例为演示简洁故全量读入。
- **自定义 IO 回调**：实现了 `read_packet` 和 `seek`，其中 `seek` 支持 `AVSEEK_SIZE`，让 FFmpeg 能获取文件总大小（用于时长估算）。
- **解码器自适应**：自动检测音频编码格式（不硬编码），动态选择解码器。
- **输出 PCM**：支持**平面**和**交错**两种采样格式，最终统一输出为交错格式（绝大多数播放器兼容）。
- **资源管理**：使用 `std::unique_ptr` 配合自定义 Deleter，自动释放 FFmpeg 对象，避免内存泄漏。`AVIOContext` 需手动释放（因为绑定了自定义 IO）。

## 6. 注意事项

1. **内存占用**：输入文件必须全部载入内存，请确保可用 RAM 足够。
2. **文件格式探测**：某些格式（如纯 AAC 流）可能无法自动探测，需指定输入格式（可修改代码添加 `avformat_open_input` 的第三个参数）。
3. **音频参数**：输出 PCM 没有文件头，需另存采样率、声道数等信息以便后续播放或处理。
4. **自定义 IO 生命周期**：`AVIOContext` 必须在 `AVFormatContext` 关闭后再释放，且 `AVFMT_FLAG_CUSTOM_IO` 务必设置，否则 FFmpeg 可能双重释放。
5. **错误处理**：本示例做了基本错误检查，生产环境建议增加更详细的日志。

## 7. 扩展思路

- **网络流**：将 `read_packet` 改为从 socket 或 HTTP 响应体读取（可配合非阻塞 IO）。
- **解密**：在 `read_packet` 中可插入解密逻辑，对数据块解密后再返回。
- **流式处理**：若不想一次性加载整个文件，可设计 `MemoryBufferContext` 支持动态追加数据（类似环形缓冲区），配合 `avio_alloc_context` 的缓冲区管理。
- **多路复用**：可同时处理多个内存流（如合并多个音频片段）。

