#ifndef PIXIV_H
#define PIXIV_H

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

// decoder state - holds all FFmpeg context
typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    struct SwsContext *sws_ctx;
    AVFrame *frame;
    AVFrame *rgb_frame;
    AVPacket *packet;
    int video_stream_index;
    int width;
    int height;
    double fps;
    unsigned char ***pixel_buffer;  // Reusable buffer to avoid per-frame allocation hell
} VideoDecoder;

// open video file and set up decoder
// NULL on error
VideoDecoder* video_decoder_open(const char *path);

unsigned char*** video_decoder_next_frame(VideoDecoder *decoder);

void video_decoder_close(VideoDecoder *decoder);

#endif
