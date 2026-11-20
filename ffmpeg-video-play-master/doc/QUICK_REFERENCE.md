# 快速参考指南

## 📂 文件说明

| 文件 | 作用 | 行数 |
|------|------|------|
| `main.cpp` | 程序入口，主循环 | 69 |
| `video_clock.h/cpp` | 时钟管理 | 48+64 |
| `video_renderer.h/cpp` | SDL渲染 | 43+107 |
| `video_player.h/cpp` | 播放器核心 | 87+375 |

## 🔑 关键API

### VideoClock（时钟）
```cpp
video_clock_init(&clock);              // 初始化
double t = video_clock_get(&clock);    // 获取当前时间
video_clock_sync_to_pts(&clock, pts);  // 校准时钟到指定PTS
video_clock_toggle_pause(&clock);      // 暂停/恢复
video_clock_adjust_speed(&clock, 0.25);// 调整倍速（正数加速，负数减速）
```

### VideoRenderer（渲染器）
```cpp
video_renderer_init(&renderer, w, h);           // 初始化
video_renderer_render(&renderer, frame);        // 渲染一帧
video_renderer_handle_resize(&r, w, h);         // 处理窗口调整
video_renderer_cleanup(&renderer);              // 清理资源
```

### VideoPlayer（播放器）
```cpp
video_player_init(&player, url);                // 初始化
video_player_decode_frame(&player);             // 解码一帧
bool running = video_player_handle_event(&p);   // 处理SDL事件
video_player_handle_keydown(&p, key);           // 处理按键
video_player_seek(&player, 5.0);                // Seek操作
video_player_sync_and_render(&player);          // 同步并渲染
video_player_render_current_frame(&player);     // 仅渲染当前帧
video_player_cleanup(&player);                  // 清理资源
```

## 🎯 主循环流程

```
┌─────────────┐
│  处理事件    │ ← video_player_handle_event
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  是否暂停？  │ → 是 → 重绘当前帧 + Delay(5ms)
└──────┬──────┘
       │ 否
       ▼
┌─────────────┐
│  解码一帧    │ ← video_player_decode_frame
└──────┬──────┘
       │ ret == -1: 错误/结束 → 退出
       │ ret == -2: 非视频包 → 继续解码
       │ ret == 0: 成功 → 继续
       ▼
┌─────────────┐
│ 同步并渲染   │ ← video_player_sync_and_render
└──────┬──────┘
       │
       └──→ 回到开始
```

## ⌨️ 快捷键映射

| 按键 | 函数调用 | 说明 |
|------|---------|------|
| SPACE | `video_clock_toggle_pause()` | 暂停/播放 |
| S | `is_step = true` | 逐帧模式 |
| [ | `video_clock_adjust_speed(clock, -0.25)` | 减速 |
| ] | `video_clock_adjust_speed(clock, +0.25)` | 加速 |
| ← | `video_player_seek(player, -5.0)` | 后退5秒 |
| → | `video_player_seek(player, +5.0)` | 前进5秒 |

## 🔧 常用修改

### 修改视频路径
编辑 `main.cpp` 第7行：
```cpp
const char *url = "你的视频路径.mp4";
```

### 调整倍速范围
编辑 `video_player.h` 第33-35行：
```cpp
const double MIN_SPEED = 0.5;   // 最小倍速
const double MAX_SPEED = 4.0;   // 最大倍速
const double SCALE_SPEED = 0.25; // 每次调整幅度
```

### 修改Seek步长
编辑 `video_player.cpp` 第240行和245行：
```cpp
video_player_seek(player, -5.0);  // 改为其他秒数
video_player_seek(player, 5.0);
```

### 调整窗口初始大小
编辑 `video_renderer.cpp` 第15-20行：
```cpp
SDL_CreateWindow(
    "FFmpeg Video Player",
    SDL_WINDOWPOS_UNDEFINED,
    SDL_WINDOWPOS_UNDEFINED,
    width,   // 宽度
    height,  // 高度
    ...
);
```

## 🐛 调试技巧

### 查看时钟状态
在任意位置添加：
```cpp
printf("Clock: time=%.2f, speed=%.2f, paused=%d\n",
       clock.video_time, clock.speed, clock.paused);
```

### 查看帧信息
在 `video_player_sync_and_render` 中添加：
```cpp
printf("Frame: pts=%.2f, clock=%.2f, delay=%.2f\n",
       frame_pts, clock_time, delay);
```

### 查看性能统计
已内置每100帧输出一次统计信息

## 📝 扩展功能示例

### 添加新的快捷键
在 `video_player_handle_keydown` 中添加：
```cpp
case SDLK_your_key: {
    // 你的逻辑
    break;
}
```

### 添加进度显示
在 `video_player_sync_and_render` 中添加：
```cpp
// 获取视频总时长
double duration = player->format_ctx->duration / (double)AV_TIME_BASE;
// 计算进度百分比
double progress = frame_pts / duration * 100;
printf("Progress: %.1f%%\n", progress);
```

### 保持宽高比缩放
修改 `video_renderer_handle_resize`：
```cpp
// 计算保持宽高比的尺寸
float video_ratio = (float)renderer->video_width / renderer->video_height;
float window_ratio = (float)new_width / new_height;

if (window_ratio > video_ratio) {
    // 窗口更宽，以高度为准
    render_rect.h = new_height;
    render_rect.w = (int)(new_height * video_ratio);
    render_rect.x = (new_width - render_rect.w) / 2;
    render_rect.y = 0;
} else {
    // 窗口更高，以宽度为准
    render_rect.w = new_width;
    render_rect.h = (int)(new_width / video_ratio);
    render_rect.x = 0;
    render_rect.y = (new_height - render_rect.h) / 2;
}
```

## 💡 最佳实践

1. **不要直接访问结构体成员**，使用提供的API函数
2. **检查返回值**，所有init函数都可能失败
3. **及时清理资源**，使用cleanup函数
4. **模块化思维**，每个模块只关心自己的职责
5. **保持简洁**，避免在主循环中添加复杂逻辑
