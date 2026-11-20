#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
}
#include <SDL.h>
#include "VideoClock.h"
#include "VideoRenderer.h"

const double SCALE_SPEED = 0.25;     // 倍速调整步长
const double SEEK_SPEED = 5000.0;    // seek调整步长（毫秒）

/**
 * 视频播放器类
 */
class VideoPlayer {
private:
    // FFmpeg 相关
    AVFormatContext *input_format_ctx;
    AVCodecContext *codec_context;
    AVPacket *packet;
    AVFrame *frame;
    int video_idx;
    AVRational time_base;
    double duration_ms;       // 视频总时长（毫秒）
    
    // 时钟和渲染器
    VideoClock clock;
    VideoRenderer renderer;
    
    // 播放控制
    bool is_step;           // 逐帧模式

    // 事件控制
    SDL_Event event;
    
    // 统计信息
    int64_t frame_count;
    Uint32 last_time;

public:
    /**
     * 构造函数
     */
    VideoPlayer();

    /**
     * 析构函数
     */
    ~VideoPlayer();

    /**
     * 初始化播放器
     */
    int init(const char *url);

    /**
     * 清理播放器资源
     */
    void cleanup();

    /**
     * 处理 SDL 事件
     * @return true 继续运行, false 退出
     */
    bool handleEvent();

    /**
     * 处理键盘事件
     */
    bool handleKeydown(SDL_Keycode key);

    /**
     * 执行 seek 操作
     * @param offset_ms 偏移毫秒数（负数后退，正数前进）
     */
    void seek(double offset_ms);

    /**
    * 获取暂停状态
    */
    bool isPaused() const;

    /**
    * 读取并解码下一帧
    * @return 0 成功, -1 失败或结束, -2：非失败，但没获取到数据，需要再次调用读取
    */
    int decodeFrame();

    /**
    * 仅渲染当前帧（不更新时钟），用于暂停时窗口拉伸重绘
    */
    int renderCurrentFrame();

    /**
     * 同步并渲染当前帧
     */
    int syncAndRender();

    /**
     * 打印性能统计信息（每100帧）
     */
    void printStats();
};

#endif // VIDEO_PLAYER_H
