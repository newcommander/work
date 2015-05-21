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
#include <time.h>

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
#include <deque>

#define DEQUE_MAX_SIZE 10000

#define msleep(x) \
    do { \
        struct timespec time_to_wait; \
        time_to_wait.tv_sec = 0; \
        time_to_wait.tv_nsec = 1000 * 1000 * x; \
        nanosleep(&time_to_wait, NULL); \
    } while(0)

struct deque_info {
    std::deque<AVPacket> pkt_deque;
    enum AVMediaType media_type;
    pthread_t receiving_thread;
    pthread_t decoding_thread;
    pthread_mutex_t mutex;
    int stream_index;
};

struct child_info {
    std::string file;
    int pid;
};

static std::vector<struct child_info> _s_child_processes;  // for global process
static AVFormatContext *_s_fmt_ctx = NULL;  // for each child process

int insert_pkt(std::deque<AVPacket> &queue, AVPacket *pkt, pthread_mutex_t *mutex)
{
    if (!pkt || !mutex) {
        return 0;
    }

    pthread_mutex_lock(mutex);
    if (queue.size() >= DEQUE_MAX_SIZE) {
        LOG_ERROR("[insert_pkt] insert failed, deque full");
        pthread_mutex_unlock(mutex);
        return -1;
    }
    queue.push_back(*pkt);
    pthread_mutex_unlock(mutex);

    return 0;
}

void pick_pkt(std::deque<AVPacket> &queue, AVPacket *pkt, pthread_mutex_t *mutex)
{
    if (!mutex) {
        pkt->size = 0;
        return;
    }

    pthread_mutex_lock(mutex);
    if (queue.empty()) {
        pkt->size = 0;
        pthread_mutex_unlock(mutex);
        return;
    }
    *pkt = queue.front();
    queue.pop_front();
    pthread_mutex_unlock(mutex);
}

void decoding_thread_clean(void *arg)
{
    struct deque_info *info = (struct deque_info*)arg;
    pthread_cancel(info->receiving_thread);
    LOG_INFO("[decoding_thread_clean] decoding thread exit, for %s stream in file %s",
            av_get_media_type_string(info->media_type), _s_fmt_ctx->filename);
}

void receiving_thread_clean(void *arg)
{
    struct deque_info *info = (struct deque_info*)arg;
    pthread_cancel(info->decoding_thread);
    LOG_INFO("[receiving_thread_clean] receiving thread exit, for %s stream in file %s",
            av_get_media_type_string(info->media_type), _s_fmt_ctx->filename);
}

void *decoding_packet(void *arg)
{
    int ret = 0;

    pthread_cleanup_push(decoding_thread_clean, arg);

    if (!arg) {
        return NULL;
    }

    struct deque_info *info = (struct deque_info*)arg;

    enum AVMediaType media_type = info->media_type;

    /*
    int bits_per_pixel = 0;
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(_s_fmt_ctx->pix_fmt);
        if (!desc) {
            LOG_ERROR("[decoding_packet] error getting pixfmt descriptor, for %s stream in file %s : %s",
                    av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
            return NULL;
        }
        bits_per_pixel = av_get_bits_per_pixel(desc);
    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        ;//TODO
    } else {
        LOG_ERROR("[decoding_packet] unsupport media type %s in file %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename);
        return NULL;
    }
    */

    AVStream *stream = NULL;
    stream = _s_fmt_ctx->streams[info->stream_index];

    AVCodec *dec = NULL;
    dec = avcodec_find_decoder(stream->codec->codec_id);
    if (!dec) {
        LOG_ERROR("[decoding_packet] Error finding decoder: %s, for %s stream in file %s : %s",
                avcodec_get_name(stream->codec->codec_id),
                av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    AVDictionary *opt = NULL;
    ret = avcodec_open2(stream->codec, dec, &opt);
    if (ret < 0) {
        LOG_ERROR("[decoding_packet] open codec: %s failed, for %s stream in file %s : %s",
                avcodec_get_name(stream->codec->codec_id), av_get_media_type_string(media_type),
                _s_fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("[decoding_packet] alloc frame failed for %s stream in file %s : %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
        avcodec_close(stream->codec);
        return NULL;
    }

    int got_frame = 0;

    std::deque<AVPacket> &p_deque = info->pkt_deque;
    pthread_mutex_t *mutex = &(info->mutex);
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while (1) {
        pick_pkt(p_deque, &pkt, mutex);
        if (pkt.size == 0) {
            msleep(100);
            continue;
        }
        do {
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                ret = avcodec_decode_video2(stream->codec, frame, &got_frame, &pkt);
            } else if (media_type == AVMEDIA_TYPE_AUDIO) {
                ret = avcodec_decode_audio4(stream->codec, frame, &got_frame, &pkt);
            }

            if (ret < 0) {
                LOG_ERROR("[decoding_packet] Error decoding %s stream in file %s : %s, "
                        "end up decoding this stream", av_get_media_type_string(media_type),
                        _s_fmt_ctx->filename, av_err2str(ret));
                goto out;
            }

            //size_t unpadded_linesize;
            //unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
            /*
            int bytes_per_sample = 0;
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                ;//TODO
            } else if (media_type == AVMEDIA_TYPE_AUDIO) {
                if (bytes_per_sample == 0) {
                    bytes_per_sample = av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
                    if (bytes_per_sample == 0) {
                        LOG_ERROR("[decoding_packet] error getting bytes per sample for %s stream in file %s : %s",
                                av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
                        goto out;
                    }
                }
            }
            */

            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
    }

out:
    av_frame_free(&frame);
    avcodec_close(stream->codec);
    pthread_cleanup_pop(1);
    return NULL;
}

void *receiving_packet(void *arg)
{
    int ret = 0;

    if (!arg) {
        return NULL;
    }

    struct deque_info *info = (struct deque_info*)arg;

    enum AVMediaType media_type = info->media_type;

    ret = av_find_best_stream(_s_fmt_ctx, media_type, -1, -1, NULL, 0);
    if (ret == AVERROR_STREAM_NOT_FOUND) {
        LOG_WARN("[receiving_packet] not found %s stream in file %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename);
        return NULL;
    } else if (ret < 0) {
        LOG_ERROR("[receiving_packet] Error finding %s stream in file %s : %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    info->stream_index = ret;

    LOG_INFO("[receiving_packet] start thread for %s stream in file %s",
            av_get_media_type_string(media_type), _s_fmt_ctx->filename);

    pthread_t decoding_thread;
    if ((ret = pthread_create(&decoding_thread, NULL, decoding_packet, info)) != 0) {
        LOG_WARN("[receiving_packet] failed to create decoding packet thread, "
                "for %s stream in file %s : %s", av_get_media_type_string(media_type),
                _s_fmt_ctx->filename, strerror(ret));
        LOG_WARN("[receiving_packet] receiving thread exit, for %s stream in file %s",
                av_get_media_type_string(media_type), _s_fmt_ctx->filename);
        return NULL;
    }

    pthread_cleanup_push(receiving_thread_clean, info);

    std::deque<AVPacket> &p_deque = info->pkt_deque;
    pthread_mutex_t *mutex = &(info->mutex);

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while (1) { // TODO : how to break ?
        ret = av_read_frame(_s_fmt_ctx, &pkt);
        if (ret < 0) {
            LOG_WARN("[receiving_packet] read frame failed, for %s stream in file %s : %s",
                    av_get_media_type_string(media_type), _s_fmt_ctx->filename, av_err2str(ret));
            continue;
        }

        LOG_DEBUG("[receiving_packet] read frame success, in file %s", _s_fmt_ctx->filename);

        insert_pkt(p_deque, &pkt, mutex);
    }

    pthread_cleanup_pop(1);
    return NULL;
}

int new_file(std::string url)
{
    pthread_t video_thread;
    pthread_t audio_thread;
    int ret = 0;

    ret = fork();
    if (ret < 0) {
        LOG_ERROR("[new_file] fork failed for file %s : %s", url.c_str(), strerror(errno));
        return ret;
    } else if (ret == 0){
        // child do not live alone
        ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ret < 0) {
            LOG_WARN("[new_file] prctl failed for file %s : %s", url.c_str(), strerror(errno));
        }
        if (getppid() == 1) {
            // parent already exit
            LOG_ERROR("[new_file] parent process alread exit, "
                    "child exit now! for file %s", url.c_str());
            exit(1);
        }
    } else {
        return ret;
    }

    _s_child_processes.clear();

    LOG_INFO("[new_file] launch file %s", url.c_str());

    av_register_all();
    avcodec_register_all();
    avformat_network_init();
    //avdevice_register_all();

    ret = avformat_open_input(&_s_fmt_ctx, url.c_str(), NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("[new_file] open input failed for file %s : %s",
                url.c_str(), av_err2str(ret));
        avformat_network_deinit();
        return -1;
    }

    ret = avformat_find_stream_info(_s_fmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("[new_file] find stream info failed for file %s : %s",
                url.c_str(), av_err2str(ret));
        avformat_close_input(&_s_fmt_ctx);
        avformat_network_deinit();
        return -1;
    }

    struct deque_info video_info;
    video_info.media_type = AVMEDIA_TYPE_VIDEO;
    pthread_mutex_init(&video_info.mutex, NULL);
    video_info.pkt_deque.clear();
    if ((ret = pthread_create(&video_thread, NULL, receiving_packet, &video_info)) != 0) {
        LOG_WARN("[new_file] failed to create receiving packet thread, for video stream in file %s : %s",
                url.c_str(), strerror(ret));
    }
    video_info.receiving_thread = video_thread;

    struct deque_info audio_info;
    audio_info.media_type = AVMEDIA_TYPE_AUDIO;
    pthread_mutex_init(&audio_info.mutex, NULL);
    audio_info.pkt_deque.clear();
    if ((ret = pthread_create(&audio_thread, NULL, receiving_packet, &audio_info)) != 0) {
        LOG_WARN("[new_file] failed to create receiving packet thread, for audio stream in file %s : %s",
                url.c_str(), strerror(ret));
    }
    audio_info.receiving_thread = audio_thread;

    LOG_INFO("[new_file] playing the file %s", url.c_str());
    av_read_play(_s_fmt_ctx);

    pthread_join(video_thread, NULL);
    pthread_join(audio_thread, NULL);

    LOG_INFO("[new_file] close the file %s", url.c_str());
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

int start(int port, std::vector<std::string> *files)
{
    std::vector<struct child_info>::iterator child_iter;
    std::vector<std::string>::iterator file_iter;
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

    _s_child_processes.clear();
    for (file_iter = files->begin(); file_iter != files->end(); file_iter++) {
        int pid = new_file(*file_iter);
        if (pid > 0) {
            struct child_info tt;
            tt.pid = pid;
            tt.file = *file_iter;
            _s_child_processes.push_back(tt);
        } else {
            break;
        }
    }

    event_base_dispatch(base);

out:
    for (child_iter = _s_child_processes.begin(); child_iter != _s_child_processes.end(); child_iter++) {
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
            fprintf(stderr, "[main] config item 'log_file' is not a string\n");
            return 1;
        }
    }
    if (!config.isMember("log_leave")) {
        fprintf(stderr, "[main] not found config item 'log_leave'\n");
        return 1;
    } else {
        if (!config["log_leave"].isInt()) {
            fprintf(stderr, "[main] config item 'log_leave' is not a integer\n");
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
