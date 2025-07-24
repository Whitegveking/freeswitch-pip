/*
 * Video Picture-in-Picture Module Header
 * 视频画中画模块头文件
 */

#ifndef MOD_VIDEO_PIP_H
#define MOD_VIDEO_PIP_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* 模块版本 */
#define VIDEO_PIP_VERSION "1.0.0"

/* 默认画中画参数 */
#define DEFAULT_PIP_WIDTH 320
#define DEFAULT_PIP_HEIGHT 240
#define DEFAULT_PIP_X 10
#define DEFAULT_PIP_Y 10
#define DEFAULT_PIP_OPACITY 0.8f

/* 最大路径长度 */
#define MAX_PATH_LENGTH 1024
#define MAX_TEXT_LENGTH 512

/* 视频格式定义 */
typedef enum {
    VIDEO_FORMAT_YUV420P,
    VIDEO_FORMAT_RGB24,
    VIDEO_FORMAT_RGBA
} video_format_t;

/* 画中画位置枚举 */
typedef enum {
    PIP_POSITION_TOP_LEFT,
    PIP_POSITION_TOP_RIGHT,
    PIP_POSITION_BOTTOM_LEFT,
    PIP_POSITION_BOTTOM_RIGHT,
    PIP_POSITION_CUSTOM
} pip_position_t;

/* 字幕样式结构 */
typedef struct {
    char *font_family;
    int font_size;
    char *font_color;
    int x;
    int y;
    float opacity;
} subtitle_style_t;

/* 背景图片配置 */
typedef struct {
    char *image_path;
    float opacity;
    int blend_mode;
} background_config_t;

/* 扩展的配置结构 */
typedef struct {
    char *background_image;
    char *subtitle_font;
    int pip_width;
    int pip_height;
    int pip_x;
    int pip_y;
    float pip_opacity;
    int enable_subtitle;
    char *subtitle_text;
    subtitle_style_t subtitle_style;
    background_config_t background;
    pip_position_t position;
    video_format_t input_format;
    video_format_t output_format;
} pip_config_t;

/* 视频帧缓冲区 */
typedef struct {
    uint8_t *data[4];
    int linesize[4];
    int width;
    int height;
    int64_t pts;
    video_format_t format;
} video_frame_buffer_t;

/* 滤镜链节点 */
typedef struct filter_node {
    char *filter_name;
    char *filter_args;
    struct filter_node *next;
} filter_node_t;

/* 扩展的视频处理上下文 */
typedef struct {
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *pip_buffersrc_ctx;
    AVFilterGraph *filter_graph;
    AVFrame *frame_in;
    AVFrame *frame_out;
    AVFrame *local_frame;
    AVFrame *remote_frame;
    AVFrame *background_frame;
    struct SwsContext *sws_ctx;
    pip_config_t config;
    void *mutex;  /* switch_mutex_t 的占位符 */
    void *pool;   /* switch_memory_pool_t 的占位符 */
    filter_node_t *custom_filters;
    int initialized;
    int frame_count;
    int64_t start_time;
} video_pip_context_t;

/* 函数声明 */

/* 初始化函数 */
int video_pip_init(void);
void video_pip_cleanup(void);

/* 上下文管理 */
video_pip_context_t* video_pip_create_context(void *pool);
void video_pip_destroy_context(video_pip_context_t *ctx);

/* 配置管理 */
int video_pip_load_config(pip_config_t *config, const char *config_file);
int video_pip_save_config(const pip_config_t *config, const char *config_file);
void video_pip_set_default_config(pip_config_t *config);

/* 滤镜管理 */
int video_pip_init_filter_graph(video_pip_context_t *ctx, int width, int height);
int video_pip_add_custom_filter(video_pip_context_t *ctx, const char *filter_name, const char *filter_args);
void video_pip_clear_custom_filters(video_pip_context_t *ctx);

/* 视频处理 */
int video_pip_process_frame(video_pip_context_t *ctx, 
                           video_frame_buffer_t *local_frame,
                           video_frame_buffer_t *remote_frame,
                           video_frame_buffer_t *output_frame);

/* 背景处理 */
int video_pip_load_background_image(video_pip_context_t *ctx, const char *image_path);
int video_pip_set_background_opacity(video_pip_context_t *ctx, float opacity);

/* 字幕处理 */
int video_pip_add_subtitle(video_pip_context_t *ctx, const char *text, const subtitle_style_t *style);
int video_pip_update_subtitle(video_pip_context_t *ctx, const char *text);
void video_pip_clear_subtitle(video_pip_context_t *ctx);

/* 工具函数 */
int video_pip_convert_frame_format(const video_frame_buffer_t *src, 
                                  video_frame_buffer_t *dst, 
                                  video_format_t target_format);
int video_pip_scale_frame(const video_frame_buffer_t *src, 
                         video_frame_buffer_t *dst, 
                         int target_width, int target_height);

/* 调试和日志 */
void video_pip_log_config(const pip_config_t *config);
void video_pip_log_context_info(const video_pip_context_t *ctx);

#endif /* MOD_VIDEO_PIP_H */
