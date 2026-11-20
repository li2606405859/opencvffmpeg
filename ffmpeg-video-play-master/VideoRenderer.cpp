#include "VideoRenderer.h"
#include <stdio.h>

VideoRenderer::VideoRenderer() {
    window = nullptr;
    renderer = nullptr;
    texture = nullptr;
    video_width = 0;
    video_height = 0;
    render_rect = {0, 0, 0, 0};
}

VideoRenderer::~VideoRenderer() {
    cleanup();
}

int VideoRenderer::init(int width, int height) {
    video_width = width;
    video_height = height;
    
    // 初始化 SDL 视频子系统
    if (SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }
    
    // 创建窗口（支持调整大小）
    window = SDL_CreateWindow(
        "FFmpeg Video Player",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        width,
        height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        printf("SDL_CreateWindow error: %s\n", SDL_GetError());
        return -1;
    }
    
    // 创建渲染器（硬件加速 + 垂直同步）
    renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        printf("SDL_CreateRenderer error: %s\n", SDL_GetError());
        return -1;
    }
    
    // 设置渲染缩放质量为线性插值
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    
    // 创建纹理（YV12 格式）
    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );
    if (!texture) {
        printf("SDL_CreateTexture error: %s\n", SDL_GetError());
        return -1;
    }
    
    // 初始化渲染区域
    render_rect.x = 0;
    render_rect.y = 0;
    render_rect.w = width;
    render_rect.h = height;
    
    return 0;
}

void VideoRenderer::cleanup() {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
}

int VideoRenderer::render(AVFrame *frame) {
    // 清空渲染器
    SDL_RenderClear(renderer);
    
    // 更新纹理数据
    SDL_UpdateYUVTexture(
        texture,
        NULL,
        frame->data[0], frame->linesize[0],
        frame->data[1], frame->linesize[1],
        frame->data[2], frame->linesize[2]
    );
    
    // 复制纹理到渲染目标（自动拉伸）
    SDL_RenderCopy(renderer, texture, NULL, &render_rect);
    
    // 呈现到屏幕
    SDL_RenderPresent(renderer);
    
    return 0;
}

void VideoRenderer::handleResize(int new_width, int new_height) {
    render_rect.w = new_width;
    render_rect.h = new_height;
    printf("Window resized to %dx%d\n", new_width, new_height);
}
