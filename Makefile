# FreeSWITCH Video Picture-in-Picture Module Makefile

# 基本设置
CC = gcc
CFLAGS = -fPIC -g -ggdb -Wall -Werror -std=c99
LDFLAGS = -shared

# FreeSWITCH相关路径
FS_INCLUDES = /usr/local/freeswitch/include/freeswitch
FS_MODULES = /usr/local/freeswitch/lib/freeswitch/mod

# 项目路径
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
CONFIG_DIR = config

# 源文件和目标文件
SOURCE = $(SRC_DIR)/mod_video_pip.c
OBJECT = $(BUILD_DIR)/mod_video_pip.o
TARGET = $(BUILD_DIR)/mod_video_pip.so

# 编译选项
INCLUDES = -I$(INCLUDE_DIR) -I$(FS_INCLUDES)

# 默认目标
all: $(TARGET)

# 创建构建目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译目标库
$(TARGET): $(BUILD_DIR) $(OBJECT)
	$(CC) $(LDFLAGS) $(OBJECT) -o $@

# 编译对象文件
$(OBJECT): $(SOURCE)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

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
	sudo /usr/local/freeswitch/bin/fs_cli -x "unload mod_video_pip" || true
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

# 显示帮助
help:
	@echo "FreeSWITCH Video PIP Module Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all         - 编译模块"
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
	@echo "模块功能："
	@echo "  - 真实视频画中画叠加"
	@echo "  - 简洁黑色边框"
	@echo "  - 位置和大小调整"
	@echo "  - 防段错误安全设计"

# 调试构建
debug: CFLAGS += -DDEBUG -O0
debug: $(TARGET)

# 发布构建
release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

.PHONY: all install uninstall clean rebuild reload safe-reload quick status help debug release
