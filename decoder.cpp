#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>


extern "C" {
#define __STDC_CONSTANT_MACROS
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavdevice/avdevice.h"
}

#include "mylog.h"

static void usage(char *prog)
{
    fprintf(stderr, "Usage: %s mp3_file.mp3\n", prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    struct stat sb;
    if (stat("log", &sb) == -1) {
        if (mkdir("log", 0755) < 0) {
            fprintf(stderr, "mkdir log failed\n");
            return 1;
        }
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFDIR) {
            if (unlink("log") != 0) {
                fprintf(stderr, "I need ./log to be a directory, but it does not, and rm failed");
                return 1;
            }
        }
    }

    my_log_init(".", "log/decoder.log", "log/decoder.log.we", 16);

    if (stat(argv[1], &sb)) {
        LOG_ERROR("[main] stat failed: %s", strerror(errno));
        my_log_close();
        return 1;
    }

    if ((sb.st_mode & S_IFMT) != S_IFREG ||
            sb.st_size == 0) {
        LOG_ERROR("[main] %s is not a regular file", argv[1]);
        my_log_close();
        return 1;
    }

    av_register_all();
    avcodec_register_all();
    avdevice_register_all();
    avformat_network_init();

    int ret = 0;
    FILE *fd;
    int got_frame;
    size_t unpadded_linesize;
    static AVFrame *frame = NULL;
    static AVCodec *dec = NULL;
    static AVFormatContext *fmt_ctx = NULL;
    static AVDictionary *opt = NULL;

    ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("[main] open input failed: %s", av_err2str(ret));
        ret = 1;
        goto end;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("[main] find stream info failed: %s", av_err2str(ret));
        ret = 1;
        goto end;
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG_ERROR("[main] find stream failed: %s", av_err2str(ret));
        ret = 1;
        goto end;
    }

    dec = avcodec_find_decoder(fmt_ctx->streams[0]->codec->codec_id);
    if (!dec) {
        LOG_ERROR("[main] find decoder failed: %s", av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        ret = 1;
        goto end;
    }

    ret = avcodec_open2(fmt_ctx->streams[0]->codec, dec, &opt);
    if (ret < 0) {
        LOG_ERROR("[main] open codec failed: %s", av_err2str(ret));
        ret = 1;
        goto end;
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("[main] alloc frame failed: %s", av_err2str(ret));
        ret = 1;
        goto end;
    }

    fd = fopen("out.pcm", "wb");
    if (fd < 0) {
        LOG_ERROR("[main] open file failed: %s", strerror(errno));
        ret = 1;
        goto end;
    }

    got_frame = 0;
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        do {
            ret = avcodec_decode_audio4(fmt_ctx->streams[0]->codec, frame, &got_frame, &pkt);
            if (ret < 0) {
                LOG_ERROR("[main] Error decoding autio: %s", av_err2str(ret));
                break;
            }
            unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
            int i;
            unsigned char *channel_l = frame->data[0];
            unsigned char *channel_r = frame->data[1];
            unsigned char *buf = (unsigned char*)malloc(unpadded_linesize*2);
            if (!buf) {
                LOG_ERROR("[main] malloc failed");
                ret = 1;
                goto end;
            }
            for (i = 0; i < frame->nb_samples; i++) {
                buf[4*i] = channel_l[2*i];
                buf[4*i+1] = channel_l[2*i+1];
                buf[4*i+2] = channel_r[2*i];
                buf[4*i+3] = channel_r[2*i+1];
            }
            fwrite(buf, 1, unpadded_linesize*2, fd);
            free(buf);
            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
    }

    pkt.data = NULL;
    pkt.size = 0;
    do {
        ret = avcodec_decode_audio4(fmt_ctx->streams[0]->codec, frame, &got_frame, &pkt);
        if (ret < 0) {
            LOG_ERROR("[main] Error decoding autio(flushing cache): %s", av_err2str(ret));
            break;
        }
        unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
        int i;
        for (i = 0; i < frame->channels; i++) {
            fwrite(frame->data[i], 1, frame->linesize[0], fd);
        }
    } while (got_frame);

    fclose(fd);

end:
    if (frame) {
        av_frame_free(&frame);
    }
    avcodec_close(fmt_ctx->streams[0]->codec);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();

    my_log_close();
    return ret;
}
