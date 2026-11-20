# FFmpeg Video Player - 重构版本

## 项目结构

```
ffmpeg-1/
├── main.cpp              # 主程序入口（69行，简洁清晰）
├── video_clock.h/cpp     # 视频时钟模块（时间同步、倍速控制）
├── video_renderer.h/cpp  # 视频渲染模块（SDL窗口、纹理管理）
├── video_player.h/cpp    # 播放器核心模块（FFmpeg解码、事件处理）
├── CMakeLists.txt        # CMake构建配置
└── res/                  # 资源文件
```

## 模块说明

### 1. video_clock (视频时钟)
**职责**：管理视频播放时间同步
- 只有4个核心变量：`start_time`, `speed`, `paused`, `pause_pts`
- 提供简洁的API：init, get, sync_to_pts, toggle_pause, adjust_speed
- 支持暂停、恢复、变速播放

**核心公式**：
```
当前视频时间 = (SDL_GetTicks() - start_time) × speed / 1000
```

### 2. video_renderer (视频渲染器)
**职责**：管理SDL窗口和渲染
- 窗口创建和销毁
- 纹理管理（YV12格式）
- 支持窗口大小调整（自动拉伸）
- 硬件加速 + 垂直同步

**特性**：
- 可拖拽调整窗口大小
- 线性插值缩放（平滑效果）
- 自动填充整个窗口

### 3. video_player (播放器核心)
**职责**：整合FFmpeg解码和业务逻辑
- FFmpeg初始化和清理
- 视频帧解码
- SDL事件处理（退出、窗口调整等）
- 键盘事件处理（暂停、快进、变速等）
- Seek功能实现
- 音视频同步

**功能**：
- SPACE: 暂停/播放
- S: 逐帧播放
- [ / ]: 减速/加速（0.5x - 4.0x）
- ← / →: 后退/前进5秒

**新增函数**：
- `video_player_handle_event()`: 统一处理SDL事件
- `video_player_seek()`: Seek功能，支持丢弃非关键帧
- `video_player_render_current_frame()`: 仅渲染当前帧（用于暂停时重绘）

### 4. main.cpp (主程序)
**职责**：程序入口和主循环
- 初始化播放器
- 事件分发
- 主播放循环
- 资源清理

**代码量**：仅69行！

**主循环逻辑**：
1. 处理SDL事件（退出、窗口调整、按键）
2. 如果暂停：重绘当前帧 + Delay(5ms)降低CPU占用
3. 解码一帧（处理返回值：成功/失败/非视频包）
4. 同步并渲染

## 编译运行

```bash
# 使用CMake
mkdir build
cd build
cmake ..
cmake --build .

# 运行
./ffmpeg-video-play
```

## 优势对比

### 重构前
- ❌ 单个文件544行，难以维护
- ❌ 所有逻辑混在一起
- ❌ 时钟控制复杂（8个变量）
- ❌ 重复代码多

### 重构后
- ✅ 模块化设计，职责清晰
- ✅ 主文件仅69行
- ✅ 时钟简化为4个变量
- ✅ 易于扩展和维护
- ✅ 支持窗口拉伸
- ✅ 完善的Seek功能
- ✅ 更好的错误处理
- ✅ 优化的逐帧播放逻辑

## 设计原则

1. **单一职责**：每个模块只负责一个功能
2. **高内聚低耦合**：模块间通过清晰的接口交互
3. **简洁性**：用最少的变量实现功能
4. **可扩展性**：易于添加新功能（如音频支持）
