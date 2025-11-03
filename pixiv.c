#include <stdio.h>
#include <stdlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "pixiv.h"

VideoDecoder* video_decoder_open(const char *path) {
    VideoDecoder *decoder = malloc(sizeof(VideoDecoder));
    if (!decoder) {
        fprintf(stderr, "Failed to allocate decoder\n");
        return NULL;
    }
    decoder->format_ctx = NULL;
    decoder->codec_ctx = NULL;
    decoder->sws_ctx = NULL;
    decoder->frame = NULL;
    decoder->rgb_frame = NULL;
    decoder->packet = NULL;
    decoder->pixel_buffer = NULL;
    decoder->video_stream_index = -1;

    int ret;

    // open the video file
    ret = avformat_open_input(&decoder->format_ctx, path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open video file: %s\n", path);
        video_decoder_close(decoder);
        return NULL;
    }

    // read stream 
    ret = avformat_find_stream_info(decoder->format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream info\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // find video stream
    const AVCodec *codec = NULL;
    AVCodecParameters *codec_params = NULL;

    for (int i = 0; i < decoder->format_ctx->nb_streams; i++) {
        AVCodecParameters *local_params = decoder->format_ctx->streams[i]->codecpar;

        if (local_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            decoder->video_stream_index = i;
            codec_params = local_params;
            codec = avcodec_find_decoder(local_params->codec_id);

            if (!codec) {
                fprintf(stderr, "Unsupported codec\n");
                video_decoder_close(decoder);
                return NULL;
            }

            // video dimensions
            decoder->width = codec_params->width;
            decoder->height = codec_params->height;

            // frame rate
            AVRational frame_rate = decoder->format_ctx->streams[i]->avg_frame_rate;
            decoder->fps = (double)frame_rate.num / (double)frame_rate.den;

            break;
        }
    }

    if (decoder->video_stream_index == -1) {
        fprintf(stderr, "No video stream found\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // decoder setup
    decoder->codec_ctx = avcodec_alloc_context3(codec);
    if (!decoder->codec_ctx) {
        fprintf(stderr, "Could not allocate codec context\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // copy params
    ret = avcodec_parameters_to_context(decoder->codec_ctx, codec_params);
    if (ret < 0) {
        fprintf(stderr, "Could not copy codec params\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // open the codec
    ret = avcodec_open2(decoder->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // alloc decoded frames
    decoder->frame = av_frame_alloc();
    if (!decoder->frame) {
        fprintf(stderr, "Could not allocate frame\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // RGB conversion
    decoder->rgb_frame = av_frame_alloc();
    if (!decoder->rgb_frame) {
        fprintf(stderr, "Could not allocate RGB frame\n");
        video_decoder_close(decoder);
        return NULL;
    }

    decoder->rgb_frame->format = AV_PIX_FMT_RGB24;
    decoder->rgb_frame->width = decoder->width;
    decoder->rgb_frame->height = decoder->height;

    ret = av_frame_get_buffer(decoder->rgb_frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate RGB frame buffer\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // color space conversion
    decoder->sws_ctx = sws_getContext(
        decoder->width, decoder->height, decoder->codec_ctx->pix_fmt,  // Source
        decoder->width, decoder->height, AV_PIX_FMT_RGB24,             // Destination
        SWS_BILINEAR, NULL, NULL, NULL
    );

    if (!decoder->sws_ctx) {
        fprintf(stderr, "Could not create scaler context\n");
        video_decoder_close(decoder);
        return NULL;
    }

    // data is compressed
    decoder->packet = av_packet_alloc();
    if (!decoder->packet) {
        fprintf(stderr, "Could not allocate packet\n");
        video_decoder_close(decoder);
        return NULL;
    }

    decoder->pixel_buffer = malloc(decoder->height * sizeof(unsigned char**));
    if (!decoder->pixel_buffer) {
        fprintf(stderr, "Could not allocate pixel buffer\n");
        video_decoder_close(decoder);
        return NULL;
    }

    for (int y = 0; y < decoder->height; y++) {
        decoder->pixel_buffer[y] = malloc(decoder->width * sizeof(unsigned char*));
        if (!decoder->pixel_buffer[y]) {
            fprintf(stderr, "Could not allocate pixel buffer row %d\n", y);
            // Free already allocated rows
            for (int cy = 0; cy < y; cy++) {
                for (int cx = 0; cx < decoder->width; cx++) {
                    free(decoder->pixel_buffer[cy][cx]);
                }
                free(decoder->pixel_buffer[cy]);
            }
            free(decoder->pixel_buffer);
            decoder->pixel_buffer = NULL;
            video_decoder_close(decoder);
            return NULL;
        }

        for (int x = 0; x < decoder->width; x++) {
            decoder->pixel_buffer[y][x] = malloc(3 * sizeof(unsigned char));
            if (!decoder->pixel_buffer[y][x]) {
                fprintf(stderr, "Could not allocate pixel at (%d, %d)\n", x, y);
                for (int cx = 0; cx < x; cx++) {
                    free(decoder->pixel_buffer[y][cx]);
                }
                for (int cy = 0; cy < y; cy++) {
                    for (int cx = 0; cx < decoder->width; cx++) {
                        free(decoder->pixel_buffer[cy][cx]);
                    }
                    free(decoder->pixel_buffer[cy]);
                }
                free(decoder->pixel_buffer[y]);
                free(decoder->pixel_buffer);
                decoder->pixel_buffer = NULL;
                video_decoder_close(decoder);
                return NULL;
            }
        }
    }

    printf("  Resolution: %dx%d\n", decoder->width, decoder->height);
    printf("  FPS: %.2f\n", decoder->fps);
    printf("  Codec: %s\n", codec->name);

    return decoder;
}

void video_decoder_close(VideoDecoder *decoder) {
    if (!decoder) return;

    if (decoder->packet) {
        av_packet_free(&decoder->packet);
    }
    if (decoder->pixel_buffer) {
        for (int y = 0; y < decoder->height; y++) {
            for (int x = 0; x < decoder->width; x++) {
                free(decoder->pixel_buffer[y][x]);
            }
            free(decoder->pixel_buffer[y]);
        }
        free(decoder->pixel_buffer);
    }
    if (decoder->sws_ctx) {
        sws_freeContext(decoder->sws_ctx);
    }
    if (decoder->rgb_frame) {
        av_frame_free(&decoder->rgb_frame);
    }
    if (decoder->frame) {
        av_frame_free(&decoder->frame);
    }
    if (decoder->codec_ctx) {
        avcodec_free_context(&decoder->codec_ctx);
    }
    if (decoder->format_ctx) {
        avformat_close_input(&decoder->format_ctx);
    }

    free(decoder);
}

unsigned char*** video_decoder_next_frame(VideoDecoder *decoder) {
    if (!decoder) return NULL;

    int ret;

    // packet might be audio, subtitles, or video
    // we only care about video rn
    while (av_read_frame(decoder->format_ctx, decoder->packet) >= 0) {
        // video packet
        if (decoder->packet->stream_index == decoder->video_stream_index) {

            // send packet to decoder
            ret = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                av_packet_unref(decoder->packet);
                return NULL;
            }

            ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
            if (ret == AVERROR(EAGAIN)) {
                // Need more packets to produce a frame
                av_packet_unref(decoder->packet);
                continue;
            } else if (ret == AVERROR_EOF) {
                // End of video
                av_packet_unref(decoder->packet);
                return NULL;
            } else if (ret < 0) {
                fprintf(stderr, "Error receiving frame from decoder\n");
                av_packet_unref(decoder->packet);
                return NULL;
            }

            av_packet_unref(decoder->packet);

            sws_scale(
                decoder->sws_ctx,
                (const uint8_t * const*)decoder->frame->data,
                decoder->frame->linesize,
                0,
                decoder->height,
                decoder->rgb_frame->data,
                decoder->rgb_frame->linesize
            );

            unsigned char *rgb_data = decoder->rgb_frame->data[0];
            int linesize = decoder->rgb_frame->linesize[0];

            for (int y = 0; y < decoder->height; y++) {
                for (int x = 0; x < decoder->width; x++) {
                    int idx = y * linesize + x * 3;
                    decoder->pixel_buffer[y][x][0] = rgb_data[idx + 0];  // R
                    decoder->pixel_buffer[y][x][1] = rgb_data[idx + 1];  // G
                    decoder->pixel_buffer[y][x][2] = rgb_data[idx + 2];  // B
                }
            }

            return decoder->pixel_buffer;
        }

        // not video packet
        av_packet_unref(decoder->packet);
    }

    // eof
    return NULL;
} 
