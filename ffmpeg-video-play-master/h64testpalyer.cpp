// 👌64.cpp

#include <iostream>
extern "C"{
#include"libavformat/avformat.h"
#include"libavutil/avutil.h"
#include "SDL.h"
}

void testUpdateYUVTexture1(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame);
void testUpdateYUVTexture2(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame);
void testRenderCopy1(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame);
void testRenderCopy2(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame);

// 使用了SDL后，需要加上这个宏定义，否则会报错
#undef main
int main() {
    // 本地文件测试，记得换成自己的视频文件路径
   char *url = "R:\\videoffempt\\ffmpeg-video-play-master\\res\\test.mp4";
    // 网络流测试
    // char *url = "https://wdl.wallstreetcn.com/41aae4d2-390a-48ff-9230-ee865552e72d";

    // 创建输入上下文，输入上下文用来保存这个视频文件的所有信息，ffmpeg操作视频文件操作都需要这个对象;就像 File 对象一样，通过 File 对象我们就能读写这个文件
    AVFormatContext *input_format_ctx = avformat_alloc_context();
    // 打开视频文件，即将视频文件绑定到输入上下文对象中，此时操作输入上下文，就是在操作视频文件了
    int ret = avformat_open_input(&input_format_ctx, url, nullptr, nullptr);
    // 保存错误码的消息
    char err2str[256];
    if (ret < 0) {
        // 将错误码转为错误信息
        av_strerror(ret, err2str, sizeof(err2str));
        printf("avformat_open_input error, ret = %d, msg = %s\n", ret, err2str);
        return -1;
    }

    // 输出这个视频文件的基础信息，没有其他作用
    av_dump_format(input_format_ctx, 0, url, 0);

    // 找到视频流在视频文件中的索引
    int video_idx = av_find_best_stream(input_format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr,0);
    // 如果找不到视频流，则返回-1，代表这个视频文件没有视频流
    if (video_idx < 0) {
        printf("av_find_best_stream error, video_idx = %d\n", video_idx);
        return -1;
    }
    printf("video_idx = %d\n", video_idx);

    // 创建解码器上下文，解码器上下文里面保存了解码器信息
    AVCodecContext *codec_context = avcodec_alloc_context3(nullptr);
    // 刚创建了解码器上下文，所以需要将视频文件的视频信息拷贝到解码器上下文对象中
    avcodec_parameters_to_context(codec_context, input_format_ctx->streams[video_idx]->codecpar);
    // 根据指定的解码器ID，查找对应的解码器，解码器上下文只是保存上下文信息的，我们需要一个干活的工具，就是这个解码器
    AVCodec *codec = avcodec_find_decoder(codec_context->codec_id);
    // 打开解码器，就和 File 对象一样，new 一个 File 对象是没用的，我们需要 open 这个 File 对象才能开始干活
    ret = avcodec_open2(codec_context, codec, nullptr);
    if (ret < 0) {
        av_strerror(ret, err2str, sizeof(err2str));
        printf("avcodec_open2 error, ret = %d, msg = %s\n", ret, err2str);
        return -1;
    }

    // 创建视频包和视频帧对象，视频包用来读取视频文件中读取出来的数据包，视频帧用来保存视频包解码后得到的原始图片
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    // 获取视频宽高
    int video_width = input_format_ctx->streams[video_idx]->codecpar->width;
    int video_height = input_format_ctx->streams[video_idx]->codecpar->height;
    printf("video_width = %d, video_height = %d\n", video_width, video_height);

    // 初始化SDL的视频部分
    if (SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init error");
        return -1;
    }

    // 创建一个窗口；其实可以当做是创建了一个画画的木板
    SDL_Window *window = SDL_CreateWindow("ffmpeg-1", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, video_width, video_height, SDL_WINDOW_OPENGL);
    if (!window) {
        printf("SDL_CreateWindow error");
        return -1;
    }
    // 根据当前窗口创建一个渲染器；其实可以当做是画画的画笔【其实应该说当成手】
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        printf("SDL_CreateRenderer error");
        return -1;
    }
    /*
     * 根据当前渲染器，创建一个纹路；
     * 其实可以当做画画的画布，即可以理解画画会有是三个工具：一个木板，手，很多个画布（纸）。我们其实是在画布上面画画，画完了之后把用手把画布贴到木板上面。
     * 最终我们肉眼其实看到的是这个木板上面，当然如果贴了画布那么就是看到这个画布画出来的内容，如果没贴画布那么我们看到的就是这个木板上面的空白颜色。
     * 不管有没有画布，我们始终能看到这个木板。
     */
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, video_width, video_height);
    // 区域；其实一个框而已，保存了 x，y，w，h 四个值
    SDL_Rect rect;
    // 事件；即系统的事件，比如鼠标点击，键盘输入，窗口关闭等等，这里我们只关注窗口关闭
    SDL_Event event;

    // 反复读取视频文件中的数据包，解码并显示，直到没有数据包为止
    while (true) {
        // 从视频文件中读取数据包到 packet 中
        ret = av_read_frame(input_format_ctx, packet);
        if (ret < 0) {
            av_strerror(ret, err2str, sizeof(err2str));
            printf("av_read_frame error, ret = %d, msg = %s\n", ret, err2str);
            break;
        }
        // 我们是从输入上下文中读取的包，而这个视频文件有视频，也有音频，所以我们需要判断下这个包是不是视频包
        if (packet->stream_index != video_idx) {
            // 清理数据包，下面的 unref 同理
            av_packet_unref(packet);
            continue;
        }

        // 将视频包放入解码器中进行解码
        ret = avcodec_send_packet(codec_context, packet);
        if (ret < 0) {
            av_strerror(ret, err2str, sizeof(err2str));
            printf("avcodec_send_packet error, ret = %d, msg = %s\n", ret, err2str);
            av_packet_unref(packet);
            // 小于0可能是解码器缓冲区满了，无法写入数据了，等读取数据后解码器缓冲区有空间了，再写入数据，所以不一定代表报错，可以continue尝试下一次
            continue;
            // break;
        }
        /*
         * 从解码器中获取一帧完整的视频帧；
         * 为什么要两步才能获取到一个完整的视频帧，因为就和 File 对象一样，我们要先 file.write 写入数据后，才能 file.read 读取数据
         */
        ret = avcodec_receive_frame(codec_context, frame);
        if (ret < 0) {
            av_strerror(ret, err2str, sizeof(err2str));
            printf("avcodec_receive_frame error, ret = %d, msg = %s\n", ret, err2str);
            av_packet_unref(packet);
            // 小于0可能是当前解码器缓冲区没有数据，不能解码，需要等待解码器拿到数据，所以不一定代表报错，可以continue尝试下一次
            continue;
            // break;
        }

        // 读取事件，或者说清空SDL的事件队列堆积
        while (SDL_PollEvent(&event)) {
            // SDL_QUIT：对应快捷键：ALT + F4
            if (event.type == SDL_QUIT) {
                printf("SDL_QUIT\n");
                // 播放结束，可以直接 return 0; 但是建议还是清理下资源
                goto cleanup;
            }
        }

        // 清空窗口；可以当成把木板上面的画布撕下来，现在窗口是空白的了【当然这里空白不一定指定的白色，我们可以自定义颜色】
        SDL_RenderClear(renderer);
        // 定义一个区域位置
        rect.x = 0;
        rect.y = 0;
        rect.w = video_width;
        rect.h = video_height;
        /*
         * 将视频帧数据复制到纹理中；就相当于这一步是在画布上面画画【注意，此时只是画布上被画画了，而木板上面还没有贴上这个画布呢】
         * 这个 rect 就是画在画布上的区域，假设画布大小是 1000 * 1000， 图片元素大小是 200 * 200，那没问题，就从（0,0）坐标开始画，画 200 * 200 的区域
         * 但是如果 画布大小是 1000 * 1000， 图片大小是 2000 * 2000，那么我们就会发现，图片漏了一部分画不到画布上面去，因为画布的大小就只有是 1000 * 1000，而图片大小是 2000 * 2000，
         * 所以正常情况我们设置画布的大小是和图片大小一致，不可能设置的比图片小【当然如果你有定制化需求也行】
         */
        SDL_UpdateYUVTexture(texture, &rect, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
        /*
         * 将纹理绘制到渲染器中；其实不应该说渲染是画笔，应该当成一个手，现在就是把画布拿到手上，执行 SDL_RenderPresent 就是把手上的画布贴到木板上
         * 这个 rect 指的是画布贴到木板的哪个区域，如果这个区域和纹理自己的大小不一致，那么会等比例拉伸
         */
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        // 刷新渲染器；即把画布贴到木板上
        SDL_RenderPresent(renderer);
        // 测试演示代码
        // testUpdateYUVTexture1(window, renderer, &rect, frame);
        // testUpdateYUVTexture2(window, renderer, &rect, frame);
        // testRenderCopy1(window, renderer, &rect, frame);
        testRenderCopy2(window, renderer, &rect, frame);

        // 等待21毫秒，目前我们就当做21毫秒显示一帧，实际开发已经计算每一帧到底播放多久，这里才等待多久
        SDL_Delay(21);

        // 清理 buf 缓存；我们需要清理 frame 和 pkt 的数据，因为这两个对象里面的 buf 不会自动清理，下一轮循环又会创建新的 buf，上一轮的 buf 不清理就导致内存泄漏了
        av_frame_unref(frame);
        av_packet_unref(packet);
    }

cleanup:
    printf("video over\n");

    // 释放 SDL 资源
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    // 释放 FFmpeg 资源
    if (frame) {
        av_frame_free(&frame);
    }
    if (packet) {
        av_packet_free(&packet);
    }
    if (codec_context) {
        avcodec_close(codec_context);
        avcodec_free_context(&codec_context);
    }
    if (input_format_ctx) {
        avformat_close_input(&input_format_ctx);
    }

    return 0;
}

// 图片只有右上角被画到画布上面去【窗口和图片是一样大的，但图片只画了左上角部分，跟拉伸没关系，是画了这么点出来】
void testUpdateYUVTexture1(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame) {
    SDL_RenderClear(renderer);
    SDL_Rect dst_rect;
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.w = rect->w / 2;
    dst_rect.h = rect->h / 2;

    static SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, rect->w, rect->h);

    SDL_UpdateYUVTexture(texture, &dst_rect, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
    SDL_RenderCopy(renderer, texture, nullptr, rect);
    SDL_RenderPresent(renderer);
}

// 图片只有右上角被画到画布上面去【纹理没有图片大，也没有窗口大，但是窗口会等比例拉伸；这里和上面的情况不一样，上面是只想画四分之一（只想），这里是只能画四分之一（只能）】
void testUpdateYUVTexture2(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame) {
    SDL_RenderClear(renderer);
    SDL_Rect dst_rect;
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.w = rect->w / 2;
    dst_rect.h = rect->h / 2;

    static SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, rect->w /2, rect->h / 2);
    // 建议这里写 nullptr，不要写 dst_rect，因为一旦 dst_rect 比 纹理的大，那么就会报错。【当然，可以指定写小，就和上面的方法效果一样】
    SDL_UpdateYUVTexture(texture, &dst_rect, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
    SDL_RenderCopy(renderer, texture, nullptr, rect);
    SDL_RenderPresent(renderer);
}

// 画布就算和图片一样大，但是我们选择把 (0,0)->(w,h) 贴到 (0,0)->(w/2, h/2)的区域上，那么画布就会被缩小了【这个是等比例自动缩小的】
void testRenderCopy1(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame) {
    SDL_RenderClear(renderer);

    static SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, rect->w, rect->h);
    SDL_UpdateYUVTexture(texture, rect, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);

    SDL_Rect dst_rect;
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.w = rect->w / 2;
    dst_rect.h = rect->h / 2;
    SDL_RenderCopy(renderer, texture, nullptr, &dst_rect);
    SDL_RenderPresent(renderer);
}

// 单个窗口多区域显示图片
void testRenderCopy2(SDL_Window *window, SDL_Renderer *renderer, SDL_Rect *rect, AVFrame *frame) {
    SDL_RenderClear(renderer);

    static SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, rect->w, rect->h);
    SDL_UpdateYUVTexture(texture, rect, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);

    SDL_Rect dst_rect;
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.w = rect->w / 2;
    dst_rect.h = rect->h / 2;
    SDL_RenderCopy(renderer, texture, nullptr, &dst_rect);

    dst_rect.x = rect->w / 2;
    dst_rect.y = 0;
    dst_rect.w = rect->w / 2;
    dst_rect.h = rect->h / 2;
    // 这里我演示用的同一个纹理，但是如果使用不同的纹理，那么就会显示不同的图片，相当于同一个窗口同时播放多个视频
    SDL_RenderCopy(renderer, texture, nullptr, &dst_rect);
    SDL_RenderPresent(renderer);
}












