# ffplay 播放器全套知识点（完整修订版）

**一句话先记住全篇**：一个播放器 = 一条"读→解→存→放"的多线程流水线 + 三个会"对时"的时钟 + 一整套用 serial 区分新旧数据、用队列做解耦和限流的设计。

---

## 一、整体框架

### 1. ffplay.c 的意义
- ffplay 是 FFmpeg 自带的一个真正能播放视频的小播放器，就一个 `.c` 文件，用 **FFmpeg + SDL** 写成。
- 它重要是因为 **B 站开源的 ijkplayer 就是拿 ffplay.c 二次开发的**，很多播放器/直播 SDK 也是这么抄的。
- 大白话：**ffplay 是"播放器界的完整 Hello World"**。你独立写播放器前，先把它吃透是捷径。

### 2. 框架：线程怎么划分（全篇最重要的一张地图）
播放器本质是"流水线+仓库"。因为读文件、解码、显示速度不一样，必须用**多线程**各干各的，用队列衔接：

| 线程                         | 干什么                       | 大白话                               |
| ---------------------------- | ---------------------------- | ------------------------------------ |
| 数据读取线程 read_thread     | 打开文件、读包、分发给各队列 | 采购员，把原材料（AVPacket）搬进仓库 |
| 音频解码线程 audio_thread    | 音频包→解码→放音频帧队列     | 音频加工线                           |
| 视频解码线程 video_thread    | 视频包→解码→放视频帧队列     | 视频加工线                           |
| 字幕解码线程 subtitle_thread | 字幕包→解码→放字幕帧队列     | 字幕加工线                           |
| 主线程                       | 视频显示、字幕显示、响应按键 | 店长：上货架 + 处理客人要求          |

两个关键队列概念：
- **PacketQueue（包队列）**：存"解码前"的压缩数据 AVPacket。**链表**，能无限塞，所以必须限制大小。
- **FrameQueue（帧队列）**：存"解码后"的成品 AVFrame。**固定大小数组**（视频3个、音频9个、字幕16个）。因为解码后的数据很大（1080p YUV420 一帧约 311 万字节），不能缓存太多。

数据流向：`文件 → 读线程(av_read_frame) → 各自PacketQueue → 解码线程 → 各自FrameQueue → 播放/显示`。**音频走 SDL 回调输出，视频在主线程显示，彼此独立**——这就是后面需要"音视频同步"的根本原因。

---

## 二、数据结构详解

### 1. struct VideoState —— 播放器的"总管家"
所有播放状态都塞在这个结构体里，相当于"播放器全部家产清单"。挑重点：

- `read_tid`：读线程句柄，退出时要回收。
- `abort_request`：**退出总开关**，=1 表示请求退出。所有线程循环里都检查它，看到就收工。
- `paused` / `last_paused`：暂停标志；`last_paused` 记住上一次状态，用来检测"状态是否变了"。
- `force_refresh`：=1 要求强制刷新一帧画面（比如暂停时也要能显示当前帧、窗口被遮挡后要重画）。
- `seek_req` / `seek_flags` / `seek_pos` / `seek_rel`：一组"seek 请求"。主线程只负责**提交请求**（写变量），真正干活的是**读线程**。这是典型的"请求标记"设计。
- **三个时钟**：`audclk`（音频）、`vidclk`（视频）、`extclk`（外部）——同步用。
- **三个帧队列**：`pictq`（视频）、`subpq`（字幕）、`sampq`（音频采样）。
- **三个解码器封装**：`auddec` / `viddec` / `subdec`。
- 三个流索引+指针：`audio_stream`/`audio_st`/`audioq`，视频字幕同理。
- **音频缓冲一组字段（音频输出核心，后面反复用）**：
  - `audio_buf`：指向"待播放的一帧音频数据"；若重采样过则指向 `audio_buf1`，否则指向 frame 里的数据。
  - `audio_buf1`：重采样后的数据区。
  - `audio_buf_size`：audio_buf 总共多大。
  - `audio_buf_index`：已经拷给 SDL 多少了（下一次该从哪个位置拷）。
  - `audio_write_buf_size`：还剩多少没拷 = `audio_buf_size - audio_buf_index`。
  - 这就是**二级缓冲**：从帧队列取一帧→放 audio_buf→SDL 回调来一点拿一点。
- `audio_src` / `audio_tgt`：源音频参数 / SDL 设备支持的参数。不一致就要重采样；`swr_ctx` 是重采样上下文。
- `frame_drops_early` / `frame_drops_late`：**提前丢帧 / 滞后丢帧的计数器**，统计同步时丢了多少帧。
- `frame_timer`：记录"最后一帧开始显示的时刻"，**视频同步的核心变量**。
- `show_mode`：显示模式。有视频就 `SHOW_MODE_VIDEO`；只有音频时用 `SHOW_MODE_WAVES`（波形）/`SHOW_MODE_RDFT`（频谱）。
- `continue_read_thread`：一个条件变量。**队列满了读线程去睡 10ms；解码线程发现队列空了就发信号把它叫醒**。
- `step`：逐帧播放标志，=1 表示"播一帧后暂停"。

### 2. struct Clock —— 时钟封装（同步的"表"）
```c
pts          // 当前时间戳（秒）
pts_drift    // = pts - 对时那一刻的系统时间
last_updated // 最后一次对时的系统时间
speed        // 播放速度
serial       // 播放序列号
paused       // 是否暂停
queue_serial // 指向包队列的 serial（判断这个时钟的数据是否过期）
```
**它怎么工作（全篇精髓，必须懂）**：
1. 时钟要不断"**对时**"（set_clock / set_clock_at）：给一个 pts 和当时的系统时间 time，算出 `pts_drift = pts - time`（这个流的播放进度比系统时间超前/落后多少）。
2. 对时之后不能直接用上次的 pts 当当前时间，因为系统时间在走。用公式 **`当前pts ≈ 当前系统时间 + pts_drift`**（get_clock）来估算。
3. 大白话：**就像校准手表**——每隔一段时间对一次表，记住"表差"，平时就能随时推算出现在是几点。`pts_drift` 就是这个"表差"。

**serial（播放序列）概念必须单独记住**：**每次 seek 都会让 serial 加 1**，这样"seek 前的旧数据"和"seek 后的新数据"就分开了。队列节点、帧、时钟都带 serial，发现对不上就说明是"过时的旧货"，直接丢。

### 3. MyAVPacketList + PacketQueue —— 包队列（链表仓库）
- `MyAVPacketList` 是**链表节点**：一个 `AVPacket`（解码前的压缩数据）+ `next` + `serial`（属于哪个播放序列）。
- `PacketQueue` 是**队列本身**：队首队尾指针、`nb_packets`（包数）、`size`（总字节数）、`duration`（总时长）、`abort_request`、`serial`、`mutex`（互斥锁）、`cond`（条件变量）。

**函数逐个讲**：
- `packet_queue_init()`：初始化，创建锁和条件变量，`abort_request=1`（还没启动）。
- `packet_queue_destroy()`：先清空队列，再销毁锁和条件变量。
- `packet_queue_start()`：`abort_request=0`，**并往队列放一个 `flush_pkt`**。
- **`flush_pkt` 是什么、为什么启动就放？** 一个特殊空包，作用两个：
  1. 放它时**队列 serial +1**，标记"从这里开始是新的一段数据"；
  2. 解码线程读到它时调 `avcodec_flush_buffers()` **清空解码器内部缓存**（解码器为 B 帧会缓存帧），从头解新序列。
  大白话：**像两批货之间夹的"分隔条"，告诉后面"前面那批不要了，从这开始是新的"。**
- `packet_queue_abort()`：置 `abort_request=1`，`SDL_CondSignal` 发信号唤醒等待线程，让它们发现"要退出了"赶紧走。
- `packet_queue_put()`（入队）：加锁→真入队→解锁。**失败要 `av_packet_unref(pkt)` 释放包数据**（av_read_frame 拿到的数据是独立申请的，不释放就泄漏）。
- `packet_queue_put_private()`（真正入队）：分配节点→**浅拷贝 AVPacket（只拷结构体，data 指针不动，靠引用计数）**→若是 flush_pkt 则 serial+1→节点的 serial 设为队列当前 serial→挂链表尾→更新三个统计量→发信号。
  
  > ⚠️ 教程里那行 `printf("q->serial = %d\n", q->serial++);` 是讲师加的**调试代码且写错了（serial 自增两次）**，官方没有，学习时忽略。
- `packet_queue_get()`（出队）：加锁→`for(;;)`：没数据且要求阻塞就 `SDL_CondWait`；有数据就从队头取，更新三个统计量，把 AVPacket 和 serial 拷给调用者，**释放节点内存（不是包数据）**。返回：-1=被中止、0=没包、1=拿到包。
- `packet_queue_put_nullpacket()`（放空包）：`data=NULL, size=0`，**表示"这个流的文件数据读完了"**，让解码器把内部缓存的帧全部"吐"出来。
  
  > ⚠️ 老代码用 `av_init_packet()`，**已被移除**（FFmpeg 5.0 废弃，之后删除）。现代写法：`AVPacket *pkt = av_packet_alloc();` 用完 `av_packet_unref()`。
- `packet_queue_flush()`（清空）：遍历链表，`av_packet_unref` 释放每个包的**数据**，`av_freep` 释放**节点**，统计量归零。用在退出和 seek。

**PacketQueue 总结（重点）**：
1. 内存管理：节点内存队列自己管（put 时 malloc / get 时 free）；AVPacket 结构体跟节点共存亡；**AVPacket 指向的数据**由调用者 unref，队列只在 flush 和 put 失败时自己处理。
2. serial 变化：放 flush_pkt 时 +1，之后的新包都是新 serial，队头可能还残留旧 serial 的包——取到发现"是旧序列的"，丢弃。

### 4. struct Frame + FrameQueue —— 帧队列（环形缓冲区）
**Frame**：给音/视/字幕帧做的**通用包装**：
- `AVFrame *frame`：真正的数据；`AVSubtitle sub`：字幕专用。
- `serial`：播放序列号。
- `pts`（秒，double）、`duration`（秒）、`pos`（文件字节位置）。
- `width/height/format/sar`：宽高、格式、宽高比（sar 只有视频有意义）。
- `uploaded`：是否已上传到 SDL 纹理（重复显示同一帧不用再传）；`flip_v`：是否垂直翻转（**= `frame->linesize[0] < 0`，即行序倒立的帧**，见视频输出部分）。

**FrameQueue**：**环形缓冲区**，数组实现，固定最大 16 个槽。
- `rindex`（读索引）、`windex`（写索引）、`size`（当前帧数）、`max_size`（最大帧数）。
- **`keep_last` + `rindex_shown`：FrameQueue 最难的机制**，也是和 PacketQueue 最大的区别。
  - `keep_last=1`：队列**永远保留"最后显示过的那一帧"不删**。为什么？因为 `video_display` 显示的是"上一帧"，即使已播过也要留着供显示/重复显示。
  - `rindex_shown`：0 或 1。第一次调 `frame_queue_next()` 时若 keep_last 且 rindex_shown==0，**只把它置 1 就返回**（不真删、不移动 rindex），相当于"这帧被标记已显示但保留"；之后再调 next() 才真正删。
  - 所以"还能读多少帧" = `size - rindex_shown`（保留的那帧不算）。

**函数逐个讲**：
- `frame_queue_init()`：分配每个槽的 AVFrame 对象（**只分配结构体，数据缓冲区是 AVBuffer 引用计数，之后 move_ref 转移**）。max_size 取 min(传入, 16)。
- `frame_queue_destory()`：每槽先 unref_item（释放数据引用）再 av_frame_free（释放对象），最后销毁锁和条件变量。
- `frame_queue_peek_writable()`：**申请可写槽**。满（size>=max_size）就等条件变量，等时能检查 `abort_request`（否则退不出来）。
- `frame_queue_push()`：**更新写索引**（windex+1 取模），size+1，发信号。注意：**数据在 push 之前就写进槽里了，push 只是"宣布入队"**。
- `frame_queue_peek_readable()`：**申请可读帧**，没帧可读就等。
- `frame_queue_peek()`：读当前帧（只读不出队），地址 `queue[(rindex + rindex_shown) % max_size]`。
- `frame_queue_peek_next()`：读下一帧（确保至少 2 帧）。
- `frame_queue_peek_last()`：读上一帧（`queue[rindex]`，被保留的那帧）。
- `frame_queue_next()`：**出队**。若 keep_last 且 rindex_shown==0，只置 1 返回；否则 unref_item 释放当前读槽数据→rindex+1→size-1→发信号。
- `frame_queue_nb_remaining()`：返回 `size - rindex_shown`（未显示的帧数）。
- `frame_queue_last_pos()`：最近播放帧的文件位置，seek 用。

**写队列 3 步**：peek_writable 拿槽 → 填数据（`av_frame_move_ref` 把解码帧"搬"进槽，搬完原帧空了）→ push。**读队列 2 步**：peek/peek_readable 读 → 需要时 next 出队。

### 5. struct AudioParams —— 音频参数
5 个字段：`freq`（采样率）、`channels`（声道数）、`channel_layout`（声道布局，如 5.1）、`fmt`（采样格式，如 S16）、`frame_size`（**一个采样单元占的字节数**，双声道 16bit = 2×2 = 4 字节）、`bytes_per_sec`（每秒字节数 = 采样率×声道数×字节/样本，48kHz 双声道 16bit = 192000）。
> ⚠️ 现代 FFmpeg 更推荐用 `AVChannelLayout` 结构体代替老的 `channel_layout`(int64_t) + `channels`(int)。

### 6. struct Decoder —— 解码器封装
- `pkt`：暂存一个 pending 的包（见下）。
- `queue`：对应包队列；`avctx`：解码器上下文。
- `pkt_serial`：最近取到的包序列号；`finished`：=1 表示这个序列的解码完成了（读到 EOF 空包时置位）。
- `packet_pending`：=1 表示有个包因为 `avcodec_send_packet` 返回 EAGAIN 没送进解码器，先存在 d->pkt。
- `empty_queue_cond`：就是 `continue_read_thread`，**队列空了唤醒读线程**。
- `start_pts/start_pts_tb`：流起始时间；`next_pts/next_pts_tb`：**用上一帧推算下一帧的 pts**（音频有些帧没 pts 时兜底）。
- `decoder_tid`：解码线程句柄。

---

## 三、数据读取线程 read_thread

这是"采购员"的完整工作流程，分三部分。

### 1. 准备工作（打开文件 + 选流 + 启动解码线程）
1. **`avformat_alloc_context()`**：创建解复用器上下文 `AVFormatContext`，存进 `is->ic`。
2. **`ic->interrupt_callback.callback = decode_interrupt_cb`**：设置**中断回调**。FFmpeg 内部做耗时操作（读网络、探测格式）时**定期调用它**，返回 1 就中断退出当前操作，返回 0 继续。这样用户按退出能立刻响应。它会在 open_input / find_stream_info / av_read_frame 三处都被触发（教程用 gdb 验证过）。
3. **`avformat_open_input()`**：打开输入（文件或 RTMP/RTSP/HTTP 网络流都行，内部抽象为 URLProtocol）。`fmt` 可强制指定封装；`options`（AVDictionary）传额外参数，比如强制设 `scan_all_pmts=1`（mpegts 扫描全部 PMT 表）。
4. **`avformat_find_stream_info()`**：**为什么打开文件后还要调它？** 有些格式（TS、FLV）头部没有完整流信息，必须**读一些包**才能分析出各流编码参数、帧率等。多读的包会缓存，不跳过。
5. **检测是否指定播放起始时间**（`-ss`）：指定了就 `avformat_seek_file` 先 seek 过去。
6. **查找 AVStream**：一个文件可能有多条音/视/字幕流（比如电影有国语和英语音轨）。两种选择方式：
   - 用户指定（`-ast`/`-vst`/`-sst` + 流描述符，`avformat_match_stream_specifier` 匹配）；
   - 没指定就 `av_find_best_stream()` 自动挑"最可能符合预期"的流。
7. **计算显示窗口大小**：用流的 `AVCodecParameters` 宽高 + `av_guess_sample_aspect_ratio()` 猜的宽高比，算出默认窗口尺寸（`set_default_window_size` → `calculate_display_rect`）。**这一步只算数值，真正建窗口在显示时**。
8. **`stream_component_open()`**：打开解码器的"总开关"。内部：
   - `avcodec_alloc_context3(NULL)` 分配解码上下文 → `avcodec_parameters_to_context(avctx, codecpar)` 拷入参数 → 设 `pkt_timebase`。
   - `avcodec_find_decoder(codec_id)` 按编码 ID 找解码器；用户用 `-acodec aac` 指定就用 `avcodec_find_decoder_by_name`。
   - 按流类型走 switch：
     - **音频**：先 `audio_open()` 打开 SDL 音频设备（设好输出参数 audio_tgt）→ 初始化同步滤波参数 → `decoder_init` + `decoder_start` 启动音频解码线程 → `SDL_PauseAudioDevice(0)` 开始出声。
     - **视频**：`decoder_init` + `decoder_start` 启动视频解码线程 → 置 `queue_attachments_req=1`（等会儿处理专辑封面）。
     - **字幕**：同理启动。
   - `decoder_init`：绑定 avctx、队列、empty_queue_cond，初始化 start_pts/pkt_serial。`decoder_start`：`packet_queue_start`（放 flush_pkt）+ `SDL_CreateThread` 创建线程。

### 2. For 循环读取数据（核心主循环）
每一步：
1. **检测退出**：`is->abort_request` 为真就 break。
2. **检测暂停/继续**：状态变化时调 `av_read_pause(ic)`/`av_read_play(ic)`，**只对网络流（如 RTSP）有意义**，文件流返回 ENOSYS 忽略。
3. **检测 seek 请求**：`seek_req` 为真就执行 `avformat_seek_file`。**seek 成功后 3 件事**：清空所有 PacketQueue（`packet_queue_flush`）并放 `flush_pkt`（serial+1、解码器清缓存，防花屏）；重设外部时钟；暂停状态下 `step_to_next_frame` 显示一帧。最后 `seek_req=0`。
4. **检测 attached_pic**：有些 MP3/AAC 的封面图是一条"附加图片流"（`AV_DISPOSITION_ATTACHED_PIC`），只有一个包。放进视频队列同时放空包表示结束。这样听歌能看到封面。
5. **检测队列是否满了（背压机制，重点）**：`audioq.size+videoq.size+subtitleq.size > MAX_QUEUE_SIZE(15MB)` **或者** 三个流各自的 `stream_has_enough_packets()` 同时成立，就 `SDL_CondWaitTimeout` 睡 10ms 让解码线程消化。**这是防止读线程把内存塞爆的关键**。15MB 是经验值（4K@50Mbps 下约 2.4 秒）。
   - **`stream_has_enough_packets` 的完整 4 类判定**（上一版漏了，现在补齐）：
     ```c
     return stream_id < 0 ||          // ① 流没打开 → 视为"足够"
            queue->abort_request ||   // ② 已请求退出 → 视为"足够"
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || // ③ 封面图流 → 只有一个包，视为足够
            queue->nb_packets > MIN_FRAMES    // ④ 包数 > 25
            && (!queue->duration ||   //    且"总时长为0"（容器给不出包时长时的兜底！）
                av_q2d(st->time_base) * queue->duration > 1.0); // 或总时长 > 1 秒
     ```
     大白话：**不是只有"包够多且时长够 1 秒"才算够**。流没打开、要退出了、是封面图流，这三种情况都不能让读线程死等；特别第④条里的 `!queue->duration` 是兜底——**有些容器拿不到 packet 的 duration，如果还非要等 1 秒时长，就永远等不满，播放会卡死**，所以此时只要包数够就放行。
6. **检测码流是否播放结束**：条件 ①非暂停 ②（没音频流 或 音频解码完成且音频帧队列空）③（没视频流 或 视频解码完成且视频帧队列空）。满足后看 `loop` 和 `autoexit`：
   - `loop==0` 无限循环；`loop` 减一后还 >0 就继续 → 重新 seek 到开头；
   - `autoexit` 则退出。
7. **`av_read_frame(ic, pkt)`**：读一个 AVPacket。**每次重新申请数据、绝不帮你释放**，谁拿谁负责 unref。
8. **检测数据是否读完**（ret<0 且 EOF）：给三个队列各放**空包（nullpacket）**，通知解码器"把缓存帧吐出来"，置 `is->eof=1`，睡 10ms 继续循环（可能还要响应 seek）。
9. **检测是否在播放范围内**：`-ss` 起点、`-t` 时长时，判断包时间戳是否在范围内（`pkt_ts - start_time <= duration`），不在的直接丢。
10. **插入对应队列**：按 `pkt->stream_index` 分到 audioq/videoq/subtitleq（视频排除 attached_pic 流）；都不匹配就 `av_packet_unref`。

### 3. 退出线程处理
`avformat_close_input` 关闭解复用器 → `SDL_PushEvent(FF_QUIT_EVENT)` 发退出事件给主线程 → 释放 wait_mutex 相关资源。**线程退出的关键：所有阻塞等待都要能被 abort_request + 条件变量信号打断**，否则线程卡死退不出去。

---

## 四、音视频解码线程

### 1. 视频解码线程 video_thread
流程：
1. 拿 stream 的 time_base（后面把 pts 转秒）。
2. `av_guess_frame_rate()` 猜帧率（算每帧时长）。
3. 循环调 `get_video_frame()` 解一帧。
4. 算 `duration = 1/帧率`（没有帧率就 0）；`pts = frame->pts * av_q2d(tb)`。
5. `queue_picture()` 放帧进视频 FrameQueue。
6. `av_frame_unref(frame)` 释放（数据已被 move_ref 搬走）。
- 返回值 <0 就退出线程。

**get_video_frame 里的"提前丢帧"（Early Drop）机制——完整五要素（上一版漏了 4、5，现在补齐）**：
丢帧发生在**入队之前**。允许丢帧的条件：`framedrop>0`，或 `framedrop=-1`（默认）且主时钟不是视频。
如果进入丢帧检测，**五条必须同时成立**才丢：
```c
if (frame->pts != AV_NOPTS_VALUE) {
    double diff = dpts - get_master_clock(is);
    if (!isnan(diff) &&                        // ① 差值合法（非NaN）
        fabs(diff) < AV_NOSYNC_THRESHOLD &&    // ② 差值在 ±10 秒同步范围内
        diff - is->frame_last_filter_delay < 0 && // ③ 扣除滤镜延迟后确实落后
        is->viddec.pkt_serial == is->vidclk.serial && // ④ 解码器序列 == 视频时钟序列
        is->videoq.nb_packets) {               // ⑤ 包队列至少还有 1 个包
        is->frame_drops_early++;
        av_frame_unref(frame);
        got_picture = 0;
    }
}
```
逐条大白话：
- ① **差值合法**：diff 是 NaN 就没法比，不做判断。
- ② **±10 秒内**：如果已经离谱到差 10 秒以上，说明是源文件时间戳本身有问题，**不能乱丢**（否则全丢光）。
- ③ **确实落后**：减去滤镜延迟后还落后才丢（没滤镜时就是 diff < 0）。
- ④ **序列一致 = 已经显示过至少一帧**：`vidclk.serial` 只有在帧真正显示过（调用 update_video_pts）才会被置位。**它同时保证主时钟不是 NaN、比较有意义**——还没显示过任何帧时绝不丢帧。
- ⑤ **队列还有余粮**：包里至少还有 1 个包才敢丢当前这帧，**防止把最后一帧丢了导致黑屏**。

### 2. 核心函数 decoder_decode_frame（三种流共用）
返回：`-1` 退出、`0` 解码器冲完没帧了（=码流播完）、`1` 正常拿到一帧。`for(;;)` 大循环，三步：

**第一步：先"取"解码器已解出的帧。**
```c
if (d->queue->serial == d->pkt_serial) {   // 序列连续才取
    do {
        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR_EOF) { d->finished = ...; avcodec_flush_buffers; return 0; }
        if (ret >= 0) return 1;
    } while (ret != AVERROR(EAGAIN));
}
```
- **为什么先取帧再喂包？** 现代解码器是异步的：喂好几个包才出一个帧，甚至一个包出多帧。所以要先把攒的帧取完（取不到 EAGAIN）才去喂新包。
- `decoder_reorder_pts`：视频帧 pts 可用 `best_effort_timestamp`（默认，自动推算最合适的单调时间戳）、原始 pts、或 pkt_dts 来定。
- 遇到 `AVERROR_EOF`（读到空包冲完了）返回 0，同时 `avcodec_flush_buffers` 清缓存，**这样循环播放还能再解**。

**第二步：取包，过滤"过时"的包。**
```c
do {
    if (队列空) SDL_CondSignal(empty_queue_cond);   // 唤醒读线程
    if (d->packet_pending) { av_packet_move_ref(&pkt, &d->pkt); d->packet_pending=0; }
    else if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0) return -1;
} while (d->queue->serial != d->pkt_serial);   // 不是最新序列就继续读
```
- `packet_pending`：当 `avcodec_send_packet` 返回 EAGAIN（解码器没空间收包），**暂存到 d->pkt**，下次循环先拿出来发。
- **序列过滤**：取出的包如果 serial 和队列最新 serial 不一致（seek 前的老数据），直接 unref 扔，继续取。**这就是"seek 后旧包被清理"的机制。**

**第三步：把包喂给解码器。**
- 取到的是 **flush_pkt**：`avcodec_flush_buffers(avctx)` 清空解码器缓存帧，重置 `finished=0`、`next_pts=start_pts`。**这就是 seek 后解码器"从头来"的地方。**
- 字幕走老的 `avcodec_decode_subtitle2`；音视频走 `avcodec_send_packet`（EAGAIN 就存 pending）。
- 最后 `av_packet_unref(&pkt)`（包数据必须自己释放）。

### 3. queue_picture（视频帧入队）
`frame_queue_peek_writable` 拿可写槽 → 填 pts/duration/pos/serial/宽高/sar/format → `av_frame_move_ref` 搬数据（搬完 src 空了）→ `frame_queue_push`。

### 4. 音频解码线程 audio_thread
流程和视频几乎一样，**timebase 处理不同**：
- 视频的 pts 用 stream 的 time_base 换算；
- **音频在 `decoder_decode_frame` 里已经把 pts 从 stream time_base 转成 `{1, sample_rate}`**（`av_rescale_q(frame->pts, avctx->pkt_timebase, (AVRational){1, frame->sample_rate})`）。
- 帧没 pts 就用 `d->next_pts`（上一帧 pts + nb_samples）推算，并更新 next_pts。
- 所以 audio_thread 直接用 `tb = {1, sample_rate}` 转秒：`af->pts = frame->pts * av_q2d(tb)`，duration = `nb_samples / sample_rate`。
- 之后 peek_writable → move_ref → push 进 sampq。

---

## 五、音频输出和重采样

### 1. 为什么要"二级缓冲"？
SDL 音频是**被动回调**：内部有个音频线程，**每隔固定时间回调你的 `sdl_audio_callback`，跟你要固定长度的数据**。但解码出来的一帧音频长度 ≠ SDL 每次要的长度（尤其变速时更对不上）。所以中间必须加一级缓冲 `audio_buf`：
```
sdl_audio_callback 要数据
   → 先看 audio_buf 还有没有剩（audio_buf_index < audio_buf_size）
   → 有：从 audio_buf 拷给 SDL
   → 没有：调 audio_decode_frame 从 sampq 取一帧填满 audio_buf，再拷
```

### 2. audio_open（打开 SDL 音频设备）
- 目标是**找一个 SDL 设备能接受的参数组合**。先按解码器参数请求，失败就降级（依次换声道数、换采样率 44100→48000→96000→192000）。
- 期望格式 `AUDIO_S16SYS`（SDL 不支持 planar 格式，S16 是交错格式）。
- `wanted_spec.samples` 控制一次回调给多少样本，**必须是 2 的幂**（代码注释写了 `power of 2`，实现用 `2 << av_log2(...)` 保证）——这是 SDL 的硬性约束，不遵守可能打不开设备。
- `SDL_OpenAudioDevice` 打开后，**实际参数存 spec，再换算成 FFmpeg 参数存进 `audio_hw_params`（即 is->audio_tgt）**：fmt/freq/channels/channel_layout/frame_size/bytes_per_sec。**这就是重采样的目标格式。**
- 返回 `spec.size`（硬件内部缓冲字节数），上层存 `audio_hw_buf_size`。

### 3. sdl_audio_callback（每次跟你要 len 字节）
```c
while (len > 0) {
    if (audio_buf 用完了) {
        audio_size = audio_decode_frame(is);   // 从 sampq 取一帧填 audio_buf
        if (audio_size < 0) 填静音(全0)；
        audio_buf_size = audio_size; audio_buf_index = 0;
    }
    拷 min(剩余, len) 字节给 stream；
    静音或音量不满就 memset 或 SDL_MixAudioFormat 混音；
    len -= len1; stream += len1; audio_buf_index += len1;
}
最后 audio_write_buf_size = audio_buf_size - audio_buf_index;
set_clock_at 更新音频时钟 audclk；sync_clock_to_slave 顺带更新 extclk。
```
关键点：
- **音量在这里做**：最大音量直接 memcpy；否则 `SDL_MixAudioFormat`（顺便混音）；静音 memset 全 0。
- **音频时钟更新公式**（同步要考）：
  ```
  set_clock_at(audclk,
      is->audio_clock - (2*audio_hw_buf_size + audio_write_buf_size) / bytes_per_sec,
      serial, 当前系统时间)
  ```
  大白话：`audio_clock` 是"audio_buf 结尾的时间点"，但 SDL 硬件里还囤着 `2*audio_hw_buf_size`（典型双缓冲）加 audio_buf 里没拷出去的部分都没播，**要减掉这些才是"现在正在播的那个声音的时刻"**。

### 4. audio_decode_frame（名字带 decode，实际不解码！）
它只是"**从 sampq 取一帧 → 必要时重采样 → 放进 audio_buf**"：
1. 暂停直接返回 -1。
2. `do { af = peek_readable; frame_queue_next; } while (af->serial != audioq.serial)`：**serial 不连续（seek 过）就把旧帧丢继续取**。
3. 算帧字节数 `av_samples_get_buffer_size`，得到声道布局。
4. `wanted_nb_samples = synchronize_audio(...)`：同步可能要调样本数。
5. 判断**是否需要重采样**：帧的 format/channel_layout/sample_rate 与 audio_src 不一致（或样本数被调过），就建重采样上下文（源=帧参数，目标=audio_tgt），并把 audio_src 更新为帧参数（下次同参数不重建）。
6. 有 swr_ctx 就 `swr_convert` 重采样到 audio_buf1；否则 `audio_buf = frame->data[0]` 零拷贝。
7. 更新 `audio_clock = af->pts + nb_samples/sample_rate`（audio_buf 结尾时间）和 `audio_clock_serial`。
8. 返回 resampled_data_size。

### 5. 重采样 + 样本补偿
- `out_count = wanted_nb_samples * audio_tgt.freq / frame->sample_rate + 256`（估算输出样本数，+256 留余量）。
- 如果 `wanted_nb_samples != frame->nb_samples`（样本数被同步逻辑调过），`swr_set_compensation(swr_ctx, sample_delta, compensation_distance)` 做**软补偿**：
  - `sample_delta` = (目标样本数 - 原样本数) × 输出采样率 / 输入采样率 —— 每个补偿周期增减多少样本；
  - `compensation_distance` = 目标样本数 × 输出采样率 / 输入采样率 —— 补偿周期长度。
  - 大白话：**不是一口气砍掉/补上样本，而是让重采样器在这段距离里均匀地多/少产出样本，听感平滑**。这就是音频同步不破音的原理（第七部分详讲）。
  
  > ⚠️ 老代码用 `swr_alloc_set_opts`，**FFmpeg 7.0 已移除，必须用 `swr_alloc_set_opts2`**（详见第十一部分）。

---

## 六、视频输出和尺寸变换

### 1. 视频输出初始化
- main 里 `SDL_Init`、`SDL_CreateWindow`（用之前算好的 default_width/height）、`SDL_CreateRenderer`（优先硬件加速）。
- `stream_open` 创建 read_thread，`event_loop` 进入事件循环（视频显示也在这里驱动）。

### 2. calculate_display_rect —— 窗口怎么放视频（含偶数对齐细节）
**上一版漏了 `& ~1`，现在补齐**：
```c
// 先以高度为基准
height = scr_height;
width  = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;  // 取偶数宽度
if (width > scr_width) {
    width  = scr_width;
    height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1; // 取偶数高度
}
x = (scr_width - width) / 2;   // 居中
y = (scr_height - height) / 2;
```
逻辑：用 `pic_sar`（宽高比）乘像素宽高得到真实显示宽高比 → 先以窗口高度为基准算宽度，超宽就以宽度为基准重算高度 → 居中，超出的留黑边（letterbox）。**这就是"怎么拉伸窗口都不变形"的原理。**

**`& ~1` 是干什么的？** 把结果**取偶数**（奇数就减 1）。原因是 **YUV 4:2:0/4:2:2 的色度下采样要求宽、高都是偶数**，奇数宽高的帧在格式转换/解码时可能出错甚至崩溃。所以宽和高都做了这个对齐。（注意：审阅里"显卡对齐"的说法不准，官方注释写的就是"取偶数宽度"，本质是满足色度下采样。）

### 3. video_refresh（主线程每帧调用的核心显示函数）
流程：
1. 队列没帧（`nb_remaining==0`）直接去 display（显示上一帧）。
2. 取 `lastvp`（上帧）、`vp`（待显示帧）。
3. **vp 的 serial 和 videoq 不一致**（seek 过）→ `frame_queue_next` 丢旧帧，goto retry。
4. lastvp 和 vp 的 serial 不一致（新序列第一帧）→ 重置 `frame_timer = 当前时间`。
5. 暂停则 goto display（一直显示上一帧）。
6. `last_duration = vp_duration(lastvp, vp)`：算上一帧该显示多久（优先两帧 pts 差，异常时用入队时的 vp->duration，跨序列返回 0）。
7. `delay = compute_target_delay(last_duration, is)`：根据主时钟做同步修正（第七部分）。
8. `time < frame_timer + delay` → 没到该换帧，`remaining_time = 还要等多久`，goto display（**继续显示上一帧 = 视频快了就多等**）。
9. 该换帧了：`frame_timer += delay`（校正大偏差）、`update_video_pts` 更新 vidclk。
10. **丢帧判断**：队列还有下一帧，且 `time > frame_timer + duration`（确实晚了），且非逐帧、且允许丢帧 → `frame_queue_next` 丢 vp，goto retry（**视频慢了就丢帧追**）。计数 `frame_drops_late`。
11. 正常：`frame_queue_next` 出队 vp（现在 vp 变"上一帧"），置 `force_refresh=1`，逐帧模式则暂停。
12. display：`video_display` 显示"上一帧"。

`vp_duration`：同序列取 nextpts-pts；异常（NaN/<=0/超 max_frame_duration）用帧长兜底；跨序列返回 0。

### 4. video_display → video_image_display → upload_texture
- `video_display`：开窗口（若没开）、清屏、有视频流调 `video_image_display`、`SDL_RenderPresent` 上屏。
- `video_image_display`：`frame_queue_peek_last` 取帧 → `calculate_display_rect` 算区域 → 若 `!uploaded` 调 `upload_texture` → `SDL_RenderCopyEx` 拷纹理。
  
  - **`flip_v` 的触发**（上一版漏了）：`vp->flip_v = vp->frame->linesize[0] < 0;`——**当第一行 linesize 为负，说明数据是倒着存的（bottom-up，常见于某些图片解码器和 BMP）**，此时 `SDL_RenderCopyEx` 传 `SDL_FLIP_VERTICAL` 做垂直翻转，否则画面上下颠倒。
- **`upload_texture`（格式转换的关键）**：
  1. `get_sdl_pix_fmt_and_blendmode`：**查映射表**把 FFmpeg 像素格式映射成 SDL 格式。能匹配（如 YUV420P→SDL_PIXELFORMAT_IYUV）就是 SDL 直接支持；匹配不到返回 UNKNOWN（SDL 不支持）。
  2. `realloc_texture`：纹理不存在/尺寸/格式变了就销毁重建。**注意窗口大小变化不会触发重建**（纹理跟视频帧尺寸走）。
  3. 三种情况：
     - 映射 `SDL_PIXELFORMAT_IYUV`：`SDL_UpdateYUVTexture` 直接传 Y/U/V 三平面（**零拷贝**）。
     - 映射其他 SDL 支持格式（如 RGB32）：`SDL_UpdateTexture` 直接传。
     - 映射 **UNKNOWN（SDL 不支持）**：用 `sws_scale` 转成 `AV_PIX_FMT_BGRA`（SDL 的 BGRA8888）。做法：`SDL_LockTexture` 锁纹理拿像素指针 → `sws_scale` 直接写进纹理 → `SDL_UnlockTexture`。**转换结果直接进纹理，避免二次拷贝。**

  - **负 linesize 的指针偏移（上一版漏了，最容易踩坑，现在补齐）**：
    当 `frame->linesize[0] < 0`（倒立帧）更新 SDL 纹理时，**不能直接把 `data[0]` 传给 SDL**——因为 `data[0]` 指向的是**最后一行**而不是第一行，直接传会上下颠倒/花屏。必须跳到**末行首地址**并把步长取反：
    ```c
    // IYUV 分支：
    frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]
    frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1]
    frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]
    // default 分支：
    frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]
    ```
    **补充一点**：UNKNOWN 分支（走 sws_scale）**不用**担心这个——`sws_scale` 的 `srcStride[]` 本身就支持传负 linesize，内部会正确处理倒立输入。所以负 stride 问题只发生在"直接走 SDL_UpdateYUVTexture/SDL_UpdateTexture"这两条零拷贝路径上。

### 5. 图像格式转换 sws_scale 家族
- `sws_getContext`：分配缩放/格式转换上下文，传源/目标宽高格式 + 缩放算法 flags。一条龙做**色彩空间转换 + 分辨率缩放 + 滤波**。缺点比 libyuv/GPU shader 慢。
- `sws_getCachedContext`：传旧 context，参数没变复用，变了重分配——**避免频繁创建销毁**。
- `sws_scale`：真正转换一帧。参数注意：`srcSlice[]` 是各平面数据指针（YUV420P 是 data[0]=Y,data[1]=U,data[2]=V），`srcStride[]` 是各行字节数（**linesize 不一定等于宽度**，因为要对齐，每行后面可能有填充字节，用 OpenGL 等特别注意），srcSliceY/srcSliceH 指定处理的行区间（方便多线程并行）。
- `sws_freeContext`：释放。

### 6. sws_scale 算法性能（教程实验，参考价值）
- 缩小（1920x1080→400x300）：**SWS_POINT 最快（427帧/秒）**，SWS_FAST_BILINEAR 也快（228），其余 30~116。
- 放大（1024x768→1920x1080）：差别不大，POINT 有锯齿，AREA 锯齿轻。
- **结论经验**：不明确放大缩小就无脑 `SWS_FAST_BILINEAR`（又快又好）；明确缩小用 POINT；追求质量选慢算法（SINC/SPLINE 等）。

---

## 七、音视频同步基础

### 1. 为什么需要同步
音、视频在**不同线程**输出，解出同一时间点帧的节奏不同，源文件 PTS 甚至不连续或有错。所以要拿一个"主时钟"当裁判，让另一个跟它对齐。

### 2. 三种同步策略
1. **以音频为基准（默认，AV_SYNC_AUDIO_MASTER）**：视频慢了→丢帧；视频快了→重复上一帧。**最常用**，因为人耳对声音不连续比眼睛对画面不连续敏感得多。
2. **以视频为基准（AV_SYNC_VIDEO_MASTER）**：音频慢了→加快/丢样本；音频快了→放慢/重复样本。**调音频靠重采样实现**。
3. **以外部时钟为基准（AV_SYNC_EXTERNAL_CLOCK）**：音视频都跟 extclk 走。教程说 seek 时体验很差，不推荐。
- 还有"FREE RUN 不同步"，碰坏封装时用。

### 3. 基础概念：PTS / DTS / time_base
- **DTS（解码时间戳）**：什么时候解码这帧。
- **PTS（显示时间戳）**：什么时候显示这帧。
- 有 **B 帧**时 DTS 和 PTS 顺序不一样（B 帧要等后面帧才能解，但显示顺序在前）。
- **time_base**：PTS 的单位。`AVRational{num, den}` 表示 num/den 秒。如 `{1,1000}` 表示毫秒，PTS=1000 就是第 1 秒。
- 换算：
  - `av_q2d(tb) = num/(double)den`。
  - 时间(秒) = `PTS * av_q2d(time_base)`。
  - `av_rescale_q(a, bq, cq)`：把 PTS 从一种时间基换到另一种（不丢精度）。
- `AV_TIME_BASE = 1,000,000`：FFmpeg 内部微秒单位；`AV_TIME_BASE_Q = {1, AV_TIME_BASE}` 是它的分数形式。

### 4. 各结构体的 time_base / duration
- `AVFormatContext.duration`：整个码流时长，单位 AV_TIME_BASE（除以 1e6 才是秒）。
- `AVStream.time_base`：由封装决定——TS 是 `{1,90000}`，FLV 是 `{1,1000}`，MP4 是 `{1, timescale}`（每轨道有自己的 timescale）。
- **AVPacket** 的 pts/dts/duration 都以 `AVStream->time_base` 为单位。
- **AVFrame** 的 pts/duration 也以 `AVStream->time_base` 为单位。
- 所以 ffplay 自己封装的 Frame 里又放 `pts/duration`（**double 秒**），就是在"ffmpeg 时间基"和"同步用的秒"之间搭桥。

### 5. ffplay 里 PTS 的三次转换（重点）
**视频**：
1. `decoder_decode_frame` 里用 `best_effort_timestamp` 得到 frame->pts（单位 stream time_base）。
2. `video_thread` 里 `pts(秒) = frame->pts * av_q2d(stream_time_base)`。
3. 存进 Frame.pts（double 秒），同步、显示都用它。

**音频**：
1. **第一次**：在 decoder_decode_frame 里把 pts 从 stream time_base 转成 `{1, 采样率}`：`av_rescale_q(frame->pts, avctx->pkt_timebase, {1,sample_rate})`。
2. **第二次**：audio_thread 里 `af->pts(秒) = frame->pts * av_q2d({1,sample_rate})`。
3. **第三次**：sdl_audio_callback 里按"实际已播了多少"往回修正（`audio_clock - (2*hw_buf + write_buf)/bytes_per_sec`），得到真正正在播的时刻。

---

## 八、音视频同步的具体实现

### 1. 以音频为基准（默认）
**音频主流程**：在 sdl_audio_callback 用 `set_clock_at` 维护 audclk（公式上面讲过），并 `sync_clock_to_slave(extclk, audclk)` 顺带校准外部时钟。

**视频主流程（video_refresh）**：
核心是 `compute_target_delay(last_duration, is)`：
```
diff = vidclk 的时间 - 主时钟(音频) 的时间
sync_threshold = 夹在 [0.04秒, 0.1秒] 之间
在准同步区(|diff| < sync_threshold)：不改，直接返回原 delay
diff <= -sync_threshold（视频慢了）：delay = max(0, delay + diff)  → 立即换帧
diff >= sync_threshold（视频快了）：
    delay > AV_SYNC_FRAMEDUP_THRESHOLD(0.1) 时：delay += diff（再多等一会儿）
    否则：delay = 2*delay（让上一帧再显示一帧）
```
大白话：**diff 就是"视频比音频超前/落后多少"**。
- 视频超前太多 → 让上一帧多显示一会儿（delay 翻倍或加 diff），等音频；
- 视频落后太多 → 把 delay 砍到 0，立刻切下一帧（配合丢帧逻辑）；
- 误差在 ±0.04~0.1 秒内属"准同步"，不管它——**同步不是无时无刻较劲，是有死区的**。
配合 video_refresh 里 `frame_timer`：`time < frame_timer + delay` 继续显示上一帧；时间到了切帧；又晚了超过一帧就丢帧。**这就是"快就重复、慢就丢帧"的完整机制。**

### 2. 以视频为基准
- 视频主流程：按帧间隔正常播放，`update_video_pts` 维护 vidclk（自己是标准，不做修正）。
- 音频主流程：由 **`synchronize_audio()`** 根据 vidclk 算该输出多少样本：
  ```
  diff = audclk - 主时钟(视频)
  audio_diff_cum = diff + coef * audio_diff_cum      // 指数加权累加，越近的 diff 权重越大
  攒够 AUDIO_DIFF_AVG_NB(20) 次后：
  avg_diff = audio_diff_cum * (1 - coef)
  if (|avg_diff| >= audio_diff_threshold) {          // 超过"准同步区"才调
      wanted = nb_samples + diff * 采样率             // 差多少时间×采样率 = 该补/减多少样本
      wanted = clamp(wanted, 90%*nb, 110%*nb)        // 每次最多调 ±10%，防音调大变
  }
  ```
- 大白话：
  - 差多久 × 采样率 = 该增加/减少多少样本。音频快了（wanted 变大）→ 输出更多样本 → 播放变慢等视频；音频慢了 → 输出更少样本 → 播放变快追视频。
  - 为什么限 ±10%？**增减样本但采样率不变 = 播放时长变化 = 声音整体频率被拉高/拉低 = 变音调（变尖/变粗）**。限幅避免明显变调。
  - `audio_diff_threshold = audio_hw_buf_size / bytes_per_sec`（音频设备缓冲时长），作"准同步死区"。
- 然后重采样环节用 `swr_set_compensation` 软补偿（前面讲过）：均匀地在补偿距离内多/少产出样本，听感平滑。
- 总结：**视频同步靠"丢/重复整帧画面"（眼睛能容忍），音频同步靠"重采样微调样本数"（耳朵很挑剔）。**

### 3. 以外部时钟为基准
- 就是前两种"叠加"：视频用 `compute_target_delay` 跟 extclk 比，音频用 `synchronize_audio` 跟 extclk 比。
- **extclk 由谁对时？** 靠 `sync_clock_to_slave(&is->extclk, &audclk/vidclk)`——视频/音频显示输出时，**用已同步好的 audclk 或 vidclk 反过来给 extclk 对时**。
- **`sync_clock_to_slave` 的完整判定条件（上一版漏了，现在补齐）**：
  ```c
  if (!isnan(slave_clock) &&
      (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
      set_clock(c, slave_clock, slave->serial);
  ```
  即**不是无条件对时**——只有当主时钟还没建立（isnan(clock)）**或**主从偏差超过 10 秒（AV_NOSYNC_THRESHOLD）时才强行用从时钟覆盖主时钟。大白话：**extclk 不会被音视频的微小抖动反复覆盖**，只有"主时钟还没建立"或"两者已经离谱到超过 10s"才校正。这个 10 秒阈值同时是"先有鸡还是先有蛋"问题的解：第一帧出来前 clock 是 NaN，不做任何校正；第一帧显示后 extclk 被对时，进入正常循环。所以"先有蛋"（先让音视频自己跑起来对时 extclk）。

---

## 九、暂停、逐帧、音量

### 1. 暂停/继续（toggle_pause → stream_toggle_pause）
- 暂停本质是"**让各时钟和 frame_timer 停下来**"。
- 关键难点：**恢复时暂停期间流逝的系统时间不能算进播放进度**。所以恢复时 `frame_timer += 当前系统时间 - vidclk.last_updated`（把暂停耽误的时间"补"进 frame_timer，画面接着暂停前节奏走）。
- 然后把 `is->paused = audclk.paused = vidclk.paused = extclk.paused = !is->paused`，四个时钟同步翻转。

### 2. 暂停时的视频
`video_refresh` 里 `if (is->paused) goto display;` → 画面**停在最后一帧**不动（永远显示上一帧）。

### 3. 暂停时的音频
`audio_decode_frame` 里 `if (is->paused) return -1;` → 回调拿不到数据就**填全 0（静音）**。声音停止。
读线程不停止：包队列满时休眠，但要能响应继续播放/seek，所以不能死等。

### 4. 逐帧（step_to_next_frame）
本质：**播放一帧画面，然后立即暂停**。
按 s 键 → 若暂停先恢复播放 → 置 `is->step=1` → video_refresh 里显示一帧后 `if (is->step && !is->paused) stream_toggle_pause(is)` 再暂停。
逐帧时禁用丢帧逻辑（`!is->step` 条件），保证一帧不少。

### 5. 音量 / 静音
音量本质 = **控制采样点的幅值**。最大音量=直接输出原始数据；静音=输出全 0；其他=用 `SDL_MixAudioFormat` 调幅值。
`toggle_mute` 翻转 `is->muted`。回调里：muted 或 audio_buf 空 → `memset(stream, 0, len1)` 填 0；否则 `SDL_MixAudioFormat(stream, audio_buf+index, AUDIO_S16SYS, len1, audio_volume)` 按音量混音（音量满时直接 memcpy，省一次混音开销）。

---

## 十、快进快退 seek

### 1. seek 的本质和整体步骤
快进/快退/拖进度条，本质**都是 seek 到某个点重新播放**。完整四步：
1. `avformat_seek_file` 让**解复用器**跳到指定位置（文件内部可能定位到某个关键帧）；
2. **清空 packet 队列**（packet_queue_flush）并放 flush_pkt（serial+1、解码器清缓存、防花屏）；
3. **清空 frame 队列**（靠 serial 机制：解码/显示线程发现 serial 不对自动丢旧帧）；
4. 重置时钟序列（重设 extclk；seek 时 `set_clock(extclk, NAN, 0)` 或用 seek 目标时间对时）。
按键：左=后退10秒、右=前进10秒、上=前进60秒、下=后退60秒、鼠标右键拖动=seek 指定位置；最终都调 `stream_seek`。

### 2. 数据结构与 SEEK 标志
- `seek_req`（有请求吗）、`seek_flags`（AVSEEK_FLAG_BYTE/FRAME/ANY/BACKWARD 等）、`seek_pos`（目标=当前位置+增量）、`seek_rel`（增量）。
- 读线程主循环：`seek_target = seek_pos`；`seek_min = seek_rel>0 ? seek_target - seek_rel + 2 : INT64_MIN`（向前 seek 的最小位置，留 `+-2` 容错），`seek_max` 同理。
- `avformat_seek_file(s, stream_index, min_ts, ts, max_ts, flags)`：目标尽力接近 ts 并保证在 [min_ts, max_ts] 内——**因为 ts 那个点未必有可正常播放的帧（关键帧），允许范围内就近找**。stream_index=-1 表示用 AV_TIME_BASE 单位。

### 3. avformat_seek_file 内部调用链
- 解复用器实现 `read_seek2`（新接口）就调它；
- 否则回退老接口：`avformat_seek_file → av_seek_frame → seek_frame_internal`。

### 4. seek_frame_internal
- **AVSEEK_FLAG_BYTE**（字节定位，TS 走这条）→ `seek_frame_byte`。
- 否则（时间戳定位，MP4 走这条）→ 流索引<0 先找默认流并换算时间戳到该流 time_base → 优先调 `read_seek`（MP4 即 mov_read_seek）→ 不行再二进制搜索/通用搜索。

### 5. MP4 vs TS 的 seek 原理（考试爱考）
- **MP4**：有 moov 里的 **Sample Table 索引**，能按时间戳**精确快速**定位数据包 → `mov_read_seek`。快。
- **TS**：没有时间戳↔文件位置索引，只能**按字节位置跳**（seek_frame_byte 直接把文件指针拨到那个 pos），靠 `mpegts_resync` 找 0x47 包头重新同步。而且**真正到目标时间要靠"先估位置、读了包看时间戳、偏了就前后找"的逼近过程** → 慢。CBR 按码率估得准，seek 快些；VBR 估不准，可能来回找，很慢。

### 6. 退出播放
关闭流、销毁队列、销毁线程。**线程怎么退出是关键**：所有阻塞等待（读包、取帧、等队列）都必须能通过 abort_request + 条件变量信号被唤醒，才能顺利收工。流程：`do_exit()` → 设 abort_request → 发信号唤醒各线程 → join → 释放资源 → 退出。

---

## 十一、现代技术栈纠正（编译不过的硬伤，必看）

1. **版本**：这套课基于 **FFmpeg 4.2.1（2019）**，你现在用 **FFmpeg 8.0 / 9.0**。ffplay.c 早在 4.3 就从根目录**移到了 `fftools/`**。
2. **`av_init_packet()` 已移除**（5.0 废弃后删除）。现在统一 `av_packet_alloc()` / `av_packet_unref()`，AVPacket 不允许在栈上声明。
3. **`swr_alloc_set_opts` 已移除——这是最容易编译失败的点**。FFmpeg 官方 APIchanges 记录：`2022-03-15 - swr 4.5.100` 添加 `swr_alloc_set_opts2()` 并**废弃** `swr_alloc_set_opts()` [cite:e4e92b95-1]；到 **FFmpeg 7.0 已彻底删除**。FFmpeg 8/9 里必须用 `swr_alloc_set_opts2`，且参数从"`channel_layout`(int64_t)+`channels`(int)"换成 `AVChannelLayout`。新写法示例：
   ```c
   AVChannelLayout src_layout, dst_layout;
   av_channel_layout_default(&src_layout, src_channels);   // 或从 frame 拷贝
   av_channel_layout_copy(&dst_layout, &target_layout);    // 目标=SDL 设备布局
   swr_alloc_set_opts2(&swr_ctx,
                       &dst_layout, AV_SAMPLE_FMT_S16, dst_sample_rate,
                       &src_layout, src_fmt,        src_sample_rate,
                       0, NULL);
   swr_init(swr_ctx);
   // 用完：
   av_channel_layout_uninit(&src_layout);
   av_channel_layout_uninit(&dst_layout);
   ```
   配套注意：`av_channel_layout_default()` 代替 `av_get_default_channel_layout()`，`av_channel_layout_copy()` 代替赋值拷贝，`av_channel_layout_uninit()` 代替 `memset`。
4. **字幕解码**：教程走老的 `avcodec_decode_subtitle2`，新版虽然还有，但建议统一理解成 send/receive 模式。
5. **教程一处代码 bug**：`packet_queue_put_private` 里的 `printf("q->serial = %d\n", q->serial++);` 是讲师调试语句且**写错（serial 自增两次）**，学习时忽略。
6. **SDL**：这套课用 SDL2（不是上古 SDL1），没问题；SDL3 已发布，新项目可考虑。
7. **风格建议**：ffplay 是纯 C + 全局变量 + 手写队列。**你用 Qt6 + C++17 不必照搬**——更现代的做法是用标准库 `std::queue`/`std::mutex`/`std::condition_variable`、RAII 管理资源、用 QMediaPlayer 或封装类。**但"读线程→包队列→解码线程→帧队列→渲染"的架构，以及 serial、flush_pkt、二级缓冲、主从时钟同步这些设计，到今天依然正确，值得照抄**——学的是道理，不是代码。

---

## 十二、遗漏检查清单（对照全部 9 份 PDF + 上一轮审阅逐条核对）

**结构体（PDF 1-3）**：
- [x] ffplay 意义、ijkplayer 背景
- [x] 框架、线程划分（读/音/视/字解码/主线程）
- [x] VideoState 全部字段（含时钟、三队列、三解码器、音频缓冲组、同步参数、滤镜、窗口、退出等）
- [x] Clock 全部字段 + pts_drift 对时原理
- [x] MyAVPacketList / PacketQueue 及 init/destroy/start/abort/put/put_private/get/put_nullpacket/flush
- [x] flush_pkt 作用、serial 变化过程、PacketQueue 内存管理总结
- [x] Frame / FrameQueue 及 init/destroy/peek_writable/push/peek_readable/peek/peek_next/peek_last/next/nb_remaining
- [x] keep_last + rindex_shown 保留最后一帧机制
- [x] AudioParams 五个字段 + 计算
- [x] Decoder 封装（pkt_serial/finished/packet_pending/empty_queue_cond/start_pts/next_pts）

**读线程（PDF 4）**：
- [x] 准备 8 步（alloc_context / interrupt_callback / open_input / find_stream_info / -ss 起始 / 选流 / 窗口尺寸 / stream_component_open）
- [x] For 循环 10 步（退出 / 暂停(网络流) / seek / attached_pic / 背压 / 播放结束 loop与autoexit / av_read_frame / EOF 空包 / 播放范围 / 入队）
- [x] **stream_has_enough_packets 完整 4 类边界（含 !duration 兜底）** ← 审阅第 3 条已补
- [x] 退出线程处理

**解码线程（PDF 5-6）**：
- [x] video_thread 全流程
- [x] **get_video_frame Early Drop 完整五要素（含序列一致、队列有余粮）** ← 审阅第 2 条已补
- [x] decoder_decode_frame 三步（先取帧 / 取包过滤 / 送包）+ flush_pkt + packet_pending + serial 过滤
- [x] queue_picture
- [x] audio_thread + 音频 pts 三次换算差异（{1, sample_rate}）

**音频输出/重采样（PDF 7-8）**：
- [x] 二级缓冲模型
- [x] audio_open（降级尝试、S16SYS、**samples 必须 2 的幂** ← 自补细节）
- [x] sdl_audio_callback（音量/静音、audclk 更新公式）
- [x] audio_decode_frame（取帧、serial 丢帧、重采样判断、audio_clock 更新）
- [x] 重采样逻辑 + swr_set_compensation 软补偿

**视频输出/尺寸（PDF 9-10）**：
- [x] 输出初始化（SDL_Init / 窗口 / renderer）
- [x] **calculate_display_rect + `& ~1` 偶数对齐（含原因修正为色度下采样）** ← 审阅第 1 条已补
- [x] video_refresh 全流程（帧时长 / 重复 / 丢帧）
- [x] **flip_v = linesize[0] < 0 触发** ← 审阅第 4 条已补
- [x] **负 linesize 指针偏移（末行首地址 + 步长取反；UNKNOWN 分支无需处理的原因）** ← 审阅第 5 条已补
- [x] upload_texture 三种情况 + 像素映射表 + realloc_texture
- [x] sws_getContext / getCachedContext / scale / freeContext + 性能测试结论

**同步基础（PDF 11）**：
- [x] 三种同步策略
- [x] PTS/DTS/time_base 概念、AV_TIME_BASE 换算
- [x] 各结构体 time_base/duration 对比（TS/FLV/MP4）
- [x] 视频/音频 PTS 的三次转换流程

**同步实现（PDF 12-14）**：
- [x] audio master（音频主流程 / 视频主流程 / compute_target_delay + sync_threshold）
- [x] video master（synchronize_audio 指数加权 / 样本调整 / ±10% 限幅 / audio_diff_threshold）
- [x] swr_set_compensation 参数含义
- [x] ext master（**sync_clock_to_slave 的 NaN 或 >10s 阈值判定** ← 审阅第 6 条已补；先有鸡还是先有蛋的解法）

**暂停/逐帧/音量（PDF 15-16）**：
- [x] toggle_pause → stream_toggle_pause（frame_timer 补时间、四时钟翻转）
- [x] 暂停时视频/音频行为
- [x] 逐帧 step_to_next_frame
- [x] 音量/静音（SDL_MixAudioFormat）

**seek（PDF 17）**：
- [x] seek 四步（解复用 seek / 清 packet + flush_pkt / 清 frame 靠 serial / 重置时钟）
- [x] seek_req/flags/pos/rel、avformat_seek_file 参数与 [min,max]
- [x] avformat_seek_file → av_seek_frame → seek_frame_internal
- [x] seek_frame_byte（TS）/ mov_read_seek（MP4）对比 + CBR/VBR
- [x] 退出播放（线程退出机制）

**现代迁移（自补 + 审阅第 7 条）**：
- [x] av_init_packet 移除、av_packet_alloc 替代
- [x] **AVChannelLayout 迁移 + swr_alloc_set_opts → swr_alloc_set_opts2（官方 APIchanges 证据）**
- [x] ffplay.c 移到 fftools、SDL3、Qt6+C++17 架构建议
