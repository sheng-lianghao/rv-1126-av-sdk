#include "audio_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

int audio_encoder_open(AudioEncoder *ae, int sample_rate,
                       int channels, int bitrate)
{
    memset(ae, 0, sizeof(*ae));
    ae->sample_rate = sample_rate;
    ae->channels    = channels;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(stderr, "AAC encoder not found\n");
        return -1;
    }

    ae->codec_ctx = avcodec_alloc_context3(codec);
    if (!ae->codec_ctx) return -1;

    ae->codec_ctx->sample_fmt    = AV_SAMPLE_FMT_FLTP;
    ae->codec_ctx->sample_rate   = sample_rate;
    ae->codec_ctx->bit_rate      = bitrate;
    ae->codec_ctx->time_base     = (AVRational){1, sample_rate};
    ae->codec_ctx->channels      = channels;
    ae->codec_ctx->channel_layout = (channels == 1)
        ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;

    if (avcodec_open2(ae->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 (AAC) failed\n");
        avcodec_free_context(&ae->codec_ctx);
        return -1;
    }

    ae->frame_size = ae->codec_ctx->frame_size;  /* 通常 1024 */

    /* SwrContext: S16LE 交错 → FLTP 平面 */
    uint64_t in_layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    ae->swr_ctx = swr_alloc_set_opts(NULL,
                      ae->codec_ctx->channel_layout, AV_SAMPLE_FMT_FLTP, sample_rate,
                      in_layout,                     AV_SAMPLE_FMT_S16,  sample_rate,
                      0, NULL);
    if (!ae->swr_ctx || swr_init(ae->swr_ctx) < 0) {
        fprintf(stderr, "swr_alloc/init failed\n");
        avcodec_free_context(&ae->codec_ctx);
        return -1;
    }

    /* AVAudioFifo: 解耦 ALSA period_size 与 AAC frame_size */
    ae->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels,
                                   ae->frame_size * 4);
    if (!ae->fifo) {
        swr_free(&ae->swr_ctx);
        avcodec_free_context(&ae->codec_ctx);
        return -1;
    }

    ae->frame = av_frame_alloc();
    if (!ae->frame) {
        av_audio_fifo_free(ae->fifo);
        swr_free(&ae->swr_ctx);
        avcodec_free_context(&ae->codec_ctx);
        return -1;
    }

    return 0;
}

int audio_encoder_encode(AudioEncoder *ae,
                         const void *pcm_data, size_t pcm_size,
                         void (*on_packet)(AVPacket *pkt, void *ctx),
                         void *ctx)
{
    int n_input = (int)(pcm_size / (ae->channels * sizeof(int16_t)));

    /* 1. 分配临时帧用于重采样输出 */
    AVFrame *tmp = av_frame_alloc();
    if (!tmp) return -1;
    tmp->nb_samples     = n_input;
    tmp->sample_rate    = ae->sample_rate;
    tmp->format         = AV_SAMPLE_FMT_FLTP;
    tmp->channels       = ae->codec_ctx->channels;
    tmp->channel_layout = ae->codec_ctx->channel_layout;
    if (av_frame_get_buffer(tmp, 0) < 0) {
        av_frame_free(&tmp);
        return -1;
    }

    /* 2. S16LE → FLTP */
    const uint8_t *in_data[1] = { (const uint8_t *)pcm_data };
    int ret = swr_convert(ae->swr_ctx,
                          tmp->data, n_input,
                          in_data,   n_input);
    if (ret < 0) {
        av_frame_free(&tmp);
        return -1;
    }

    /* 3. 写入 FIFO */
    av_audio_fifo_write(ae->fifo, (void **)tmp->data, ret);
    av_frame_free(&tmp);

    /* 4. 只要 FIFO 中有足够的采样点就编码一帧 */
    while (av_audio_fifo_size(ae->fifo) >= ae->frame_size) {
        ae->frame->nb_samples     = ae->frame_size;
        ae->frame->format         = AV_SAMPLE_FMT_FLTP;
        ae->frame->sample_rate    = ae->sample_rate;
        ae->frame->channels       = ae->codec_ctx->channels;
        ae->frame->channel_layout = ae->codec_ctx->channel_layout;
        av_frame_get_buffer(ae->frame, 0);

        av_audio_fifo_read(ae->fifo, (void **)ae->frame->data, ae->frame_size);
        ae->frame->pts  = ae->next_pts;
        ae->next_pts   += ae->frame_size;

        avcodec_send_frame(ae->codec_ctx, ae->frame);
        av_frame_unref(ae->frame);

        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(ae->codec_ctx, pkt) == 0) {
            on_packet(pkt, ctx);
            pkt = av_packet_alloc();
        }
        av_packet_free(&pkt);
    }
    return 0;
}

int audio_encoder_flush(AudioEncoder *ae, AVPacket **pkt)
{
    static int flush_sent = 0;
    if (!flush_sent) {
        avcodec_send_frame(ae->codec_ctx, NULL);
        flush_sent = 1;
    }

    *pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(ae->codec_ctx, *pkt);
    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        av_packet_free(pkt);
        *pkt = NULL;
        flush_sent = 0;
        return 1;
    }
    if (ret < 0) {
        av_packet_free(pkt);
        *pkt = NULL;
        flush_sent = 0;
        return -1;
    }
    return 0;
}

void audio_encoder_close(AudioEncoder *ae)
{
    av_frame_free(&ae->frame);
    av_audio_fifo_free(ae->fifo);
    swr_free(&ae->swr_ctx);
    avcodec_free_context(&ae->codec_ctx);
}

