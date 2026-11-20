#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <cstdio>

// ====================== 切换BUG类型 ======================
#define BUG_TYPE 0
/*
0 = 正常渲染
1 = linesize行跨度错误（忽略linesize，直接按width拷贝，斜波纹错位）
2 = UV平面颠倒（YUV420P传给YV12纹理，紫绿对半分，就是你截图的问题）
3 = NV12误判YV12（把NV12当成YUV三平面解析，碎色块噪点）
4 = 纹理宽高不匹配（纹理尺寸强行写错，画面拉伸截断留白）
5 = 多线程竞态模拟（模拟半帧旧数据+半帧新数据，上下两半不同画面）
*/
// =========================================================

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("用法：./yuv_bug_demo test.mp4\n");
        return -1;
    }
    const char* filePath = argv[1];

    // 初始化SDL
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win = SDL_CreateWindow("YUV Bug Demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,720, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = nullptr;

    // FFmpeg 打开文件
    AVFormatContext* fmtCtx = avformat_alloc_context();
    if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) < 0) return -1;
    avformat_find_stream_info(fmtCtx, nullptr);

    int videoStreamIdx = -1;
    AVCodecParameters* codecParam = nullptr;
    for(int i=0; i<fmtCtx->nb_streams; i++)
    {
        if(fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIdx = i;
            codecParam = fmtCtx->streams[i]->codecpar;
            break;
        }
    }
    AVCodec* codec = avcodec_find_decoder(codecParam->codec_id);
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecParam);
    avcodec_open2(codecCtx, codec, nullptr);

    AVFrame* frame = av_frame_alloc();
    AVPacket pkt;
    av_init_packet(&pkt);

    int w = codecCtx->width;
    int h = codecCtx->height;

    // 目标YUV420P frame
    AVFrame* yuvFrame = av_frame_alloc();
    yuvFrame->format = AV_PIX_FMT_YUV420P;
    yuvFrame->width = w;
    yuvFrame->height = h;
    av_frame_get_buffer(yuvFrame, 1);

    SwsContext* sws = sws_getContext(w, h, codecCtx->pix_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);

    // 创建SDL纹理
    Uint32 texFmt;
    int texW = w;
    int texH = h;
#if BUG_TYPE == 2
    texFmt = SDL_PIXELFORMAT_YV12; // YV12: Y V U
#else
    texFmt = SDL_PIXELFORMAT_IYUV; // IYUV=YUV420P: Y U V
#endif

#if BUG_TYPE == 4
    texW = w/2; texH = h/2; // 错误：纹理宽高缩小一半，模拟宽高不匹配
#endif
    texture = SDL_CreateTexture(renderer, texFmt, SDL_TEXTUREACCESS_STREAMING, texW, texH);

    bool running = true;
    while(running && av_read_frame(fmtCtx, &pkt)>=0)
    {
        if(pkt.stream_index == videoStreamIdx)
        {
            avcodec_send_packet(codecCtx, &pkt);
            while(avcodec_receive_frame(codecCtx, frame) == 0)
            {
                sws_scale(sws, frame->data, frame->linesize, 0, h, yuvFrame->data, yuvFrame->linesize);

                // ========== 各个BUG分支 ==========
                if(BUG_TYPE == 1)
                {
                    // BUG1: linesize错误：强行按width拷贝，忽略linesize填充
                    uint8_t* yBad = new uint8_t[w*h];
                    uint8_t* uBad = new uint8_t[(w*h)/4];
                    uint8_t* vBad = new uint8_t[(w*h)/4];
                    for(int y=0;y<h;y++)
                    {
                        memcpy(yBad + y*w, yuvFrame->data[0] + y*yuvFrame->linesize[0], w);
                    }
                    for(int y=0;y<h/2;y++)
                    {
                        memcpy(uBad + y*(w/2), yuvFrame->data[1] + y*yuvFrame->linesize[1], w/2);
                        memcpy(vBad + y*(w/2), yuvFrame->data[2] + y*yuvFrame->linesize[2], w/2);
                    }
                    SDL_UpdateYUVTexture(texture, nullptr, yBad, w, uBad, w/2, vBad, w/2);
                    delete[] yBad; delete[] uBad; delete[] vBad;
                }
                else if(BUG_TYPE == 2)
                {
                    // BUG2: UV颠倒，传给YV12纹理，Y U V 改成 Y V U
                    SDL_UpdateYUVTexture(texture, nullptr,
                        yuvFrame->data[0], yuvFrame->linesize[0],
                        yuvFrame->data[2], yuvFrame->linesize[2],
                        yuvFrame->data[1], yuvFrame->linesize[1]);
                }
                else if(BUG_TYPE ==3)
                {
                    // BUG3: NV12误判YUV420P，构造假NV12数据，强行送入三平面纹理
                    int ySize = w*h;
                    uint8_t* fakeNV12 = new uint8_t[ySize + w*h/2];
                    memcpy(fakeNV12, yuvFrame->data[0], ySize);
                    // UV交织打包 NV12: U V U V...
                    uint8_t* uvDst = fakeNV12 + ySize;
                    uint8_t* uSrc = yuvFrame->data[1];
                    uint8_t* vSrc = yuvFrame->data[2];
                    for(int i=0;i<w*h/4;i++)
                    {
                        uvDst[i*2] = uSrc[i];
                        uvDst[i*2+1] = vSrc[i];
                    }
                    // 错误：把NV12内存当成独立U、V平面传入
                    SDL_UpdateYUVTexture(texture, nullptr,
                        fakeNV12, w,
                        fakeNV12 + ySize, w/2,
                        fakeNV12 + ySize + w*h/4, w/2);
                    delete[] fakeNV12;
                }
                else if(BUG_TYPE ==5)
                {
                    // BUG5: 模拟竞态，上半帧旧数据，下半帧新数据
                    static uint8_t* oldY = nullptr;
                    static uint8_t* oldU = nullptr;
                    static uint8_t* oldV = nullptr;
                    if(!oldY){
                        oldY = new uint8_t[yuvFrame->linesize[0]*h];
                        oldU = new uint8_t[yuvFrame->linesize[1]*(h/2)];
                        oldV = new uint8_t[yuvFrame->linesize[2]*(h/2)];
                        memcpy(oldY, yuvFrame->data[0], yuvFrame->linesize[0]*h);
                        memcpy(oldU, yuvFrame->data[1], yuvFrame->linesize[1]*(h/2));
                        memcpy(oldV, yuvFrame->data[2], yuvFrame->linesize[2]*(h/2));
                    }
                    // 上半部分用旧帧，下半部分新帧
                    int halfH = h/2;
                    uint8_t* mixY = new uint8_t[yuvFrame->linesize[0]*h];
                    uint8_t* mixU = new uint8_t[yuvFrame->linesize[1]*(h/2)];
                    uint8_t* mixV = new uint8_t[yuvFrame->linesize[2]*(h/2)];
                    memcpy(mixY, oldY, yuvFrame->linesize[0]*halfH);
                    memcpy(mixY + yuvFrame->linesize[0]*halfH, yuvFrame->data[0] + yuvFrame->linesize[0]*halfH, yuvFrame->linesize[0]*(h-halfH));
                    memcpy(mixU, oldU, yuvFrame->linesize[1]*(halfH/2));
                    memcpy(mixU + yuvFrame->linesize[1]*(halfH/2), yuvFrame->data[1] + yuvFrame->linesize[1]*(halfH/2), yuvFrame->linesize[1]*(h/2 - halfH/2));
                    memcpy(mixV, oldV, yuvFrame->linesize[2]*(halfH/2));
                    memcpy(mixV + yuvFrame->linesize[2]*(halfH/2), yuvFrame->data[2] + yuvFrame->linesize[2]*(halfH/2), yuvFrame->linesize[2]*(h/2 - halfH/2));

                    SDL_UpdateYUVTexture(texture, nullptr, mixY, yuvFrame->linesize[0], mixU, yuvFrame->linesize[1], mixV, yuvFrame->linesize[2]);
                    // 更新旧帧缓存
                    memcpy(oldY, yuvFrame->data[0], yuvFrame->linesize[0]*h);
                    memcpy(oldU, yuvFrame->data[1], yuvFrame->linesize[1]*(h/2));
                    memcpy(oldV, yuvFrame->data[2], yuvFrame->linesize[2]*(h/2));
                    delete[] mixY; delete[] mixU; delete[] mixV;
                }
                else
                {
                    // BUG4 / BUG0 正常分支
                    SDL_UpdateYUVTexture(texture, nullptr,
                        yuvFrame->data[0], yuvFrame->linesize[0],
                        yuvFrame->data[1], yuvFrame->linesize[1],
                        yuvFrame->data[2], yuvFrame->linesize[2]);
                }

                SDL_SetRenderDrawColor(renderer,0,0,0,255);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
            }
        }
        av_packet_unref(&pkt);

        SDL_Event e;
        while(SDL_PollEvent(&e))
        {
            if(e.type == SDL_QUIT) running = false;
        }
    }

    // 资源释放
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&yuvFrame);
    avcodec_close(codecCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}