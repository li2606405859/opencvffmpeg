#include "VideoClock.h"

VideoClock::VideoClock() {
    start_time = SDL_GetTicks();
    speed = 1.0;
    paused = false;
    pause_pts_ms = 0;
}

double VideoClock::get() const {
    // 如果处于暂停状态，返回暂停时的 PTS（毫秒）
    if (paused) {
        return pause_pts_ms;
    }
    
    // 当前视频时间（毫秒）= (当前系统时间 - 虚拟开始时间) * 倍速
    Uint32 elapsed_ms = SDL_GetTicks() - start_time;
    return elapsed_ms * speed;
}

void VideoClock::syncToPts(double pts_ms) {
    // 根据目标 PTS（毫秒）反推 start_time
    // pts_ms = (SDL_GetTicks() - start_time) * speed
    // start_time = SDL_GetTicks() - (pts_ms / speed)
    start_time = SDL_GetTicks() - (Uint32)(pts_ms / speed);
}

void VideoClock::togglePause() {
    double current_pts_ms = get();
    paused = !paused;

    if (paused) {
        // 暂停时记录当前 PTS（毫秒）
        pause_pts_ms = current_pts_ms;
    } else {
        // 恢复播放时，根据 pause_pts_ms 重新校准 start_time
        // 保证恢复后从暂停的位置继续播放
        syncToPts(pause_pts_ms);
    }
}

void VideoClock::adjustSpeed(double delta_speed) {
    // 计算新速度
    double new_speed = speed + delta_speed;
    
    // 边界检查
    if (new_speed < MIN_SPEED) {
        new_speed = MIN_SPEED;
    }
    if (new_speed > MAX_SPEED) {
        new_speed = MAX_SPEED;
    }

    /*
     * 假设播放了 10 min，那么此时 current_pts_ms = 10 min，将 speed 设置为 2.0 之后，再调用 syncToPts(10) ，就会将 start_time 设置为 5 min
     * syncToPts 就是根据传入的 pts，判断现在的 start_time 应该是多少并赋值
     */
    // 改变速度时，先获取当前视频时间（毫秒），然后校准 start_time
    double current_pts_ms = get();
    speed = new_speed;
    // 根据新的速度重新校准 start_time，保持当前 PTS 不变
    syncToPts(current_pts_ms);
}
