#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

typedef struct {
    AVCodecContext  *codec_ctx;
    AVFrame         *frame;
    struct SwsContext *sws_ctx;    /* YUYV422 → YUV420P */
    int              width;
    int              height;
    int              fps_num;
    int              fps_den;
} VideoEncoder;

/* 创建 H.264 编码器上下文。oformat_flags 用于判断是否需要 global_header */
int  video_encoder_open(VideoEncoder *ve, int width, int height,
                        int fps_num, int fps_den, int bitrate,
                        int oformat_flags);

/* 编码一帧 YUYV 数据。pts_ns 为纳秒时间戳。
   *pkt 返回编码结果，NULL 表示编码器正在缓冲（需继续喂帧）。
   调用者负责 av_packet_free(pkt)。*/
int  video_encoder_encode(VideoEncoder *ve, const void *yuyv_data,
                          size_t data_size, int64_t pts_ns,
                          AVPacket **pkt);

/* 刷新编码器：调用一次发送 NULL 帧，随后循环调用直到返回非 0（AVERROR_EOF）。
   每次成功返回 0 时 *pkt 持有一个数据包，调用者负责 av_packet_free。*/
int  video_encoder_flush(VideoEncoder *ve, AVPacket **pkt);

void video_encoder_close(VideoEncoder *ve);

#endif /* VIDEO_ENCODER_H */