#include "muxer.h"

#include <stdio.h>
#include <string.h>
#include <libavutil/opt.h>

int muxer_open(Muxer *m, const char *output_path,
               const AVCodecContext *vctx,
               const AVCodecContext *actx)
{
    memset(m, 0, sizeof(*m));

    if (avformat_alloc_output_context2(&m->fmt_ctx, NULL, NULL, output_path) < 0) {
        fprintf(stderr, "avformat_alloc_output_context2 failed\n");
        return -1;
    }

    /* ------ 视频流 ------ */
    m->video_stream = avformat_new_stream(m->fmt_ctx, NULL);
    if (!m->video_stream) return -1;
    m->video_stream_idx  = m->video_stream->index;
    m->video_enc_tb      = vctx->time_base;
    m->video_stream->time_base = vctx->time_base;

    if (avcodec_parameters_from_context(m->video_stream->codecpar, vctx) < 0) {
        fprintf(stderr, "video avcodec_parameters_from_context failed\n");
        return -1;
    }

    /* ------ 音频流 ------ */
    m->audio_stream = avformat_new_stream(m->fmt_ctx, NULL);
    if (!m->audio_stream) return -1;
    m->audio_stream_idx  = m->audio_stream->index;
    m->audio_enc_tb      = actx->time_base;
    m->audio_stream->time_base = actx->time_base;

    if (avcodec_parameters_from_context(m->audio_stream->codecpar, actx) < 0) {
        fprintf(stderr, "audio avcodec_parameters_from_context failed\n");
        return -1;
    }

    /* ------ 打开输出文件 ------ */
    if (!(m->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m->fmt_ctx->pb, output_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "avio_open(%s) failed\n", output_path);
            return -1;
        }
    }

    return 0;
}

int muxer_write_header(Muxer *m)
{
    AVDictionary *opts = NULL;
    /* MP4 fast-start：将 moov atom 移到文件头，便于流式播放 */
    av_dict_set(&opts, "movflags", "faststart", 0);
    int ret = avformat_write_header(m->fmt_ctx, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        fprintf(stderr, "avformat_write_header failed: %d\n", ret);
        return -1;
    }
    return 0;
}

int muxer_write_packet(Muxer *m, AVPacket *pkt, int stream_idx)
{
    AVStream *st = m->fmt_ctx->streams[stream_idx];
    AVRational enc_tb = (stream_idx == m->video_stream_idx)
                        ? m->video_enc_tb
                        : m->audio_enc_tb;

    /* 将时间戳从编码器 timebase 转换为容器流 timebase */
    av_packet_rescale_ts(pkt, enc_tb, st->time_base);
    pkt->stream_index = stream_idx;

    /* av_interleaved_write_frame 负责按 DTS 顺序缓冲后写出 */
    int ret = av_interleaved_write_frame(m->fmt_ctx, pkt);
    if (ret < 0)
        fprintf(stderr, "av_interleaved_write_frame: %d\n", ret);

    /* pkt 的 data 已由 FFmpeg 接管（或已 unref），此处仅释放结构体 */
    av_packet_unref(pkt);
    return ret;
}

int muxer_write_trailer(Muxer *m)
{
    int ret = av_write_trailer(m->fmt_ctx);
    if (ret < 0)
        fprintf(stderr, "av_write_trailer failed: %d\n", ret);
    return ret;
}

void muxer_close(Muxer *m)
{
    if (m->fmt_ctx) {
        if (!(m->fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m->fmt_ctx->pb);
        avformat_free_context(m->fmt_ctx);
        m->fmt_ctx = NULL;
    }
}
