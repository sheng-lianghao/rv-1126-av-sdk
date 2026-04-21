#include "video_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>

int video_encoder_open(VideoEncoder *ve, int width, int height,
                       int fps_num, int fps_den, int bitrate,
                       int oformat_flags)
{
    memset(ve, 0, sizeof(*ve));
    ve->width   = width;
    ve->height  = height;
    ve->fps_num = fps_num;
    ve->fps_den = fps_den;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H.264 encoder not found\n");
        return -1;
    }

    ve->codec_ctx = avcodec_alloc_context3(codec);
    if (!ve->codec_ctx) return -1;

    ve->codec_ctx->width       = width;
    ve->codec_ctx->height      = height;
    ve->codec_ctx->time_base   = (AVRational){fps_den, fps_num};
    ve->codec_ctx->framerate   = (AVRational){fps_num, fps_den};
    ve->codec_ctx->pix_fmt     = AV_PIX_FMT_NV12;  /* h264_rkmpp 硬件编码器要求 NV12 */
    ve->codec_ctx->bit_rate    = bitrate;
    ve->codec_ctx->gop_size    = fps_num;
    ve->codec_ctx->max_b_frames = 0;

    if (oformat_flags & AVFMT_GLOBALHEADER)
        ve->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(ve->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 (H.264) failed\n");
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    ve->frame = av_frame_alloc();
    if (!ve->frame) {
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }
    ve->frame->format = AV_PIX_FMT_NV12;
    ve->frame->width  = width;
    ve->frame->height = height;
    if (av_frame_get_buffer(ve->frame, 32) < 0) {
        av_frame_free(&ve->frame);
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    return 0;
}

int video_encoder_encode(VideoEncoder *ve, const void *nv12_data,
                         size_t data_size, int64_t pts_ns,
                         AVPacket **pkt)
{
    (void)data_size;

    /* 摄像头输出 NV12，直接填入 frame，无需颜色空间转换
     * plane[0]: Y，大小 width*height
     * plane[1]: UV 交错，大小 width*height/2            */
    av_frame_make_writable(ve->frame);
    const uint8_t *y  = (const uint8_t *)nv12_data;
    const uint8_t *uv = y + ve->width * ve->height;

    for (int i = 0; i < ve->height; i++)
        memcpy(ve->frame->data[0] + i * ve->frame->linesize[0],
               y + i * ve->width, ve->width);
    for (int i = 0; i < ve->height / 2; i++)
        memcpy(ve->frame->data[1] + i * ve->frame->linesize[1],
               uv + i * ve->width, ve->width);

    ve->frame->pts = av_rescale_q(pts_ns,
                                  (AVRational){1, 1000000000},
                                  ve->codec_ctx->time_base);

    int ret = avcodec_send_frame(ve->codec_ctx, ve->frame);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        fprintf(stderr, "avcodec_send_frame (video): %d\n", ret);
        return -1;
    }

    *pkt = av_packet_alloc();
    ret = avcodec_receive_packet(ve->codec_ctx, *pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_packet_free(pkt);
        *pkt = NULL;
        return 0;
    }
    if (ret < 0) {
        av_packet_free(pkt);
        *pkt = NULL;
        return -1;
    }
    return 0;
}

int video_encoder_flush(VideoEncoder *ve, AVPacket **pkt)
{
    static int flush_sent = 0;
    if (!flush_sent) {
        avcodec_send_frame(ve->codec_ctx, NULL);
        flush_sent = 1;
    }

    *pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(ve->codec_ctx, *pkt);
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

void video_encoder_close(VideoEncoder *ve)
{
    av_frame_free(&ve->frame);
    avcodec_free_context(&ve->codec_ctx);
}

