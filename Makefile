# FreeSWITCH Video Picture-in-Picture Module Makefile
# 专为 FFmpeg 4.4 静态编译版本和 FreeSWITCH 1.10.12 优化

# 基本设置
CC = gcc
CFLAGS = -fPIC -g -ggdb -Wall -std=c99 -O2
LDFLAGS = -shared

# FreeSWITCH相关路径 (适配1.10.12)
FS_INCLUDES = /usr/local/freeswitch/include/freeswitch
FS_MODULES = /usr/local/freeswitch/mod

# FFmpeg路径选择 (使用系统级FFmpeg 4.4)
FFMPEG_DIR = /usr/local
FFMPEG_INCLUDES = $(FFMPEG_DIR)/include
FFMPEG_LIBS = $(FFMPEG_DIR)/lib/libavcodec.a \
              $(FFMPEG_DIR)/lib/libavformat.a \
              $(FFMPEG_DIR)/lib/libavfilter.a \
              $(FFMPEG_DIR)/lib/libavutil.a \
              $(FFMPEG_DIR)/lib/libswscale.a \
              $(FFMPEG_DIR)/lib/libswresample.a

# 备选: 使用workspace中的FFmpeg 4.4 (如果系统级不可用)
# FFMPEG_DIR = $(shell pwd)/../ffmpeg-4.4
# FFMPEG_INCLUDES = $(FFMPEG_DIR)
# FFMPEG_LIBS = $(FFMPEG_DIR)/libavcodec/libavcodec.a \
#               $(FFMPEG_DIR)/libavformat/libavformat.a \
#               $(FFMPEG_DIR)/libavfilter/libavfilter.a \
#               $(FFMPEG_DIR)/libavutil/libavutil.a \
#               $(FFMPEG_DIR)/libswscale/libswscale.a \
#               $(FFMPEG_DIR)/libswresample/libswresample.a

# 项目路径
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
CONFIG_DIR = config

# 源文件和目标文件
SOURCE = $(SRC_DIR)/mod_video_pip.c
OBJECT = $(BUILD_DIR)/mod_video_pip.o
TARGET = $(BUILD_DIR)/mod_video_pip.so

# 编译选项 (使用pkg-config获取FFmpeg的编译选项)
INCLUDES = -I$(INCLUDE_DIR) -I$(FS_INCLUDES) $(FFMPEG_CFLAGS)

# 链接库 (使用pkg-config获取正确的FFmpeg链接选项)
FFMPEG_CFLAGS = $(shell pkg-config --cflags libavcodec libavformat libavfilter libavutil libswscale libswresample 2>/dev/null || echo "-I$(FFMPEG_INCLUDES)")
FFMPEG_LDFLAGS = $(shell pkg-config --libs --static libavcodec libavformat libavfilter libavutil libswscale libswresample 2>/dev/null || echo "$(FFMPEG_LIBS) -lm -lz -lpthread -ldl")
LIBS = $(FFMPEG_LDFLAGS)

# 默认目标
all: $(TARGET)

# 创建构建目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译目标库
$(TARGET): $(BUILD_DIR) $(OBJECT)
	$(CC) $(LDFLAGS) $(OBJECT) $(LIBS) -o $@

# 编译对象文件
$(OBJECT): $(SOURCE)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 检查FFmpeg库是否存在
check-ffmpeg:
	@echo "检查FFmpeg库..."
	@if [ ! -f "$(FFMPEG_DIR)/lib/libavcodec.a" ]; then \
		echo "错误: FFmpeg静态库不存在: $(FFMPEG_DIR)/lib/libavcodec.a"; \
		echo "当前使用路径: $(FFMPEG_DIR)"; \
		echo "请检查FFmpeg是否正确安装或修改Makefile中的路径"; \
		exit 1; \
	fi
	@echo "FFmpeg库检查通过: $(FFMPEG_DIR)"

# 检查FreeSWITCH头文件
check-freeswitch:
	@echo "检查FreeSWITCH开发环境..."
	@if [ ! -d "$(FS_INCLUDES)" ]; then \
		echo "错误: FreeSWITCH头文件目录不存在: $(FS_INCLUDES)"; \
		echo "请确保FreeSWITCH开发包已安装"; \
		exit 1; \
	fi
	@echo "FreeSWITCH开发环境检查通过"

# 验证环境（手动调用）
check: check-freeswitch check-ffmpeg
	@echo "所有依赖检查通过，可以开始编译"

# 安装模块
install: $(TARGET)
	sudo cp $(TARGET) $(FS_MODULES)/mod_video_pip.so

# 卸载模块
uninstall:
	sudo rm -f $(FS_MODULES)/mod_video_pip.so

# 清理构建文件
clean:
	rm -rf $(BUILD_DIR)

# 重新编译并安装
rebuild: clean all install

# 重新加载模块（需要FreeSWITCH运行中）
reload:
	sudo /usr/local/freeswitch/bin/fs_cli -x "reload mod_video_pip"

# 安全的重新加载（带延迟）
safe-reload:
	@echo "卸载模块..."
	sudo /usr/local/freeswitch/bin/fs_cli -x "unload mod_video_pip" || true
	@echo "等待资源清理..."
	sleep 2
	@echo "重新加载模块..."
	sudo /usr/local/freeswitch/bin/fs_cli -x "load mod_video_pip"

# 快速测试（编译、安装、安全重载）
quick: 
	@echo "=== 开始快速部署 ==="
	@echo "1. 卸载旧模块..."
	sudo /usr/local/freeswitch/bin/fs_cli -x "unload mod_video_pip"
	@echo "2. 清理构建文件..."
	$(MAKE) clean
	@echo "3. 等待资源释放..."
	sleep 3
	@echo "4. 重新编译..."
	$(MAKE) all
	@echo "5. 安装新模块..."
	$(MAKE) install
	@echo "6. 等待文件系统同步..."
	sync && sleep 2
	@echo "7. 加载新模块..."
	sudo /usr/local/freeswitch/bin/fs_cli -x "load mod_video_pip"
	@echo "=== 部署完成 ==="

# 检查模块状态
status:
	sudo /usr/local/freeswitch/bin/fs_cli -x "pip_status"

help:
	@echo "FreeSWITCH Video PIP Module Build System"
	@echo "适配: FFmpeg 4.4 + FreeSWITCH 1.10.12 + MicroSIP"
	@echo ""
	@echo "Available targets:"
	@echo "  all         - 编译模块"
	@echo "  check       - 检查编译环境和依赖"
	@echo "  install     - 安装模块到FreeSWITCH"
	@echo "  uninstall   - 卸载模块"
	@echo "  clean       - 清理构建文件"
	@echo "  rebuild     - 重新编译并安装"
	@echo "  reload      - 重新加载模块"
	@echo "  safe-reload - 安全重新加载（带延迟）"
	@echo "  quick       - 快速测试（编译+安装+安全重载）"
	@echo "  status      - 检查PIP模块状态"
	@echo "  help        - 显示此帮助信息"
	@echo ""
	@echo "当前配置："
	@echo "  FreeSWITCH: $(FS_INCLUDES)"
	@echo "  FFmpeg:     $(FFMPEG_DIR)"
	@echo ""
	@echo "模块功能："
	@echo "  - 远程视频画中画叠加到本地视频"
	@echo "  - 支持MicroSIP客户端"
	@echo "  - 实时位置和大小调整"
	@echo "  - 线程安全设计"

# 调试构建
debug: CFLAGS += -DDEBUG -O0
debug: $(TARGET)

# 发布构建
release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

.PHONY: all check check-ffmpeg check-freeswitch install uninstall clean rebuild reload safe-reload quick status help debug release
