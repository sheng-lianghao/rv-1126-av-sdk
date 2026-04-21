#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

typedef struct {
    AVCodecContext  *codec_ctx;
    AVFrame         *frame;
    struct SwrContext *swr_ctx;   /* S16LE 交错 → FLTP 平面（AAC 所需） */
    AVAudioFifo     *fifo;        /* 解耦 ALSA period_size 与 AAC frame_size */
    int              sample_rate;
    int              channels;
    int              frame_size;  /* codec_ctx->frame_size，通常 1024 */
    int64_t          next_pts;    /* 单位：采样点数（codec time_base = {1, sample_rate}） */
} AudioEncoder;

/* 创建 AAC 编码器上下文 */
int  audio_encoder_open(AudioEncoder *ae, int sample_rate,
                        int channels, int bitrate);

/* 将 S16LE PCM 数据喂入 FIFO，并尽可能多地编码出完整帧。
   每次调用可能产生 0..N 个 AVPacket，通过回调 on_packet 传出。
   on_packet 取得 pkt 所有权（需调用 av_packet_free）。*/
int  audio_encoder_encode(AudioEncoder *ae,
                          const void *pcm_data, size_t pcm_size,
                          void (*on_packet)(AVPacket *pkt, void *ctx),
                          void *ctx);

/* 刷新编码器（同 video_encoder_flush 语义） */
int  audio_encoder_flush(AudioEncoder *ae, AVPacket **pkt);

void audio_encoder_close(AudioEncoder *ae);

#endif /* AUDIO_ENCODER_H */
