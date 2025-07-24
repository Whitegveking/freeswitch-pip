# FreeSWITCH 视频画中画 (PIP) 模块

一个用于 FreeSWITCH 的高性能视频画中画模块，支持实时视频叠加和可配置的显示效果。

## 功能特性

- ✅ **真实视频叠加**: 将一个会话的视频实时叠加到另一个会话的视频上
- ✅ **灵活定位**: 支持四个预设位置（左上、右上、左下、右下）
- ✅ **可调大小**: 支持 0.1-0.5 倍缩放比例
- ✅ **简洁边框**: 3像素黑色边框，视觉效果清晰不干扰
- ✅ **动态调整**: 运行时可实时修改PIP位置和大小
- ✅ **线程安全**: 完整的互斥锁保护，支持并发操作
- ✅ **资源管理**: 自动清理视频帧缓存，防止内存泄漏

## 系统要求

- FreeSWITCH 1.8+
- 支持 I420 格式的视频编解码器
- Linux/Unix 系统（推荐）

## 安装

### 1. 编译模块

```bash
# 将源码文件放置到 FreeSWITCH 源码目录
cp mod_video_pip.c /usr/src/freeswitch/src/mod/applications/

# 编译模块
cd /usr/src/freeswitch
make mod_video_pip
make mod_video_pip-install
```

### 2. 加载模块

在 FreeSWITCH 配置文件中添加：

```xml
<!-- conf/autoload_configs/modules.conf.xml -->
<load module="mod_video_pip"/>
```

或者在 FreeSWITCH 控制台手动加载：

```
freeswitch> load mod_video_pip
```

## API 使用说明

### 启用 PIP

```bash
# 语法: enable_pip <主会话UUID> <PIP会话UUID>
freeswitch> enable_pip 12345678-1234-1234-1234-123456789012 87654321-4321-4321-4321-210987654321
+OK 真实视频PIP已启用 (视频叠加+简洁黑边框)
```

### 禁用 PIP

```bash
# 语法: disable_pip <会话UUID>
freeswitch> disable_pip 12345678-1234-1234-1234-123456789012
+OK PIP已禁用
```

### 设置 PIP 位置

```bash
# 语法: pip_position <会话UUID> <位置>
# 位置选项: top_left, top_right, bottom_left, bottom_right
freeswitch> pip_position 12345678-1234-1234-1234-123456789012 top_left
+OK PIP位置已更新
```

### 设置 PIP 大小

```bash
# 语法: pip_size <会话UUID> <缩放比例>
# 比例范围: 0.1 - 0.5
freeswitch> pip_size 12345678-1234-1234-1234-123456789012 0.3
+OK PIP大小已更新
```

### 查看 PIP 状态

```bash
freeswitch> pip_status
+OK 真实视频PIP状态 (视频叠加+简洁黑边框):
  会话: 12345678-1234-1234-1234-123456789012
    位置: top_right, 大小: 0.25, 活跃: 是
    分辨率: 1280x720, PIP: 320x180 在 (940,20)
    视频帧: 就绪, 最后更新: 16742微秒前
总计: 1 个真实视频PIP会话
```

## 配置参数

### PIP 位置选项

| 位置           | 说明   | 坐标计算                                           |
| -------------- | ------ | -------------------------------------------------- |
| `top_left`     | 左上角 | (margin, margin)                                   |
| `top_right`    | 右上角 | (width-pip_width-margin, margin)                   |
| `bottom_left`  | 左下角 | (margin, height-pip_height-margin)                 |
| `bottom_right` | 右下角 | (width-pip_width-margin, height-pip_height-margin) |

### 默认设置

- **默认位置**: 右上角 (`top_right`)
- **默认大小**: 0.25 (25% 缩放)
- **边框间距**: 20像素
- **边框厚度**: 3像素
- **边框颜色**: 黑色

## 使用场景

### 视频会议

```bash
# 主持人会话显示参会者的小窗口
enable_pip host-session-uuid participant-session-uuid
pip_position host-session-uuid bottom_right
pip_size host-session-uuid 0.2
```

### 屏幕共享

```bash
# 在共享屏幕上显示演讲者视频
enable_pip screen-share-uuid presenter-video-uuid
pip_position screen-share-uuid top_left
pip_size screen-share-uuid 0.15
```

### 监控场景

```bash
# 在主监控画面上叠加次要画面
enable_pip main-monitor-uuid secondary-uuid
pip_position main-monitor-uuid bottom_left
```

## 性能优化

### 视频格式支持

- **推荐格式**: I420 (YUV420P)
- **缩放算法**: 最近邻插值（性能优化）
- **内存管理**: 自动视频帧缓存和释放

### 资源消耗

- **CPU 占用**: 约 5-15% (取决于分辨率和帧率)
- **内存占用**: 每个 PIP 会话约 2-8MB
- **网络带宽**: 无额外带宽消耗

## 故障排除

### 常见问题

#### 1. PIP 不显示

```bash
# 检查会话状态
show channels
# 确认视频编解码器
show channel <uuid> codec
# 查看模块日志
console loglevel debug
```

#### 2. 视频质量问题

```bash
# 调整 PIP 大小
pip_size <uuid> 0.2  # 减小尺寸提升质量
# 检查原始视频分辨率
pip_status
```

#### 3. 性能问题

```bash
# 减少并发 PIP 会话数量
# 降低视频分辨率和帧率
# 检查系统资源使用情况
top -p `pidof freeswitch`
```

### 日志调试

```bash
# 启用调试日志
console loglevel debug
# 查看 PIP 相关日志
grep "PIP" /usr/local/freeswitch/log/freeswitch.log
```

## 技术实现

### 架构设计

- **媒体钩子**: 使用 FreeSWITCH 媒体 bug 机制
- **视频处理**: I420 格式 YUV 平面处理
- **线程安全**: 递归互斥锁保护
- **内存管理**: 基于会话的内存池

### 核心算法

- **位置计算**: 基于主视频分辨率和边距的动态计算
- **视频缩放**: 最近邻插值算法
- **帧叠加**: 逐像素 YUV 数据复制
- **边框绘制**: Y 平面像素直接设置

## 开发计划

### 待实现功能

-  支持更多视频格式 (NV12, RGB)
-  高质量缩放算法 (双线性插值)
-  透明度/Alpha 混合支持
-  动画过渡效果
-  多 PIP 窗口支持
-  自定义边框样式

### 性能优化

-  SIMD 指令集优化
-  GPU 加速支持
-  多线程处理
-  缓存优化

## 许可证

本项目基于 MPL 2.0 许可证开源，与 FreeSWITCH 保持一致。

## 贡献

欢迎提交 Issue 和 Pull Request！

### 代码风格

- 遵循 FreeSWITCH 代码规范
- 使用 4 空格缩进
- 添加详细的函数注释
- 保持线程安全

------

**注意**: 本模块仍在活跃开发中，生产环境使用前请充分测试。