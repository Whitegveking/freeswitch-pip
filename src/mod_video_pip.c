#include <switch.h>

/* 模块声明 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_pip_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_video_pip_load);

SWITCH_MODULE_DEFINITION(mod_video_pip, mod_video_pip_load, mod_video_pip_shutdown, NULL);

/* PIP位置枚举 */
typedef enum {
    PIP_POSITION_TOP_LEFT = 0,
    PIP_POSITION_TOP_RIGHT,
    PIP_POSITION_BOTTOM_LEFT,
    PIP_POSITION_BOTTOM_RIGHT
} pip_position_t;

/* PIP会话数据结构 - 支持真实视频叠加 */
typedef struct {
    switch_core_session_t *session;
    switch_core_session_t *pip_session;
    switch_media_bug_t *bug;            /* 主会话的媒体bug */
    switch_media_bug_t *pip_bug;        /* PIP会话的媒体bug */
    pip_position_t position;
    float pip_ratio;
    int active;
    int width;
    int height;
    int pip_width;
    int pip_height;
    int pip_x;
    int pip_y;
    /* 记录上一次的位置，用于清理 */
    int last_pip_x;
    int last_pip_y;
    int last_pip_width;
    int last_pip_height;
    int position_changed;
    switch_mutex_t *mutex;
    
    /* 视频帧缓存 */
    switch_image_t *pip_frame;         /* 缓存的PIP视频帧 */
    int pip_frame_ready;               /* PIP帧是否就绪 */
    switch_time_t last_pip_frame_time; /* 上次PIP帧时间戳 */
} pip_session_data_t;
/* 全局变量 */
static switch_memory_pool_t *module_pool = NULL;
static switch_hash_t *pip_sessions = NULL;
static switch_mutex_t *pip_mutex = NULL;

/* 函数声明 */
static switch_bool_t video_pip_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
static switch_bool_t pip_video_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
static switch_status_t enable_pip_for_session(switch_core_session_t *main_session, switch_core_session_t *pip_session);
static switch_status_t disable_pip_for_session(switch_core_session_t *session);
static void calculate_pip_position(pip_session_data_t *pip_data);
static switch_status_t process_video_frame(pip_session_data_t *pip_data, switch_frame_t *frame);
static switch_status_t blend_pip_video(pip_session_data_t *pip_data, switch_frame_t *main_frame);

/* API函数声明 */
SWITCH_STANDARD_API(enable_pip_api);
SWITCH_STANDARD_API(disable_pip_api);
SWITCH_STANDARD_API(pip_position_api);
SWITCH_STANDARD_API(pip_size_api);
SWITCH_STANDARD_API(pip_status_api);

/**
 * @brief 计算PIP窗口在主视频中的位置和尺寸
 * @details 根据主视频分辨率、PIP位置枚举值和缩放比例计算PIP窗口的坐标和尺寸，
 *          同时记录上一次的位置信息用于清理，并进行边界检查防止越界
 * @param pip_data PIP会话数据结构指针，包含位置、尺寸等配置信息
 * @return void
 */
static void calculate_pip_position(pip_session_data_t *pip_data)
{
    if (!pip_data) return;
    
    /* 保存旧位置用于清理 */
    pip_data->last_pip_x = pip_data->pip_x;
    pip_data->last_pip_y = pip_data->pip_y;
    pip_data->last_pip_width = pip_data->pip_width;
    pip_data->last_pip_height = pip_data->pip_height;
    
    pip_data->pip_width = (int)(pip_data->width * pip_data->pip_ratio);
    pip_data->pip_height = (int)(pip_data->height * pip_data->pip_ratio);
    
    int margin = 20;
    
    switch (pip_data->position) {
        case PIP_POSITION_TOP_LEFT:
            pip_data->pip_x = margin;
            pip_data->pip_y = margin;
            break;
        case PIP_POSITION_TOP_RIGHT:
            pip_data->pip_x = pip_data->width - pip_data->pip_width - margin;
            pip_data->pip_y = margin;
            break;
        case PIP_POSITION_BOTTOM_LEFT:
            pip_data->pip_x = margin;
            pip_data->pip_y = pip_data->height - pip_data->pip_height - margin;
            break;
        case PIP_POSITION_BOTTOM_RIGHT:
            pip_data->pip_x = pip_data->width - pip_data->pip_width - margin;
            pip_data->pip_y = pip_data->height - pip_data->pip_height - margin;
            break;
    }
    
    /* 确保坐标不会超出边界 */
    if (pip_data->pip_x < 0) pip_data->pip_x = 0;
    if (pip_data->pip_y < 0) pip_data->pip_y = 0;
    if (pip_data->pip_x + pip_data->pip_width > pip_data->width) {
        pip_data->pip_x = pip_data->width - pip_data->pip_width;
    }
    if (pip_data->pip_y + pip_data->pip_height > pip_data->height) {
        pip_data->pip_y = pip_data->height - pip_data->pip_height;
    }
    
    /* 检查位置是否真的改变了 - 但跳过第一次初始化 */
    if (pip_data->last_pip_width > 0 && pip_data->last_pip_height > 0 &&
        (pip_data->pip_x != pip_data->last_pip_x || 
         pip_data->pip_y != pip_data->last_pip_y ||
         pip_data->pip_width != pip_data->last_pip_width ||
         pip_data->pip_height != pip_data->last_pip_height)) {
        pip_data->position_changed = 1;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "PIP位置计算: %dx%d主窗口, PIP %dx%d在(%d,%d), 比例%.2f\n",
                     pip_data->width, pip_data->height,
                     pip_data->pip_width, pip_data->pip_height,
                     pip_data->pip_x, pip_data->pip_y, pip_data->pip_ratio);
}

/**
 * @brief 将PIP视频帧叠加到主视频帧上
 * @details 使用最近邻缩放算法将PIP视频帧缩放并叠加到主视频帧的指定位置，
 *          支持I420格式的YUV视频帧处理，包括Y、U、V三个平面的处理
 * @param pip_data PIP会话数据结构指针，包含PIP帧和位置信息
 * @param main_frame 主视频帧指针，将在此帧上叠加PIP内容
 * @return switch_status_t 返回操作状态，成功返回SWITCH_STATUS_SUCCESS
 */
static switch_status_t blend_pip_video(pip_session_data_t *pip_data, switch_frame_t *main_frame)
{
    if (!pip_data || !main_frame || !pip_data->pip_frame_ready || !pip_data->pip_frame) {
        return SWITCH_STATUS_SUCCESS;
    }
    
    if (main_frame->img->fmt != SWITCH_IMG_FMT_I420 || pip_data->pip_frame->fmt != SWITCH_IMG_FMT_I420) {
        return SWITCH_STATUS_SUCCESS;
    }
    
    /* 获取主帧和PIP帧的数据指针 */

    // uint8_t = unsigned char
    // planes是一个图像平面数据的起始地址，stride是每个平面行步长
    uint8_t *main_y = main_frame->img->planes[SWITCH_PLANE_Y];
    uint8_t *main_u = main_frame->img->planes[SWITCH_PLANE_U];
    uint8_t *main_v = main_frame->img->planes[SWITCH_PLANE_V];
    int main_y_stride = main_frame->img->stride[SWITCH_PLANE_Y];
    int main_u_stride = main_frame->img->stride[SWITCH_PLANE_U];
    int main_v_stride = main_frame->img->stride[SWITCH_PLANE_V];
    
    uint8_t *pip_y = pip_data->pip_frame->planes[SWITCH_PLANE_Y];
    uint8_t *pip_u = pip_data->pip_frame->planes[SWITCH_PLANE_U];
    uint8_t *pip_v = pip_data->pip_frame->planes[SWITCH_PLANE_V];
    int pip_y_stride = pip_data->pip_frame->stride[SWITCH_PLANE_Y];
    int pip_u_stride = pip_data->pip_frame->stride[SWITCH_PLANE_U];
    int pip_v_stride = pip_data->pip_frame->stride[SWITCH_PLANE_V];
    
    /* 计算缩放后的PIP帧尺寸 */
    int pip_src_w = pip_data->pip_frame->d_w;
    int pip_src_h = pip_data->pip_frame->d_h;
    int pip_dst_w = pip_data->pip_width;
    int pip_dst_h = pip_data->pip_height;
    int pip_dst_x = pip_data->pip_x;
    int pip_dst_y = pip_data->pip_y;
    
    /* 边界检查 */
    if (pip_dst_x + pip_dst_w > main_frame->img->d_w) {
        pip_dst_w = main_frame->img->d_w - pip_dst_x;
    }
    if (pip_dst_y + pip_dst_h > main_frame->img->d_h) {
        pip_dst_h = main_frame->img->d_h - pip_dst_y;
    }
    
    if (pip_dst_w <= 0 || pip_dst_h <= 0) {
        return SWITCH_STATUS_FALSE;
    }
    
    /* 简单的最近邻缩放和叠加Y平面 */
    for (int y = 0; y < pip_dst_h; y++) {
        int src_y = (y * pip_src_h) / pip_dst_h;
        if (src_y >= pip_src_h) src_y = pip_src_h - 1;
        
        uint8_t *main_line = main_y + (pip_dst_y + y) * main_y_stride + pip_dst_x;
        uint8_t *pip_line = pip_y + src_y * pip_y_stride;
        
        for (int x = 0; x < pip_dst_w; x++) {
            int src_x = (x * pip_src_w) / pip_dst_w;
            if (src_x >= pip_src_w) src_x = pip_src_w - 1;
            
            main_line[x] = pip_line[src_x];
        }
    }
    
    /* 叠加U和V平面 (色度平面，尺寸是Y平面的一半) */
    int pip_dst_w_uv = pip_dst_w / 2;
    int pip_dst_h_uv = pip_dst_h / 2;
    int pip_dst_x_uv = pip_dst_x / 2;
    int pip_dst_y_uv = pip_dst_y / 2;
    int pip_src_w_uv = pip_src_w / 2;
    int pip_src_h_uv = pip_src_h / 2;
    
    /* U平面 */
    for (int y = 0; y < pip_dst_h_uv; y++) {
        int src_y = (y * pip_src_h_uv) / pip_dst_h_uv;
        if (src_y >= pip_src_h_uv) src_y = pip_src_h_uv - 1;
        
        uint8_t *main_line = main_u + (pip_dst_y_uv + y) * main_u_stride + pip_dst_x_uv;
        uint8_t *pip_line = pip_u + src_y * pip_u_stride;
        
        for (int x = 0; x < pip_dst_w_uv; x++) {
            int src_x = (x * pip_src_w_uv) / pip_dst_w_uv;
            if (src_x >= pip_src_w_uv) src_x = pip_src_w_uv - 1;
            // 叠加主帧
            main_line[x] = pip_line[src_x];
        }
    }
    
    /* V平面 */
    for (int y = 0; y < pip_dst_h_uv; y++) {
        int src_y = (y * pip_src_h_uv) / pip_dst_h_uv;
        if (src_y >= pip_src_h_uv) src_y = pip_src_h_uv - 1;
        
        uint8_t *main_line = main_v + (pip_dst_y_uv + y) * main_v_stride + pip_dst_x_uv;
        uint8_t *pip_line = pip_v + src_y * pip_v_stride;
        
        for (int x = 0; x < pip_dst_w_uv; x++) {
            int src_x = (x * pip_src_w_uv) / pip_dst_w_uv;
            if (src_x >= pip_src_w_uv) src_x = pip_src_w_uv - 1;
            
            main_line[x] = pip_line[src_x];
        }
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "视频叠加完成: PIP %dx%d 缩放到 %dx%d 叠加在 (%d,%d)\n",
                     pip_src_w, pip_src_h, pip_dst_w, pip_dst_h, pip_dst_x, pip_dst_y);
    
    return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief 处理视频帧，进行PIP视频叠加和边框绘制
 * @details 对输入的视频帧进行PIP处理，包括叠加PIP视频内容和绘制简洁的黑色边框，
 *          支持动态分辨率更新和位置变化处理
 * @param pip_data PIP会话数据结构指针，包含PIP配置和状态信息
 * @param frame 待处理的视频帧指针
 * @return switch_status_t 返回操作状态，成功返回SWITCH_STATUS_SUCCESS
 */
static switch_status_t process_video_frame(pip_session_data_t *pip_data, switch_frame_t *frame)
{
    if (!pip_data || !frame || !pip_data->active || !frame->img) {
        return SWITCH_STATUS_SUCCESS;
    }
    
    if (!pip_data->mutex) {
        return SWITCH_STATUS_SUCCESS;
    }
    
    switch_mutex_lock(pip_data->mutex);
    
    /* 获取视频帧信息 */
    int width = frame->img->d_w;
    int height = frame->img->d_h;
    
    /* 更新尺寸如果改变了 */
    if (pip_data->width != width || pip_data->height != height) {
        pip_data->width = width;
        pip_data->height = height;
        calculate_pip_position(pip_data);
        
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                         "PIP分辨率更新: %dx%d -> PIP %dx%d在(%d,%d)\n",
                         width, height, pip_data->pip_width, pip_data->pip_height,
                         pip_data->pip_x, pip_data->pip_y);
    }
    
    /* 首先叠加PIP视频内容 */
    if (pip_data->pip_frame_ready && pip_data->pip_frame) {
        blend_pip_video(pip_data, frame);
    }
    
    /* 然后在PIP区域画边框 */
    if (frame->img->fmt == SWITCH_IMG_FMT_I420 && frame->img->planes[SWITCH_PLANE_Y]) {
        /* 如果位置改变了，先清理旧位置的边框 - 优化：不用灰色覆盖，让视频自然更新 */
        if (pip_data->position_changed && 
            pip_data->last_pip_width > 0 && pip_data->last_pip_height > 0) {
            
            pip_data->position_changed = 0; /* 重置标志 */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                             "PIP位置已更改，等待视频自然更新: %dx%d从(%d,%d)到(%d,%d)\n",
                             pip_data->last_pip_width, pip_data->last_pip_height, 
                             pip_data->last_pip_x, pip_data->last_pip_y,
                             pip_data->pip_x, pip_data->pip_y);
        }
        
        /* 简化边框：只画黑色边框，减少视觉干扰 */
        int border_thickness = 2;     /* 减少边框厚度到3像素 */
        uint8_t border_color_y = 0;   /* 黑色边框，更加简洁 */
        
        /* 计算安全的边界 */
        int safe_x = pip_data->pip_x;
        int safe_y = pip_data->pip_y;
        int safe_w = pip_data->pip_width;
        int safe_h = pip_data->pip_height;
        
        if (safe_x + safe_w > width) safe_w = width - safe_x;
        if (safe_y + safe_h > height) safe_h = height - safe_y;
        
        if (safe_x >= 0 && safe_y >= 0 && safe_w > 0 && safe_h > 0) {
            uint8_t *y_plane = frame->img->planes[SWITCH_PLANE_Y];
            int y_stride = frame->img->stride[SWITCH_PLANE_Y];
            
            /* 画简洁的黑色边框 */
            /* 上边框 */
            for (int i = 0; i < border_thickness && (safe_y + i) < height; i++) {
                uint8_t *line = y_plane + (safe_y + i) * y_stride + safe_x;
                for (int j = 0; j < safe_w && (safe_x + j) < width; j++) {
                    line[j] = border_color_y;
                }
            }
            
            /* 下边框 */
            for (int i = 0; i < border_thickness && (safe_y + safe_h - 1 - i) >= 0; i++) {
                uint8_t *line = y_plane + (safe_y + safe_h - 1 - i) * y_stride + safe_x;
                for (int j = 0; j < safe_w && (safe_x + j) < width; j++) {
                    line[j] = border_color_y;
                }
            }
            
            /* 左右边框 */
            for (int i = 0; i < safe_h; i++) {
                uint8_t *line = y_plane + (safe_y + i) * y_stride + safe_x;
                /* 左边框 */
                for (int j = 0; j < border_thickness && (safe_x + j) < width; j++) {
                    line[j] = border_color_y;
                }
                /* 右边框 */
                for (int j = 0; j < border_thickness && (safe_x + safe_w - 1 - j) >= safe_x; j++) {
                    line[safe_w - 1 - j] = border_color_y;
                }
            }
        }
        
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "PIP黑边框绘制完成: %dx%d在(%d,%d)\n",
                         safe_w, safe_h, safe_x, safe_y);
    }
    
    switch_mutex_unlock(pip_data->mutex);
    return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief 主会话的媒体钩子回调函数
 * @details 处理主会话的视频帧，在视频帧上进行PIP叠加和边框绘制
 * @param bug 媒体钩子指针
 * @param user_data 用户数据指针，实际为pip_session_data_t结构
 * @param type 回调类型，指示当前回调的事件类型
 * @return switch_bool_t 返回SWITCH_TRUE继续处理，SWITCH_FALSE停止处理
 */
static switch_bool_t video_pip_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    pip_session_data_t *pip_data = (pip_session_data_t *)user_data;
    switch_frame_t *frame;
    
    /* 增强安全检查 */
    if (!pip_data || !module_pool) {
        return SWITCH_TRUE;
    }
    
    switch (type) {
        case SWITCH_ABC_TYPE_INIT:
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP媒体钩子初始化\n");
            break;
            
        case SWITCH_ABC_TYPE_READ_VIDEO_PING:
            /* 检查模块是否还在运行 */
            if (!pip_sessions || !pip_mutex) {
                return SWITCH_TRUE;
            }
            frame = switch_core_media_bug_get_video_ping_frame(bug);
            if (frame && frame->img && pip_data->active) {
                process_video_frame(pip_data, frame);
            }
            break;
            
        case SWITCH_ABC_TYPE_CLOSE:
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP媒体钩子关闭\n");
            if (pip_data) {
                pip_data->active = 0;
            }
            break;
            
        default:
            break;
    }
    
    return SWITCH_TRUE;
}

/**
 * @brief PIP会话的视频帧捕获回调函数
 * @details 从PIP会话中捕获视频帧并缓存，供主会话进行视频叠加使用
 * @param bug 媒体钩子指针
 * @param user_data 用户数据指针，实际为pip_session_data_t结构
 * @param type 回调类型，指示当前回调的事件类型
 * @return switch_bool_t 返回SWITCH_TRUE继续处理，SWITCH_FALSE停止处理
 */
static switch_bool_t pip_video_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    pip_session_data_t *pip_data = (pip_session_data_t *)user_data;
    switch_frame_t *frame;
    
    /* 增强安全检查 */
    if (!pip_data || !module_pool) {
        return SWITCH_TRUE;
    }
    
    switch (type) {
        case SWITCH_ABC_TYPE_INIT:
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP视频捕获钩子初始化\n");
            break;
            
        case SWITCH_ABC_TYPE_READ_VIDEO_PING:
            /* 检查模块是否还在运行 */
            if (!pip_sessions || !pip_mutex) {
                return SWITCH_TRUE;
            }
            frame = switch_core_media_bug_get_video_ping_frame(bug);
            if (frame && frame->img && pip_data->active && pip_data->mutex) {
                switch_mutex_lock(pip_data->mutex);
                
                /* 如果有旧的PIP帧，先释放 */
                if (pip_data->pip_frame) {
                    switch_img_free(&pip_data->pip_frame);
                }
                
                /* 复制新的PIP帧 */
                if (frame->img) {
                    /* 创建新的图像缓冲区 */
                    switch_img_copy(frame->img, &pip_data->pip_frame);
                    pip_data->pip_frame_ready = 1;
                    pip_data->last_pip_frame_time = switch_time_now();
                    
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                                     "捕获PIP视频帧: %dx%d\n", 
                                     frame->img->d_w, frame->img->d_h);
                }
                
                switch_mutex_unlock(pip_data->mutex);
            }
            break;
            
        case SWITCH_ABC_TYPE_CLOSE:
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP视频捕获钩子关闭\n");
            if (pip_data && pip_data->mutex) {
                switch_mutex_lock(pip_data->mutex);
                if (pip_data->pip_frame) {
                    switch_img_free(&pip_data->pip_frame);
                    pip_data->pip_frame = NULL;
                }
                pip_data->pip_frame_ready = 0;
                switch_mutex_unlock(pip_data->mutex);
            }
            break;
            
        default:
            break;
    }
    
    return SWITCH_TRUE;
}

/**
 * @brief 为指定会话启用PIP功能
 * @details 创建PIP数据结构，初始化配置参数，为主会话和PIP会话分别添加媒体钩子，
 *          并将PIP会话信息存储到全局哈希表中
 * @param main_session 主会话指针，将在此会话的视频上叠加PIP内容
 * @param pip_session PIP会话指针，此会话的视频将作为PIP内容
 * @return switch_status_t 返回操作状态，成功返回SWITCH_STATUS_SUCCESS
 */
static switch_status_t enable_pip_for_session(switch_core_session_t *main_session, switch_core_session_t *pip_session)
{
    pip_session_data_t *pip_data;
    switch_status_t status;
    const char *uuid;
    
    if (!main_session || !pip_session) {
        return SWITCH_STATUS_FALSE;
    }
    
    uuid = switch_core_session_get_uuid(main_session);
    
    /* 分配PIP数据结构 */
    pip_data = switch_core_session_alloc(main_session, sizeof(pip_session_data_t));
    memset(pip_data, 0, sizeof(pip_session_data_t));
    
    /* 初始化互斥锁 */
    switch_mutex_init(&pip_data->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(main_session));
    
    /* 初始化PIP数据 */
    pip_data->session = main_session;
    pip_data->pip_session = pip_session;
    pip_data->position = PIP_POSITION_TOP_RIGHT;
    pip_data->pip_ratio = 0.25f;
    pip_data->active = 1;
    pip_data->width = 720;  /* 默认分辨率 */
    pip_data->height = 480;
    /* 初始化位置跟踪字段 */
    pip_data->last_pip_x = 0;
    pip_data->last_pip_y = 0;
    pip_data->last_pip_width = 0;
    pip_data->last_pip_height = 0;
    pip_data->position_changed = 0;
    /* 初始化视频帧相关字段 */
    pip_data->pip_frame = NULL;
    pip_data->pip_frame_ready = 0;
    pip_data->last_pip_frame_time = 0;
    
    /* 计算PIP位置 */
    calculate_pip_position(pip_data);
    
    /* 为主会话添加媒体钩子 - 用于输出处理 */
    status = switch_core_media_bug_add(main_session, "video_pip_safe", uuid,
                                      video_pip_callback, pip_data, 0, 
                                      SMBF_READ_VIDEO_PING | SMBF_NO_PAUSE,
                                      &pip_data->bug);
    
    if (status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "添加主会话PIP媒体钩子失败\n");
        return status;
    }
    
    /* 为PIP会话添加媒体钩子 - 用于视频捕获 */
    const char *pip_uuid = switch_core_session_get_uuid(pip_session);
    status = switch_core_media_bug_add(pip_session, "video_pip_capture", pip_uuid,
                                      pip_video_callback, pip_data, 0, 
                                      SMBF_READ_VIDEO_PING | SMBF_NO_PAUSE,
                                      &pip_data->pip_bug);
    
    if (status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "添加PIP会话视频捕获钩子失败\n");
        /* 清理主会话的bug */
        switch_core_media_bug_remove(main_session, &pip_data->bug);
        return status;
    }
    
    /* 存储到全局hash表 */
    if (pip_mutex && pip_sessions) {
        switch_mutex_lock(pip_mutex);
        switch_core_hash_insert(pip_sessions, uuid, pip_data);
        switch_mutex_unlock(pip_mutex);
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
                     "为会话 %s 启用真实视频PIP功能 (视频叠加+简洁黑边框)\n", uuid);
    
    return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 为指定会话禁用PIP功能
* @details 从全局哈希表中查找并移除PIP会话数据，清理视频帧缓存，
*          移除主会话和PIP会话的媒体钩子，释放相关资源
* @param session 会话指针，要禁用PIP功能的会话
* @return switch_status_t 返回操作状态，成功返回SWITCH_STATUS_SUCCESS
*/
static switch_status_t disable_pip_for_session(switch_core_session_t *session)
{
   const char *uuid;
   pip_session_data_t *pip_data;
   
   if (!session) {
       return SWITCH_STATUS_FALSE;
   }
   
   uuid = switch_core_session_get_uuid(session);
   
   if (pip_mutex && pip_sessions) {
       switch_mutex_lock(pip_mutex);
       pip_data = switch_core_hash_find(pip_sessions, uuid);
       if (pip_data) {
           /* 首先停用PIP */
           pip_data->active = 0;
           
           /* 安全清理PIP视频帧 */
           if (pip_data->mutex) {
               switch_mutex_lock(pip_data->mutex);
               if (pip_data->pip_frame) {
                   switch_img_free(&pip_data->pip_frame);
                   pip_data->pip_frame = NULL;
               }
               pip_data->pip_frame_ready = 0;
               switch_mutex_unlock(pip_data->mutex);
           }
           
           /* 等待可能的并发操作完成 */
           switch_yield(500); /* 等待0.5ms */
           
           /* 清理主会话的媒体bug */
           if (pip_data->bug && pip_data->session) {
               switch_core_media_bug_remove(pip_data->session, &pip_data->bug);
               pip_data->bug = NULL;
           }
           
           /* 清理PIP会话的媒体bug */
           if (pip_data->pip_bug && pip_data->pip_session) {
               switch_core_media_bug_remove(pip_data->pip_session, &pip_data->pip_bug);
               pip_data->pip_bug = NULL;
           }
           
           /* 从hash表中移除 */
           switch_core_hash_delete(pip_sessions, uuid);
       }
       switch_mutex_unlock(pip_mutex);
       
       if (pip_data) {
           switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
                            "为会话 %s 禁用PIP功能\n", uuid);
           return SWITCH_STATUS_SUCCESS;
       }
   }
   
   return SWITCH_STATUS_FALSE;
}

/**
* @brief 启用PIP功能的API接口
* @details 解析命令参数，获取主会话和PIP会话的UUID，调用enable_pip_for_session启用PIP功能
* @param cmd 命令字符串，格式为"enable_pip <main_uuid> <pip_uuid>"
* @param session API调用的会话（未使用）
* @param stream 输出流，用于返回API调用结果
* @return switch_status_t 始终返回SWITCH_STATUS_SUCCESS
*/
SWITCH_STANDARD_API(enable_pip_api)
{
   char *main_uuid, *pip_uuid;
   switch_core_session_t *main_session = NULL, *pip_session = NULL;
   char *mycmd = NULL;
   
   if (zstr(cmd)) {
       stream->write_function(stream, "-ERR 用法: enable_pip <main_uuid> <pip_uuid>\n");
       return SWITCH_STATUS_SUCCESS;
   }
   
   mycmd = strdup(cmd);
   main_uuid = mycmd;
   
   if ((pip_uuid = strchr(mycmd, ' '))) {
       *pip_uuid++ = '\0';
   }
   
   if (zstr(main_uuid) || zstr(pip_uuid)) {
       stream->write_function(stream, "-ERR 用法: enable_pip <main_uuid> <pip_uuid>\n");
       goto done;
   }
   
   main_session = switch_core_session_locate(main_uuid);
   pip_session = switch_core_session_locate(pip_uuid);
   
   if (!main_session) {
       stream->write_function(stream, "-ERR 找不到主会话\n");
       goto done;
   }
   
   if (!pip_session) {
       stream->write_function(stream, "-ERR 找不到PIP会话\n");
       goto done;
   }
   
   if (enable_pip_for_session(main_session, pip_session) == SWITCH_STATUS_SUCCESS) {
       stream->write_function(stream, "+OK 真实视频PIP已启用 (视频叠加+简洁黑边框)\n");
   } else {
       stream->write_function(stream, "-ERR PIP启用失败\n");
   }
   
done:
   if (main_session) switch_core_session_rwunlock(main_session);
   if (pip_session) switch_core_session_rwunlock(pip_session);
   switch_safe_free(mycmd);
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 禁用PIP功能的API接口
* @details 根据提供的会话UUID禁用该会话的PIP功能
* @param cmd 命令字符串，格式为"disable_pip <uuid>"
* @param session API调用的会话（未使用）
* @param stream 输出流，用于返回API调用结果
* @return switch_status_t 始终返回SWITCH_STATUS_SUCCESS
*/
SWITCH_STANDARD_API(disable_pip_api)
{
   switch_core_session_t *session_p;
   
   if (zstr(cmd)) {
       stream->write_function(stream, "-ERR 用法: disable_pip <uuid>\n");
       return SWITCH_STATUS_SUCCESS;
   }
   
   session_p = switch_core_session_locate(cmd);
   if (!session_p) {
       stream->write_function(stream, "-ERR 找不到会话\n");
       return SWITCH_STATUS_SUCCESS;
   }
   
   if (disable_pip_for_session(session_p) == SWITCH_STATUS_SUCCESS) {
       stream->write_function(stream, "+OK PIP已禁用\n");
   } else {
       stream->write_function(stream, "-ERR PIP禁用失败\n");
   }
   
   switch_core_session_rwunlock(session_p);
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 设置PIP位置的API接口
* @details 根据提供的会话UUID和位置参数更新PIP窗口的显示位置
* @param cmd 命令字符串，格式为"pip_position <uuid> <top_left|top_right|bottom_left|bottom_right>"
* @param session API调用的会话（未使用）
* @param stream 输出流，用于返回API调用结果
* @return switch_status_t 始终返回SWITCH_STATUS_SUCCESS
*/
SWITCH_STANDARD_API(pip_position_api)
{
   char *uuid, *position_str;
   pip_session_data_t *pip_data;
   pip_position_t position;
   char *mycmd = NULL;
   
   if (zstr(cmd)) {
       stream->write_function(stream, "-ERR 用法: pip_position <uuid> <top_left|top_right|bottom_left|bottom_right>\n");
       return SWITCH_STATUS_SUCCESS;
   }
   
   mycmd = strdup(cmd);
   uuid = mycmd;
   
   if ((position_str = strchr(mycmd, ' '))) {
       *position_str++ = '\0';
   }
   
   if (zstr(uuid) || zstr(position_str)) {
       stream->write_function(stream, "-ERR 用法: pip_position <uuid> <position>\n");
       goto done;
   }
   
   if (!strcasecmp(position_str, "top_left")) {
       position = PIP_POSITION_TOP_LEFT;
   } else if (!strcasecmp(position_str, "top_right")) {
       position = PIP_POSITION_TOP_RIGHT;
   } else if (!strcasecmp(position_str, "bottom_left")) {
       position = PIP_POSITION_BOTTOM_LEFT;
   } else if (!strcasecmp(position_str, "bottom_right")) {
       position = PIP_POSITION_BOTTOM_RIGHT;
   } else {
       stream->write_function(stream, "-ERR 无效的位置参数\n");
       goto done;
   }
   
   if (pip_mutex && pip_sessions) {
       switch_mutex_lock(pip_mutex);
       pip_data = switch_core_hash_find(pip_sessions, uuid);
       if (pip_data && pip_data->mutex) {
           switch_mutex_lock(pip_data->mutex);
           pip_data->position = position;
           calculate_pip_position(pip_data);
           switch_mutex_unlock(pip_data->mutex);
           stream->write_function(stream, "+OK PIP位置已更新\n");
       } else {
           stream->write_function(stream, "-ERR 未找到PIP会话\n");
       }
       switch_mutex_unlock(pip_mutex);
   } else {
       stream->write_function(stream, "-ERR PIP系统未初始化\n");
   }
   
done:
   switch_safe_free(mycmd);
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 设置PIP大小的API接口
* @details 根据提供的会话UUID和缩放比例更新PIP窗口的显示大小
* @param cmd 命令字符串，格式为"pip_size <uuid> <ratio>"，ratio范围为0.1-0.5
* @param session API调用的会话（未使用）
* @param stream 输出流，用于返回API调用结果
* @return switch_status_t 始终返回SWITCH_STATUS_SUCCESS
*/
SWITCH_STANDARD_API(pip_size_api)
{
   char *uuid, *ratio_str;
   pip_session_data_t *pip_data;
   float ratio;
   char *mycmd = NULL;
   
   if (zstr(cmd)) {
       stream->write_function(stream, "-ERR 用法: pip_size <uuid> <ratio>\n");
       return SWITCH_STATUS_SUCCESS;
   }
   
   mycmd = strdup(cmd);
   uuid = mycmd;
   
   if ((ratio_str = strchr(mycmd, ' '))) {
       *ratio_str++ = '\0';
   }
   
   if (zstr(uuid) || zstr(ratio_str)) {
       stream->write_function(stream, "-ERR 用法: pip_size <uuid> <ratio>\n");
       goto done;
   }
   
   ratio = (float)atof(ratio_str);
   if (ratio < 0.1 || ratio > 0.5) {
       stream->write_function(stream, "-ERR 比例必须在0.1-0.5之间\n");
       goto done;
   }
   
   if (pip_mutex && pip_sessions) {
       switch_mutex_lock(pip_mutex);
       pip_data = switch_core_hash_find(pip_sessions, uuid);
       if (pip_data && pip_data->mutex) {
           switch_mutex_lock(pip_data->mutex);
           pip_data->pip_ratio = ratio;
           calculate_pip_position(pip_data);
           switch_mutex_unlock(pip_data->mutex);
           stream->write_function(stream, "+OK PIP大小已更新\n");
       } else {
           stream->write_function(stream, "-ERR 未找到PIP会话\n");
       }
       switch_mutex_unlock(pip_mutex);
   } else {
       stream->write_function(stream, "-ERR PIP系统未初始化\n");
   }
   
done:
   switch_safe_free(mycmd);
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 查看PIP状态的API接口
* @details 遍历所有活跃的PIP会话，显示每个会话的详细状态信息，
*          包括位置、大小、分辨率、视频帧状态等
* @param cmd 命令字符串（未使用）
* @param session API调用的会话（未使用）
* @param stream 输出流，用于返回PIP状态信息
* @return switch_status_t 始终返回SWITCH_STATUS_SUCCESS
*/
SWITCH_STANDARD_API(pip_status_api)
{
   switch_hash_index_t *hi;
   pip_session_data_t *pip_data;
   const char *uuid;
   int count = 0;
   
   stream->write_function(stream, "+OK 真实视频PIP状态 (视频叠加+简洁黑边框):\n");
   
   if (pip_mutex && pip_sessions) {
       switch_mutex_lock(pip_mutex);
       for (hi = switch_core_hash_first(pip_sessions); hi; hi = switch_core_hash_next(&hi)) {
           switch_core_hash_this(hi, (const void **)&uuid, NULL, (void **)&pip_data);
           if (pip_data) {
               const char *position_str = "unknown";
               switch (pip_data->position) {
                   case PIP_POSITION_TOP_LEFT: position_str = "top_left"; break;
                   case PIP_POSITION_TOP_RIGHT: position_str = "top_right"; break;
                   case PIP_POSITION_BOTTOM_LEFT: position_str = "bottom_left"; break;
                   case PIP_POSITION_BOTTOM_RIGHT: position_str = "bottom_right"; break;
               }
               
               stream->write_function(stream, "  会话: %s\n", uuid);
               stream->write_function(stream, "    位置: %s, 大小: %.2f, 活跃: %s\n",
                                    position_str, pip_data->pip_ratio,
                                    pip_data->active ? "是" : "否");
               stream->write_function(stream, "    分辨率: %dx%d, PIP: %dx%d 在 (%d,%d)\n",
                                    pip_data->width, pip_data->height,
                                    pip_data->pip_width, pip_data->pip_height,
                                    pip_data->pip_x, pip_data->pip_y);
               stream->write_function(stream, "    视频帧: %s, 最后更新: %lld微秒前\n",
                                    pip_data->pip_frame_ready ? "就绪" : "未就绪",
                                    pip_data->last_pip_frame_time ? 
                                    (switch_time_now() - pip_data->last_pip_frame_time) : 0);
               count++;
           }
       }
       switch_mutex_unlock(pip_mutex);
   }
   
   stream->write_function(stream, "总计: %d 个真实视频PIP会话\n", count);
   
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 模块加载函数
* @details 初始化模块，创建全局内存池、哈希表和互斥锁，注册所有API接口
* @param module_interface 模块接口指针，用于注册API
* @param pool 内存池指针
* @return switch_status_t 返回模块加载状态
*/
SWITCH_MODULE_LOAD_FUNCTION(mod_video_pip_load)
{
   switch_api_interface_t *api_interface;
   
   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "加载Video PIP模块 (真实视频叠加版本)...\n");
   
   /* 创建模块内存池 */
   module_pool = pool;
   
   /* 创建全局hash表和互斥锁 */
   switch_core_hash_init(&pip_sessions);
   switch_mutex_init(&pip_mutex, SWITCH_MUTEX_NESTED, module_pool);
   
   /* 连接内部结构 */
   *module_interface = switch_loadable_module_create_module_interface(pool, modname);
   
   /* 注册API命令 */
   SWITCH_ADD_API(api_interface, "enable_pip", "启用真实视频画中画", enable_pip_api, "enable_pip <main_uuid> <pip_uuid>");
   SWITCH_ADD_API(api_interface, "disable_pip", "禁用画中画", disable_pip_api, "disable_pip <uuid>");
   SWITCH_ADD_API(api_interface, "pip_position", "设置画中画位置", pip_position_api, "pip_position <uuid> <position>");
   SWITCH_ADD_API(api_interface, "pip_size", "设置画中画大小", pip_size_api, "pip_size <uuid> <ratio>");
   SWITCH_ADD_API(api_interface, "pip_status", "查看画中画状态", pip_status_api, "pip_status");
   
   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Video PIP模块加载完成 (真实视频叠加版本) - 5个API已注册\n");
   
   return SWITCH_STATUS_SUCCESS;
}

/**
* @brief 模块卸载函数
* @details 安全地卸载模块，清理所有活跃的PIP会话，释放视频帧缓存，
*          移除媒体钩子，销毁哈希表和互斥锁，释放所有资源
* @return switch_status_t 返回模块卸载状态
*/
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_pip_shutdown)
{
   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "开始安全卸载真实视频PIP模块...\n");
   
   /* 安全的清理所有活跃PIP会话 */
   if (pip_sessions && pip_mutex) {
       switch_hash_index_t *hi;
       pip_session_data_t *pip_data;
       const char *uuid;
       
       switch_mutex_lock(pip_mutex);
       
       /* 第一遍：停用所有PIP会话 */
       for (hi = switch_core_hash_first(pip_sessions); hi; hi = switch_core_hash_next(&hi)) {
           switch_core_hash_this(hi, (const void **)&uuid, NULL, (void **)&pip_data);
           if (pip_data) {
               pip_data->active = 0;
               switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
                                "停用PIP会话: %s\n", uuid);
           }
       }
       
       /* 等待活跃操作完成 */
       switch_yield(1000); /* 等待1ms */
       
       /* 第二遍：安全清理资源 */
       for (hi = switch_core_hash_first(pip_sessions); hi; hi = switch_core_hash_next(&hi)) {
           switch_core_hash_this(hi, (const void **)&uuid, NULL, (void **)&pip_data);
           if (pip_data) {
               /* 安全清理PIP视频帧 */
               if (pip_data->mutex) {
                   switch_mutex_lock(pip_data->mutex);
                   if (pip_data->pip_frame) {
                       switch_img_free(&pip_data->pip_frame);
                       pip_data->pip_frame = NULL;
                   }
                   pip_data->pip_frame_ready = 0;
                   switch_mutex_unlock(pip_data->mutex);
               }
               
               /* 清理主会话媒体bug */
               if (pip_data->bug && pip_data->session) {
                   switch_core_media_bug_remove(pip_data->session, &pip_data->bug);
                   pip_data->bug = NULL;
               }
               
               /* 清理PIP会话媒体bug */
               if (pip_data->pip_bug && pip_data->pip_session) {
                   switch_core_media_bug_remove(pip_data->pip_session, &pip_data->pip_bug);
                   pip_data->pip_bug = NULL;
               }
               
               switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
                                "已清理PIP会话: %s\n", uuid);
           }
       }
       
       /* 清空hash表 */
       switch_core_hash_destroy(&pip_sessions);
       pip_sessions = NULL;
       
       switch_mutex_unlock(pip_mutex);
       
       /* 销毁全局互斥锁 */
       switch_mutex_destroy(pip_mutex);
       pip_mutex = NULL;
   }
   
   /* 清理模块池引用 */
   module_pool = NULL;
   
   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "真实视频PIP模块已安全卸载\n");
   
   return SWITCH_STATUS_SUCCESS;
}
