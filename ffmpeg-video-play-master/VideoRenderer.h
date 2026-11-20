#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

extern "C" {
#include "libavformat/avformat.h"
}
#include <SDL.h>

/**
 * 视频渲染器类
 */
class VideoRenderer {
private:
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Rect render_rect;
    int video_width;
    int video_height;

public:
    /**
     * 构造函数
     */
    VideoRenderer();

    /**
     * 析构函数
     */
    ~VideoRenderer();

    /**
     * 初始化渲染器
     */
    int init(int width, int height);

    /**
     * 清理渲染器资源
     */
    void cleanup();

    /**
     * 渲染一帧视频
     */
    int render(AVFrame *frame);

    /**
     * 处理窗口大小改变事件
     */
    void handleResize(int new_width, int new_height);
};

#endif // VIDEO_RENDERER_H
