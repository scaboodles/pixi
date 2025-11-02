#define _XOPEN_SOURCE_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ncurses.h>
#include <wchar.h>
#include <jpeglib.h>
#include <ctype.h>
#include "pixiv.h"

wchar_t * lowerH = L"▄";

typedef enum {
    FILE_TYPE_UNKNOWN,
    FILE_TYPE_IMAGE,
    FILE_TYPE_VIDEO
} FileType;

const char *IMAGE_EXTENSIONS[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tiff", ".webp", NULL};
const char *VIDEO_EXTENSIONS[] = {".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv", ".webm", ".m4v", NULL};

FileType detect_file_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return FILE_TYPE_UNKNOWN;
    }

    char lower_ext[32];
    int i = 0;
    while (ext[i] && i < 31) {
        lower_ext[i] = tolower(ext[i]);
        i++;
    }
    lower_ext[i] = '\0';

    // image
    for (int j = 0; IMAGE_EXTENSIONS[j] != NULL; j++) {
        if (strcmp(lower_ext, IMAGE_EXTENSIONS[j]) == 0) {
            return FILE_TYPE_IMAGE;
        }
    }

    // video
    for (int j = 0; VIDEO_EXTENSIONS[j] != NULL; j++) {
        if (strcmp(lower_ext, VIDEO_EXTENSIONS[j]) == 0) {
            return FILE_TYPE_VIDEO;
        }
    }

    return FILE_TYPE_UNKNOWN;
}

unsigned char*** decode_jpeg(const char *path, int *width, int *height){
    FILE * f = fopen(path, "rb");
    if(!f){
        perror("invalid path");
        exit(-1);
    }

    struct jpeg_decompress_struct decomp;
    struct jpeg_error_mgr decomp_err;

    decomp.err = jpeg_std_error(&decomp_err);
    jpeg_create_decompress(&decomp);
    jpeg_stdio_src(&decomp, f);
    jpeg_read_header(&decomp, TRUE);
    jpeg_start_decompress(&decomp);

    *width = decomp.output_width;
    *height = decomp.output_height;
    int num_channel = decomp.output_components;

    unsigned char ***pixels = malloc(*height * sizeof(unsigned char**));
    for(int y = 0; y < *height; y++){
        pixels[y] = malloc(*width * sizeof(unsigned char*));
        for(int x = 0; x < *width; x++){
            pixels[y][x] = malloc(3 * sizeof(unsigned char));
        }
    }

    unsigned char *row = malloc(*width * num_channel);
    while(decomp.output_scanline < *height){
        int y = decomp.output_scanline;
        jpeg_read_scanlines(&decomp, &row, 1);
        for(int x = 0; x < *width; x++){
            pixels[y][x][0] = row[x*num_channel + 0];
            pixels[y][x][1] = row[x*num_channel + 1];
            pixels[y][x][2] = row[x*num_channel + 2];
        }
    }

    free(row);
    jpeg_finish_decompress(&decomp);
    jpeg_destroy_decompress(&decomp);
    fclose(f);

    return pixels;
}

void get_terminal_size(int *term_height, int *term_width){
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    getmaxyx(stdscr, *term_height, *term_width);
    endwin();
}

void calculate_scaled_dimensions(int width, int height, int term_width, int term_height, int *scaled_width, int *scaled_height){
    int available_height = term_height * 2;
    int available_width = term_width;

    float img_aspect = (float)width / (float)height;
    float term_aspect = (float)available_width / (float)available_height;

    if(img_aspect > term_aspect){
        *scaled_width = available_width;
        *scaled_height = (int)(available_width / img_aspect);
    } else {
        *scaled_height = available_height;
        *scaled_width = (int)(available_height * img_aspect);
    }
}

unsigned char*** downscale_image(unsigned char ***pixels, int width, int height, int scaled_width, int scaled_height){
    unsigned char ***downscaled = malloc(scaled_height * sizeof(unsigned char**));
    for(int y = 0; y < scaled_height; y++){
        downscaled[y] = malloc(scaled_width * sizeof(unsigned char*));
        for(int x = 0; x < scaled_width; x++){
            downscaled[y][x] = malloc(3 * sizeof(unsigned char));

            int src_y = (y * height) / scaled_height;
            int src_x = (x * width) / scaled_width;

            downscaled[y][x][0] = pixels[src_y][src_x][0];
            downscaled[y][x][1] = pixels[src_y][src_x][1];
            downscaled[y][x][2] = pixels[src_y][src_x][2];
        }
    }
    return downscaled;
}

void render_to_terminal_buffered(unsigned char ***pixels, int width, int height){
    size_t buffer_size = width * (height / 2) * 60 + 1000;
    char *frame_buffer = malloc(buffer_size);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        return;
    }

    int offset = 0;

    offset += snprintf(frame_buffer + offset, buffer_size - offset, "\033[H");

    for(int y = 0; y < height; y += 2){
        for(int x = 0; x < width; x++){
            unsigned char r_top = pixels[y][x][0];
            unsigned char g_top = pixels[y][x][1];
            unsigned char b_top = pixels[y][x][2];

            unsigned char r_bot, g_bot, b_bot;
            if(y + 1 < height){
                r_bot = pixels[y+1][x][0];
                g_bot = pixels[y+1][x][1];
                b_bot = pixels[y+1][x][2];
            } else {
                r_bot = g_bot = b_bot = 0;
            }

            offset += snprintf(frame_buffer + offset, buffer_size - offset,
                             "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm▄",
                             r_bot, g_bot, b_bot, r_top, g_top, b_top);
        }
        offset += snprintf(frame_buffer + offset, buffer_size - offset, "\033[0m\n");
    }

    offset += snprintf(frame_buffer + offset, buffer_size - offset, "\033[0m");

    write(STDOUT_FILENO, frame_buffer, offset);

    free(frame_buffer);
}

void render_to_terminal(unsigned char ***pixels, int width, int height){
    printf("\033[2J\033[H");

    for(int y = 0; y < height; y += 2){
        for(int x = 0; x < width; x++){
            unsigned char r_top = pixels[y][x][0];
            unsigned char g_top = pixels[y][x][1];
            unsigned char b_top = pixels[y][x][2];

            unsigned char r_bot, g_bot, b_bot;
            if(y + 1 < height){
                r_bot = pixels[y+1][x][0];
                g_bot = pixels[y+1][x][1];
                b_bot = pixels[y+1][x][2];
            } else {
                r_bot = g_bot = b_bot = 0;
            }

            printf("\033[38;2;%d;%d;%dm", r_bot, g_bot, b_bot);
            printf("\033[48;2;%d;%d;%dm", r_top, g_top, b_top);
            printf("%s", "▄");
        }
        printf("\033[0m\n");
    }

    printf("\033[0m");
    fflush(stdout);
}

void free_pixel_buffer(unsigned char ***pixels, int width, int height){
    for(int y = 0; y < height; y++){
        for(int x = 0; x < width; x++){
            free(pixels[y][x]);
        }
        free(pixels[y]);
    }
    free(pixels);
}

void image_pipeline(const char * path){
    int width, height;
    unsigned char ***pixels = decode_jpeg(path, &width, &height);

    int term_height, term_width;
    get_terminal_size(&term_height, &term_width);

    int scaled_width, scaled_height;
    calculate_scaled_dimensions(width, height, term_width, term_height, &scaled_width, &scaled_height);

    unsigned char ***downscaled = downscale_image(pixels, width, height, scaled_width, scaled_height);

    render_to_terminal(downscaled, scaled_width, scaled_height);

    getchar();

    free_pixel_buffer(downscaled, scaled_width, scaled_height);
    free_pixel_buffer(pixels, width, height);
}

void video_pipeline(const char * path){
    VideoDecoder *decoder = video_decoder_open(path);
    if (!decoder) {
        fprintf(stderr, "Failed to open video: %s\n", path);
        return;
    }

    int term_height, term_width;
    get_terminal_size(&term_height, &term_width);

    int scaled_width, scaled_height;
    calculate_scaled_dimensions(decoder->width, decoder->height,
                                term_width, term_height,
                                &scaled_width, &scaled_height);

    int frame_delay_us = (int)(1000000.0 / decoder->fps);

    printf("Starting playback... (Press Ctrl+C to stop)\n");
    sleep(1);

    printf("\033[?1049h");
    printf("\033[?25l");
    printf("\033[2J");
    fflush(stdout);

    unsigned char ***frame;
    while ((frame = video_decoder_next_frame(decoder)) != NULL) {
        unsigned char ***downscaled = downscale_image(frame,
                                                      decoder->width, decoder->height,
                                                      scaled_width, scaled_height);

        render_to_terminal_buffered(downscaled, scaled_width, scaled_height);

        free_pixel_buffer(downscaled, scaled_width, scaled_height);
        free_pixel_buffer(frame, decoder->width, decoder->height);

        usleep(frame_delay_us);
    }

    printf("\033[?25h");
    printf("\033[?1049l");
    fflush(stdout);

    printf("Playback finished!\n");

    video_decoder_close(decoder);
}

int main(int argc, char * args[]){
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image_or_video_file>\n", args[0]);
        return 1;
    }

    const char * path = args[1];

    FileType file_type = detect_file_type(path);

    switch (file_type) {
        case FILE_TYPE_IMAGE:
            image_pipeline(path);
            break;
        case FILE_TYPE_VIDEO:
            video_pipeline(path);
            break;
        case FILE_TYPE_UNKNOWN:
            fprintf(stderr, "Unknown file type: %s\n", path);
            fprintf(stderr, "Supported image formats: jpg, jpeg, png, gif, bmp, tiff, webp\n");
            fprintf(stderr, "Supported video formats: mp4, avi, mkv, mov, wmv, flv, webm, m4v\n");
            return 1;
    }

    return 0;
}
