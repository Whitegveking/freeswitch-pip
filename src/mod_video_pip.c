#include "../include/mod_video_pip.h"

/* 从本地MP4文件读取下一帧 */
static switch_status_t read_local_video_frame(pip_session_data_t *pip_data)
{
    int ret;
    static int retry_count = 0;

    // 确保正确初始化，避免空指针访问
    if (!pip_data || !pip_data->local_fmt_ctx || !pip_data->local_codec_ctx)
    {
        return SWITCH_STATUS_FALSE;
    }

    /* 读取视频包 */
    while ((ret = av_read_frame(pip_data->local_fmt_ctx, pip_data->local_packet)) >= 0)
    {
        // 通过索引判断是否是本地视频流
        if (pip_data->local_packet->stream_index == pip_data->local_video_stream_index)
        {
            /* 解码视频帧 */
            // 把视频包发送给解码器
            ret = avcodec_send_packet(pip_data->local_codec_ctx, pip_data->local_packet);
            if (ret < 0)
            {
                av_packet_unref(pip_data->local_packet);
                continue;
            }

            // 从解码器接收视频帧
            ret = avcodec_receive_frame(pip_data->local_codec_ctx, pip_data->frame_main);
            // 视频包处理完成后释放
            av_packet_unref(pip_data->local_packet);

            // 获取了一个完整的视频帧，函数的任务完成
            if (ret >= 0)
            {
                pip_data->local_frames_count++;
                retry_count = 0; // 重置重试计数
                return SWITCH_STATUS_SUCCESS;
            }
        }
        // 当前不是本地视频流，直接释放包
        else
        {
            av_packet_unref(pip_data->local_packet);
        }
    }

    /* 到达文件末尾，循环播放 */
    if (ret == AVERROR_EOF)
    {
        // 防止无限递归
        if (retry_count >= 10)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "视频文件循环播放重试次数过多，停止处理\n");
            retry_count = 0;
            return SWITCH_STATUS_FALSE;
        }

        retry_count++;
        ret = av_seek_frame(pip_data->local_fmt_ctx, pip_data->local_video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "视频文件seek失败: %d\n", ret);
            retry_count = 0;
            return SWITCH_STATUS_FALSE;
        }
        return read_local_video_frame(pip_data); /* 递归调用重新开始 */
    }

    retry_count = 0;
    return SWITCH_STATUS_FALSE;
}

/* 加载本地图片文件 */
static switch_status_t load_local_image(pip_session_data_t *pip_data, const char *image_file)
{
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVCodec *codec = NULL;
    AVPacket *packet = NULL;
    int video_stream_index = -1;
    int ret;

    if (!pip_data || !image_file)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无效的参数\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 检查文件是否存在 */
    if (access(image_file, R_OK) != 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法访问图片文件: %s\n", image_file);
        return SWITCH_STATUS_FALSE;
    }

    /* 保存图片路径 */
    switch_copy_string(pip_data->local_image_path, image_file, sizeof(pip_data->local_image_path));

    /* 打开图片文件 */
    ret = avformat_open_input(&fmt_ctx, image_file, NULL, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法打开图片文件: %s (错误码: %d)\n", image_file, ret);
        return SWITCH_STATUS_FALSE;
    }

    /* 获取流信息 */
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法获取流信息 (错误码: %d)\n", ret);
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 查找视频流（图片在FFmpeg中作为单帧视频处理） */
    for (int i = 0; i < fmt_ctx->nb_streams; i++)
    {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "在图片文件中未找到视频流\n");
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 获取解码器 */
    codec = avcodec_find_decoder(fmt_ctx->streams[video_stream_index]->codecpar->codec_id);
    if (!codec)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "未找到图片解码器\n");
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 创建解码器上下文 */
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法分配解码器上下文\n");
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 复制解码器参数 */
    ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法复制解码器参数\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 打开解码器 */
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法打开解码器\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 分配帧和包 */
    pip_data->local_image_frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!pip_data->local_image_frame || !packet)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法分配帧或包\n");
        if (pip_data->local_image_frame)
            av_frame_free(&pip_data->local_image_frame);
        if (packet)
            av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 读取并解码第一帧（图片） */
    ret = av_read_frame(fmt_ctx, packet);
    if (ret >= 0 && packet->stream_index == video_stream_index)
    {
        ret = avcodec_send_packet(codec_ctx, packet);
        if (ret >= 0)
        {
            ret = avcodec_receive_frame(codec_ctx, pip_data->local_image_frame);
        }
    }

    /* 清理临时资源 */
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法解码图片帧\n");
        av_frame_free(&pip_data->local_image_frame);
        return SWITCH_STATUS_FALSE;
    }

    /* 检查图片格式并转换为YUV420P */
    if (pip_data->local_image_frame->format != AV_PIX_FMT_YUV420P)
    {
        AVFrame *yuv_frame = av_frame_alloc();
        struct SwsContext *sws_ctx = NULL;

        if (!yuv_frame)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法分配YUV帧\n");
            av_frame_free(&pip_data->local_image_frame);
            return SWITCH_STATUS_FALSE;
        }

        /* 设置YUV帧参数 */
        yuv_frame->format = AV_PIX_FMT_YUV420P;
        yuv_frame->width = pip_data->local_image_frame->width;
        yuv_frame->height = pip_data->local_image_frame->height;

        if (av_frame_get_buffer(yuv_frame, 32) < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法分配YUV帧缓冲区\n");
            av_frame_free(&yuv_frame);
            av_frame_free(&pip_data->local_image_frame);
            return SWITCH_STATUS_FALSE;
        }

        /* 创建格式转换上下文 */
        sws_ctx = sws_getContext(
            pip_data->local_image_frame->width, pip_data->local_image_frame->height, pip_data->local_image_frame->format,
            yuv_frame->width, yuv_frame->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, NULL, NULL, NULL);

        if (!sws_ctx)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法创建图片格式转换上下文\n");
            av_frame_free(&yuv_frame);
            av_frame_free(&pip_data->local_image_frame);
            return SWITCH_STATUS_FALSE;
        }

        /* 执行格式转换 */
        int scale_ret = sws_scale(sws_ctx,
                                  (const uint8_t *const *)pip_data->local_image_frame->data,
                                  pip_data->local_image_frame->linesize,
                                  0, pip_data->local_image_frame->height,
                                  yuv_frame->data, yuv_frame->linesize);

        sws_freeContext(sws_ctx);

        if (scale_ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "图片格式转换失败\n");
            av_frame_free(&yuv_frame);
            av_frame_free(&pip_data->local_image_frame);
            return SWITCH_STATUS_FALSE;
        }

        /* 替换原始帧为转换后的YUV帧 */
        av_frame_free(&pip_data->local_image_frame);
        pip_data->local_image_frame = yuv_frame;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "图片已转换为YUV420P格式\n");
    }

    /* 设置图片模式标志 */
    pip_data->use_image_mode = SWITCH_TRUE;
    pip_data->main_width = pip_data->local_image_frame->width;
    pip_data->main_height = pip_data->local_image_frame->height;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "成功加载图片: %s (%dx%d, 格式: %s -> YUV420P)\n",
                      image_file, pip_data->main_width, pip_data->main_height,
                      av_get_pix_fmt_name(pip_data->local_image_frame->format));

    return SWITCH_STATUS_SUCCESS;
} /* 初始化本地视频文件 */
static switch_status_t init_local_video_file(pip_session_data_t *pip_data, const char *video_file)
{
    AVCodec *codec;
    int ret;

    if (!pip_data || !video_file)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无效的参数\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 打开本地视频文件 */
    ret = avformat_open_input(&pip_data->local_fmt_ctx, video_file, NULL, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法打开本地视频文件: %s (错误码: %d)\n", video_file, ret);
        return SWITCH_STATUS_FALSE;
    }

    /* 获取流信息 */
    ret = avformat_find_stream_info(pip_data->local_fmt_ctx, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法获取流信息 (错误码: %d)\n", ret);
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 查找视频流 */
    // 遍历所有流，找到第一个视频流
    pip_data->local_video_stream_index = -1;
    for (unsigned int i = 0; i < pip_data->local_fmt_ctx->nb_streams; i++)
    {
        if (pip_data->local_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            pip_data->local_video_stream_index = i;
            break;
        }
    }

    // 未找到视频流
    if (pip_data->local_video_stream_index == -1)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "未找到视频流\n");
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 获取解码器 */
    // 根据流的编解码参数查找对应的解码器
    codec =
        avcodec_find_decoder(pip_data->local_fmt_ctx->streams[pip_data->local_video_stream_index]->codecpar->codec_id);
    if (!codec)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "未找到解码器\n");
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 分配解码器上下文 */
    // 通过找到的解码器创建解码器上下文，类似于C++中的类实例化
    pip_data->local_codec_ctx = avcodec_alloc_context3(codec);
    if (!pip_data->local_codec_ctx)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配解码器上下文失败\n");
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 复制编解码参数 */
    // 将流的编解码参数复制到解码器上下文中
    ret = avcodec_parameters_to_context(pip_data->local_codec_ctx,
                                        pip_data->local_fmt_ctx->streams[pip_data->local_video_stream_index]->codecpar);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "复制编解码参数失败 (错误码: %d)\n", ret);
        avcodec_free_context(&pip_data->local_codec_ctx);
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 打开解码器 */
    ret = avcodec_open2(pip_data->local_codec_ctx, codec, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "打开解码器失败 (错误码: %d)\n", ret);
        avcodec_free_context(&pip_data->local_codec_ctx);
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 分配包 */
    pip_data->local_packet = av_packet_alloc();
    if (!pip_data->local_packet)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配包失败\n");
        avcodec_close(pip_data->local_codec_ctx);
        avcodec_free_context(&pip_data->local_codec_ctx);
        avformat_close_input(&pip_data->local_fmt_ctx);
        return SWITCH_STATUS_FALSE;
    }

    /* 更新视频尺寸 */
    pip_data->main_width = pip_data->local_codec_ctx->width;
    pip_data->main_height = pip_data->local_codec_ctx->height;

    /* 获取本地视频帧率 */
    AVStream *video_stream = pip_data->local_fmt_ctx->streams[pip_data->local_video_stream_index];
    if (video_stream->r_frame_rate.num > 0 && video_stream->r_frame_rate.den > 0)
    {
        pip_data->local_fps = (double)video_stream->r_frame_rate.num / video_stream->r_frame_rate.den;
    }
    else if (video_stream->avg_frame_rate.num > 0 && video_stream->avg_frame_rate.den > 0)
    {
        pip_data->local_fps = (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }
    else
    {
        pip_data->local_fps = 30.0; // 默认帧率
    }

    pip_data->local_frame_time = 1.0 / pip_data->local_fps;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "本地视频文件初始化成功: %s (%dx%d, %.2f fps)\n", video_file,
                      pip_data->main_width, pip_data->main_height, pip_data->local_fps);

    return SWITCH_STATUS_SUCCESS;
}

/* 初始化输出视频文件 */
static switch_status_t init_output_video_file(pip_session_data_t *pip_data, const char *output_file)
{
    AVCodec *encoder;
    int ret;

    /* 创建输出格式上下文 */
    ret = avformat_alloc_output_context2(&pip_data->output_fmt_ctx, NULL, NULL, output_file);
    if (ret < 0 || !pip_data->output_fmt_ctx)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法创建输出格式上下文\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 查找H264编码器 */
    encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "未找到H264编码器\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 创建输出流 */
    pip_data->output_stream = avformat_new_stream(pip_data->output_fmt_ctx, encoder);
    if (!pip_data->output_stream)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法创建输出流\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 分配编码器上下文 */
    pip_data->output_codec_ctx = avcodec_alloc_context3(encoder);
    if (!pip_data->output_codec_ctx)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配编码器上下文失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 设置编码器参数 */
    pip_data->output_codec_ctx->width = pip_data->main_width;
    pip_data->output_codec_ctx->height = pip_data->main_height;
    pip_data->output_codec_ctx->time_base = (AVRational){1, 30}; /* 30fps */
    pip_data->output_codec_ctx->framerate = (AVRational){30, 1};
    pip_data->output_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    pip_data->output_codec_ctx->bit_rate = 1000000; /* 1Mbps */
    pip_data->output_codec_ctx->gop_size = 30;
    pip_data->output_codec_ctx->max_b_frames = 1;

    /* H264特定设置 */
    if (pip_data->output_codec_ctx->codec_id == AV_CODEC_ID_H264)
    {
        av_opt_set(pip_data->output_codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(pip_data->output_codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    /* 如果是MP4格式，需要全局头 */
    // 判断是否需要全局头
    if (pip_data->output_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    {
        // 设置编码器标志以包含全局头
        pip_data->output_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    /* 打开编码器 */
    ret = avcodec_open2(pip_data->output_codec_ctx, encoder, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "打开编码器失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 复制编码器参数到流 */
    ret = avcodec_parameters_from_context(pip_data->output_stream->codecpar, pip_data->output_codec_ctx);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "复制编码器参数失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 设置流的时间基 - 确保时间戳从0开始 */
    pip_data->output_stream->time_base = pip_data->output_codec_ctx->time_base;
    pip_data->output_stream->start_time = 0;

    /* 打开输出文件 */
    // 我们准备使用的输出格式，是不是那种不需要物理文件的特殊格式？
    // mp4格式通常需要物理文件，所以我们需要打开文件进行写入
    if (!(pip_data->output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&pip_data->output_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法打开输出文件: %s\n", output_file);
            return SWITCH_STATUS_FALSE;
        }
    }

    /* 写入文件头 */
    ret = avformat_write_header(pip_data->output_fmt_ctx, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "写入文件头失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 分配输出包 */
    pip_data->output_packet = av_packet_alloc();
    if (!pip_data->output_packet)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配输出包失败\n");
        return SWITCH_STATUS_FALSE;
    }

    strncpy(pip_data->output_filename, output_file, sizeof(pip_data->output_filename) - 1);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "输出视频文件初始化成功: %s (%dx%d)\n", output_file,
                      pip_data->main_width, pip_data->main_height);

    return SWITCH_STATUS_SUCCESS;
}

/* 写入输出帧 */
static switch_status_t write_output_frame(pip_session_data_t *pip_data)
{
    int ret;

    if (!pip_data->output_codec_ctx || !pip_data->frame_output)
    {
        return SWITCH_STATUS_FALSE;
    }

    /* 设置帧时间戳 - 使用会话专用的PTS计数器 */
    pip_data->frame_output->pts = pip_data->output_pts++;

    /* 发送帧到编码器 */
    ret = avcodec_send_frame(pip_data->output_codec_ctx, pip_data->frame_output);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "发送帧到编码器失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 从编码器接收包 */
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(pip_data->output_codec_ctx, pip_data->output_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "接收编码包失败\n");
            return SWITCH_STATUS_FALSE;
        }

        /* 设置包时间戳 */
        av_packet_rescale_ts(pip_data->output_packet, pip_data->output_codec_ctx->time_base,
                             pip_data->output_stream->time_base);
        pip_data->output_packet->stream_index = pip_data->output_stream->index;

        /* 写入包到文件 */
        ret = av_interleaved_write_frame(pip_data->output_fmt_ctx, pip_data->output_packet);
        av_packet_unref(pip_data->output_packet);

        if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "写入帧失败\n");
            return SWITCH_STATUS_FALSE;
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* 刷新编码器缓冲区 */
static switch_status_t flush_encoder(pip_session_data_t *pip_data)
{
    int ret;

    if (!pip_data->output_codec_ctx || !pip_data->output_fmt_ctx || !pip_data->output_packet)
    {
        return SWITCH_STATUS_FALSE;
    }

    /* 发送NULL帧来刷新编码器 */
    ret = avcodec_send_frame(pip_data->output_codec_ctx, NULL);
    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "刷新编码器时发送NULL帧失败: %d\n", ret);
        return SWITCH_STATUS_FALSE;
    }

    /* 接收所有剩余的包 */
    while (1)
    {
        ret = avcodec_receive_packet(pip_data->output_codec_ctx, pip_data->output_packet);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        {
            break; // 没有更多帧了
        }
        else if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "刷新编码器时接收包失败: %d\n", ret);
            break;
        }

        /* 设置包时间戳 */
        av_packet_rescale_ts(pip_data->output_packet, pip_data->output_codec_ctx->time_base,
                             pip_data->output_stream->time_base);
        pip_data->output_packet->stream_index = pip_data->output_stream->index;

        /* 写入包到文件 */
        ret = av_interleaved_write_frame(pip_data->output_fmt_ctx, pip_data->output_packet);
        av_packet_unref(pip_data->output_packet);

        if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "刷新时写入帧失败: %d\n", ret);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* 媒体钩子回调：处理远程视频（读取） */
static switch_bool_t pip_read_video_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    pip_session_data_t *pip_data = (pip_session_data_t *)user_data;
    switch_frame_t *frame = NULL;

    switch (type)
    {
    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "PIP远程视频钩子初始化\n");
        break;

    case SWITCH_ABC_TYPE_READ_VIDEO_PING:
        // 从媒体钩子中获取视频帧
        frame = switch_core_media_bug_get_video_ping_frame(bug);
        if (frame && frame->img && pip_data->active)
        {
            // 锁定互斥锁，确保线程安全
            switch_mutex_lock(pip_data->frame_mutex);

            /* 保存最新的远程视频帧 */
            if (pip_data->last_remote_frame)
            {
                if (pip_data->last_remote_frame->img)
                {
                    // 释放之前的图像数据，防止内存泄漏
                    switch_img_free(&pip_data->last_remote_frame->img);
                }
            }
            else
            {
                // 如果没有分配过last_remote_frame，分配内存
                pip_data->last_remote_frame =
                    switch_core_alloc(switch_core_session_get_pool(pip_data->session), sizeof(switch_frame_t));
                // 初始化内存
                memset(pip_data->last_remote_frame, 0, sizeof(switch_frame_t));
            }

            /* 复制帧数据 */
            pip_data->last_remote_frame->img =
                switch_img_alloc(NULL, frame->img->fmt, frame->img->d_w, frame->img->d_h, 1);
            if (pip_data->last_remote_frame->img)
            {
                switch_img_copy(frame->img, &pip_data->last_remote_frame->img);
                pip_data->remote_frames_count++;

                /* 处理画中画叠加 */
                process_pip_overlay(pip_data);
            }

            switch_mutex_unlock(pip_data->frame_mutex);

            if (pip_data->remote_frames_count % 300 == 0)
            { /* 每10秒记录一次 */
                switch_log_printf(
                    SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "捕获远程视频帧: %dx%d, 远程: %llu, 本地: %llu, PIP: %llu\n",
                    frame->img->d_w, frame->img->d_h, (unsigned long long)pip_data->remote_frames_count,
                    (unsigned long long)pip_data->local_frames_count, (unsigned long long)pip_data->frames_processed);
            }
        }
        break;

    case SWITCH_ABC_TYPE_CLOSE:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "PIP远程视频钩子关闭\n");
        /* 确保清理PIP会话 */
        if (pip_data)
        {
            cleanup_pip_session(pip_data);
        }
        break;

    default:
        break;
    }

    return SWITCH_TRUE;
}

/* 处理画中画叠加 */
static switch_status_t process_pip_overlay(pip_session_data_t *pip_data)
{
    if (!pip_data || !pip_data->active)
    {
        return SWITCH_STATUS_FALSE;
    }

    /* 更新当前时间（基于远程视频帧率） */
    pip_data->current_time = pip_data->remote_frames_count / pip_data->target_fps;

    /* 检查是否需要读取新的本地视频帧 */
    if (pip_data->current_time >= pip_data->last_local_time + pip_data->local_frame_time)
    {
        /* 从本地视频文件读取帧 */
        if (read_local_video_frame(pip_data) != SWITCH_STATUS_SUCCESS)
        {
            return SWITCH_STATUS_FALSE;
        }
        pip_data->last_local_time = pip_data->current_time;
    }

    /* 确保有远程视频帧 */
    if (!pip_data->last_remote_frame || !pip_data->last_remote_frame->img)
    {
        return SWITCH_STATUS_FALSE;
    }

    /* 将switch_image_t转换为AVFrame进行处理 */
    return convert_and_overlay_frames(pip_data);
}

/* 转换switch_image_t为AVFrame并执行叠加 */
static switch_status_t convert_and_overlay_frames(pip_session_data_t *pip_data)
{
    switch_image_t *remote_img = pip_data->last_remote_frame->img;

    /* 检查远程视频帧尺寸 */
    if (!remote_img || remote_img->d_w <= 0 || remote_img->d_h <= 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "远程视频帧尺寸无效: %dx%d\n",
                          remote_img ? remote_img->d_w : 0, remote_img ? remote_img->d_h : 0);
        return SWITCH_STATUS_FALSE;
    }

    /* 如果远程视频尺寸改变，重新创建缩放上下文 */
    if (!pip_data->sws_ctx_pip || pip_data->remote_width != remote_img->d_w ||
        pip_data->remote_height != remote_img->d_h)
    {

        if (pip_data->sws_ctx_pip)
        {
            sws_freeContext(pip_data->sws_ctx_pip);
        }

        pip_data->remote_width = remote_img->d_w;
        pip_data->remote_height = remote_img->d_h;

        pip_data->sws_ctx_pip =
            sws_getContext(pip_data->remote_width, pip_data->remote_height, AV_PIX_FMT_YUV420P,
                           pip_data->pip_width, pip_data->pip_height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

        if (!pip_data->sws_ctx_pip)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "重新创建缩放上下文失败: %dx%d -> %dx%d\n",
                              pip_data->remote_width, pip_data->remote_height, pip_data->pip_width,
                              pip_data->pip_height);
            return SWITCH_STATUS_FALSE;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "缩放上下文已更新: %dx%d -> %dx%d\n",
                          pip_data->remote_width, pip_data->remote_height, pip_data->pip_width, pip_data->pip_height);
    }

    /* 设置远程视频帧数据 */
    pip_data->frame_pip->format = AV_PIX_FMT_YUV420P;
    pip_data->frame_pip->width = remote_img->d_w;
    pip_data->frame_pip->height = remote_img->d_h;

    /* 复制远程视频数据到AVFrame */
    pip_data->frame_pip->data[0] = remote_img->planes[0];
    pip_data->frame_pip->data[1] = remote_img->planes[1];
    pip_data->frame_pip->data[2] = remote_img->planes[2];
    pip_data->frame_pip->linesize[0] = remote_img->stride[0];
    pip_data->frame_pip->linesize[1] = remote_img->stride[1];
    pip_data->frame_pip->linesize[2] = remote_img->stride[2];

    /* 验证数据完整性 */
    if (!pip_data->frame_pip->data[0] || !pip_data->frame_pip->data[1] || !pip_data->frame_pip->data[2])
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "远程视频数据指针为空\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 缩放远程视频 */
    int ret = sws_scale(pip_data->sws_ctx_pip, (const uint8_t *const *)pip_data->frame_pip->data,
                        pip_data->frame_pip->linesize, 0, pip_data->frame_pip->height, pip_data->frame_pip_scaled->data,
                        pip_data->frame_pip_scaled->linesize);

    if (ret < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "视频缩放失败: %d (%dx%d -> %dx%d)\n", ret,
                          pip_data->frame_pip->width, pip_data->frame_pip->height, pip_data->pip_width,
                          pip_data->pip_height);
        return SWITCH_STATUS_FALSE;
    }

    /* 叠加视频 */
    overlay_yuv420p_frames(pip_data->frame_main, pip_data->frame_pip_scaled, pip_data->frame_output, pip_data->pip_x,
                           pip_data->pip_y, pip_data->pip_opacity);

    /* 写入叠加后的帧到输出文件 */
    if (pip_data->output_fmt_ctx)
    {
        write_output_frame(pip_data);
        pip_data->frames_processed++; /* 增加处理帧数计数 */
    }

    return SWITCH_STATUS_SUCCESS;
}

/* 初始化画中画处理上下文 */
static switch_status_t init_pip_context(pip_session_data_t *pip_data, const char *local_video_file)
{
    char output_file[512];
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    const char *file_ext;

    /* 检查文件扩展名以确定是图片还是视频 */
    file_ext = strrchr(local_video_file, '.');
    pip_data->use_image_mode = SWITCH_FALSE;

    if (file_ext)
    {
        /* 检查是否为常见的图片格式 */
        if (strcasecmp(file_ext, ".jpg") == 0 || strcasecmp(file_ext, ".jpeg") == 0 ||
            strcasecmp(file_ext, ".png") == 0 || strcasecmp(file_ext, ".bmp") == 0 ||
            strcasecmp(file_ext, ".gif") == 0 || strcasecmp(file_ext, ".tiff") == 0)
        {
            /* 图片模式：加载静态图片 */
            if (load_local_image(pip_data, local_video_file) != SWITCH_STATUS_SUCCESS)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "加载本地图片失败: %s\n", local_video_file);
                return SWITCH_STATUS_FALSE;
            }
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "使用图片模式: %s\n", local_video_file);
        }
        else
        {
            /* 视频模式：加载视频文件 */
            if (init_local_video_file(pip_data, local_video_file) != SWITCH_STATUS_SUCCESS)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "初始化本地视频失败: %s\n", local_video_file);
                return SWITCH_STATUS_FALSE;
            }
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "使用视频模式: %s\n", local_video_file);
        }
    }
    else
    {
        /* 无扩展名，默认尝试视频模式 */
        if (init_local_video_file(pip_data, local_video_file) != SWITCH_STATUS_SUCCESS)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "初始化本地文件失败: %s\n", local_video_file);
            return SWITCH_STATUS_FALSE;
        }
    }

    /* 生成输出文件名 */
    snprintf(output_file, sizeof(output_file),
             "/home/white/桌面/freeswitch-video-pip-module/output_pip_%04d%02d%02d_%02d%02d%02d.mp4",
             tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday, tm_now->tm_hour, tm_now->tm_min,
             tm_now->tm_sec);

    /* 初始化输出视频文件 */
    if (init_output_video_file(pip_data, output_file) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "输出文件初始化失败，将跳过保存\n");
    }

    /* 初始化PTS计数器 */
    pip_data->output_pts = 0;

    /* 初始化帧率同步 */
    pip_data->target_fps = 30.0; /* 目标输出帧率 */
    pip_data->current_time = 0.0;
    pip_data->last_local_time = 0.0;

    /* 分配AVFrame */
    pip_data->frame_main = av_frame_alloc();
    pip_data->frame_pip = av_frame_alloc();
    pip_data->frame_pip_scaled = av_frame_alloc();
    pip_data->frame_output = av_frame_alloc();

    if (!pip_data->frame_main || !pip_data->frame_pip || !pip_data->frame_pip_scaled || !pip_data->frame_output)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配AVFrame失败\n");
        // 清理已分配的frame
        if (pip_data->frame_main)
            av_frame_free(&pip_data->frame_main);
        if (pip_data->frame_pip)
            av_frame_free(&pip_data->frame_pip);
        if (pip_data->frame_pip_scaled)
            av_frame_free(&pip_data->frame_pip_scaled);
        if (pip_data->frame_output)
            av_frame_free(&pip_data->frame_output);
        return SWITCH_STATUS_FALSE;
    }

    /* 初始化远程视频尺寸（将在运行时动态设置） */
    pip_data->remote_width = 0;
    pip_data->remote_height = 0;
    pip_data->sws_ctx_pip = NULL; /* 将在第一次处理时创建 */

    /* 为缩放后的帧分配内存 */
    pip_data->frame_pip_scaled->format = AV_PIX_FMT_YUV420P;
    pip_data->frame_pip_scaled->width = pip_data->pip_width;
    pip_data->frame_pip_scaled->height = pip_data->pip_height;

    if (av_frame_get_buffer(pip_data->frame_pip_scaled, 32) < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配缩放帧缓冲区失败\n");
        return SWITCH_STATUS_FALSE;
    }

    /* 为输出帧分配内存 */
    pip_data->frame_output->format = AV_PIX_FMT_YUV420P;
    pip_data->frame_output->width = pip_data->main_width;
    pip_data->frame_output->height = pip_data->main_height;

    if (av_frame_get_buffer(pip_data->frame_output, 32) < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配输出帧缓冲区失败\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP上下文初始化成功: 本地视频%dx%d, PIP%dx%d@(%d,%d)\n",
                      pip_data->main_width, pip_data->main_height, pip_data->pip_width, pip_data->pip_height,
                      pip_data->pip_x, pip_data->pip_y);

    return SWITCH_STATUS_SUCCESS;
}

/* 简单的YUV420P帧叠加函数 */
static void overlay_yuv420p_frames(AVFrame *main_frame, AVFrame *pip_frame_scaled, AVFrame *output_frame, int x, int y,
                                   float opacity)
{
    int pip_width = pip_frame_scaled->width;
    int pip_height = pip_frame_scaled->height;
    int main_width = main_frame->width;
    int main_height = main_frame->height;

    /* 边界检查 */
    if (x + pip_width > main_width)
        pip_width = main_width - x;
    if (y + pip_height > main_height)
        pip_height = main_height - y;
    if (x < 0 || y < 0 || pip_width <= 0 || pip_height <= 0)
        return;

    /* 首先复制主视频到输出 */
    av_frame_copy(output_frame, main_frame);

    /* Y分量叠加 */
    for (int i = 0; i < pip_height; i++)
    {
        uint8_t *main_y = output_frame->data[0] + (y + i) * output_frame->linesize[0] + x;
        uint8_t *pip_y = pip_frame_scaled->data[0] + i * pip_frame_scaled->linesize[0];

        for (int j = 0; j < pip_width; j++)
        {
            main_y[j] = (uint8_t)(main_y[j] * (1.0f - opacity) + pip_y[j] * opacity);
        }
    }

    /* U分量叠加 (色度分量，尺寸减半) */
    int pip_width_uv = pip_width / 2;
    int pip_height_uv = pip_height / 2;
    int x_uv = x / 2;
    int y_uv = y / 2;

    for (int i = 0; i < pip_height_uv; i++)
    {
        uint8_t *main_u = output_frame->data[1] + (y_uv + i) * output_frame->linesize[1] + x_uv;
        uint8_t *pip_u = pip_frame_scaled->data[1] + i * pip_frame_scaled->linesize[1];

        for (int j = 0; j < pip_width_uv; j++)
        {
            // Alpha混合算法
            main_u[j] = (uint8_t)(main_u[j] * (1.0f - opacity) + pip_u[j] * opacity);
        }
    }

    /* V分量叠加 */
    for (int i = 0; i < pip_height_uv; i++)
    {
        uint8_t *main_v = output_frame->data[2] + (y_uv + i) * output_frame->linesize[2] + x_uv;
        uint8_t *pip_v = pip_frame_scaled->data[2] + i * pip_frame_scaled->linesize[2];

        for (int j = 0; j < pip_width_uv; j++)
        {
            main_v[j] = (uint8_t)(main_v[j] * (1.0f - opacity) + pip_v[j] * opacity);
        }
    }
}

/* 处理视频帧 */
// static switch_status_t process_video_frame(pip_session_data_t *pip_data, switch_frame_t *main_frame,
//                                            switch_frame_t *pip_frame)
// {
//     if (!pip_data || !main_frame || !pip_frame)
//     {
//         return SWITCH_STATUS_FALSE;
//     }

//     switch_mutex_lock(pip_data->mutex);

//     if (!pip_data->active)
//     {
//         switch_mutex_unlock(pip_data->mutex);
//         return SWITCH_STATUS_FALSE;
//     }

//     /* 设置主视频帧数据 */
//     if (av_image_fill_arrays(pip_data->frame_main->data, pip_data->frame_main->linesize, (uint8_t *)main_frame->data,
//                              AV_PIX_FMT_YUV420P, pip_data->main_width, pip_data->main_height, 1) < 0)
//     {
//         switch_mutex_unlock(pip_data->mutex);
//         return SWITCH_STATUS_FALSE;
//     }
//     pip_data->frame_main->width = pip_data->main_width;
//     pip_data->frame_main->height = pip_data->main_height;
//     pip_data->frame_main->format = AV_PIX_FMT_YUV420P;

//     /* 设置PIP视频帧数据 */
//     if (av_image_fill_arrays(pip_data->frame_pip->data, pip_data->frame_pip->linesize, (uint8_t *)pip_frame->data,
//                              AV_PIX_FMT_YUV420P, pip_data->main_width, pip_data->main_height, 1) < 0)
//     {
//         switch_mutex_unlock(pip_data->mutex);
//         return SWITCH_STATUS_FALSE;
//     }
//     pip_data->frame_pip->width = pip_data->main_width;
//     pip_data->frame_pip->height = pip_data->main_height;
//     pip_data->frame_pip->format = AV_PIX_FMT_YUV420P;

//     /* 缩放PIP视频 */
//     if (sws_scale(pip_data->sws_ctx_pip, (const uint8_t *const *)pip_data->frame_pip->data,
//                   pip_data->frame_pip->linesize, 0, pip_data->main_height, pip_data->frame_pip_scaled->data,
//                   pip_data->frame_pip_scaled->linesize) < 0)
//     {
//         switch_mutex_unlock(pip_data->mutex);
//         return SWITCH_STATUS_FALSE;
//     }

//     /* 叠加视频 */
//     overlay_yuv420p_frames(pip_data->frame_main, pip_data->frame_pip_scaled, pip_data->frame_output, pip_data->pip_x,
//                            pip_data->pip_y, pip_data->pip_opacity);

//     /* 将处理后的数据拷贝回main_frame */
//     int frame_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pip_data->main_width, pip_data->main_height, 1);

//     if (frame_size > 0 && frame_size <= main_frame->buflen)
//     {
//         // (const uint8_t *const *)  常量指针+指针常量的组合，表示指向常量数据的指针的指针
//         av_image_copy_to_buffer((uint8_t *)main_frame->data, frame_size,
//                                 (const uint8_t *const *)pip_data->frame_output->data, pip_data->frame_output->linesize,
//                                 AV_PIX_FMT_YUV420P, pip_data->main_width, pip_data->main_height, 1);
//         main_frame->datalen = frame_size;
//         pip_data->frames_processed++;
//     }

//     switch_mutex_unlock(pip_data->mutex);
//     return SWITCH_STATUS_SUCCESS;
// }

/* 清理PIP会话 */
static void cleanup_pip_session(pip_session_data_t *pip_data)
{
    if (!pip_data)
        return;

    /* 避免重复清理 */
    if (!pip_data->active)
        return;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "开始清理PIP会话...\n");

    /* 立即设置为非活跃状态 */
    pip_data->active = SWITCH_FALSE;

    /* 清理媒体钩子 */
    if (pip_data->read_bug)
    {
        switch_core_media_bug_remove(pip_data->session, &pip_data->read_bug);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "已移除媒体钩子\n");
        pip_data->read_bug = NULL;
    }

    /* 清理本地视频文件资源 */
    // 清理视频包
    if (pip_data->local_packet)
    {
        av_packet_free(&pip_data->local_packet);
        pip_data->local_packet = NULL;
    }
    // 关闭解码器上下文
    if (pip_data->local_codec_ctx)
    {
        avcodec_close(pip_data->local_codec_ctx);
        avcodec_free_context(&pip_data->local_codec_ctx);
        pip_data->local_codec_ctx = NULL;
    }
    // 关闭输入格式上下文
    if (pip_data->local_fmt_ctx)
    {
        avformat_close_input(&pip_data->local_fmt_ctx);
        pip_data->local_fmt_ctx = NULL;
    }

    /* 清理输出视频文件资源 */
    if (pip_data->output_codec_ctx && pip_data->output_fmt_ctx)
    {
        /* 刷新编码器 */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "开始刷新编码器...\n");
        flush_encoder(pip_data);

        avcodec_close(pip_data->output_codec_ctx);
        avcodec_free_context(&pip_data->output_codec_ctx);
        pip_data->output_codec_ctx = NULL;
    }

    if (pip_data->output_packet)
    {
        av_packet_free(&pip_data->output_packet);
        pip_data->output_packet = NULL;
    }

    if (pip_data->output_fmt_ctx)
    {
        /* 写入文件尾 */
        int ret = av_write_trailer(pip_data->output_fmt_ctx);
        if (ret < 0)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "写入视频文件尾失败: %d\n", ret);
        }

        if (!(pip_data->output_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&pip_data->output_fmt_ctx->pb);
        }
        avformat_free_context(pip_data->output_fmt_ctx);
        pip_data->output_fmt_ctx = NULL;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP输出视频已保存: %s\n", pip_data->output_filename);
    }

    /* 清理FFmpeg资源 */
    if (pip_data->sws_ctx_pip)
    {
        sws_freeContext(pip_data->sws_ctx_pip);
        pip_data->sws_ctx_pip = NULL;
    }

    /* 安全释放AVFrame */
    if (pip_data->frame_main)
    {
        av_frame_free(&pip_data->frame_main);
        pip_data->frame_main = NULL;
    }
    if (pip_data->frame_pip)
    {
        av_frame_free(&pip_data->frame_pip);
        pip_data->frame_pip = NULL;
    }
    if (pip_data->frame_pip_scaled)
    {
        av_frame_free(&pip_data->frame_pip_scaled);
        pip_data->frame_pip_scaled = NULL;
    }
    if (pip_data->frame_output)
    {
        av_frame_free(&pip_data->frame_output);
        pip_data->frame_output = NULL;
    }

    /* 清理本地图片帧 */
    if (pip_data->local_image_frame)
    {
        av_frame_free(&pip_data->local_image_frame);
        pip_data->local_image_frame = NULL;
    }

    /* 清理远程视频帧 */
    if (pip_data->last_remote_frame && pip_data->last_remote_frame->img)
    {
        switch_img_free(&pip_data->last_remote_frame->img);
        pip_data->last_remote_frame->img = NULL;
        pip_data->last_remote_frame = NULL;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "PIP会话清理完成，处理帧数: %llu, 远程帧: %llu, 本地帧: %llu\n",
                      (unsigned long long)pip_data->frames_processed, (unsigned long long)pip_data->remote_frames_count,
                      (unsigned long long)pip_data->local_frames_count);
}

/* API: 启动画中画 */
SWITCH_STANDARD_API(video_pip_start_function)
{
    switch_core_session_t *psession = NULL;
    pip_session_data_t *pip_data = NULL;
    char *uuid = NULL;
    char *local_video_file = NULL;
    switch_memory_pool_t *pool = NULL;

    /* 创建临时内存池 */
    switch_core_new_memory_pool(&pool);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "开始处理video_pip_start命令: %s\n", cmd ? cmd : "(null)");

    /* 解析参数 */
    if (!zstr(cmd))
    {
        char *cmd_copy = switch_core_strdup(pool, cmd);
        char *argv[2];
        int argc = 0;
        // 用空格分割字符串为uuid 和文件路径
        char *token = strtok(cmd_copy, " ");
        while (token != NULL && argc < 2)
        {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }

        if (argc >= 1)
        {
            uuid = switch_core_strdup(pool, argv[0]);
        }
        if (argc >= 2)
        {
            local_video_file = switch_core_strdup(pool, argv[1]);
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "解析参数完成 - UUID: %s, 视频文件: %s\n",
                      uuid ? uuid : "(auto)", local_video_file ? local_video_file : "(default)");

    /* 如果没有提供本地视频文件，使用默认路径 */
    if (!local_video_file)
    {
        local_video_file = switch_core_strdup(pool, "/home/white/桌面/freeswitch-video-pip-module/test_pictures/test.jpg");
    } /* 检查本地视频文件是否存在 */
    if (access(local_video_file, R_OK) != 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法访问本地视频文件: %s\n", local_video_file);
        stream->write_function(stream, "-ERR 无法访问本地视频文件: %s\n", local_video_file);
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }

    /* 如果没有提供UUID，尝试查找当前活跃的会话 */
    if (!uuid)
    {
        switch_hash_index_t *hi;
        const void *key;
        void *val;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "没有提供UUID，查找活跃会话\n");

        /* 查找第一个活跃的会话 */
        switch_mutex_lock(module_mutex);
        for (hi = switch_core_hash_first(session_pip_map); hi; hi = switch_core_hash_next(&hi))
        {
            switch_core_hash_this(hi, &key, NULL, &val);
            uuid = switch_core_strdup(pool, (const char *)key);
            break; /* 使用第一个找到的会话 */
        }
        switch_mutex_unlock(module_mutex);

        if (!uuid)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "未找到活跃会话\n");
            stream->write_function(stream, "-ERR 需要会话UUID，没有找到活跃会话\n");
            stream->write_function(stream, "用法: video_pip_start [uuid] [local_video_file]\n");
            switch_core_destroy_memory_pool(&pool);
            return SWITCH_STATUS_SUCCESS;
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "使用会话UUID: %s\n", uuid);

    /* 查找会话 */
    // 使用UUID查找会话,并且会上锁
    psession = switch_core_session_locate(uuid);
    if (!psession)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "找不到会话: %s\n", uuid);
        stream->write_function(stream, "-ERR 找不到会话: %s\n", uuid);
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "成功找到会话，开始分配PIP数据结构\n");

    /* 分配PIP数据结构 */
    pip_data = switch_core_alloc(switch_core_session_get_pool(psession), sizeof(pip_session_data_t));
    if (!pip_data)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "分配PIP数据结构失败\n");
        switch_core_session_rwunlock(psession);
        stream->write_function(stream, "-ERR 内存分配失败\n");
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }
    memset(pip_data, 0, sizeof(pip_session_data_t));

    pip_data->session = psession;
    pip_data->channel = switch_core_session_get_channel(psession);

    /* 设置默认参数 */
    pip_data->main_width = 640; /* 将由本地视频文件确定 */
    pip_data->main_height = 480;
    pip_data->pip_width = DEFAULT_PIP_WIDTH;
    pip_data->pip_height = DEFAULT_PIP_HEIGHT;
    pip_data->pip_x = DEFAULT_PIP_X;
    pip_data->pip_y = DEFAULT_PIP_Y;
    pip_data->pip_opacity = DEFAULT_PIP_OPACITY;
    pip_data->active = SWITCH_TRUE;

    /* 初始化互斥锁 */
    if (switch_mutex_init(&pip_data->mutex, SWITCH_MUTEX_UNNESTED, switch_core_session_get_pool(psession)) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "初始化mutex失败\n");
        switch_core_session_rwunlock(psession);
        stream->write_function(stream, "-ERR 初始化互斥锁失败\n");
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }

    if (switch_mutex_init(&pip_data->frame_mutex, SWITCH_MUTEX_UNNESTED, switch_core_session_get_pool(psession)) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "初始化frame_mutex失败\n");
        switch_mutex_destroy(pip_data->mutex);
        switch_core_session_rwunlock(psession);
        stream->write_function(stream, "-ERR 初始化帧互斥锁失败\n");
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }

    /* 初始化PIP上下文（包含本地视频文件） */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "开始初始化PIP上下文\n");
    if (init_pip_context(pip_data, local_video_file) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "初始化PIP上下文失败\n");
        cleanup_pip_session(pip_data);
        switch_core_session_rwunlock(psession);
        stream->write_function(stream, "-ERR 初始化PIP上下文失败\n");
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP上下文初始化成功\n");

    /* 创建媒体钩子来捕获远程视频 */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "开始创建媒体钩子\n");
    // 第四个参数是一个回调函数指针，指向处理远程视频帧的函数
    if (switch_core_media_bug_add(psession, "video_pip_read", uuid, pip_read_video_callback, pip_data, 0,
                                  SMBF_READ_VIDEO_PING, &pip_data->read_bug) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "创建媒体钩子失败\n");
        cleanup_pip_session(pip_data);
        switch_core_session_rwunlock(psession);
        stream->write_function(stream, "-ERR 创建媒体钩子失败\n");
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "媒体钩子创建成功\n");

    /* 存储到哈希表 */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "存储会话到哈希表\n");
    switch_mutex_lock(module_mutex);
    switch_core_hash_insert(session_pip_map, uuid, pip_data);
    switch_mutex_unlock(module_mutex);

    switch_core_session_rwunlock(psession);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PIP启动完成\n");
    stream->write_function(stream, "+OK PIP启动成功 UUID=%s, 本地视频=%s\n", uuid, local_video_file);

    /* 清理临时内存池 */
    switch_core_destroy_memory_pool(&pool);
    return SWITCH_STATUS_SUCCESS;
}

/* API: 停止画中画 */
SWITCH_STANDARD_API(video_pip_stop_function)
{
    pip_session_data_t *pip_data = NULL;

    if (zstr(cmd))
    {
        /* 如果没有提供UUID，停止所有活跃的PIP会话 */
        switch_hash_index_t *hi;
        const void *key;
        void *val;
        int stopped_count = 0;

        switch_mutex_lock(module_mutex);

        /* 简单遍历并标记停止 */
        for (hi = switch_core_hash_first(session_pip_map); hi; hi = switch_core_hash_next(&hi))
        {
            switch_core_hash_this(hi, &key, NULL, &val);
            pip_data = (pip_session_data_t *)val;
            if (pip_data && pip_data->active)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "停止PIP会话: %s\n", (const char *)key);
                /* 使用异步方式清理，避免阻塞 */
                pip_data->active = SWITCH_FALSE;
                stopped_count++;
            }
        }

        /* 清空哈希表 */
        switch_core_hash_delete_multi(session_pip_map, NULL, NULL);

        switch_mutex_unlock(module_mutex);

        /* 异步清理会话 */
        switch_mutex_lock(module_mutex);
        for (hi = switch_core_hash_first(session_pip_map); hi; hi = switch_core_hash_next(&hi))
        {
            switch_core_hash_this(hi, &key, NULL, &val);
            pip_data = (pip_session_data_t *)val;
            if (pip_data)
            {
                cleanup_pip_session(pip_data);
            }
        }
        switch_mutex_unlock(module_mutex);

        if (stopped_count > 0)
        {
            stream->write_function(stream, "+OK 停止了 %d 个PIP会话\n", stopped_count);
        }
        else
        {
            stream->write_function(stream, "+OK 没有活跃的PIP会话需要停止\n");
        }
        return SWITCH_STATUS_SUCCESS;
    }

    /* 停止指定UUID的会话 */
    switch_mutex_lock(module_mutex);
    pip_data = (pip_session_data_t *)switch_core_hash_find(session_pip_map, cmd);
    if (pip_data)
    {
        switch_core_hash_delete(session_pip_map, cmd);
        pip_data->active = SWITCH_FALSE; /* 立即标记为非活跃 */
    }
    switch_mutex_unlock(module_mutex);

    if (pip_data)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "手动停止PIP会话: %s\n", cmd);
        cleanup_pip_session(pip_data);
        stream->write_function(stream, "+OK PIP停止成功，视频已保存\n");
    }
    else
    {
        stream->write_function(stream, "-ERR 找不到对应的PIP会话: %s\n", cmd);
    }

    return SWITCH_STATUS_SUCCESS;
}

/* API: 查看状态 */
SWITCH_STANDARD_API(video_pip_status_function)
{
    pip_session_data_t *pip_data = NULL;

    if (zstr(cmd))
    {
        /* 显示所有活跃会话 */
        switch_hash_index_t *hi;
        const void *key;
        void *val;
        int count = 0;

        switch_mutex_lock(module_mutex);
        for (hi = switch_core_hash_first(session_pip_map); hi; hi = switch_core_hash_next(&hi))
        {
            switch_core_hash_this(hi, &key, NULL, &val);
            pip_data = (pip_session_data_t *)val;

            stream->write_function(stream, "会话: %s, 帧数: %llu, 状态: %s\n", (char *)key,
                                   (unsigned long long)pip_data->frames_processed, pip_data->active ? "活跃" : "停止");
            count++;
        }
        switch_mutex_unlock(module_mutex);

        if (count == 0)
        {
            stream->write_function(stream, "没有活跃的PIP会话\n");
        }
    }
    else
    {
        /* 显示特定会话 */
        switch_mutex_lock(module_mutex);
        pip_data = (pip_session_data_t *)switch_core_hash_find(session_pip_map, cmd);
        switch_mutex_unlock(module_mutex);

        if (pip_data)
        {
            stream->write_function(stream,
                                   "会话UUID: %s\n"
                                   "主视频: %dx%d\n"
                                   "PIP: %dx%d@(%d,%d) 透明度=%.2f\n"
                                   "处理帧数: %llu\n"
                                   "状态: %s\n",
                                   cmd, pip_data->main_width, pip_data->main_height, pip_data->pip_width,
                                   pip_data->pip_height, pip_data->pip_x, pip_data->pip_y, pip_data->pip_opacity,
                                   (unsigned long long)pip_data->frames_processed, pip_data->active ? "活跃" : "停止");
        }
        else
        {
            stream->write_function(stream, "找不到会话: %s\n", cmd);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* 模块加载 */
SWITCH_MODULE_LOAD_FUNCTION(mod_video_pip_load)
{
    switch_api_interface_t *api_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    module_pool = pool;
    switch_mutex_init(&module_mutex, SWITCH_MUTEX_UNNESTED, module_pool);
    switch_core_hash_init(&session_pip_map);

    /* 注册API */
    SWITCH_ADD_API(api_interface, "video_pip_start", "启动PIP", video_pip_start_function, "<uuid> [local_video_file]");
    SWITCH_ADD_API(api_interface, "video_pip_stop", "停止PIP", video_pip_stop_function, "<uuid>");
    SWITCH_ADD_API(api_interface, "video_pip_status", "PIP状态", video_pip_status_function, "[uuid]");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "视频画中画模块加载成功 - 支持远程视频叠加到本地MP4文件\n");

    return SWITCH_STATUS_SUCCESS;
}

/* 模块卸载 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_pip_shutdown)
{
    switch_hash_index_t *hi;
    const void *key;
    void *val;
    pip_session_data_t *pip_data;

    /* 清理所有会话 */
    switch_mutex_lock(module_mutex);
    for (hi = switch_core_hash_first(session_pip_map); hi; hi = switch_core_hash_next(&hi))
    {
        switch_core_hash_this(hi, &key, NULL, &val);
        pip_data = (pip_session_data_t *)val;
        cleanup_pip_session(pip_data);
    }
    switch_core_hash_destroy(&session_pip_map);
    switch_mutex_unlock(module_mutex);

    switch_mutex_destroy(module_mutex);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "视频画中画模块卸载完成\n");

    return SWITCH_STATUS_SUCCESS;
}