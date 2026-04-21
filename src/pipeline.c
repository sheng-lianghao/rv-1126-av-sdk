#include "pipeline.h"
#include "safe_queue.h"
#include "video_capture.h"
#include "audio_capture.h"
#include "video_encoder.h"
#include "audio_encoder.h"
#include "muxer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <libavutil/mathematics.h>

/* ------------------------------------------------------------------ */
/* 内部完整结构体                                                       */
/* ------------------------------------------------------------------ */
struct Pipeline {
    PipelineConfig cfg;

    VideoCapture vcap;
    AudioCapture acap;
    VideoEncoder venc;
    AudioEncoder aenc;
    Muxer        mux;

    SafeQueue raw_video_q;   /* vcap  → venc */
    SafeQueue raw_audio_q;   /* acap  → aenc */
    SafeQueue pkt_video_q;   /* venc  → muxer */
    SafeQueue pkt_audio_q;   /* aenc  → muxer */

    pthread_t t_vcap;
    pthread_t t_acap;
    pthread_t t_venc;
    pthread_t t_aenc;
    pthread_t t_mux;

    volatile int running;
};

/* ------------------------------------------------------------------ */
/* 线程：视频采集                                                       */
/* ------------------------------------------------------------------ */
static void *thread_video_capture(void *arg)
{
    Pipeline *p = (Pipeline *)arg;
    while (p->running) {
        void   *data = NULL;
        size_t  sz   = 0;
        int64_t pts  = 0;
        int ret = video_capture_grab(&p->vcap, &data, &sz, &pts);
        if (ret == -EAGAIN) continue;
        if (ret < 0) break;

        if (safe_queue_push(&p->raw_video_q, data, sz, pts, pts) != 0)
            free(data);   /* 队列已关闭 */
    }
    safe_queue_signal_eof(&p->raw_video_q);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 线程：音频采集                                                       */
/* ------------------------------------------------------------------ */
static void *thread_audio_capture(void *arg)
{
    Pipeline *p = (Pipeline *)arg;
    while (p->running) {
        void   *data = NULL;
        size_t  sz   = 0;
        int64_t pts  = 0;
        int ret = audio_capture_grab(&p->acap, &data, &sz, &pts);
        if (ret == -EAGAIN) continue;
        if (ret < 0) break;

        if (safe_queue_push(&p->raw_audio_q, data, sz, pts, pts) != 0)
            free(data);
    }
    safe_queue_signal_eof(&p->raw_audio_q);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 线程：视频编码                                                       */
/* ------------------------------------------------------------------ */
static void *thread_video_encode(void *arg)
{
    Pipeline *p = (Pipeline *)arg;
    QueueItem item;

    while (safe_queue_pop(&p->raw_video_q, &item) == 0) {
        AVPacket *pkt = NULL;
        video_encoder_encode(&p->venc, item.data, item.size, item.pts, &pkt);
        free(item.data);

        if (pkt) {
            if (safe_queue_push(&p->pkt_video_q, pkt, 0, pkt->pts, pkt->dts) != 0)
                av_packet_free(&pkt);
        }
    }

    /* 刷新编码器 */
    AVPacket *pkt = NULL;
    while (video_encoder_flush(&p->venc, &pkt) == 0) {
        if (pkt) {
            if (safe_queue_push(&p->pkt_video_q, pkt, 0, pkt->pts, pkt->dts) != 0)
                av_packet_free(&pkt);
        }
    }
    safe_queue_signal_eof(&p->pkt_video_q);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* audio_encoder_encode 的回调：将 AVPacket 推入队列                    */
/* ------------------------------------------------------------------ */
static void on_audio_packet(AVPacket *pkt, void *ctx)
{
    Pipeline *p = (Pipeline *)ctx;
    if (safe_queue_push(&p->pkt_audio_q, pkt, 0, pkt->pts, pkt->dts) != 0)
        av_packet_free(&pkt);
}

/* ------------------------------------------------------------------ */
/* 线程：音频编码                                                       */
/* ------------------------------------------------------------------ */
static void *thread_audio_encode(void *arg)
{
    Pipeline *p = (Pipeline *)arg;
    QueueItem item;

    while (safe_queue_pop(&p->raw_audio_q, &item) == 0) {
        audio_encoder_encode(&p->aenc, item.data, item.size,
                             on_audio_packet, p);
        free(item.data);
    }

    /* 刷新编码器 */
    AVPacket *pkt = NULL;
    while (audio_encoder_flush(&p->aenc, &pkt) == 0) {
        if (pkt) {
            if (safe_queue_push(&p->pkt_audio_q, pkt, 0, pkt->pts, pkt->dts) != 0)
                av_packet_free(&pkt);
        }
    }
    safe_queue_signal_eof(&p->pkt_audio_q);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 线程：封装（基于 PTS 交错写入）                                      */
/* ------------------------------------------------------------------ */
static void *thread_mux(void *arg)
{
    Pipeline *p = (Pipeline *)arg;

    int v_eof = 0;   /* pkt_video_q 已耗尽 */
    int a_eof = 0;   /* pkt_audio_q 已耗尽 */

    while (!v_eof || !a_eof) {
        int64_t v_pts = INT64_MAX;
        int64_t a_pts = INT64_MAX;
        int     v_ret = 1, a_ret = 1;

        if (!v_eof)
            v_ret = safe_queue_peek(&p->pkt_video_q, &v_pts);
        if (!a_eof)
            a_ret = safe_queue_peek(&p->pkt_audio_q, &a_pts);

        /* 如果两个队列当前均没有可用数据（但尚未 eof），稍等后重试 */
        if (v_pts == INT64_MAX && a_pts == INT64_MAX) {
            /* 检查是否真的 eof */
            if (v_ret == 1) v_eof = 1;
            if (a_ret == 1) a_eof = 1;
            if (!v_eof || !a_eof)
                usleep(1000);  /* 1 ms 等待新数据 */
            continue;
        }

        /* 选出 PTS 较小的那个流进行写入 */
        int write_video;
        if (v_pts == INT64_MAX)
            write_video = 0;
        else if (a_pts == INT64_MAX)
            write_video = 1;
        else {
            /* av_compare_ts 做跨 timebase 比较 */
            int cmp = av_compare_ts(v_pts, p->venc.codec_ctx->time_base,
                                    a_pts, p->aenc.codec_ctx->time_base);
            write_video = (cmp <= 0);
        }

        QueueItem item;
        if (write_video) {
            if (safe_queue_pop(&p->pkt_video_q, &item) == 0) {
                AVPacket *pkt = (AVPacket *)item.data;
                muxer_write_packet(&p->mux, pkt, p->mux.video_stream_idx);
                av_packet_free(&pkt);
            } else {
                v_eof = 1;
            }
        } else {
            if (safe_queue_pop(&p->pkt_audio_q, &item) == 0) {
                AVPacket *pkt = (AVPacket *)item.data;
                muxer_write_packet(&p->mux, pkt, p->mux.audio_stream_idx);
                av_packet_free(&pkt);
            } else {
                a_eof = 1;
            }
        }
    }

    muxer_write_trailer(&p->mux);
    return NULL;
}

/* ================================================================== */
/* 公共 API                                                             */
/* ================================================================== */

Pipeline *pipeline_init(const PipelineConfig *cfg)
{
    Pipeline *p = (Pipeline *)calloc(1, sizeof(Pipeline));
    if (!p) return NULL;
    p->cfg = *cfg;

    /* 初始化四个队列 */
    safe_queue_init(&p->raw_video_q);
    safe_queue_init(&p->raw_audio_q);
    safe_queue_init(&p->pkt_video_q);
    safe_queue_init(&p->pkt_audio_q);

    /* 打开视频采集 */
    if (video_capture_open(&p->vcap, cfg->video_device,
                           cfg->video_width, cfg->video_height,
                           cfg->video_fps, 1) < 0) {
        fprintf(stderr, "video_capture_open failed\n");
        goto err;
    }

    /* 打开音频采集 */
    if (audio_capture_open(&p->acap, cfg->audio_device,
                           cfg->audio_sample_rate,
                           cfg->audio_channels) < 0) {
        fprintf(stderr, "audio_capture_open failed\n");
        goto err;
    }

    /* 先临时创建 muxer 格式上下文，以获取 oformat_flags */
    AVFormatContext *tmp_ctx = NULL;
    avformat_alloc_output_context2(&tmp_ctx, NULL, NULL, cfg->output_path);
    int oformat_flags = tmp_ctx ? tmp_ctx->oformat->flags : 0;
    if (tmp_ctx) avformat_free_context(tmp_ctx);

    /* 打开视频编码器 */
    if (video_encoder_open(&p->venc,
                           (int)cfg->video_width, (int)cfg->video_height,
                           (int)cfg->video_fps, 1,
                           cfg->video_bitrate, oformat_flags) < 0) {
        fprintf(stderr, "video_encoder_open failed\n");
        goto err;
    }

    /* 打开音频编码器 */
    if (audio_encoder_open(&p->aenc,
                           (int)cfg->audio_sample_rate,
                           (int)cfg->audio_channels,
                           cfg->audio_bitrate) < 0) {
        fprintf(stderr, "audio_encoder_open failed\n");
        goto err;
    }

    /* 打开封装器（需在编码器 open 之后，extradata 已就绪） */
    if (muxer_open(&p->mux, cfg->output_path,
                   p->venc.codec_ctx, p->aenc.codec_ctx) < 0) {
        fprintf(stderr, "muxer_open failed\n");
        goto err;
    }

    if (muxer_write_header(&p->mux) < 0) {
        fprintf(stderr, "muxer_write_header failed\n");
        goto err;
    }

    return p;

err:
    pipeline_destroy(p);
    return NULL;
}

int pipeline_start(Pipeline *p)
{
    p->running = 1;

    if (video_capture_start(&p->vcap) < 0) return -1;
    if (audio_capture_start(&p->acap) < 0) return -1;

    pthread_create(&p->t_vcap, NULL, thread_video_capture, p);
    pthread_create(&p->t_acap, NULL, thread_audio_capture, p);
    pthread_create(&p->t_venc, NULL, thread_video_encode,  p);
    pthread_create(&p->t_aenc, NULL, thread_audio_encode,  p);
    pthread_create(&p->t_mux,  NULL, thread_mux,           p);
    return 0;
}

void pipeline_stop(Pipeline *p)
{
    /* 1. 通知所有线程退出采集循环 */
    p->running = 0;

    /* 2. 等待采集线程退出，它们内部会 signal_eof */
    pthread_join(p->t_vcap, NULL);
    video_capture_stop(&p->vcap);
    video_capture_close(&p->vcap);

    pthread_join(p->t_acap, NULL);
    audio_capture_stop(&p->acap);
    audio_capture_close(&p->acap);

    /* 3. 等待编码线程完成（含编码器冲洗），它们内部会 signal_eof packet 队列 */
    pthread_join(p->t_venc, NULL);
    pthread_join(p->t_aenc, NULL);

    /* 4. 等待封装线程写完 trailer */
    pthread_join(p->t_mux, NULL);

    muxer_close(&p->mux);
}

void pipeline_destroy(Pipeline *p)
{
    if (!p) return;

    video_encoder_close(&p->venc);
    audio_encoder_close(&p->aenc);

    safe_queue_destroy(&p->raw_video_q);
    safe_queue_destroy(&p->raw_audio_q);
    safe_queue_destroy(&p->pkt_video_q);
    safe_queue_destroy(&p->pkt_audio_q);

    free(p);
}
