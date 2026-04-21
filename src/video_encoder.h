#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

typedef struct {
    AVCodecContext *codec_ctx;
    AVFrame        *frame;
    int             width;
    int             height;
    int             fps_num;
    int             fps_den;
} VideoEncoder;

int  video_encoder_open(VideoEncoder *ve, int width, int height,
                        int fps_num, int fps_den, int bitrate,
                        int oformat_flags);

int  video_encoder_encode(VideoEncoder *ve, const void *nv12_data,
                          size_t data_size, int64_t pts_ns,
                          AVPacket **pkt);

int  video_encoder_flush(VideoEncoder *ve, AVPacket **pkt);

void video_encoder_close(VideoEncoder *ve);

#endif /* VIDEO_ENCODER_H */
