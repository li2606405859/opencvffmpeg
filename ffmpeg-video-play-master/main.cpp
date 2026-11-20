#include <iostream>
#include "VideoPlayer.h"

#undef main
int main() {
    // 视频文件路径（修改为你的视频路径）
    const char *url = "res/test.mp4";
    
    printf("=== FFmpeg Video Player ===\n");
    printf("Controls:\n");
    printf("  SPACE     - Play/Pause\n");
    printf("  S         - Step frame\n");
    printf("  [ / ]     - Speed down/up\n");
    printf("  LEFT/RIGHT - Seek -5s/+5s\n");
    printf("  ALT+F4    - Quit\n");
    printf("===========================\n\n");
    
    // 创建播放器
    VideoPlayer player;
    if (player.init(url) < 0) {
        printf("Failed to initialize player\n");
        return -1;
    }

    // 主循环
    while (true) {
        // 处理事件
        if (!player.handleEvent()) {
            break;
        }
        
        // 如果暂停,则不需要解码新帧和渲染
        if (player.isPaused()) {
            SDL_Delay(5);  // 暂停时降低 CPU 占用，但是这样会导致事件响应有延迟,毕竟这里 Delay 了。具体如何选择就看大家自己的需求了。
            continue;
        }
        
        // 解码一帧
        int ret = player.decodeFrame();
        if (ret == -1) {
            // 错误或播放结束
            break;
        } else if (ret == -2) {
            // 不是视频包，继续读取
            continue;
        }
        
        // 同步并渲染
        if (player.syncAndRender() < 0) {
            break;
        }
        
        // 打印性能统计（每100帧）
        player.printStats();
    }
    
    printf("\nVideo playback finished\n");
    
    return 0;
}
