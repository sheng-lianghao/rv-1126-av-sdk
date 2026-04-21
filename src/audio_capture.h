#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <alsa/asoundlib.h>

typedef struct {
    snd_pcm_t         *handle;
    char               device[64];
    unsigned int       sample_rate;
    unsigned int       channels;
    snd_pcm_uframes_t  period_size;    /* 实际协商后的值（帧数） */
    int16_t           *capture_buf;    /* period_size * channels * 2 字节 */
    uint64_t           total_frames;   /* 累计采集帧数，用于计算无漂移 PTS */
} AudioCapture;

/* 打开 ALSA PCM 设备，配置硬件参数 */
int  audio_capture_open(AudioCapture *ac, const char *device,
                        unsigned int sample_rate, unsigned int channels);

/* 启动采集（snd_pcm_start） */
int  audio_capture_start(AudioCapture *ac);

/* 读取一个 period 的 PCM 数据，拷贝到堆上。
   调用者负责 free(*pcm_data)。pts_ns = total_frames/sample_rate * 1e9。*/
int  audio_capture_grab(AudioCapture *ac, void **pcm_data,
                        size_t *pcm_size, int64_t *pts_ns);

/* 停止采集 */
int  audio_capture_stop(AudioCapture *ac);

/* 释放资源（snd_pcm_close） */
void audio_capture_close(AudioCapture *ac);

#endif /* AUDIO_CAPTURE_H */