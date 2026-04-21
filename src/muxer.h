#ifndef MUXER_H
#define MUXER_H

#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct {
    AVFormatContext *fmt_ctx;
    AVStream        *video_stream;
    AVStream        *audio_stream;
    int              video_stream_idx;
    int              audio_stream_idx;
    AVRational       video_enc_tb;   /* 视频编码器 time_base，用于 rescale */
    AVRational       audio_enc_tb;   /* 音频编码器 time_base，用于 rescale */
} Muxer;

/* 创建输出上下文，添加音视频流。
   vctx/actx 必须在 avcodec_open2 之后传入（extradata 已就绪）。*/
int  muxer_open(Muxer *m, const char *output_path,
                const AVCodecContext *vctx,
                const AVCodecContext *actx);

/* 写 MP4 文件头 */
int  muxer_write_header(Muxer *m);

/* 将 pkt 时间戳从编码器 timebase 转换到容器 timebase 后写入。
   stream_idx 取 m->video_stream_idx 或 m->audio_stream_idx。
   函数内调用 av_interleaved_write_frame，完成后 av_packet_unref。*/
int  muxer_write_packet(Muxer *m, AVPacket *pkt, int stream_idx);

/* 写 MP4 trailer（刷出 FFmpeg 内部交错缓冲） */
int  muxer_write_trailer(Muxer *m);

/* 关闭输出文件，释放格式上下文 */
void muxer_close(Muxer *m);

#endif /* MUXER_H */
