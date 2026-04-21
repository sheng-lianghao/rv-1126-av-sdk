#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile int      g_stop     = 0;
static          Pipeline *g_pipeline = NULL;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(void)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    PipelineConfig cfg = {
        .video_device      = "/dev/video0",
        .video_width       = 1280,
        .video_height      = 720,
        .video_fps         = 30,
        .audio_device      = "default",
        .audio_sample_rate = 44100,
        .audio_channels    = 2,
        .video_bitrate     = 2000000,   /* 2 Mbps H.264 */
        .audio_bitrate     = 128000,    /* 128 kbps AAC */
        .output_path       = "output.mp4",
    };

    printf("Initializing pipeline...\n");
    g_pipeline = pipeline_init(&cfg);
    if (!g_pipeline) {
        fprintf(stderr, "pipeline_init failed\n");
        return 1;
    }

    printf("Starting capture. Press Ctrl+C to stop.\n");
    if (pipeline_start(g_pipeline) != 0) {
        fprintf(stderr, "pipeline_start failed\n");
        pipeline_destroy(g_pipeline);
        return 1;
    }

    /* 主循环：等待用户发送 SIGINT */
    while (!g_stop)
        sleep(1);

    printf("\nStopping pipeline, flushing encoders...\n");
    pipeline_stop(g_pipeline);
    pipeline_destroy(g_pipeline);

    printf("Done. Output written to: %s\n", cfg.output_path);
    return 0;
}
