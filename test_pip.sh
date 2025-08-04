#!/bin/bash

# FreeSWITCH 视频画中画模块测试脚本
# 功能：将远程视频叠加到本地MP4文件

echo "=== FreeSWITCH 视频画中画模块测试 ==="

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查测试视频文件
echo -e "${YELLOW}1. 检查测试视频文件...${NC}"
if [ ! -f "test_videos/local_video.mp4" ]; then
    echo -e "${RED}错误: 本地测试视频不存在${NC}"
    echo "请先运行以下命令创建测试视频："
    echo "ffmpeg -f lavfi -i \"color=blue:duration=30:size=640x480:rate=30\" -c:v libx264 -pix_fmt yuv420p test_videos/local_video.mp4"
    exit 1
fi

echo -e "${GREEN}✓ 本地测试视频存在${NC}"

# 编译模块
echo -e "${YELLOW}2. 编译模块...${NC}"
make clean
if make all; then
    echo -e "${GREEN}✓ 编译成功${NC}"
else
    echo -e "${RED}✗ 编译失败${NC}"
    exit 1
fi

# 安装模块
echo -e "${YELLOW}3. 安装模块...${NC}"
if make install; then
    echo -e "${GREEN}✓ 安装成功${NC}"
else
    echo -e "${RED}✗ 安装失败${NC}"
    exit 1
fi

# 检查FreeSWITCH状态
echo -e "${YELLOW}4. 检查FreeSWITCH状态...${NC}"
if pgrep -x "freeswitch" > /dev/null; then
    echo -e "${GREEN}✓ FreeSWITCH正在运行${NC}"
else
    echo -e "${RED}✗ FreeSWITCH未运行，请启动FreeSWITCH${NC}"
    exit 1
fi

# 卸载旧模块
echo -e "${YELLOW}5. 卸载旧模块...${NC}"
sudo /usr/local/freeswitch/bin/fs_cli -x "unload mod_video_pip" 2>/dev/null || true

sleep 2

# 加载新模块
echo -e "${YELLOW}6. 加载新模块...${NC}"
if sudo /usr/local/freeswitch/bin/fs_cli -x "load mod_video_pip"; then
    echo -e "${GREEN}✓ 模块加载成功${NC}"
else
    echo -e "${RED}✗ 模块加载失败${NC}"
    exit 1
fi

# 显示模块状态
echo -e "${YELLOW}7. 检查模块状态...${NC}"
sudo /usr/local/freeswitch/bin/fs_cli -x "video_pip_status"

echo ""
echo -e "${GREEN}=== 测试完成 ===${NC}"
echo ""
echo "模块已成功加载！现在可以："
echo "1. 使用MicroSIP进行视频通话"
echo "2. 获取通话的UUID："
echo "   sudo /usr/local/freeswitch/bin/fs_cli -x \"show calls\""
echo ""
echo "3. 启动画中画（使用实际的UUID）："
echo "   sudo /usr/local/freeswitch/bin/fs_cli -x \"video_pip_start <UUID> test_videos/local_video.mp4\""
echo ""
echo "4. 查看状态："
echo "   sudo /usr/local/freeswitch/bin/fs_cli -x \"video_pip_status <UUID>\""
echo ""
echo "5. 停止画中画："
echo "   sudo /usr/local/freeswitch/bin/fs_cli -x \"video_pip_stop <UUID>\""
