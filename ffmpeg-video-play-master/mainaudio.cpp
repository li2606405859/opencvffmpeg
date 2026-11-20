#include <iostream>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

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
// ============================================================
// 全局音频状态 (完全照搬 1main.cpp 正确实现)
// - 从 buf 头部 pop_front 消费, speed 用 pop_accum 累积
// ============================================================
static std::deque<uint8_t> g_audio_buf;
static std::mutex          g_audio_mtx;
static const int           AUDIO_SAMPLE_RATE = 44100;
static const int           BYTES_PER_SAMPLE  = 4;
static double              g_audio_clock_ms = 0.0;
static double              g_speed = 1.0;

// ============================================================
// WSOLA 时域伸缩 — 变速不变调 (rubberband 核心算法)
// 关键公式: src_pos 推进 = CHUNK * speed (不是 /)
// ============================================================
static const int WSOLA_WIN  = 1024;   // 窗口帧数 (~23ms)
static const int WSOLA_OV   = 512;    // 交叠帧数 (~11.6ms)
static const int WSOLA_SRCH = 256;    // 搜索半径 (~5.8ms)
static const int WSOLA_CHUNK = WSOLA_WIN - WSOLA_OV;  // 512
static double   g_wsola_pos   = 0.0;  // 源位置 (浮点)
static int16_t  g_wsola_prev_ov[WSOLA_OV * 2] = {0};
static bool     g_wsola_init  = false;

// NCC 归一化互相关: 找 target 和 src 中最相似的位置 (只向前搜索)
static int wsola_best_match(const std::deque<uint8_t>& buf, int center,
                            const int16_t* target) {
    int buf_frames = (int)buf.size() / BYTES_PER_SAMPLE;
    int best = center;
    double best_score = -1e18;
    // 只向前搜索 (音频单向播放)
    int start = std::max(0, center - WSOLA_SRCH / 2);
    int end   = std::min(buf_frames - WSOLA_OV, center + WSOLA_SRCH / 2);
    for (int p = start; p <= end; p += 2) {
        double sp = 0, st2 = 0, sc2 = 0;
        for (int i = 0; i < WSOLA_OV; i += 2) {   // stride=2 加速
            int16_t tL = target[i*2],  tR = target[i*2+1];
            int16_t cL = *(int16_t*)&buf[(p+i)*BYTES_PER_SAMPLE];
            int16_t cR = *(int16_t*)&buf[(p+i)*BYTES_PER_SAMPLE+2];
            sp += (double)tL*cL + (double)tR*cR;
            st2 += (double)tL*tL + (double)tR*tR;
            sc2 += (double)cL*cL + (double)cR*cR;
        }
        if (st2 < 1e-9 || sc2 < 1e-9) continue;
        double ncc = sp / (sqrt(st2) * sqrt(sc2));
        if (ncc > best_score) { best_score = ncc; best = p; }
    }
    return best;
}

// WSOLA 音频回调 — 变速不变调
static void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    int out_frames = len / BYTES_PER_SAMPLE;
    int buf_frames = (int)g_audio_buf.size() / BYTES_PER_SAMPLE;

    // speed 接近 1.0 → 简单线性消费 (零处理, 完美音质)
    if (fabs(g_speed - 1.0) < 0.05) {
        int consumed = std::min(out_frames, buf_frames);
        // 从 buf 头部拷 consumed 帧到 stream
        for (int i = 0; i < consumed; i++) {
            stream[i*4+0] = g_audio_buf[i*4+0];
            stream[i*4+1] = g_audio_buf[i*4+1];
            stream[i*4+2] = g_audio_buf[i*4+2];
            stream[i*4+3] = g_audio_buf[i*4+3];
        }
        // pop consumed 帧
        for (int i = 0; i < consumed * BYTES_PER_SAMPLE; i++)
            g_audio_buf.pop_front();
        // 补零
        for (int i = consumed; i < out_frames; i++) {
            stream[i*4+0] = 0; stream[i*4+1] = 0;
            stream[i*4+2] = 0; stream[i*4+3] = 0;
        }
        g_audio_clock_ms += (double)consumed * 1000.0 / AUDIO_SAMPLE_RATE;
        g_wsola_init = false;
        return;
    }

    // speed 偏离 1.0 → WSOLA 变速不变调
    // 确保有足够数据
    if (buf_frames < WSOLA_WIN + WSOLA_SRCH) {
        std::memset(stream, 0, len);
        return;
    }

    // 初始化: 首次或 speed 变化后
    if (!g_wsola_init) {
        for (int i = 0; i < WSOLA_OV; i++) {
            g_wsola_prev_ov[i*2]   = *(int16_t*)&g_audio_buf[i*BYTES_PER_SAMPLE];
            g_wsola_prev_ov[i*2+1] = *(int16_t*)&g_audio_buf[i*BYTES_PER_SAMPLE+2];
        }
        g_wsola_pos = WSOLA_OV;
        g_wsola_init = true;
    }

    int produced = 0;
    while (produced < out_frames && g_wsola_pos + WSOLA_WIN < buf_frames) {
        int base = (int)g_wsola_pos;
        // Step1: 搜索最佳拼接点
        int best = wsola_best_match(g_audio_buf, base, g_wsola_prev_ov);
        if (best + WSOLA_WIN > buf_frames) break;

        // Step2: 提取当前窗口 (S16 stereo packed)
        int16_t cur_win[WSOLA_WIN * 2];
        for (int i = 0; i < WSOLA_WIN; i++) {
            cur_win[i*2]   = *(int16_t*)&g_audio_buf[(best+i)*BYTES_PER_SAMPLE];
            cur_win[i*2+1] = *(int16_t*)&g_audio_buf[(best+i)*BYTES_PER_SAMPLE+2];
        }

        // Step3: 交叉淡入淡出前 OV 帧
        int copy_ov = std::min(WSOLA_OV, out_frames - produced);
        for (int i = 0; i < copy_ov; i++) {
            double alpha = (double)i / WSOLA_OV;
            int16_t L = (int16_t)(g_wsola_prev_ov[i*2]*(1-alpha) + cur_win[i*2]*alpha);
            int16_t R = (int16_t)(g_wsola_prev_ov[i*2+1]*(1-alpha) + cur_win[i*2+1]*alpha);
            stream[produced*4+0] = L & 0xFF; stream[produced*4+1] = L >> 8;
            stream[produced*4+2] = R & 0xFF; stream[produced*4+3] = R >> 8;
            produced++;
        }

        // Step4: 直接拷贝后 CHUNK 帧 (从 OV 到 WIN)
        for (int i = WSOLA_OV; i < WSOLA_WIN && produced < out_frames; i++) {
            int16_t L = cur_win[i*2], R = cur_win[i*2+1];
            stream[produced*4+0] = L & 0xFF; stream[produced*4+1] = L >> 8;
            stream[produced*4+2] = R & 0xFF; stream[produced*4+3] = R >> 8;
            produced++;
        }

        // Step5: 更新 prev_ov = cur_win 末尾 OV 帧
        for (int i = 0; i < WSOLA_OV; i++) {
            g_wsola_prev_ov[i*2]   = cur_win[(WSOLA_WIN-WSOLA_OV+i)*2];
            g_wsola_prev_ov[i*2+1] = cur_win[(WSOLA_WIN-WSOLA_OV+i)*2+1];
        }

        // Step6: src_pos 推进 = best + CHUNK * speed  ← 关键公式!
        g_wsola_pos = best + (double)WSOLA_CHUNK * g_speed;
    }

    // Step7: 回收老采样
    int pop_up_to = (int)g_wsola_pos - WSOLA_WIN;
    if (pop_up_to > 0) {
        for (int i = 0; i < pop_up_to * BYTES_PER_SAMPLE; i++)
            g_audio_buf.pop_front();
        g_wsola_pos -= pop_up_to;
    }

    // 补零
    while (produced < out_frames) {
        stream[produced*4+0] = 0; stream[produced*4+1] = 0;
        stream[produced*4+2] = 0; stream[produced*4+3] = 0;
        produced++;
    }

    g_audio_clock_ms += (double)produced * 1000.0 / AUDIO_SAMPLE_RATE * g_speed;
}

// ============================================================
// atempo 滤镜 (已删除 — DLL 不兼容导致崩溃)
// speed=1.0 完美, speed≠1.0 用跳帧/重复 (物理本质会变调)
// 真正变速不变调需要引入第三方库如 rubberband
// ============================================================

// 把一帧音频重采样成 S16 立体声 44100 (固定! audio_callback 里处理倍速)
static SwrContext* g_swr = nullptr;
static void push_audio_frame(AVFrame* frame) {
    if (!g_swr) {
        int chlayout = frame->channel_layout ? frame->channel_layout
                       : av_get_default_channel_layout(frame->channels);
        g_swr = swr_alloc_set_opts(nullptr,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, AUDIO_SAMPLE_RATE,
            chlayout, (AVSampleFormat)frame->format, frame->sample_rate,
            0, nullptr);
        swr_init(g_swr);
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

// 底部控件区: 上半 进度条, 下半 速度按钮
const int BAR_HEIGHT      = 50;
const int BAR_INNER_H     = 12;
const int BAR_Y_OFFSET    = 6;   // 进度条在 BAR_HEIGHT 内的 y 偏移
const int BTN_ROW_Y       = 28;  // 按钮行顶部相对 BAR_HEIGHT 的偏移
const int BTN_H           = 18;  // 按钮高度
const int BTN_GAP         = 6;   // 按钮间隙
const int BTN_PAD_RIGHT   = 10;  // 右对齐留出的边距
static const double BTN_SPEEDS[5] = {0.5, 1.0, 1.5, 2.0, 3.0};
static const char* BTN_LABELS[5] = {"0.5x", "1x", "1.5x", "2x", "3x"};

// ---- 给 SDL_RenderDrawRect 画边框用 ----
static void draw_rect_outline(SDL_Renderer* r, SDL_Rect* rect, Uint8 R, Uint8 G, Uint8 B, Uint8 A = 255) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_RenderDrawRect(r, rect);
}

// ============================================================
// 纯 SDL2 迷你 5x7 点阵字体 (仅包含倍速标签需要的字符)
// 每个字符 7 行, 每行 5 bits (bit4=最左, bit0=最右)
// ============================================================
static const char FONT_CHARS[] = "0123456789.xX ";
static const uint8_t FONT_5x7[14][7] = {
    // '0'
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
    // '1'
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    // '2'
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    // '3'
    {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},
    // '4'
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    // '5'
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
    // '6'
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
    // '7'
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    // '8'
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    // '9'
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},
    // '.'
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100},
    // 'x'
    {0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001},
    // 'X'
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},
    // ' ' 空格占位
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
};

// 在 renderer 上画 5x7 点阵字符串
static void draw_bitmap_text(SDL_Renderer* r, const char* text, int x, int y,
                             int scale = 2, Uint8 R = 20, Uint8 G = 20, Uint8 B = 20, Uint8 A = 255) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    int cx = x;
    while (*text) {
        int idx = -1;
        for (int i = 0; FONT_CHARS[i] != '\0'; i++) {
            if (FONT_CHARS[i] == *text) { idx = i; break; }
        }
        if (idx < 0) idx = 13; // 未识别字符用空格占位
        const uint8_t* glyph = FONT_5x7[idx];
        for (int row = 0; row < 7; row++) {
            uint8_t line = glyph[row];
            for (int col = 0; col < 5; col++) {
                // bit4 是最左侧像素, 从高到低遍历
                if (line & (1 << (4 - col))) {
                    SDL_Rect pixel = {cx + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &pixel);
                }
            }
        }
        cx += 5 * scale + (scale / 2); // 字符间额外留半scale间隙
        text++;
    }
}

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
// - 清音频队列 + 重置 WSOLA 状态 + audio_clock_ms = target
// ============================================================
// speed 变化: 只需更新 g_speed!
// - 不 clear buf: 残留采样被新 speed 自然消费 (WSOLA 按新比例前进)
// - 不 reset clock: 避免 video diff 瞬间巨大 → 跳帧追赶 → 卡顿好几秒
// 这样实现无缝切换, 声音画面完全不停顿
static void apply_speed(double new_speed) {
    double clamped = new_speed;
    if (clamped < 0.25) clamped = 0.25;
    if (clamped > 4.0)  clamped = 4.0;
    if (std::abs(clamped - g_speed) < 0.001) return;
    g_speed = clamped;
    std::cout << "[SPEED] x" << g_speed << " (WSOLA, 变速不变调)" << std::endl;
}

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
        g_audio_clock_ms = target_ms;
        g_wsola_init = false;
        g_wsola_pos = 0.0;
        std::memset(g_wsola_prev_ov, 0, sizeof(g_wsola_prev_ov));
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
// 速度按钮: 右对齐的一排 (0.5x / 1x / 1.5x / 2x / 3x)
// - active 用亮青色, inactive 深灰
// - 提供 hit_test 返回命中的按钮索引 (-1 表示未命中)
// ============================================================
static SDL_Rect get_speed_btn_rect(int idx) {
    int total_w = 0;
    for (int i = 0; i < 5; i++) total_w += 60 + BTN_GAP;
    total_w -= BTN_GAP;
    int btn_w = 60;
    int base_x = g_window_w - BTN_PAD_RIGHT - total_w;
    int x = base_x + idx * (btn_w + BTN_GAP);
    int y = g_window_h - BAR_HEIGHT + BTN_ROW_Y;
    SDL_Rect r = {x, y, btn_w, BTN_H};
    return r;
}

// 统一的底部控件区背景: 深灰色 (不是纯黑, 视觉上和视频画面区分开)
static void draw_control_panel(SDL_Renderer* r) {
    SDL_Rect panel = {0, g_window_h - BAR_HEIGHT, g_window_w, BAR_HEIGHT};
    SDL_SetRenderDrawColor(r, 35, 35, 35, 255);   // 深灰, 比视频(0,0,0)亮很多
    SDL_RenderFillRect(r, &panel);
}

static void draw_speed_buttons(SDL_Renderer* r) {
    // 只画按钮, 不画 panel (panel 由 draw_control_panel 统一画)
    for (int i = 0; i < 5; i++) {
        SDL_Rect rect = get_speed_btn_rect(i);
        bool active = std::abs(BTN_SPEEDS[i] - g_speed) < 0.001;
        if (active) {
            SDL_SetRenderDrawColor(r, 80, 200, 255, 255);   // 亮青: 当前选中
        } else {
            SDL_SetRenderDrawColor(r, 100, 100, 100, 255);  // 浅灰: 未选中
        }
        SDL_RenderFillRect(r, &rect);
        // 边框 (比按钮主体亮, 勾出轮廓)
        draw_rect_outline(r, &rect, 180, 180, 180);

        // 画按钮标签 (倍速数值), 居中, 近黑色文字保证对比度
        const char* label = BTN_LABELS[i];
        int scale = 2;          // 点阵放大倍数 (5*2=10px宽, 7*2=14px高)
        int char_w = 5 * scale + (scale / 2);
        int label_total_w = 0;
        for (const char* c = label; *c != '\0'; c++) label_total_w += char_w;
        label_total_w -= (scale / 2);   // 去掉最后一个字符的间隙
        int text_x = rect.x + (rect.w - label_total_w) / 2;
        int text_y = rect.y + (rect.h - 7 * scale) / 2;
        draw_bitmap_text(r, label, text_x, text_y, scale, 20, 20, 20, 255);
    }
}

static int hit_test_speed_btn(int mx, int my) {
    for (int i = 0; i < 5; i++) {
        SDL_Rect rect = get_speed_btn_rect(i);
        if (mx >= rect.x && mx <= rect.x + rect.w &&
            my >= rect.y && my <= rect.y + rect.h) {
            return i;
        }
    }
    return -1;
}

// ============================================================
// 绘制进度条: 背景灰 + 已播放蓝 + 当前位置红点
// ============================================================
static void draw_progress_bar(SDL_Renderer* r, double current_ms) {
    int bar_x = 0;
    int bar_y = g_window_h - BAR_HEIGHT + BAR_Y_OFFSET;
    int bar_w = g_window_w;
    int bar_h = BAR_INNER_H;

    // 进度条前景已播放部分 (青蓝), 背景由 draw_control_panel 统一提供
    // 额外画一条细边框让进度条本体在 panel 上轮廓清晰
    SDL_Rect bar_frame = {bar_x, bar_y, bar_w, bar_h};
    SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
    SDL_RenderFillRect(r, &bar_frame);   // 比 panel 稍亮, 框出进度条区域

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
    av_register_all();
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
    av_dump_format(g_fmt_ctx, 0, url, 0);  // 打印视频流信息

    // ---------- 找视频流 + 音频流 ----------
    g_video_stream_idx = av_find_best_stream(g_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);  // 找视频流
    g_audio_stream_idx = av_find_best_stream(g_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);  // 找音频流
    g_audio_stream_idx = av_find_best_stream(g_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, g_video_stream_idx, NULL, 0);  // 找音频流
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
        wanted.freq = AUDIO_SAMPLE_RATE;     // 固定 44100, WSOLA 变速在回调里做
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 2048;
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
    std::cout << "[PLAYBACK] start (F=全屏, [/]=减速/加速, \\=重置1x, 点击进度条 seek, ESC 退出)" << std::endl;

    // ============================================================
    // 主循环
    // ============================================================
    bool mouse_dragging = false;
    bool seek_queued = false;
    double seek_target = 0.0;

// while(true)
// ├─ av_read_frame 读取 AVPacket
// ├─ if 音频包
// │  ├─ avcodec_send_packet → avcodec_receive_frame 解码音频
// │  ├─ 解码出 AVFrame → push_audio_frame 放入音频队列（音频时钟g_audio_clock_ms由音频回调驱动）
// │  └─ av_packet_unref
// ├─ else if 视频包
// │  ├─ avcodec_send_packet → avcodec_receive_frame 解码视频YUV帧
// │  ├─ 取视频帧pts，转ms；读取音频时钟g_audio_clock_ms，计算音视频差值diff
// │  ├─ 音同步逻辑：
// │     · diff > 30ms：视频比音频快，SDL_Delay等待（带倍速缩放 wait_ms = diff/g_speed，上限50ms）
// │     · diff < -80ms：视频严重落后音频 → 直接丢弃当前帧，不渲染
// │  ├─ SDL渲染：更新YUV纹理 → RenderCopy画视频 → 绘制控制面板/进度条/速度按钮
// │  ├─ SDL_PollEvent 处理事件：窗口缩放、全屏、倍速快捷键、鼠标拖拽进度条seek
// │  └─ av_frame_unref / av_packet_unref
// └─ 读到EOF，退出循环

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
                // 按倍速缩短等待: 2x 时 diff 100ms 只等 50ms
                double wait_ms = diff / g_speed;
                if (wait_ms > 50.0) wait_ms = 50.0;
                SDL_Delay((Uint32)wait_ms);
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

            // ---------- 渲染底部控件 ----------
            // 顺序: 统一panel背景 → 进度条 → 速度按钮 (后画的不覆盖前画的)
            draw_control_panel(renderer);
            double display_ms = seek_queued ? seek_target : pts_ms;
            draw_progress_bar(renderer, display_ms);
            draw_speed_buttons(renderer);

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
                    if (event.key.keysym.sym == SDLK_RIGHTBRACKET ||
                        event.key.keysym.sym == SDLK_EQUALS) {
                        apply_speed(g_speed + 0.25);    // 加速
                    }
                    if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
                        apply_speed(g_speed - 0.25);    // 减速
                    }
                    if (event.key.keysym.sym == SDLK_BACKSLASH) {
                        apply_speed(1.0);               // 重置
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
                    int mx = event.button.x;
                    int my = event.button.y;
                    // 1) 优先检测速度按钮 (下半行)
                    int btn = hit_test_speed_btn(mx, my);
                    if (btn >= 0) {
                        apply_speed(BTN_SPEEDS[btn]);
                    } else if (my >= g_window_h - BAR_HEIGHT &&
                               my <  g_window_h - BAR_HEIGHT + BAR_INNER_H + 4) {
                        // 2) 进度条区 (上半行) -> seek 拖动
                        mouse_dragging = true;
                        double t = (double)mx / g_window_w * g_duration_ms;
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