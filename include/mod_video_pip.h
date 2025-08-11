/*
 * FreeSWITCH Video Picture-in-Picture Module (简化版)
 * 专注于视频捕获、缩放和叠加功能
 * 适配: FFmpeg 4.4 + FreeSWITCH 1.10.12 + MicroSIP
 */

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <switch.h>
#include <unistd.h> /* for access() */
#include <string.h> /* for string functions */

/* 模块声明 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_pip_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_video_pip_load);
SWITCH_MODULE_DEFINITION(mod_video_pip, mod_video_pip_load, mod_video_pip_shutdown, NULL);

/* 简化的画中画会话数据 */
typedef struct pip_session_data
{
    switch_core_session_t *session;
    switch_channel_t *channel;

    /* 视频参数 */
    int main_width;
    int main_height;
    int pip_width;
    int pip_height;
    int pip_x;
    int pip_y;
    float pip_opacity;

    /* 远程视频参数（动态检测） */
    int remote_width;
    int remote_height;

    /* FFmpeg处理上下文 */
    struct SwsContext *sws_ctx_pip; /* 用于缩放PIP视频 */
    AVFrame *frame_main;            /* 本地视频帧（从mp4文件读取） */
    AVFrame *frame_pip;             /* 远程视频帧 */
    AVFrame *frame_pip_scaled;      /* 缩放后的远程视频帧 */
    AVFrame *frame_output;          /* 输出帧 */

    /* 本地视频文件处理 */
    AVFormatContext *local_fmt_ctx;  /* 本地MP4文件格式上下文 */
    AVCodecContext *local_codec_ctx; /* 本地视频解码器 */
    int local_video_stream_index;    /* 本地视频流索引 */
    AVPacket *local_packet;          /* 本地视频包 */

    /* 输出视频文件处理 */
    AVFormatContext *output_fmt_ctx;  /* 输出文件格式上下文 */
    AVCodecContext *output_codec_ctx; /* 输出视频编码器 */
    AVStream *output_stream;          /* 输出视频流 */
    AVPacket *output_packet;          /* 输出视频包 */
    char output_filename[256];        /* 输出文件名 */
    int64_t output_pts;               /* 输出视频PTS计数器 */

    /* 媒体钩子 */
    switch_media_bug_t *read_bug; /* 读取远程视频 */

    /* 帧缓存 */
    switch_frame_t *last_remote_frame; /* 最新的远程视频帧 */
    switch_mutex_t *frame_mutex;       /* 帧访问互斥锁 */

    /* 线程安全 */
    switch_mutex_t *mutex;
    switch_bool_t active;

    /* 统计 */
    uint64_t frames_processed;
    uint64_t remote_frames_count;
    uint64_t local_frames_count;

    /* 帧率同步 */
    double local_fps;        /* 本地视频文件的帧率 */
    double target_fps;       /* 目标输出帧率 */
    double local_frame_time; /* 本地视频每帧对应的时间间隔 */
    double current_time;     /* 当前处理的时间位置 */
    double last_local_time;  /* 上次读取本地帧的时间 */
} pip_session_data_t;

/* 全局变量 */
static switch_memory_pool_t *module_pool = NULL;
static switch_mutex_t *module_mutex = NULL;
static switch_hash_t *session_pip_map = NULL;

/* 默认参数 */
#define DEFAULT_PIP_WIDTH 320
#define DEFAULT_PIP_HEIGHT 240
#define DEFAULT_PIP_X 10
#define DEFAULT_PIP_Y 10
#define DEFAULT_PIP_OPACITY 0.8f

/* 函数声明 */
static switch_status_t read_local_video_frame(pip_session_data_t *pip_data);
static switch_status_t init_local_video_file(pip_session_data_t *pip_data, const char *video_file);
static switch_status_t init_output_video_file(pip_session_data_t *pip_data, const char *output_file);
static switch_status_t write_output_frame(pip_session_data_t *pip_data);
static switch_status_t flush_encoder(pip_session_data_t *pip_data);
static switch_status_t process_pip_overlay(pip_session_data_t *pip_data);
static switch_status_t convert_and_overlay_frames(pip_session_data_t *pip_data);
static switch_status_t init_pip_context(pip_session_data_t *pip_data, const char *local_video_file);
static void overlay_yuv420p_frames(AVFrame *main_frame, AVFrame *pip_frame_scaled, AVFrame *output_frame, int x, int y,
                                   float opacity);
static switch_status_t process_video_frame(pip_session_data_t *pip_data, switch_frame_t *main_frame,
                                           switch_frame_t *pip_frame);
static void cleanup_pip_session(pip_session_data_t *pip_data);
static switch_bool_t pip_read_video_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);