#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

extern "C" {
#define __STDC_CONSTANT_MACROS
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avio.h"
}
#include "json/json.h"
#include "evhttp.h"
#include "mylog.h"

#include <vector>

struct ch_info {
    int pid;
    std::string stream;
};

static std::vector<struct ch_info> _s_ch_proc;
static AVFormatContext *_s_fmt_ctx = NULL;

void *decoding(void *type)
{
    int ret = 0;

    if (!type) {
        return NULL;
    }

    enum AVMediaType media_type = *(enum AVMediaType*)type;

    ret = av_find_best_stream(_s_fmt_ctx, media_type, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG_WARN("[decoding] Error finding stream with type of %s in file %s: %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
        return NULL;
    } else if (ret == AVERROR_STREAM_NOT_FOUND) {
        LOG_WARN("[decoding] not found stream with type of %s in file %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename);
        return NULL;
    }

    LOG_DEBUG("[decoding] thread for #%d stream of %s", ret, _s_fmt_ctx->filename);

    AVStream *stream = NULL;
    stream = _s_fmt_ctx->streams[ret];

    AVCodec *dec = NULL;
    dec = avcodec_find_decoder(stream->codec->codec_id);
    if (!dec) {
        LOG_ERROR("[decoding] Error finding decoder: %s, with type of %s, for file %s",
                avcodec_get_name(stream->codec->codec_id),
                av_get_media_type_string(media_type), _s_fmt_ctx->filename);
        return NULL;
    }

    AVDictionary *opt = NULL;
    ret = avcodec_open2(stream->codec, dec, &opt);
    if (ret < 0) {
        LOG_ERROR("[decoding] open codec %s for file %s failed: %s",
                avcodec_get_name(stream->codec->codec_id), _s_fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("[decoding] alloc frame failed for file %s", _s_fmt_ctx->filename);
        avcodec_close(stream->codec);
        return NULL;
    }

    int got_frame = 0;

    while (1) {
        ret = av_read_frame(_s_fmt_ctx, &pkt);
        if (ret < 0) {
            LOG_WARN("[decoding] read frame failed, in file %s : %s",
                    _s_fmt_ctx->filename, av_err2str(ret));
            continue;
        }

        LOG_DEBUG("[decoding] read frame success, in file %s", _s_fmt_ctx->filename);

        do {
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                ret = avcodec_decode_video2(stream->codec, frame, &got_frame, &pkt);
            } else if (media_type == AVMEDIA_TYPE_AUDIO) {
                ret = avcodec_decode_audio4(stream->codec, frame, &got_frame, &pkt);
            }
            if (ret < 0) {
                LOG_ERROR("[decoding] Error decoding with type of %s in file %s : %s, \
                        end up decoding this stream", av_get_media_type_string(media_type),
                        _s_fmt_ctx->filename, av_err2str(ret));
                goto out;
            }
            //size_t unpadded_linesize;
            //unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
    }

    // flushing cache
    pkt.data = NULL;
    pkt.size = 0;
    do {
        ret = avcodec_decode_audio4(stream->codec, frame, &got_frame, &pkt);
        if (ret < 0) {
            LOG_ERROR("[decoding] Error decoding(flushing cache) with type of %s in file %s : %s, \
                    end up decoding this stream", av_get_media_type_string(media_type),
                    _s_fmt_ctx->filename, av_err2str(ret));
            goto out;
        }
    } while (got_frame);

out:
    av_frame_free(&frame);
    avcodec_close(stream->codec);
    return NULL;
}

int new_stream(std::string url)
{
    enum AVMediaType media_video = AVMEDIA_TYPE_VIDEO;
    enum AVMediaType media_audio = AVMEDIA_TYPE_AUDIO;
    pthread_t video_thread;
    pthread_t audio_thread;
    int ret = 0;

    ret = fork();
    if (ret < 0) {
        LOG_ERROR("[new_stream] fork failed: %s", strerror(errno));
        return ret;
    } else if (ret == 0){
        // child do not live alone
        ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ret < 0) {
            LOG_WARN("[new_stream] prctl failed: %s", strerror(errno));
        }
        if (getppid() == 1) {
            // parent already exit
            LOG_ERROR("[new_stream] parent process alread exit, \
                    this process which deal with stream: '%s' will also exit!", url.c_str());
            exit(1);
        }
    } else {
        return ret;
    }

    LOG_DEBUG("[new_stream] launch stream: %s", url.c_str());

    av_register_all();
    avcodec_register_all();
    avformat_network_init();
    //avdevice_register_all();

    ret = avformat_open_input(&_s_fmt_ctx, url.c_str(), NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("[new_stream] open input failed: %s", av_err2str(ret));
        avformat_network_deinit();
        return -1;
    }

    ret = avformat_find_stream_info(_s_fmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("[new_stream] find stream info failed: %s", av_err2str(ret));
        avformat_close_input(&_s_fmt_ctx);
        avformat_network_deinit();
        return -1;
    }

    if (pthread_create(&video_thread, NULL, decoding, &media_video) != 0) {
        LOG_WARN("[new_stream] create video handling thread failed");
    }

    if (pthread_create(&audio_thread, NULL, decoding, &media_audio) != 0) {
        LOG_WARN("[new_stream] create audio handling thread failed");
    }

    LOG_DEBUG("[new_stream] start play the stream: %s", url.c_str());
    av_read_play(_s_fmt_ctx);

    pthread_join(video_thread, NULL);
    pthread_join(audio_thread, NULL);

    LOG_DEBUG("[new_stream] close the stream: %s", url.c_str());
    avformat_close_input(&_s_fmt_ctx);
    avformat_network_deinit();

    return 0;
}

void stream_control(struct evhttp_request *req, void *arg)
{
    ;
}

int prepar_socket(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket failed");
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(int));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind failed");
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        LOG_ERROR("listen failed");
        return -1;
    }

    int flags;
    if ((flags = fcntl(fd, F_GETFL, 0)) < 0
            || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("fcntl failed");
        return -1;
    }

    return fd;
}

int start(int port, std::vector<std::string> *streams)
{
    std::vector<struct ch_info>::iterator child_iter;
    std::vector<std::string>::iterator stream_iter;
    struct event_base *base = NULL;
    struct evhttp *httpd = NULL;
    int ret = 0;

    int fd = prepar_socket(port, 1024);
    if (fd < 0) {
        LOG_ERROR("[start] prepar socket failed: %s", strerror(errno));
        return -1;
    }

    base = event_base_new();
    if (base == NULL) {
        LOG_ERROR("[start] event_base_new failed");
        ret = -1;
        goto out;
    }

    httpd = evhttp_new(base);
    if (httpd == NULL) {
        LOG_ERROR("[start] evhttp_new failed");
        event_base_free(base);
        base = NULL;
        ret = -1;
        goto out;
    }

    if (evhttp_accept_socket(httpd, fd) != 0) {
        LOG_ERROR("[start] evhttp_accept_socket failed");
        evhttp_free(httpd);
        httpd = NULL;
        event_base_free(base);
        base = NULL;
        ret = -1;
        goto out;
    }

    evhttp_set_cb(httpd, "/stream_control", stream_control, NULL);
    //evhttp_set_gencb(httpd, gen_handler, NULL);

    _s_ch_proc.clear();
    for (stream_iter = streams->begin(); stream_iter != streams->end(); stream_iter++) {
        int pid = new_stream(*stream_iter);
        if (pid > 0) {
            struct ch_info tt;
            tt.pid = pid;
            tt.stream = *stream_iter;
            _s_ch_proc.push_back(tt);
        }
    }

    event_base_dispatch(base);

out:
    for (child_iter = _s_ch_proc.begin(); child_iter != _s_ch_proc.end(); child_iter++) {
        kill(child_iter->pid, SIGKILL);
    }

    if (httpd) {
        evhttp_free(httpd);
    }
    if (base) {
        event_base_free(base);
    }

    return ret;
}

int main(int argc, char **argv)
{
    unsigned int streams_count = 0;
    char *config_file = NULL;
    char *buf = NULL;
    FILE *fd = NULL;
    int ret = 0;
    int c = 0;

    std::vector<std::string> streams;
    Json::Reader reader;
    Json::Value config;
    Json::Value empty;

    while ((c = getopt(argc, argv, "f:")) != EOF) {
        switch (c) {
        case 'f':
            config_file = optarg;
            break;
        case '?':
            return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Usage: %s -f config_file\n", argv[0]);
        return 1;
    }

    struct stat sb;
    /*
    if (stat("log", &sb) == -1) {
        if (mkdir("log", 0755) < 0) {
            fprintf(stderr, "[main] mkdir log failed\n");
            return 1;
        }
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFDIR) {
            if (unlink("log") != 0) {
                fprintf(stderr, "[main] ./log need to be a directory, but not, and rm failed\n");
                return 1;
            }
        }
    }
    */

    memset(&sb, 0, sizeof(struct stat));
    if (stat(config_file, &sb) == -1) {
        fprintf(stderr, "[main] cannot stat file %s: %s\n", config_file, strerror(errno));
        return 1;
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFREG) {
            fprintf(stderr, "[main] %s is not a regular file\n", config_file);
            return 1;
        }
    }

    fd = fopen(config_file, "r");
    if (!fd) {
        fprintf(stderr, "[main] open configure file failed: %s\n", strerror(errno));
        return 1;
    }

    buf = (char*)calloc(sb.st_size + 1, 1);
    if (!buf) {
        fprintf(stderr, "[main] calloc failed: %s\n", strerror(errno));
        fclose(fd);
        return 1;
    }

    ret = 0;
    while (!feof(fd)) {
        ret = fread(&buf[ret], 1, sb.st_size - ret, fd);
        if (ferror(fd)) {
            fprintf(stderr, "[main] something error when reading configure file: %s\n", strerror(errno));
            fclose(fd);
            free(buf);
            return 1;
        }
    }

    if (!reader.parse(buf, config)) {
        fprintf(stderr, "[main] the configure file is not in a valid json format\n");
        fclose(fd);
        free(buf);
        return 1;
    }
    fclose(fd);
    free(buf);

    if (!config.isMember("log_file")) {
        fprintf(stderr, "[main] not found config item 'log_file'\n");
        return 1;
    } else {
        if (!config["log_file"].isString()) {
            fprintf(stderr, "config item 'log_file' is not a string\n");
            return 1;
        }
    }
    if (!config.isMember("log_leave")) {
        fprintf(stderr, "[main] not found config item 'log_leave'\n");
        return 1;
    } else {
        if (!config["log_leave"].isInt()) {
            fprintf(stderr, "config item 'log_leave' is not a integer\n");
            return 1;
        }
    }

    if (my_log_init(".", config["log_file"].asString().c_str(),
                (config["log_file"].asString() + ".we").c_str(),
                config["log_leave"].asInt()) < 0) {
        fprintf(stderr, "[main] my_log_init failed: %s\n", strerror(errno));
        unlink(config["log_file"].asString().c_str());
        unlink((config["log_file"].asString() + ".we").c_str());
        //unlink("log");
        return 1;
    }

    if (!freopen("dev/null", "r", stdin)) {
        LOG_ERROR("[main] failed to redirect STDIN to /dev/null");
    }
    if (!freopen(config["log_file"].asString().c_str(), "w", stdout)) {
        LOG_ERROR("[main] failed to redirect STDIN to %s", config["log_file"].asString().c_str());
    }
    if (!freopen((config["log_file"].asString() + ".we").c_str(), "w", stderr)) {
        LOG_ERROR("[main] failed to redirect STDIN to %s", (config["log_file"].asString() + ".we").c_str());
    }

    if (!config.isMember("streams")) {
        LOG_ERROR("[main] there is no item named 'streams' in configure file");
        ret = 1;
        goto end;
    }
    if (!config["streams"].isArray()) {
        LOG_ERROR("[main] the item 'streams' is not a array");
        ret = 1;
        goto end;
    }

    streams_count = config["streams"].size();
    while (streams_count--) {
        Json::Value temp = config["streams"].get(streams_count, empty);
        if (temp.empty()) {
            continue;
        }
        if (temp.isString()) {
            streams.push_back(temp.asString());
        } else {
            LOG_WARN("[main] the %dth item of 'streams' is not a string", streams_count + 1);
        }
    }

    if (streams.size() == 0) {
        LOG_WARN("[main] no stream found yet");
    }

    if (!config.isMember("port")) {
        LOG_ERROR("[main] there is no item named 'port' in configure file");
        ret = 1;
        goto end;
    }
    if (!config["port"].isInt()) {
        LOG_ERROR("[main] the item 'port' is not a integer");
        ret = 1;
        goto end;
    }

    if (start(config["port"].asInt(), &streams) < 0) {
        LOG_ERROR("[main] start failed");
        ret = 1;
        goto end;
    }

    ret = 0;

end:
    my_log_close();
    return ret;
}
