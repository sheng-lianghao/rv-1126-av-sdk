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

    ve->codec_ctx->width     = width;
    ve->codec_ctx->height    = height;
    ve->codec_ctx->time_base = (AVRational){fps_den, fps_num};
    ve->codec_ctx->framerate = (AVRational){fps_num, fps_den};
    ve->codec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    ve->codec_ctx->bit_rate  = bitrate;
    ve->codec_ctx->gop_size  = fps_num;   /* 每秒一个关键帧 */
    ve->codec_ctx->max_b_frames = 0;      /* 禁用 B 帧，确保 DTS==PTS */

    /* MP4 容器需要 global_header（SPS/PPS 写入 extradata） */
    if (oformat_flags & AVFMT_GLOBALHEADER)
        ve->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(ve->codec_ctx->priv_data, "preset", "fast",        0);
    av_opt_set(ve->codec_ctx->priv_data, "tune",   "zerolatency", 0);

    if (avcodec_open2(ve->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 (H.264) failed\n");
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    /* SwsContext: NV12 → YUV420P */
    ve->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12,
                                 width, height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (!ve->sws_ctx) {
        fprintf(stderr, "sws_getContext failed\n");
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    ve->frame = av_frame_alloc();
    if (!ve->frame) {
        sws_freeContext(ve->sws_ctx);
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }
    ve->frame->format = AV_PIX_FMT_YUV420P;
    ve->frame->width  = width;
    ve->frame->height = height;
    if (av_frame_get_buffer(ve->frame, 32) < 0) {
        av_frame_free(&ve->frame);
        sws_freeContext(ve->sws_ctx);
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    return 0;
}

int video_encoder_encode(VideoEncoder *ve, const void *yuyv_data,
                         size_t data_size, int64_t pts_ns,
                         AVPacket **pkt)
{
    (void)data_size;

    /* NV12 → YUV420P：Y 平面 stride=width，UV 平面 stride=width */
    const uint8_t *src_data[2]  = {
        (const uint8_t *)yuyv_data,
        (const uint8_t *)yuyv_data + ve->width * ve->height
    };
    int src_stride[2] = { ve->width, ve->width };

    av_frame_make_writable(ve->frame);
    sws_scale(ve->sws_ctx, src_data, src_stride, 0, ve->height,
              ve->frame->data, ve->frame->linesize);

    /* 将纳秒时间戳转换为编码器 time_base（{fps_den, fps_num}）单位 */
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
        return 1;  /* 冲洗完毕 */
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
    sws_freeContext(ve->sws_ctx);
    avcodec_free_context(&ve->codec_ctx);
}
