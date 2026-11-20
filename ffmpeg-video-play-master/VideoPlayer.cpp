#include "VideoPlayer.h"
#include <stdio.h>
#include <math.h>

VideoPlayer::VideoPlayer() {
    input_format_ctx = nullptr;
    codec_context = nullptr;
    packet = nullptr;
    frame = nullptr;
    video_idx = -1;
    time_base = {0, 1};
    duration_ms = 0;
    is_step = false;
    frame_count = 0;
    last_time = SDL_GetTicks();
}

VideoPlayer::~VideoPlayer() {
    cleanup();
}

int VideoPlayer::init(const char *url) {
    char errbuf[256];
    
    // 打开视频文件
    input_format_ctx = avformat_alloc_context();
    int ret = avformat_open_input(&input_format_ctx, url, NULL, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("avformat_open_input error: %s\n", errbuf);
        return -1;
    }
    
    // 获取流信息
    av_dump_format(input_format_ctx, 0, url, 0);
    
    // 查找视频流
    video_idx = av_find_best_stream(
        input_format_ctx,
        AVMEDIA_TYPE_VIDEO, 
        -1, -1, NULL, 0
    );
    if (video_idx < 0) {
        printf("av_find_best_stream error: no video stream found\n");
        return -1;
    }
    printf("Video stream index: %d\n", video_idx);
    
    // 创建解码器上下文
    codec_context = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(
        codec_context,
        input_format_ctx->streams[video_idx]->codecpar
    );
    
    // 查找并打开解码器
    AVCodec *codec = avcodec_find_decoder(codec_context->codec_id);
    if (!codec) {
        printf("avcodec_find_decoder error: codec not found\n");
        return -1;
    }
    
    ret = avcodec_open2(codec_context, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("avcodec_open2 error: %s\n", errbuf);
        return -1;
    }
    
    // 分配 packet 和 frame
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    
    // 获取视频尺寸、时间基和总时长
    int width = input_format_ctx->streams[video_idx]->codecpar->width;
    int height = input_format_ctx->streams[video_idx]->codecpar->height;
    time_base = input_format_ctx->streams[video_idx]->time_base;
    
    // 获取视频总时长（毫秒）
    duration_ms = 0;
    if (input_format_ctx->duration != AV_NOPTS_VALUE) {
        duration_ms = input_format_ctx->duration / (double)AV_TIME_BASE * 1000.0;
    }
    
    printf("Video size: %dx%d\n", width, height);
    if (duration_ms > 0) {
        printf("Video duration: %.2fs (%.0f:%.0f)\n", 
               duration_ms / 1000.0, 
               duration_ms / 60000.0, 
               fmod(duration_ms / 1000.0, 60));
    }
    
    // 初始化渲染器
    if (renderer.init(width, height) < 0) {
        return -1;
    }

    // 🎯 时钟对齐到 0: VideoClock 在构造函数里就开始计时了,
    // 而 avformat_find_stream_info 耗费的 1~3 秒会让时钟领先于视频 PTS,
    // 导致开头十几秒视频疯狂快放 (delay<0 不等待, 全速解码)
    clock.syncToPts(0);
    last_time = SDL_GetTicks();

    return 0;
}

void VideoPlayer::cleanup() {
    // 清理 FFmpeg 资源
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
    
    // 清理渲染器（析构函数会自动调用）
}

bool VideoPlayer::handleEvent() {
    /*
     * 如果有我们指定的事件触发了，那么结束事件循环，开始处理这个事件
     * 或者没有我们需要处理的事件，那么就正常运行即可
     */
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;  // 退出

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    renderer.handleResize(
                        event.window.data1,
                        event.window.data2
                    );
                    // 如果暂停需要手动渲染一次
                    if (isPaused()) {
                        renderCurrentFrame();
                    }
                    return true;  // 窗口大小改变，需要重新渲染
                }
                break;

            case SDL_KEYDOWN:
                if (handleKeydown(event.key.keysym.sym)) {
                    return true;  // 按键事件已处理，需要重新渲染
                }
                break;
            default:
                break;
        }
    }
    return true;  // 继续运行
}

bool VideoPlayer::handleKeydown(SDL_Keycode key) {
    switch (key) {
        case SDLK_SPACE:
            // 暂停/播放
            clock.togglePause();
            if (clock.isPaused()) {
                printf("Playback paused at %.2f s\n", clock.get() / 1000.0);
            } else {
                is_step = false;
                printf("Playback resumed\n");
            }
            return true;

        case SDLK_s:
            // 逐帧播放
            is_step = true;
            if (clock.isPaused()) {
                clock.togglePause();
            }
            printf("Playback step\n");
            return true;

        case SDLK_LEFTBRACKET:
            // 减速
            clock.adjustSpeed(-SCALE_SPEED);
            printf("Playback speed: %.2fx\n", clock.getSpeed());
            return true;
        
        case SDLK_RIGHTBRACKET:
            // 加速
            clock.adjustSpeed(SCALE_SPEED);
            printf("Playback speed: %.2fx\n", clock.getSpeed());
            return true;
        
        case SDLK_LEFT:
            // 向左 seek 5秒
            seek(-SEEK_SPEED);
            return true;
        
        case SDLK_RIGHT:
            // 向右 seek 5秒
            seek(SEEK_SPEED);
            return true;
        
        default:
            return false;  // 未处理的按键
    }
}

void VideoPlayer::seek(double offset_ms) {
    char errbuf[256];
    
    // 计算目标位置（毫秒）
    double current_pts_ms = clock.get();
    double seek_target_ms = current_pts_ms + offset_ms;
    
    // 边界检查
    if (seek_target_ms < 0) {
        seek_target_ms = 0;
    }
    
    if (duration_ms > 0 && seek_target_ms > duration_ms) {
        seek_target_ms = duration_ms;
    }
    
    // 将毫秒转换为视频时间基单位
    int64_t seek_target = av_rescale_q(
        (int64_t)(seek_target_ms),
        (AVRational){1, 1000},
        time_base
    );
    
    // 执行 seek（使用 BACKWARD 标志，定位到目标之前的关键帧）
    int ret = av_seek_frame(input_format_ctx, video_idx,
                           seek_target, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("av_seek_frame error: %s\n", errbuf);
        return;
    }
    
    // seek 成功后清理缓冲区
    avcodec_flush_buffers(codec_context);
    av_packet_unref(packet);
    av_frame_unref(frame);
    
    // 打印 seek 信息（输出为秒）
    const char *direction = offset_ms < 0 ? "backward" : "forward";
    printf("Seek %s to %.2f s\n", direction, seek_target_ms / 1000.0);
    
    // Seek 后丢弃非关键帧：循环解码直到找到 PTS >= seek_target_ms 的帧
    while (true) {
        ret = decodeFrame();
        if (ret == -1) {
            // 错误或播放结束
            printf("Seek failed: decode error or EOF\n");
            return;
        } else if (ret == -2) {
            // 需要继续读取
            continue;
        }
        
        // 成功解码一帧，检查 PTS（毫秒）
        double frame_pts_ms = av_q2d(time_base) * frame->pts * 1000.0;
        if (frame_pts_ms >= seek_target_ms) {
            // 找到目标帧或之后的帧，退出循环
            printf("Seek complete: found frame at %.2f s\n", frame_pts_ms / 1000.0);
            break;
        }
        
        // PTS 还小于目标，丢弃这帧，继续解码下一帧
        // 不渲染，直接继续循环
    }
    
    // 如果处于暂停状态，切换到逐帧模式
    if (isPaused()) {
        is_step = true;
        // 当前方法只是为了切换播放暂停状态，内部的时间修改是无意义的，因为实际上应该设置为seek后的时间
        clock.togglePause();
    }
    // 校准时钟到目标位置（毫秒）
    clock.syncToPts(seek_target_ms);
}

bool VideoPlayer::isPaused() const {
    return clock.isPaused();
}

int VideoPlayer::decodeFrame() {
    // 按道理应该是用了之后马上释放，而我这里改成了下一次用之前释放。这样写可以保证一定不会有多余的内存泄漏，最多不使用了之后只会有当前一帧内存泄漏。这样很方便，不需要在每个异常地方都加一个释放。
    av_packet_unref(packet);
    av_frame_unref(frame);

    char errbuf[256];

    // 读取数据包
    int ret = av_read_frame(input_format_ctx, packet);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("av_read_frame error: %s\n", errbuf);
        return -1;
    }

    // 检查是否是视频包
    if (packet->stream_index != video_idx) {
        return -2; // 不是视频包，继续读取
    }

    // 发送数据包到解码器
    ret = avcodec_send_packet(codec_context, packet);

    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("avcodec_send_packet error: %s\n", errbuf);
        return -1;
    }

    // 接收解码后的帧
    ret = avcodec_receive_frame(codec_context, frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // EAGAIN: 需要更多输入数据，EOF: 解码结束
            // 这是正常情况，继续读取下一帧
            return -2;
        }
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("avcodec_receive_frame error: %s\n", errbuf);
        return -1;
    }

    return 0;
}

int VideoPlayer::renderCurrentFrame() {
    // 仅渲染当前帧，不更新时钟
    // 用于暂停时窗口拉伸后的重绘
    if (!frame || !frame->data[0]) {
        return -1;  // 没有可渲染的帧
    }
    return renderer.render(frame);
}

int VideoPlayer::syncAndRender() {
    // 获取当前帧的 PTS（毫秒）
    double frame_pts_ms = av_q2d(time_base) * frame->pts * 1000.0;
    
    // 计算需要等待的时间（毫秒）
    double clock_time_ms = clock.get();
    double delay_ms = frame_pts_ms - clock_time_ms;
    
    if (delay_ms > 0) {
        Uint32 wait_time = (Uint32)(delay_ms / clock.getSpeed());
        SDL_Delay(wait_time);
    }
    
    // 如果是逐帧模式，播放一帧后暂停
    if (is_step && !clock.isPaused()) {
        clock.togglePause();
    }
    
    // 渲染帧
    return renderer.render(frame);
}

void VideoPlayer::printStats() {
    // 打印性能统计（每100帧）
    frame_count++;
    if (frame_count % 100 == 0) {
        Uint32 elapsed = SDL_GetTicks() - last_time;
        
        // 获取当前帧的 PTS（毫秒），输出时转换为秒
        double frame_pts_ms = av_q2d(time_base) * frame->pts * 1000.0;
        
        // 获取当前时钟时间（毫秒），输出时转换为秒
        double clock_time_ms = clock.get();
        
        // 计算播放进度
        double progress = 0;
        if (duration_ms > 0) {
            progress = (frame_pts_ms / duration_ms) * 100;
        }
        
        printf("Frame %lld | PTS: %.2fs | Clock: %.2fs | Progress: %.1f%% | Speed: %.2fx | Time: %.2fs\n",
               frame_count, frame_pts_ms / 1000.0, clock_time_ms / 1000.0, 
               progress, clock.getSpeed(), elapsed / 1000.0);
        last_time = SDL_GetTicks();
    }
}
