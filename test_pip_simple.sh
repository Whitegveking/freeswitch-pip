#!/bin/bash

echo "=== FreeSWITCH视频画中画简化测试 ==="

# 检查视频文件
LOCAL_VIDEO="/home/white/桌面/freeswitch-video-pip-module/test_videos/pip_video.mp4"

if [ ! -f "$LOCAL_VIDEO" ]; then
    echo "错误：找不到本地视频文件 $LOCAL_VIDEO"
    exit 1
fi

echo "1. 本地视频文件检查: OK"
echo "   路径: $LOCAL_VIDEO"

# 检查文件权限
ls -la "$LOCAL_VIDEO"

# 使用ffprobe检查视频信息
echo ""
echo "2. 视频文件信息:"
ffprobe -v quiet -print_format json -show_format -show_streams "$LOCAL_VIDEO"

echo ""
echo "3. 测试说明:"
echo "   - 确保MicroSIP已连接到FreeSWITCH"
echo "   - 进行视频通话"
echo "   - 在通话过程中执行: video_pip_start <UUID> $LOCAL_VIDEO"
echo "   - 停止画中画: video_pip_stop <UUID>"
echo ""
echo "4. 当前活跃会话:"
fs_cli -x "show channels"
