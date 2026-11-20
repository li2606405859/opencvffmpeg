#include <cstdio>
#include <cstring>
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
}

int main()
{
    const char* input_file = "res/test.mp4";
    AVFormatContext* input_format_ctx = nullptr;
    int video_stream_idx = -1;
    char err2str[AV_ERROR_MAX_STRING_SIZE] = {0};
    int ret;

    // ========== 1. 打开媒体文件，查找视频流 ==========
    ret = avformat_open_input(&input_format_ctx, input_file, nullptr, nullptr);
    if (ret < 0) {
        av_strerror(ret, err2str, sizeof(err2str));
        printf("avformat_open_input error: %s\n", err2str);
        return -1;
    }
    // 读取流信息
    ret = avformat_find_stream_info(input_format_ctx, nullptr);
    if (ret < 0) {
        av_strerror(ret, err2str, sizeof(err2str));
        printf("avformat_find_stream_info error: %s\n", err2str);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    // 找到视频流索引
    for (int i = 0; i < input_format_ctx->nb_streams; i++) {
        if (input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx == -1) {
        printf("can not find video stream!\n");
        avformat_close_input(&input_format_ctx);
        return -1;
    }

    // ========== 获取视频宽高，初始化解码器 ==========
    int video_width = input_format_ctx->streams[video_stream_idx]->codecpar->width;
    int video_height = input_format_ctx->streams[video_stream_idx]->codecpar->height;
    printf("video_width = %d, video_height = %d\n", video_width, video_height);

    AVCodecContext *codec_context = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(codec_context, input_format_ctx->streams[video_stream_idx]->codecpar);
    AVCodec *codec = avcodec_find_decoder(codec_context->codec_id);
    ret = avcodec_open2(codec_context, codec, nullptr);
    if (ret < 0) {
        av_strerror(ret, err2str, sizeof(err2str));
        printf("avcodec_open2 error, ret = %d, msg = %s\n", ret, err2str);
        return -1;
    }

    // ========== 分配数据包和解码帧 ==========
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frame_cnt = 0;

    // ========== 循环读取并解码视频帧 ==========
    while (true) {
        ret = av_read_frame(input_format_ctx, packet);
        if (ret < 0) {
            av_strerror(ret, err2str, sizeof(err2str));
            printf("av_read_frame error, ret = %d, msg = %s\n", ret, err2str);
            break;
        }

        // 过滤视频包，丢弃音频/字幕包
        if (packet->stream_index == video_stream_idx)
        {
            // 把压缩packet送入解码器
            ret = avcodec_send_packet(codec_context, packet);
            if (ret < 0)
            {
                av_strerror(ret, err2str, sizeof(err2str));
                printf("avcodec_send_packet error: %s\n", err2str);
                av_packet_unref(packet);
                break;
            }

            // 循环从解码器接收解码完成的AVFrame(YUV)
            while (true)
            {
                ret = avcodec_receive_frame(codec_context, frame);
                if (ret == AVERROR(EAGAIN))
                {
                    // EAGAIN: 解码器当前没有可用帧，需要继续喂packet
                    break;
                }
                else if (ret == AVERROR_EOF)
                {
                    // 解码器已经读完所有帧
                    printf("decoder eof\n");
                    break;
                }
                else if (ret < 0)
                {
                    av_strerror(ret, err2str, sizeof(err2str));
                    printf("avcodec_receive_frame error: %s\n", err2str);
                    break;
                }

                // 解码成功，输出帧信息
                printf("Decode frame %d, pts: %lld\n", frame_cnt++, frame->pts);
                av_frame_unref(frame); // 释放当前frame的buffer，准备接收下一帧
            }
        }
        av_packet_unref(packet); // 释放packet缓冲区，防止内存泄漏
    }

    // ========== 冲洗解码器缓存，取出残留帧 ==========
    printf("Start flush decoder buffer\n");
    ret = avcodec_send_packet(codec_context, nullptr); // 传入NULL通知解码器结束
    while (ret >= 0)
    {
        ret = avcodec_receive_frame(codec_context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
            break;
        printf("Flush decode frame %d, pts: %lld\n", frame_cnt++, frame->pts);
        av_frame_unref(frame);
    }

    // ========== 逆序释放资源，防止内存泄漏 ==========
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_context);
    avformat_close_input(&input_format_ctx);

    printf("解码完成，总共解码 %d 帧\n", frame_cnt);
    return 0;
}
