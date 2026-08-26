#include <iostream>
#include <iomanip>
#include <string>
#include <memory>

extern "C" {
	#include "libavformat/avformat.h"   
	#include "libavcodec/avcodec.h"     
	#include "libavutil/avutil.h"       
	#include "libavutil/timestamp.h" 
}

void print_ffmpeg_error(int errnum, const std::string& prefix)
{
	char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	av_strerror(errnum, errbuf, sizeof(errbuf));
	std::cerr << prefix << ":" << errbuf << "number:" << errnum << "\n";
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "Usage: " << argv[0] << " <input_media_file>\n";
		return -1;
	}
	const char* input_url = argv[1];

	AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, input_url, nullptr, nullptr);
	if (ret < 0)
	{
		print_ffmpeg_error(ret, "avformat_open_input failed");
		return -1;
	}

	auto fmt_guard = std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>(fmt_ctx, [](AVFormatContext* p) { avformat_close_input(&p); });

	ret = avformat_find_stream_info(fmt_ctx, nullptr);
	if (ret < 0)
	{
		print_ffmpeg_error(ret, "avformat_find_stream_info failed");
		return -1;
	}

	av_dump_format(fmt_ctx, 0, input_url, 0);

	int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

	std::cout << "==========================================\n";
	std::cout << "Video Stream Index: " << video_stream_idx << "\n";
    std::cout << "Audio Stream Index: " << audio_stream_idx << "\n";

	AVPacket* pkt = av_packet_alloc();
	if (!pkt)
	{
		std::cerr << "av_packet_alloc failed\n";
		return -1;

	}

	int64_t video_pkt_count = 0;
	int64_t audio_pkt_count = 0;
	int64_t video_bytes = 0;
	int64_t audio_bytes = 0;

	while ((ret = av_read_frame(fmt_ctx, pkt)) >= 0)
	{
		AVStream* stream = fmt_ctx->streams[pkt->stream_index];

		if (pkt->stream_index == video_stream_idx)
		{
			video_pkt_count++;
			video_bytes += pkt->size;

			if (video_pkt_count <= 5)
			{
				double pts_sec = (pkt->pts != AV_NOPTS_VALUE) ? (pkt->pts * av_q2d(stream->time_base)) : -1.0;
				double dts_sec = (pkt->dts != AV_NOPTS_VALUE) ? (pkt->dts * av_q2d(stream->time_base)) : -1.0;
				bool is_key = (pkt->flags & AV_PKT_FLAG_KEY);  // 是否为关键帧

				std::cout << "[Video PKT] #" << std::setw(3) << video_pkt_count
					<< " | KeyFrame: " << (is_key ? "YES" : " NO")
					<< " | Size: " << std::setw(6) << pkt->size << "B"
					<< " | PTS: " << std::fixed << std::setprecision(3) << pts_sec << "s"
					<< " | DTS: " << std::fixed << std::setprecision(3) << dts_sec << "s\n";
			}
			

		}
		else if (pkt->stream_index == audio_stream_idx)
		{
			audio_pkt_count++;
			audio_bytes += pkt->size;
		}
		av_packet_unref(pkt);
		
	}
	if (ret != AVERROR_EOF && ret < 0) {
		print_ffmpeg_error(ret, "av_read_frame read error");
	}

	// ---------- 6. 输出统计报告 ----------
	std::cout << "\n================= 解封装统计 =================\n";
	std::cout << "Video Packets : " << video_pkt_count << " | Total Size: " << video_bytes / 1024.0 << " KB\n";
	std::cout << "Audio Packets : " << audio_pkt_count << " | Total Size: " << audio_bytes / 1024.0 << " KB\n";
	std::cout << "=============================================\n";

	// 返回 0 表示成功
	return 0;
}