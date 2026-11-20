//  1h64testpalyer.cpp

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
   // char *url = "C:\\data\\bilibili\\ffmpeg-video-play\\res\\test.mp4";
    // 网络流测试
     char *url = "https://wdl.wallstreetcn.com/41aae4d2-390a-48ff-9230-ee865552e72d";
 //   char *url = "R:\\videoffempt\\ffmpeg-video-play-master\\res\\test.mp4";
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

    // 创建一个窗口；其实可以当做是创建了一个画画的木板，添加 SDL_WINDOW_RESIZABLE 标志使得窗口大小可以改变
    SDL_Window *window = SDL_CreateWindow("ffmpeg-1", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, video_width, video_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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

    // 1获取视频流的时间基 (time_base)
    AVRational time_base = input_format_ctx->streams[video_idx]->time_base;
    // printf("time_base.num = %d, time_base.den = %d\n", time_base.num, time_base.den);

    // 外部时钟基准：以第一帧的系统时间为起点
    Uint32 external_clock_start = 0;
    // 标记当前帧是否是第一帧
    bool first_frame = true;
    // 帧计数器（用于统计）
    int64_t frame_count = 0;
    // 暂停状态，为true代表暂停中
    bool is_paused = false;
    // 暂停时的系统时间戳
    Uint32 pause_start_time = 0;
    // 累计暂停的总时长
    Uint32 total_pause_duration = 0;
    // 逐帧播放状态，每次置为true时会播放一帧画面
    bool is_step = false;


    // 播放倍速，默认1.0倍速
    double play_speed = 1.0;
    // 最小和最大倍速限制
    const double MIN_SPEED = 0.5;
    const double MAX_SPEED = 4.0;
    const double SCALE_SPEED = 0.25;
    Uint32 last_time = SDL_GetTicks();
    // 倍速切换时的校准基准点
    int64_t speed_change_pts_ms = 0;      // 倍速改变时的视频PTS（毫秒）
    Uint32 speed_change_actual_ms = 0;    // 倍速改变时的实际流逝时间（毫秒）
    // 更新基准值供下一帧使用
    int64_t last_frame_pts_ms = 0;
    // 记录当前窗口的宽高
    int window_width = video_width;
    int window_height = video_height;

    // 反复读取视频文件中的数据包，解码并显示，直到没有数据包为止
    while (true) {
        // 读取事件
        if (SDL_PollEvent(&event)) {
            switch (event.type) {
                // SDL_QUIT：对应快捷键：ALT + F4
                case SDL_QUIT: {
                    printf("SDL_QUIT\n");
                    // 播放结束，可以直接 return 0; 但是建议还是清理下资源
                    goto cleanup;
                }
                    break;
                // 监听窗口大小变化事件
                case SDL_WINDOWEVENT: {
                    // 也可以使用 SDL_WINDOWEVENT_SIZE_CHANGED，但是 SDL_WINDOWEVENT_SIZE_CHANGED 太广泛，我们使用 SDL_WINDOWEVENT_RESIZED 即可
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int new_width = event.window.data1;
                        int new_height = event.window.data2;
                        // 输出窗口大小变化信息
                        printf("Window size changed from %dx%d to %dx%d\n", window_width, window_height, new_width, new_height);
                        window_width = new_width;
                        window_height = new_height;

                        // 如果处于暂停状态且有上一帧数据，则手动重新渲染
                        if (is_paused && frame->data[0] != nullptr) {
                              SDL_RenderClear(renderer);
                            rect.x = 0;
                            rect.y = 0;
                            rect.w = video_width;
                            rect.h = video_height;
                            SDL_UpdateYUVTexture(texture, &rect,
                                               frame->data[0], frame->linesize[0],
                                               frame->data[1], frame->linesize[1],
                                               frame->data[2], frame->linesize[2]);
                            rect.w = window_width;
                            rect.h = window_height;
                            SDL_RenderCopy(renderer, texture, NULL, &rect);
                            SDL_RenderPresent(renderer);
                            printf("Manual redraw\n");
                        }
                    }
                }
                    break;
                // 监听键盘按键
                case SDL_KEYDOWN: {
                    // printf("SDL_KEYDOWN\n");
                    switch (event.key.keysym.sym) {
                        // 监听空格：暂停播放
                        case SDLK_SPACE: {
                            // 更新暂停状态
                            is_paused = !is_paused;
                            if (is_paused) {
                                // 记录暂停开始时间
                                pause_start_time = SDL_GetTicks();
                                printf("Playback paused\n");
                            } else {
                                // 恢复播放，累加暂停时长【暂停可能会有很多次，所以需要的是累加，而不是直接=】
                                total_pause_duration += SDL_GetTicks() - pause_start_time;
                                // 取消逐帧播放
                                is_step = false;
                                printf("Playback resumed\n");
                            }
                        }
                            break;
                        // 监听 s ，实现逐帧播放
                        case SDLK_s: {
step_flag:
                            // 开启逐帧播放
                            is_step = true;
                            /*
                            * 上一次逐帧结束，到这一次逐帧开始，这段时间也算是暂停时间，所以需要累加暂停时间
                            * 这个 if 是必要的，防止从播放状态中直接按下逐帧，那么实际上没有上一个逐帧，那么也就没有暂停时间，不应该累加，而且此时 pause_time 是上一次的暂停时间，加了肯定有问题
                            */
                            if (is_paused) {
                                total_pause_duration += SDL_GetTicks() - pause_start_time;
                            }
                            // 逐帧即播放播放一帧->暂停，这里是开启播放一帧【因为刚刚可能处于暂停中，我们需要播放一帧】
                            is_paused = false;
                            printf("Playback step\n");
                        }
                            break;
                        // 监听 [ 键，实现减速播放
                        case SDLK_LEFTBRACKET: {
                            play_speed -= SCALE_SPEED;
                            if (play_speed < MIN_SPEED) {
                                play_speed = MIN_SPEED;
                            }
                            // 如果处于暂停状态，先累加暂停时长
                            if (is_paused) {
                                total_pause_duration += SDL_GetTicks() - pause_start_time;
                                pause_start_time = SDL_GetTicks();
                            }
                            // 倍数改变时更新基准值
                            speed_change_pts_ms = last_frame_pts_ms;
                            speed_change_actual_ms = SDL_GetTicks() - external_clock_start - total_pause_duration;
                            printf("Playback speed: %.2fx\n", play_speed);
                        }
                            break;
                        // 监听 ] 键，实现加速播放
                        case SDLK_RIGHTBRACKET: {
                            play_speed += SCALE_SPEED;
                            if (play_speed > MAX_SPEED) {
                                play_speed = MAX_SPEED;
                            }
                            // 如果处于暂停状态，先累加暂停时长
                            if (is_paused) {
                                total_pause_duration += SDL_GetTicks() - pause_start_time;
                                pause_start_time = SDL_GetTicks();
                            }
                            // 倍数改变时更新基准值
                            speed_change_pts_ms = last_frame_pts_ms;
                            speed_change_actual_ms = SDL_GetTicks() - external_clock_start - total_pause_duration;
                            printf("Playback speed: %.2fx\n", play_speed);
                        }
                            break;
                        // 监听左箭头，向左 seek 5秒
                        case SDLK_LEFT: {
                            int64_t seek_target_ms = last_frame_pts_ms - 5000;
                            if (seek_target_ms < 0) {
                                seek_target_ms = 0;
                            }

                            // 将毫秒转换为视频的时间基单位
                            int64_t seek_target = av_rescale_q(seek_target_ms, (AVRational){1, 1000}, time_base);

                            // 执行seek操作，seek到关键帧
                            ret = av_seek_frame(input_format_ctx, video_idx, seek_target, AVSEEK_FLAG_BACKWARD);
                            if (ret < 0) {
                                av_strerror(ret, err2str, sizeof(err2str));
                                printf("av_seek_frame error, ret = %d, msg = %s\n", ret, err2str);
                            } else {
                                // seek成功后需要清空解码器缓冲区
                                avcodec_flush_buffers(codec_context);
                                // 清空当前packet，避免使用旧数据
                                av_packet_unref(packet);

                                // 重置帧计数器和时间基准
                                frame_count = 0;
                                /*
                                 * 更新外部时钟的基准值，假设seek成功，外部时钟的基准值就是当前时间减去seek到的时间
                                 * ➗️ play_speed 是因为，假设向左 seek 5秒，实际上只需要 减去现实时间的 2.5 秒即可，因为等会我们只花费 2.5 秒播放其实也就是播放了 5 秒的视频
                                 */
                                external_clock_start = SDL_GetTicks() - (Uint32)(seek_target_ms / play_speed);
                                total_pause_duration = 0;
                                speed_change_pts_ms = 0;
                                speed_change_actual_ms = 0;

                                printf("Seek backward to %lld ms\n", seek_target_ms);
                            }
                            if (is_paused)
                                goto step_flag;
                        }
                            break;
                        // 监听右箭头，向右seek 5秒
                        case SDLK_RIGHT: {
                            int64_t seek_target_ms = last_frame_pts_ms + 5000;

                            // 获取视频总时长（毫秒）
                            int64_t duration_ms = 0;
                            if (input_format_ctx->duration != AV_NOPTS_VALUE) {
                                duration_ms = av_rescale_q(input_format_ctx->duration, (AVRational){1, AV_TIME_BASE}, (AVRational){1, 1000});
                            }

                            // 确保不超过视频总时长
                            if (duration_ms > 0 && seek_target_ms > duration_ms) {
                                seek_target_ms = duration_ms;
                            }

                            // 将毫秒转换为视频的时间基单位
                            int64_t seek_target = av_rescale_q(seek_target_ms, (AVRational){1, 1000}, time_base);
                            printf("seek_target_ms:%lld\n", seek_target_ms);


                            // 执行seek操作
                            ret = av_seek_frame(input_format_ctx, video_idx, seek_target, AVSEEK_FLAG_BACKWARD);
                            if (ret < 0) {
                                av_strerror(ret, err2str, sizeof(err2str));
                                printf("av_seek_frame error, ret = %d, msg = %s\n", ret, err2str);
                            } else {
                                // seek成功后需要清空解码器缓冲区
                                avcodec_flush_buffers(codec_context);
                                // 清空当前packet，避免使用旧数据
                                av_packet_unref(packet);

                                // 重置帧计数器和时间基准
                                frame_count = 0;
                                // 更新外部时钟的基准值，假设seek成功，外部时钟的基准值就是当前时间减去seek到的时间
                                external_clock_start = SDL_GetTicks() - (Uint32)(seek_target_ms / play_speed);
                                total_pause_duration = 0;
                                speed_change_pts_ms = 0;
                                speed_change_actual_ms = 0;

                                printf("Seek forward to %lld ms\n", seek_target_ms);
                            }
                            if (is_paused)
                                goto step_flag;
                        }
                            break;
                        default:
                            break;
                    }
                }
                    break;
                default:
                    break;
            }
        }
        // 如果处于暂停中，那么就不需要进行播放或者解码，等待恢复播放才能进行下一步
        if (is_paused) {
            SDL_Delay(10); // 暂停时减少CPU占用，不写也没事
            continue;
        }

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

        // 获取当前帧的 PTS（转换为毫秒）
        int64_t frame_pts_ms = av_rescale_q(frame->pts, time_base, (AVRational){1, 1000});

        // 初始化外部时钟
        if (first_frame) {
            external_clock_start = SDL_GetTicks();
            first_frame = false;
            printf("First frame PTS: %03lld ms\n", frame_pts_ms);
        }

        // 计算实际已经经过的时间（减去暂停时长），假设播放了40秒，暂停了20秒，那么实际时长应该是20秒，即需要减去暂停的时间
        Uint32 actual_elapsed_ms = SDL_GetTicks() - external_clock_start - total_pause_duration;

        // 计算从倍速改变点到现在的物理时间增量[如果没改变过速度，speed_change_actual_ms = 0，那么其实 delta_actual_ms 就等于 actual_elapsed_ms】
        Uint32 delta_actual_ms = actual_elapsed_ms - speed_change_actual_ms;

        // 根据倍速调整实际流逝时间：倍速越快，相同物理时间内应该显示更多帧
        // 关键：不能直接用 actual_elapsed_ms * play_speed，因为倍速改变时会把过去的时间也重新计算
        // 正确做法：基于倍速改变时的基准点，只对新经过的时间应用新倍速
        double adjusted_elapsed_ms = speed_change_pts_ms + (delta_actual_ms * play_speed);

        // 计算需要等待的时间【即当前帧还有多久才能显示】
        if (frame_pts_ms > adjusted_elapsed_ms) {
            Uint32 delay_ms = (Uint32)(frame_pts_ms - adjusted_elapsed_ms);
            // 根据倍速调整延迟时间：倍速越快，延迟越短【本来 sleep 2 才能到 2，现在 2 倍数了，sleep 1 其实就到 2 了】
            delay_ms = (Uint32)(delay_ms / play_speed);
            // printf("Delay: %03lld ms (speed: %.2fx)\n", delay_ms, play_speed);
            SDL_Delay(delay_ms);
        }
        // 计算上一张图片到这一张图片的耗时
        {
            Uint32 elapsed = SDL_GetTicks() - last_time;
            // printf("Loop elapsed time: %u ms (speed: %.2fx)\n", elapsed, play_speed);
            last_time = SDL_GetTicks();
        }
        // 保存当前帧的pts，即最后一帧的pts
        last_frame_pts_ms = frame_pts_ms;
        // 播放帧数+1
        frame_count++;
        // 每 100 帧打印一次调试信息【
        if (frame_count % 100 == 0) {
            printf("Frame %lld: Expected=%lldms, Actual=%ums, Diff=%dms\n",
                   frame_count,
                   frame_pts_ms,
                   actual_elapsed_ms,
                   (int)frame_pts_ms - (int)actual_elapsed_ms);
        }

        // 逐帧即播放播放一帧->暂停，这里是再次开启暂停
        if (is_step) {
            is_paused = true;
            // 此时也相当于暂停了，那么我们需要记录暂停的时间，方便下一次播放的时候计算一共暂停了多久时间
            pause_start_time = SDL_GetTicks();
        }

        /*
         * 这里开始就是下一张图片显示了
         */
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
        // 窗口大小变了，渲染区域的大小也要跟着变
        rect.w = window_width;
        rect.h = window_height;
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
        // testRenderCopy2(window, renderer, &rect, frame);

        if (ret < 0) {
            av_strerror(ret, err2str, sizeof(err2str));
            printf("av_frame_ref error, ret = %d, msg = %s\n", ret, err2str);
        }
        
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





