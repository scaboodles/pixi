#define _XOPEN_SOURCE_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <wchar.h>
#include <jpeglib.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/time.h>
#include "pixiv.h"

#define PIXEL(pixels, width, x, y, c) ((pixels)[((y) * (width) + (x)) * 3 + (c)])

// shutdown flag
volatile sig_atomic_t should_exit = 0;

// benchmark flag
int benchmark_enabled = 0;

void handle_sigint(int sig) {
    (void)sig;
    should_exit = 1;
}

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

unsigned char* decode_jpeg(const char *path, int *width, int *height){
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

    unsigned char *pixels = malloc(*width * *height * 3);

    unsigned char *row = malloc(*width * num_channel);
    while(decomp.output_scanline < *height){
        int y = decomp.output_scanline;
        jpeg_read_scanlines(&decomp, &row, 1);
        for(int x = 0; x < *width; x++){
            PIXEL(pixels, *width, x, y, 0) = row[x*num_channel + 0];
            PIXEL(pixels, *width, x, y, 1) = row[x*num_channel + 1];
            PIXEL(pixels, *width, x, y, 2) = row[x*num_channel + 2];
        }
    }

    free(row);
    jpeg_finish_decompress(&decomp);
    jpeg_destroy_decompress(&decomp);
    fclose(f);

    return pixels;
}

void get_terminal_size(int *term_height, int *term_width){
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    *term_height = w.ws_row;
    *term_width = w.ws_col;
}

void calculate_scaled_dimensions(int width, int height, int term_width, int term_height, int *scaled_width, int *scaled_height){
    int available_height = (term_height - 1) * 2;
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

unsigned char* downscale_image(unsigned char *pixels, int width, int height, int scaled_width, int scaled_height){
    unsigned char *downscaled = malloc(scaled_width * scaled_height * 3);

    for(int y = 0; y < scaled_height; y++){
        for(int x = 0; x < scaled_width; x++){
            int src_y = (y * height) / scaled_height;
            int src_x = (x * width) / scaled_width;

            PIXEL(downscaled, scaled_width, x, y, 0) = PIXEL(pixels, width, src_x, src_y, 0);
            PIXEL(downscaled, scaled_width, x, y, 1) = PIXEL(pixels, width, src_x, src_y, 1);
            PIXEL(downscaled, scaled_width, x, y, 2) = PIXEL(pixels, width, src_x, src_y, 2);
        }
    }
    return downscaled;
}

// fast int to string 0-255 no bounds checking
// returns chars written
static inline int fast_u8_to_str(unsigned char val, char *buf) {
    if (val >= 100) {
        buf[0] = '0' + (val / 100);
        buf[1] = '0' + ((val / 10) % 10);
        buf[2] = '0' + (val % 10);
        return 3;
    } else if (val >= 10) {
        buf[0] = '0' + (val / 10);
        buf[1] = '0' + (val % 10);
        return 2;
    } else {
        buf[0] = '0' + val;
        return 1;
    }
}

size_t calculate_frame_buffer_size(int width, int height) {
    // worst case per pixel is both colors change:
    //   "\033[38;2;RRR;GGG;BBB;48;2;RRR;GGG;BBBm▄"
    //   max is 3 + 18 + 18 + 1 + 3 = 43 bytes
    // color state tracking => most pixels skip color codes entirely
    // for each row:
    //   newline 1 byte
    //   "\033[H" (3 bytes) to reset cursor position

    int rows = (height + 1) / 2;
    return 3 + (rows * width * 50) + (rows * 1) + 4;
}

void render_to_terminal_buffered(unsigned char *pixels, int width, int height, char *frame_buffer){
    char *buf = frame_buffer;

    // color state tracking init
    static int last_fg_r = -1, last_fg_g = -1, last_fg_b = -1;
    static int last_bg_r = -1, last_bg_g = -1, last_bg_b = -1;

    // cursor pos reset "\033[H"
    *buf++ = '\033';
    *buf++ = '[';
    *buf++ = 'H';

    // reset state at start of frame **TODO**
    last_fg_r = last_fg_g = last_fg_b = -1;
    last_bg_r = last_bg_g = last_bg_b = -1;

    int total_rows = (height + 1) / 2;
    int current_row = 0;

    for(int y = 0; y < height; y += 2){
        for(int x = 0; x < width; x++){
            unsigned char r_top = PIXEL(pixels, width, x, y, 0);
            unsigned char g_top = PIXEL(pixels, width, x, y, 1);
            unsigned char b_top = PIXEL(pixels, width, x, y, 2);

            unsigned char r_bot, g_bot, b_bot;
            if(y + 1 < height){
                r_bot = PIXEL(pixels, width, x, y+1, 0);
                g_bot = PIXEL(pixels, width, x, y+1, 1);
                b_bot = PIXEL(pixels, width, x, y+1, 2);
            } else {
                r_bot = g_bot = b_bot = 0;
            }

            // compare curr color state to needed
            int fg_changed = (r_bot != last_fg_r || g_bot != last_fg_g || b_bot != last_fg_b);
            int bg_changed = (r_top != last_bg_r || g_top != last_bg_g || b_top != last_bg_b);

            if (fg_changed && bg_changed) {
                *buf++ = '\033';
                *buf++ = '[';
                *buf++ = '3';
                *buf++ = '8';
                *buf++ = ';';
                *buf++ = '2';
                *buf++ = ';';
                buf += fast_u8_to_str(r_bot, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(g_bot, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(b_bot, buf);
                *buf++ = ';';
                *buf++ = '4';
                *buf++ = '8';
                *buf++ = ';';
                *buf++ = '2';
                *buf++ = ';';
                *buf++ = ';';
                buf += fast_u8_to_str(g_top, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(b_top, buf);
                *buf++ = 'm';

                last_fg_r = r_bot;
                last_fg_g = g_bot;
                last_fg_b = b_bot;
                last_bg_r = r_top;
                last_bg_g = g_top;
                last_bg_b = b_top;
            } else if (fg_changed) {
                *buf++ = '\033';
                *buf++ = '[';
                *buf++ = '3';
                *buf++ = '8';
                *buf++ = ';';
                *buf++ = '2';
                *buf++ = ';';
                buf += fast_u8_to_str(r_bot, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(g_bot, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(b_bot, buf);
                *buf++ = 'm';

                last_fg_r = r_bot;
                last_fg_g = g_bot;
                last_fg_b = b_bot;
            } else if (bg_changed) {
                *buf++ = '\033';
                *buf++ = '[';
                *buf++ = '4';
                *buf++ = '8';
                *buf++ = ';';
                *buf++ = '2';
                *buf++ = ';';
                buf += fast_u8_to_str(r_top, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(g_top, buf);
                *buf++ = ';';
                buf += fast_u8_to_str(b_top, buf);
                *buf++ = 'm';

                last_bg_r = r_top;
                last_bg_g = g_top;
                last_bg_b = b_top;
            }

            // half block is 0xE2 0x96 0x84
            *buf++ = 0xE2;
            *buf++ = 0x96;
            *buf++ = 0x84;
        }

        current_row++;
        if (current_row < total_rows) {
            *buf++ = '\n';
        }
    }

    write(STDOUT_FILENO, frame_buffer, buf - frame_buffer);
}

void render_to_terminal(unsigned char *pixels, int width, int height){
    printf("\033[2J\033[H");

    for(int y = 0; y < height; y += 2){
        for(int x = 0; x < width; x++){
            unsigned char r_top = PIXEL(pixels, width, x, y, 0);
            unsigned char g_top = PIXEL(pixels, width, x, y, 1);
            unsigned char b_top = PIXEL(pixels, width, x, y, 2);

            unsigned char r_bot, g_bot, b_bot;
            if(y + 1 < height){
                r_bot = PIXEL(pixels, width, x, y+1, 0);
                g_bot = PIXEL(pixels, width, x, y+1, 1);
                b_bot = PIXEL(pixels, width, x, y+1, 2);
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

unsigned char* allocate_pixel_buffer(int width, int height) {
    return malloc(width * height * 3);
}

void free_pixel_buffer(unsigned char *pixels){
    free(pixels);
}

void image_pipeline(const char * path){
    int width, height;
    unsigned char *pixels = decode_jpeg(path, &width, &height);

    int term_height, term_width;
    get_terminal_size(&term_height, &term_width);

    int scaled_width, scaled_height;
    calculate_scaled_dimensions(width, height, term_width, term_height, &scaled_width, &scaled_height);

    unsigned char *downscaled = downscale_image(pixels, width, height, scaled_width, scaled_height);

    render_to_terminal(downscaled, scaled_width, scaled_height);

    getchar();

    free_pixel_buffer(downscaled);
    free_pixel_buffer(pixels);
}

void video_pipeline(const char * path){
    VideoDecoder *decoder = video_decoder_open(path);
    if (!decoder) {
        fprintf(stderr, "Failed to open video: %s\n", path);
        return;
    }

    int term_height, term_width;
    get_terminal_size(&term_height, &term_width);
    printf("Terminal resolution: %d x %d\n", term_width, 2 * term_height);
    int scaled_width, scaled_height;
    calculate_scaled_dimensions(decoder->width, decoder->height,
                                term_width, term_height,
                                &scaled_width, &scaled_height);

    int frame_delay_us = (int)(1000000.0 / decoder->fps);

    size_t buffer_size = calculate_frame_buffer_size(scaled_width, scaled_height);
    char *frame_buffer = malloc(buffer_size);
    unsigned char *downscaled = allocate_pixel_buffer(scaled_width, scaled_height);

    if (!frame_buffer || !downscaled) {
        fprintf(stderr, "Failed to allocate playback buffers\n");
        if (frame_buffer) free(frame_buffer);
        if (downscaled) free_pixel_buffer(downscaled);
        video_decoder_close(decoder);
        return;
    }

    printf("Starting playback... (Press Ctrl+C to stop)\n");
    sleep(1);

    signal(SIGINT, handle_sigint);

    // alternate screen buffer init
    printf("\033[?1049h");
    printf("\033[?25l");// hide cursor
    printf("\033[2J");// clear
    fflush(stdout);

    // benchmark variables
    struct timeval start_time, end_time;
    long total_time_us = 0;
    int frame_count = 0;

    // playback
    unsigned char *frame;
    while ((frame = video_decoder_next_frame(decoder)) != NULL && !should_exit) {
        if (benchmark_enabled) {
            gettimeofday(&start_time, NULL);
        }

        for(int y = 0; y < scaled_height; y++){
            for(int x = 0; x < scaled_width; x++){
                int src_y = (y * decoder->height) / scaled_height;
                int src_x = (x * decoder->width) / scaled_width;

                PIXEL(downscaled, scaled_width, x, y, 0) = PIXEL(frame, decoder->width, src_x, src_y, 0);
                PIXEL(downscaled, scaled_width, x, y, 1) = PIXEL(frame, decoder->width, src_x, src_y, 1);
                PIXEL(downscaled, scaled_width, x, y, 2) = PIXEL(frame, decoder->width, src_x, src_y, 2);
            }
        }

        render_to_terminal_buffered(downscaled, scaled_width, scaled_height, frame_buffer);

        if (benchmark_enabled) {
            gettimeofday(&end_time, NULL);
            long elapsed_us = (end_time.tv_sec - start_time.tv_sec) * 1000000L +
                             (end_time.tv_usec - start_time.tv_usec);
            total_time_us += elapsed_us;
            frame_count++;
        }

        //usleep(frame_delay_us);
    }

    // cleanup
    free(frame_buffer);
    free_pixel_buffer(downscaled);

    // restore terminal
    printf("\033[?25h"); // show cursor
    printf("\033[?1049l");
    fflush(stdout);

    if (should_exit) {
        printf("Playback interrupted by user.\n");
    } else {
        printf("Playback finished!\n");
    }

    // print benchmark results
    if (benchmark_enabled && frame_count > 0) {
        double avg_time_ms = (double)total_time_us / (double)frame_count / 1000.0;
        double avg_fps = 1000.0 / avg_time_ms;
        printf("\nBenchmark Results:\n");
        printf("  Total frames processed: %d\n", frame_count);
        printf("  Average time per frame: %.3f ms\n", avg_time_ms);
        printf("  Average FPS: %.2f\n", avg_fps);
        printf("  Total processing time: %.3f s\n", (double)total_time_us / 1000000.0);
    }

    video_decoder_close(decoder);
}

int main(int argc, char * args[]){
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [--benchmark] <image_or_video_file>\n", args[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  --benchmark    Enable benchmark mode (video only)\n");
        return 1;
    }

    const char * path = NULL;

    // parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(args[i], "--benchmark") == 0 || strcmp(args[i], "-b") == 0) {
            benchmark_enabled = 1;
        } else {
            path = args[i];
        }
    }

    if (!path) {
        fprintf(stderr, "Error: No file specified\n");
        fprintf(stderr, "Usage: %s [--benchmark] <image_or_video_file>\n", args[0]);
        return 1;
    }

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
