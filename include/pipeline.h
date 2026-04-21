#ifndef PIPELINE_H
#define PIPELINE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PipelineConfig {
    /* V4L2 视频采集 */
    const char  *video_device;       /* 例如 "/dev/video0" */
    unsigned int video_width;
    unsigned int video_height;
    unsigned int video_fps;          /* 帧率，例如 30 */

    /* ALSA 音频采集 */
    const char  *audio_device;       /* 例如 "default" 或 "hw:0,0" */
    unsigned int audio_sample_rate;  /* 采样率，例如 44100 */
    unsigned int audio_channels;     /* 声道数，1 或 2 */

    /* 编码参数 */
    int video_bitrate;               /* H.264 码率，例如 2000000 */
    int audio_bitrate;               /* AAC 码率，例如 128000 */

    /* 输出 */
    const char  *output_path;        /* 例如 "output.mp4" */
} PipelineConfig;

typedef struct Pipeline Pipeline;    /* 对外不透明句柄 */

/* 初始化 pipeline，返回 NULL 表示失败 */
Pipeline *pipeline_init(const PipelineConfig *cfg);

/* 启动所有采集/编码/封装线程，返回 0 成功 */
int pipeline_start(Pipeline *p);

/* 停止并等待所有线程退出，刷新编码器，写 MP4 trailer */
void pipeline_stop(Pipeline *p);

/* 释放所有资源（需在 pipeline_stop 之后调用） */
void pipeline_destroy(Pipeline *p);

#ifdef __cplusplus
}
#endif

#endif /* PIPELINE_H */
