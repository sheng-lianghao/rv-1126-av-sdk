#include "audio_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int audio_capture_open(AudioCapture *ac, const char *device,
                       unsigned int sample_rate, unsigned int channels)
{
    memset(ac, 0, sizeof(*ac));
    snprintf(ac->device, sizeof(ac->device), "%s", device);
    ac->sample_rate = sample_rate;
    ac->channels    = channels;

    int err;
    if ((err = snd_pcm_open(&ac->handle, device,
                            SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(ac->handle, params);

    snd_pcm_hw_params_set_access(ac->handle, params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ac->handle, params,
                                 SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(ac->handle, params, channels);

    unsigned int rate = sample_rate;
    snd_pcm_hw_params_set_rate_near(ac->handle, params, &rate, NULL);

    /* 协商 period_size（以帧为单位） */
    ac->period_size = 1024;
    snd_pcm_hw_params_set_period_size_near(ac->handle, params,
                                           &ac->period_size, NULL);

    snd_pcm_uframes_t buffer_size = ac->period_size * 4;
    snd_pcm_hw_params_set_buffer_size_near(ac->handle, params, &buffer_size);

    if ((err = snd_pcm_hw_params(ac->handle, params)) < 0) {
        fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(ac->handle);
        return -1;
    }

    /* 读回实际协商值 */
    snd_pcm_hw_params_get_period_size(params, &ac->period_size, NULL);

    ac->capture_buf = malloc(ac->period_size * channels * sizeof(int16_t));
    if (!ac->capture_buf) {
        snd_pcm_close(ac->handle);
        return -1;
    }

    snd_pcm_prepare(ac->handle);
    return 0;
}

int audio_capture_start(AudioCapture *ac)
{
    int err = snd_pcm_start(ac->handle);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_start: %s\n", snd_strerror(err));
        return -1;
    }
    return 0;
}

int audio_capture_grab(AudioCapture *ac, void **pcm_data,
                       size_t *pcm_size, int64_t *pts_ns)
{
    snd_pcm_sframes_t frames =
        snd_pcm_readi(ac->handle, ac->capture_buf, ac->period_size);

    if (frames == -EPIPE) {
        /* 缓冲区溢出，恢复后继续（不重置 total_frames，保持同步） */
        fprintf(stderr, "audio capture: overrun, recovering\n");
        snd_pcm_prepare(ac->handle);
        return -EAGAIN;
    }
    if (frames < 0) {
        frames = snd_pcm_recover(ac->handle, (int)frames, 0);
        if (frames < 0) {
            fprintf(stderr, "snd_pcm_recover: %s\n", snd_strerror((int)frames));
            return -1;
        }
    }

    ac->total_frames += (uint64_t)frames;
    *pts_ns = (int64_t)(ac->total_frames * 1000000000ULL / ac->sample_rate);

    size_t sz = (size_t)frames * ac->channels * sizeof(int16_t);
    *pcm_size = sz;
    *pcm_data = malloc(sz);
    if (!*pcm_data)
        return -1;
    memcpy(*pcm_data, ac->capture_buf, sz);
    return 0;
}

int audio_capture_stop(AudioCapture *ac)
{
    snd_pcm_drop(ac->handle);
    return 0;
}

void audio_capture_close(AudioCapture *ac)
{
    free(ac->capture_buf);
    ac->capture_buf = NULL;
    if (ac->handle) {
        snd_pcm_close(ac->handle);
        ac->handle = NULL;
    }
}
