#include <iostream>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cmath>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libswresample/swresample.h"
#include "SDL.h"
}

#undef main

// ============================================================
// 全局音频状态 (SDL 音频回调线程读取, demux 线程写入)
// ============================================================
static std::deque<uint8_t> g_audio_buf;          // S16 立体声 PCM 字节流
static std::mutex          g_audio_mtx;
static int                 g_audio_bytes_per_sec = 0;   // 字节率 = sample_rate * channels * 2(S16)
static int64_t            g_audio_bytes_consumed = 0;  // 回调累计消费的字节数
static double             g_audio_clock_ms = 0.0;       // 已播放音频时长 (ms)

// 音频回调: SDL 拉指定字节数的 PCM 填入 stream
static void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;   // SDL 回调固定签名, 此处不需要
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    int n = 0;
    while (n < len && !g_audio_buf.empty()) {
        stream[n++] = g_audio_buf.front();
        g_audio_buf.pop_front();
    }
    if (n < len) {
        std::memset(stream + n, 0, len - n);
    }
    g_audio_bytes_consumed += n;
    if (g_audio_bytes_per_sec > 0) {
        g_audio_clock_ms = (double)g_audio_bytes_consumed * 1000.0 / g_audio_bytes_per_sec;
    }
}

// 把一帧音频 (任意格式) 重采样成 S16 立体声 44100 并 push 到队列
static SwrContext* g_swr = nullptr;
static void push_audio_frame(AVFrame* frame) {
    if (!g_swr) {
        g_swr = swr_alloc_set_opts(nullptr,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
            frame->channel_layout ? frame->channel_layout : av_get_default_channel_layout(frame->channels),
            (AVSampleFormat)frame->format, frame->sample_rate,
            0, nullptr);
        swr_init(g_swr);
        g_audio_bytes_per_sec = 44100 * 2 * 2;
    }
    int64_t out_samples = swr_get_out_samples(g_swr, frame->nb_samples);
    uint8_t* out_buf = nullptr;
    int out_linesize;
    av_samples_alloc(&out_buf, &out_linesize, 2, out_samples, AV_SAMPLE_FMT_S16, 0);
    int got = swr_convert(g_swr, &out_buf, out_samples, (const uint8_t**)frame->data, frame->nb_samples);
    if (got > 0) {
        int bytes = got * 2 * 2;
        std::lock_guard<std::mutex> lk(g_audio_mtx);
        g_audio_buf.insert(g_audio_buf.end(), out_buf, out_buf + bytes);
    }
    av_freep(&out_buf);
}

// ============================================================
// 全局播放状态 (供 seek 用)
// ============================================================
static AVFormatContext* g_fmt_ctx = nullptr;
static int              g_video_stream_idx = -1;
static int              g_audio_stream_idx = -1;
static AVRational       g_video_time_base = {1, 1000};
static AVRational       g_audio_time_base = {1, 1000};
static AVCodecContext*  g_video_codec_ctx = nullptr;
static AVCodecContext*  g_audio_codec_ctx = nullptr;
static double           g_duration_ms = 0.0;

// 进度条 UI 参数
const int BAR_HEIGHT = 30;
const int BAR_INNER_H = 12;     // 进度条本身高度
const int BAR_Y_OFFSET = 9;     // 在 BAR_HEIGHT 区域内的偏移
static int g_window_w = 0;
static int g_window_h = 0;
static int g_windowed_w = 0;   // 窗口模式尺寸 (用于切换回窗口)
static int g_windowed_h = 0;
static bool g_fullscreen = false;
static int g_video_width = 0;  // 视频原始尺寸 (固定)
static int g_video_height = 0;

// ============================================================
// seek 实现: 跳到指定毫秒位置
// - av_seek_frame 视频 + 音频流 (BACKWARD 找关键帧)
// - flush 解码器缓冲
// - 清音频队列 + 重置 audio_clock_ms = target
// ============================================================
static void seek_to(double target_ms) {
    if (target_ms < 0) target_ms = 0;
    if (g_duration_ms > 0 && target_ms > g_duration_ms) target_ms = g_duration_ms;

    std::cout << "[SEEK] to " << target_ms << "ms" << std::endl;

    // 视频流 seek (用 ms → 时间基)
    int64_t video_ts = (int64_t)llround(target_ms * g_video_time_base.den / (1000.0 * g_video_time_base.num));
    int ret = av_seek_frame(g_fmt_ctx, g_video_stream_idx, video_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char err[256] = {0};
        av_strerror(ret, err, sizeof(err));
        std::cout << "[SEEK] video seek failed: " << err << std::endl;
    }
    // 音频流 seek
    if (g_audio_stream_idx >= 0) {
        int64_t audio_ts = (int64_t)llround(target_ms * g_audio_time_base.den / (1000.0 * g_audio_time_base.num));
        av_seek_frame(g_fmt_ctx, g_audio_stream_idx, audio_ts, AVSEEK_FLAG_BACKWARD);
    }

    // flush 解码器内部缓冲的帧 (丢弃关键帧前的非对齐帧)
    if (g_video_codec_ctx) avcodec_flush_buffers(g_video_codec_ctx);
    if (g_audio_codec_ctx) avcodec_flush_buffers(g_audio_codec_ctx);

    // 重置音频队列 + 直接把时钟设到 target_ms
    {
        std::lock_guard<std::mutex> lk(g_audio_mtx);
        g_audio_buf.clear();
        g_audio_bytes_consumed = (int64_t)llround(target_ms * g_audio_bytes_per_sec / 1000.0);
        g_audio_clock_ms = target_ms;
    }
}

// ============================================================
// 全屏切换
// ============================================================
static void toggle_fullscreen(SDL_Window* window) {
    g_fullscreen = !g_fullscreen;
    if (g_fullscreen) {
        // 进入虚拟全屏 (用桌面分辨率, 不改变显示模式, 切换更快)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        // 回到窗口模式
        SDL_SetWindowFullscreen(window, 0);
        SDL_SetWindowSize(window, g_windowed_w, g_windowed_h);
    }
    // 同步 g_window_w/h (全屏时是屏幕尺寸, 窗口时是设定尺寸)
    SDL_GetWindowSize(window, &g_window_w, &g_window_h);
    std::cout << "[FULLSCREEN] " << (g_fullscreen ? "ON" : "OFF")
              << " " << g_window_w << "x" << g_window_h << std::endl;
}

// ============================================================
// 动态计算视频 rect: 按宽高比居中缩放, 留出底部进度条空间
// ============================================================
static SDL_Rect compute_video_rect() {
    double aspect = (double)g_video_width / g_video_height;
    int avail_w = g_window_w;
    int avail_h = g_window_h - BAR_HEIGHT;  // 进度条占底部 30px
    int target_w = avail_w;
    int target_h = (int)(target_w / aspect);
    if (target_h > avail_h) {
        target_h = avail_h;
        target_w = (int)(target_h * aspect);
    }
    SDL_Rect r;
    r.x = (g_window_w - target_w) / 2;
    r.y = (avail_h - target_h) / 2;
    r.w = target_w;
    r.h = target_h;
    return r;
}

// ============================================================
// 绘制进度条: 背景灰 + 已播放蓝 + 当前位置红点
// ============================================================
static void draw_progress_bar(SDL_Renderer* r, double current_ms) {
    int bar_x = 0;
    int bar_y = g_window_h - BAR_HEIGHT + BAR_Y_OFFSET;
    int bar_w = g_window_w;
    int bar_h = BAR_INNER_H;

    // 背景 (深灰)
    SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
    SDL_Rect bg = {bar_x, bar_y, bar_w, bar_h};
    SDL_RenderFillRect(r, &bg);

    // 已播放部分 (青蓝)
    double progress = (g_duration_ms > 0) ? (current_ms / g_duration_ms) : 0.0;
    if (progress > 1.0) progress = 1.0;
    if (progress < 0) progress = 0;
    int played_w = (int)(bar_w * progress);
    if (played_w > 0) {
        SDL_SetRenderDrawColor(r, 70, 170, 230, 255);
        SDL_Rect played = {bar_x, bar_y, played_w, bar_h};
        SDL_RenderFillRect(r, &played);
    }

    // 当前位置指示 (亮红小方块)
    int knob_x = bar_x + played_w - 3;
    if (knob_x < bar_x) knob_x = bar_x;
    if (knob_x > bar_x + bar_w - 6) knob_x = bar_x + bar_w - 6;
    SDL_SetRenderDrawColor(r, 255, 80, 80, 255);
    SDL_Rect knob = {knob_x, bar_y - 3, 6, bar_h + 6};
    SDL_RenderFillRect(r, &knob);
}

int main()
{
    std::cout << "Hello, World!12  " << std::endl;
    const char *url = "res/test.mp4";
    g_fmt_ctx = avformat_alloc_context();
    int ret = avformat_open_input(&g_fmt_ctx, url, NULL, NULL);
    char errTostr[1024] = {0};
    if(ret < 0) {
        av_strerror(ret, errTostr, sizeof(errTostr));
        std::cout << "avformat_open_input open input file ret:" << ret << " err:" << errTostr << std::endl;
        return -1;
    }
    av_dump_format(g_fmt_ctx, 0, url, 0);

    // ---------- 找视频流 + 音频流 ----------
    g_video_stream_idx = av_find_best_stream(g_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_audio_stream_idx = av_find_best_stream(g_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, g_video_stream_idx, NULL, 0);
    if (g_video_stream_idx < 0) { std::cout << "no video stream" << std::endl; return -1; }

    int video_width  = g_fmt_ctx->streams[g_video_stream_idx]->codecpar->width;
    int video_height = g_fmt_ctx->streams[g_video_stream_idx]->codecpar->height;
    g_video_time_base = g_fmt_ctx->streams[g_video_stream_idx]->time_base;
    if (g_audio_stream_idx >= 0)
        g_audio_time_base = g_fmt_ctx->streams[g_audio_stream_idx]->time_base;

    // ---------- 总时长 (毫秒) ----------
    if (g_fmt_ctx->duration != AV_NOPTS_VALUE && g_fmt_ctx->duration > 0) {
        g_duration_ms = g_fmt_ctx->duration / (double)AV_TIME_BASE * 1000.0;
    }
    std::cout << "video: " << video_width << "x" << video_height
              << " tb=" << g_video_time_base.num << "/" << g_video_time_base.den
              << " duration=" << g_duration_ms << "ms" << std::endl;

    // ---------- 视频解码器 ----------
    g_video_codec_ctx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(g_video_codec_ctx, g_fmt_ctx->streams[g_video_stream_idx]->codecpar);
    AVCodec* video_codec = avcodec_find_decoder(g_video_codec_ctx->codec_id);
    if (!video_codec) { std::cout << "find video decoder failed" << std::endl; return -1; }
    if (avcodec_open2(g_video_codec_ctx, video_codec, NULL) < 0) { std::cout << "open video codec failed" << std::endl; return -1; }

    // ---------- 音频解码器 ----------
    if (g_audio_stream_idx >= 0) {
        g_audio_codec_ctx = avcodec_alloc_context3(NULL);
        avcodec_parameters_to_context(g_audio_codec_ctx, g_fmt_ctx->streams[g_audio_stream_idx]->codecpar);
        AVCodec* audio_codec = avcodec_find_decoder(g_audio_codec_ctx->codec_id);
        if (!audio_codec || avcodec_open2(g_audio_codec_ctx, audio_codec, NULL) < 0) {
            std::cout << "open audio codec failed" << std::endl;
            g_audio_codec_ctx = nullptr;
        }
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame*  video_frame = av_frame_alloc();
    AVFrame*  audio_frame = av_frame_alloc();

    // ---------- SDL 初始化 ----------
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cout << "SDL_Init failed: " << SDL_GetError() << std::endl; return -1;
    }
    g_window_w = video_width;
    g_window_h = video_height + BAR_HEIGHT;   // 🎯 窗口高度 += 30 给进度条
    g_windowed_w = g_window_w;                // 保存窗口模式尺寸
    g_windowed_h = g_window_h;
    g_video_width = video_width;               // 保存视频原始尺寸
    g_video_height = video_height;
    SDL_Window* window = SDL_CreateWindow("Video Player (audio-sync + seek)",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_window_w, g_window_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);   // 🎯 RESIZABLE 让最大化按钮变亮可点
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STATIC, video_width, video_height);

    // ---------- 打开 SDL 音频设备 ----------
    if (g_audio_codec_ctx) {
        SDL_AudioSpec wanted, obtained;
        std::memset(&wanted, 0, sizeof(wanted));
        wanted.freq = 44100;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 1024;
        wanted.callback = audio_callback;
        wanted.userdata = nullptr;
        if (SDL_OpenAudio(&wanted, &obtained) < 0) {
            std::cout << "SDL_OpenAudio failed: " << SDL_GetError() << std::endl;
        } else {
            SDL_PauseAudio(0);
            std::cout << "audio device opened: " << obtained.freq << "Hz "
                      << (int)obtained.channels << "ch" << std::endl;
        }
    }

    SDL_Rect video_rect = {0, 0, video_width, video_height};
    std::cout << "[PLAYBACK] start (F=全屏, 点击进度条 seek, ESC 退出)" << std::endl;

    // ============================================================
    // 主循环
    // ============================================================
    bool mouse_dragging = false;
    bool seek_queued = false;
    double seek_target = 0.0;

    while (true) {
        ret = av_read_frame(g_fmt_ctx, packet);
        if (ret < 0) {
            std::cout << "[EOF] av_read_frame ret=" << ret << std::endl;
            break;
        }

        // ---------- 处理音频包 ----------
        if (packet->stream_index == g_audio_stream_idx && g_audio_codec_ctx) {
            ret = avcodec_send_packet(g_audio_codec_ctx, packet);
            if (ret >= 0) {
                while (true) {
                    ret = avcodec_receive_frame(g_audio_codec_ctx, audio_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    push_audio_frame(audio_frame);
                    av_frame_unref(audio_frame);
                }
            }
            av_packet_unref(packet);
            continue;
        }

        // ---------- 处理视频包 ----------
        if (packet->stream_index != g_video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }
        ret = avcodec_send_packet(g_video_codec_ctx, packet);
        if (ret < 0) { av_packet_unref(packet); continue; }

        while (true) {
            ret = avcodec_receive_frame(g_video_codec_ctx, video_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int64_t pts = video_frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = video_frame->pts;
            double pts_ms = (pts == AV_NOPTS_VALUE) ? 0.0
                : pts * av_q2d(g_video_time_base) * 1000.0;

            double audio_ms;
            {
                std::lock_guard<std::mutex> lk(g_audio_mtx);
                audio_ms = g_audio_clock_ms;
            }
            double diff = pts_ms - audio_ms;

            if (diff > 30.0) {
                SDL_Delay((Uint32)(diff > 50 ? 50 : diff));
            } else if (diff < -80.0) {
                av_frame_unref(video_frame);
                continue;
            }

            // ---------- 渲染视频 ----------
            // 🎯 动态计算 rect (全屏/窗口切换后尺寸会变)
            video_rect = compute_video_rect();
            // 🎯 清屏前显式设回黑色: 上一帧 draw_progress_bar 末尾把 draw color
            // 设成了红色 (knob), RenderClear 会沿用该颜色, 全屏两侧"黑边"因此变红
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_UpdateYUVTexture(texture, NULL,
                video_frame->data[0], video_frame->linesize[0],
                video_frame->data[1], video_frame->linesize[1],
                video_frame->data[2], video_frame->linesize[2]);
            SDL_RenderCopy(renderer, texture, NULL, &video_rect);

            // ---------- 渲染进度条 (用视频 PTS, 因为它是当前显示帧的时间) ----------
            // seek 拖动中用 seek_target 显示预览, 否则用视频实际 PTS
            double display_ms = seek_queued ? seek_target : pts_ms;
            draw_progress_bar(renderer, display_ms);
            SDL_RenderPresent(renderer);

            // ---------- 事件循环 ----------
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) goto cleanup;
                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE) goto cleanup;
                    if (event.key.keysym.sym == SDLK_f) {
                        toggle_fullscreen(window);
                    }
                }
                if (event.type == SDL_WINDOWEVENT) {
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        g_window_w = event.window.data1;
                        g_window_h = event.window.data2;
                        if (!g_fullscreen) {
                            g_windowed_w = g_window_w;
                            g_windowed_h = g_window_h;
                        }
                    }
                }

                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    int y = event.button.y;
                    if (y >= g_window_h - BAR_HEIGHT) {
                        mouse_dragging = true;
                        int x = event.button.x;
                        double t = (double)x / g_window_w * g_duration_ms;
                        seek_queued = true;
                        seek_target = t;
                    }
                }
                if (event.type == SDL_MOUSEBUTTONUP) {
                    if (mouse_dragging) {
                        mouse_dragging = false;
                        // 释放时执行 seek
                        seek_to(seek_target);
                        seek_queued = false;
                    }
                }
                if (event.type == SDL_MOUSEMOTION && mouse_dragging) {
                    int x = event.motion.x;
                    if (x < 0) x = 0;
                    if (x > g_window_w) x = g_window_w;
                    seek_target = (double)x / g_window_w * g_duration_ms;
                }
            }

            av_frame_unref(video_frame);
        }
        av_packet_unref(packet);
    }

    // 等待音频播完 (带休眠, 避免忙等待空耗 CPU)
    std::cout << "[EOF] wait audio buffer drain..." << std::endl;
    while (true) {
        {
            std::lock_guard<std::mutex> lk(g_audio_mtx);
            if (g_audio_buf.empty()) break;
        }
        SDL_Delay(50);   // 锁释放后再睡眠, 让音频回调线程能取数据
    }
    SDL_Delay(200);

cleanup:
    if (g_audio_codec_ctx) {
        SDL_PauseAudio(1);
        SDL_CloseAudio();
    }
    if (g_swr) swr_free(&g_swr);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    av_frame_free(&video_frame);
    av_frame_free(&audio_frame);
    av_packet_free(&packet);
    avcodec_free_context(&g_video_codec_ctx);
    if (g_audio_codec_ctx) avcodec_free_context(&g_audio_codec_ctx);
    avformat_close_input(&g_fmt_ctx);
    return 0;
}
