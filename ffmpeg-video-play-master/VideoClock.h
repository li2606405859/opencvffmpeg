#ifndef VIDEO_CLOCK_H
#define VIDEO_CLOCK_H

#include <SDL.h>

// 速度限制常量
const double MIN_SPEED = 0.5;
const double MAX_SPEED = 4.0;
/**
 * 基于系统时间的视频时钟类
 * 核心思想：维护一个虚拟的播放开始时间 start_time
 * 当前视频时间 = (SDL_GetTicks() - start_time) * speed
 */
class VideoClock {
private:
    Uint32 start_time;    // 虚拟播放开始时间（毫秒），用于计算当前视频时间
    double speed;         // 播放速度倍率
    bool paused;          // 暂停状态
    double pause_pts_ms;  // 暂停时的 PTS（毫秒），用于恢复时保持时间轴连续

public:
    /**
     * 构造函数
     */
    VideoClock();

    /**
     * 获取当前视频时间（毫秒）
     */
    double get() const;

    /**
     * 校准时钟到指定 PTS（通过调整 start_time 实现）
     * @param pts_ms 目标视频时间（毫秒）
     */
    void syncToPts(double pts_ms);

    /**
     * 切换暂停/播放状态
     */
    void togglePause();

    /**
     * 调整播放速度（增加或减少）
     * @param delta_speed 速度变化量（正数加速，负数减速）
     */
    void adjustSpeed(double delta_speed);

    /**
     * 获取播放速度
     */
    double getSpeed() const { return speed; }

    /**
     * 获取暂停状态
     */
    bool isPaused() const { return paused; }
};

#endif // VIDEO_CLOCK_H
