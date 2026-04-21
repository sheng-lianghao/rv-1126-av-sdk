#include "video_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>

/* 从编码器支持列表中挑选像素格式。
 * 若只支持 drm_prime（需要 DMA 缓冲区，普通内存无法直接使用）返回 NONE。
 * 优先 NV12，其次 YUV420P，否则取第一个非 drm_prime 格式。 */
static enum AVPixelFormat pick_pix_fmt(const AVCodec *codec)
{
    if (!codec->pix_fmts)
        return AV_PIX_FMT_YUV420P;

    enum AVPixelFormat fallback = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat *p = codec->pix_fmts;
         *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12)    return AV_PIX_FMT_NV12;
        if (*p == AV_PIX_FMT_YUV420P) { fallback = AV_PIX_FMT_YUV420P; continue; }
        if (*p != AV_PIX_FMT_DRM_PRIME && fallback == AV_PIX_FMT_NONE)
            fallback = *p;
    }
    return fallback;  /* NONE = 只有 drm_prime，需跳过此编码器 */
}

int video_encoder_open(VideoEncoder *ve, int width, int height,
                       int fps_num, int fps_den, int bitrate,
                       int oformat_flags)
{
    memset(ve, 0, sizeof(*ve));
    ve->width   = width;
    ve->height  = height;
    ve->fps_num = fps_num;
    ve->fps_den = fps_den;

    /* 按优先级尝试编码器，跳过只支持 drm_prime 的硬件编码器 */
    static const char *candidates[] = { "h264_rkmpp", "libx264", NULL };
    const AVCodec *codec = NULL;
    for (int i = 0; candidates[i]; i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(candidates[i]);
        if (!c) continue;
        if (pick_pix_fmt(c) == AV_PIX_FMT_NONE) {
            fprintf(stderr, "[info] skip %s (drm_prime only)\n", c->name);
            continue;
        }
        codec = c;
        break;
    }
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H.264 encoder not found\n");
        return -1;
    }
    fprintf(stderr, "[info] video encoder: %s\n", codec->name);

    ve->enc_fmt = pick_pix_fmt(codec);
    fprintf(stderr, "[info] encoder pix_fmt: %s\n",
            av_get_pix_fmt_name(ve->enc_fmt));

    ve->codec_ctx = avcodec_alloc_context3(codec);
    if (!ve->codec_ctx) return -1;

    ve->codec_ctx->width        = width;
    ve->codec_ctx->height       = height;
    ve->codec_ctx->time_base    = (AVRational){fps_den, fps_num};
    ve->codec_ctx->framerate    = (AVRational){fps_num, fps_den};
    ve->codec_ctx->pix_fmt      = ve->enc_fmt;
    ve->codec_ctx->bit_rate     = bitrate;
    ve->codec_ctx->gop_size     = fps_num;
    ve->codec_ctx->max_b_frames = 0;

    if (oformat_flags & AVFMT_GLOBALHEADER)
        ve->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* preset/tune 仅对 libx264 有效 */
    if (strcmp(codec->name, "libx264") == 0) {
        av_opt_set(ve->codec_ctx->priv_data, "preset", "fast",        0);
        av_opt_set(ve->codec_ctx->priv_data, "tune",   "zerolatency", 0);
    }

    if (avcodec_open2(ve->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 (H.264) failed\n");
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }

    /* 若编码器不支持 NV12，建立 NV12→enc_fmt 的 sws 转换 */
    if (ve->enc_fmt != AV_PIX_FMT_NV12) {
        ve->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12,
                                     width, height, ve->enc_fmt,
                                     SWS_BILINEAR, NULL, NULL, NULL);
        if (!ve->sws_ctx) {
            fprintf(stderr, "sws_getContext failed\n");
            avcodec_free_context(&ve->codec_ctx);
            return -1;
        }
    }

    ve->frame = av_frame_alloc();
    if (!ve->frame) {
        if (ve->sws_ctx) sws_freeContext(ve->sws_ctx);
        avcodec_free_context(&ve->codec_ctx);
        return -1;
    }
    ve->frame->format = ve->enc_fmt;
    ve->frame->width  = width;
    ve->frame->height = height;
    if (av_frame_get_buffer(ve->frame, 32) < 0) {
        av_frame_free(&ve->frame);
        if (ve->sws_ctx) sws_freeContext(ve->sws_ctx);
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

    av_frame_make_writable(ve->frame);

    const uint8_t *y  = (const uint8_t *)nv12_data;
    const uint8_t *uv = y + ve->width * ve->height;

    if (ve->enc_fmt == AV_PIX_FMT_NV12) {
        /* 直接拷贝两个平面，无需转换 */
        for (int i = 0; i < ve->height; i++)
            memcpy(ve->frame->data[0] + i * ve->frame->linesize[0],
                   y + i * ve->width, ve->width);
        for (int i = 0; i < ve->height / 2; i++)
            memcpy(ve->frame->data[1] + i * ve->frame->linesize[1],
                   uv + i * ve->width, ve->width);
    } else {
        /* NV12 → enc_fmt（如 YUV420P）通过 sws 转换 */
        const uint8_t *src_data[2]  = { y, uv };
        int            src_stride[2] = { ve->width, ve->width };
        sws_scale(ve->sws_ctx, src_data, src_stride, 0, ve->height,
                  ve->frame->data, ve->frame->linesize);
    }

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
    if (ve->sws_ctx) sws_freeContext(ve->sws_ctx);
    avcodec_free_context(&ve->codec_ctx);
}
