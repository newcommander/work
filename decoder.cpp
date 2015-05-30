#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
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
#include "libavutil/imgutils.h"
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
    AVFormatContext *fmt_ctx;
    pthread_mutex_t mutex;
    int stream_index;
};

struct child_info {
    std::string file;
    int pid;
};

/*
extern int show_init(char *ip, int port, int *screen_width, int *screen_height);
extern int show(int sockfd, unsigned char *buf);
extern int show_end(int sockfd);

void test_image(uint8_t **data, int *linesize, int width, int height)
{
    uint8_t *data_y = data[0];
    uint8_t *data_u = data[1];
    uint8_t *data_v = data[2];
    int size_y = linesize[0];
    int size_u = linesize[1];
    int size_v = linesize[2];
    int len = width * height;
    int screen_height = 0;
    int screen_width = 0;

    unsigned char *buf = (unsigned char*)calloc(4 + len * 7, 1);
    if (!buf) {
        printf("calloc failed\n");
        return;
    }

    std::string ip = "192.168.1.2";
    int fd = show_init((char*)(ip.c_str()), 19900, &screen_width, &screen_height);
    if (fd < 0) {
        printf("show_init failed\n");
        free(buf);
        return;
    }

    buf[0] = len & 0xff;
    buf[1] = ( len >> 8 ) & 0xff;
    buf[2] = ( len >> 16 ) & 0xff;
    buf[3] = ( len >> 24 ) & 0xff;

    int h = 0, w = 0;
    unsigned char *p = &buf[4];
    unsigned char *c = &buf[4 + len * 4];
    for (h = 0; h < height; h++) {
        for (w = 0; w < width; w++) {
            p[(h * width + w) * 4 + 0] = (unsigned char)(w & 0xff);
            p[(h * width + w) * 4 + 1] = (unsigned char)((w >> 8) & 0xff);
            p[(h * width + w) * 4 + 2] = (unsigned char)(h & 0xff);
            p[(h * width + w) * 4 + 3] = (unsigned char)((h >> 8) & 0xff);
            float y = (float)data_y[h * size_y + w];
            float u = (float)data_u[(h / 2) * size_u + w / 2];
            float v = (float)data_v[(h / 2) * size_v + w / 2];
            c[(h * width + w) * 3 + 0] = (unsigned char)(y + 1.4075 * (v - 128));
            c[(h * width + w) * 3 + 1] = (unsigned char)(y - 0.3455 * (u - 128) - 0.7169 * (v - 128));
            c[(h * width + w) * 3 + 2] = (unsigned char)(y + 1.779 * (u - 128));
        }
    }
    show(fd, buf);

    free(buf);
    show_end(fd);
}
*/

int insert_pkt(std::deque<AVPacket> &queue, AVPacket *pkt, pthread_mutex_t *mutex)
{
    if (!pkt || !mutex) {
        return 0;
    }

    pthread_mutex_lock(mutex);
    if (queue.size() >= DEQUE_MAX_SIZE) {
        LOG_WARN("insert failed, deque full");  // TODO: for which file ?
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

void receiving_thread_clean(void *arg)
{
    if (!arg) {
        return;
    }

    struct deque_info *info = (struct deque_info*)arg;
    if (info->stream_index != -1) {
        pthread_cancel(info->decoding_thread);
    }
    LOG_INFO("decoding thread exit, for %s stream in file %s",
            av_get_media_type_string(info->media_type), info->fmt_ctx->filename);
}

void *decoding_packet(void *arg)
{
    int ret = 0;

    if (!arg) {
        return NULL;
    }

    struct deque_info *info = (struct deque_info*)arg;

    enum AVMediaType media_type = info->media_type;

    AVStream *stream = NULL;
    stream = info->fmt_ctx->streams[info->stream_index];
    int width  = stream->codec->width;
    int height = stream->codec->height;

    AVCodec *dec = NULL;
    dec = avcodec_find_decoder(stream->codec->codec_id);
    if (!dec) {
        LOG_ERROR("Error finding decoder: %s, for %s stream in file %s : %s",
                avcodec_get_name(stream->codec->codec_id),
                av_get_media_type_string(media_type), info->fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    AVDictionary *opt = NULL;
    ret = avcodec_open2(stream->codec, dec, &opt);
    if (ret < 0) {
        LOG_ERROR("open codec: %s failed, for %s stream in file %s : %s",
                avcodec_get_name(stream->codec->codec_id), av_get_media_type_string(media_type),
                info->fmt_ctx->filename, av_err2str(ret));
        return NULL;
    }

    if ((stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) &&
            (stream->codec->pix_fmt != AV_PIX_FMT_YUV420P)) {
        LOG_ERROR("no support video stream in non-YUV420P format yet");
        avcodec_close(stream->codec);
        return NULL;
    }

    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("alloc frame failed for %s stream in file %s : %s",
                av_get_media_type_string(media_type), info->fmt_ctx->filename, av_err2str(ret));
        avcodec_close(stream->codec);
        return NULL;
    }

    int got_frame = 0;

    std::deque<AVPacket> &p_deque = info->pkt_deque;
    pthread_mutex_t *mutex = &info->mutex;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    uint8_t *video_data[4] = {NULL};
    int video_linesize[4];
    int data_size = 0;
    data_size = av_image_alloc(video_data, video_linesize, width, height, AV_PIX_FMT_YUV420P, 1);

    while (1) {
        pick_pkt(p_deque, &pkt, mutex);
        if (pkt.size == 0) {
            msleep(100);
            continue;
        }
        do {
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                ret = avcodec_decode_video2(stream->codec, frame, &got_frame, &pkt);
                av_image_copy(video_data, video_linesize, (const uint8_t **)(frame->data),
                        frame->linesize, AV_PIX_FMT_YUV420P, frame->width, frame->height);
            } else if (media_type == AVMEDIA_TYPE_AUDIO) {
                ret = avcodec_decode_audio4(stream->codec, frame, &got_frame, &pkt);
            }

            if (ret < 0) {
                LOG_ERROR("Error decoding %s stream in file %s : %s, end up decoding this stream",
                        av_get_media_type_string(media_type),
                        info->fmt_ctx->filename, av_err2str(ret));
                goto out;
            }

            //size_t unpadded_linesize;
            //unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((enum AVSampleFormat)frame->format);

            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
    }

out:
    av_frame_free(&frame);
    avcodec_close(stream->codec);
    return NULL;
}

void *receiving_packet(void *arg)
{
    AVFormatContext *fmt_ctx = NULL;
    struct deque_info video_info;
    struct deque_info audio_info;
    char *filename = NULL;
    int ret = 0;

    if (!arg) {
        return NULL;
    }

    filename = (char*)arg;

    video_info.receiving_thread = pthread_self();
    audio_info.receiving_thread = pthread_self();
    pthread_mutex_init(&video_info.mutex, NULL);
    pthread_mutex_init(&audio_info.mutex, NULL);
    video_info.media_type = AVMEDIA_TYPE_VIDEO;
    audio_info.media_type = AVMEDIA_TYPE_AUDIO;
    video_info.pkt_deque.clear();
    audio_info.pkt_deque.clear();
    video_info.stream_index = -1;
    audio_info.stream_index = -1;

    av_register_all();
    avcodec_register_all();
    avformat_network_init();
    //avdevice_register_all();

    ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("open input failed for file %s : %s", filename, av_err2str(ret));
        avformat_network_deinit();
        return NULL;
    }
    video_info.fmt_ctx = fmt_ctx;
    audio_info.fmt_ctx = fmt_ctx;
    LOG_INFO("open file %s", filename);

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("find stream info failed for file %s : %s", filename, av_err2str(ret));
        avformat_close_input(&fmt_ctx);
        avformat_network_deinit();
        return NULL;
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret == AVERROR_STREAM_NOT_FOUND) {
        LOG_WARN("not found video stream in file %s", filename);
    } else if (ret < 0) {
        LOG_ERROR("Error finding video stream in file %s : %s", filename, av_err2str(ret));
    } else {
        video_info.stream_index = ret;
        LOG_INFO("found video stream for file %s", filename);
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret == AVERROR_STREAM_NOT_FOUND) {
        LOG_WARN("not found audio stream in file %s", filename);
    } else if (ret < 0) {
        LOG_ERROR("Error finding audio stream in file %s : %s", filename, av_err2str(ret));
    } else {
        audio_info.stream_index = ret;
        LOG_INFO("found audio stream for file %s", filename);
    }

    if ((video_info.stream_index == -1) && (audio_info.stream_index == -1)) {
        avformat_close_input(&fmt_ctx);
        avformat_network_deinit();
        return NULL;
    }

    LOG_INFO("start receiving thread for file %s", filename);
    av_read_play(fmt_ctx);

    if (video_info.stream_index != -1) {
        if ((ret = pthread_create(&video_info.decoding_thread, NULL,
                        decoding_packet, &video_info)) != 0) {
            LOG_WARN("failed to create decoding thread, for video stream in file %s : %s",
                    filename, strerror(ret));
        }
    }
    pthread_cleanup_push(receiving_thread_clean, &video_info);

    if (audio_info.stream_index != -1) {
        if ((ret = pthread_create(&audio_info.decoding_thread, NULL,
                        decoding_packet, &audio_info)) != 0) {
            LOG_WARN("failed to create decoding thread, for audio stream in file %s : %s",
                    filename, strerror(ret));
        }
    }
    pthread_cleanup_push(receiving_thread_clean, &audio_info);

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    std::deque<AVPacket> &v_deque = video_info.pkt_deque;
    std::deque<AVPacket> &a_deque = audio_info.pkt_deque;
    pthread_mutex_t *v_mutex = &video_info.mutex;
    pthread_mutex_t *a_mutex = &audio_info.mutex;
    int video_index = video_info.stream_index;
    int audio_index = audio_info.stream_index;

    while (1) { // TODO : how to break ?
        ret = av_read_frame(fmt_ctx, &pkt);
        if (ret < 0) {
            LOG_WARN("read frame failed, in file %s : %s", filename, av_err2str(ret));
            continue;
        }

        LOG_DEBUG("read frame success, in file %s", filename);

        if (pkt.stream_index == video_index) {
            insert_pkt(v_deque, &pkt, v_mutex);
        } else if (pkt.stream_index == audio_index) {
            insert_pkt(a_deque, &pkt, a_mutex);
        }
    }

    LOG_INFO("close file %s", filename);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    pthread_cleanup_pop(1); //audio decoding thread
    pthread_cleanup_pop(1); //video decoding thread
    return NULL;
}

int new_file(std::string url)
{
    int ret = 0;

    ret = fork();
    if (ret < 0) {
        LOG_ERROR("fork failed for file %s : %s", url.c_str(), strerror(errno));
        return ret;
    } else if (ret == 0){
        // child do not live alone
        ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ret < 0) {
            LOG_WARN("prctl failed for file %s : %s", url.c_str(), strerror(errno));
        }
        if (getppid() == 1) {
            // parent already exit
            LOG_ERROR("parent process alread exit, "
                    "child exit now! for file %s", url.c_str());
            exit(1);
        }
    } else {
        return ret;
    }

    pthread_t thread;
    if ((ret = pthread_create(&thread, NULL, receiving_packet, (void*)url.c_str())) != 0) {
        LOG_WARN("failed to create receiving thread, for file %s : %s",
                url.c_str(), strerror(ret));
    }

    pthread_join(thread, NULL);
    LOG_INFO("stop receiving thread for file %s", url.c_str());

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
    std::vector<struct child_info> child_processes;
    std::vector<std::string>::iterator file_iter;
    struct event_base *base = NULL;
    struct evhttp *httpd = NULL;
    int ret = 0;

    child_processes.clear();
    for (file_iter = files->begin(); file_iter != files->end(); file_iter++) {
        int pid = new_file(*file_iter);
        if (pid < 0) {
            LOG_ERROR("fork failed, for file %s", file_iter->c_str());
            continue;
        } else if (pid == 0) {
            exit(0);
        } else {
            struct child_info tt;
            tt.pid = pid;
            tt.file = *file_iter;
            child_processes.push_back(tt);
        }
    }

    while (child_processes.size() != 0) {
        pid_t pid = wait(NULL);
        for (child_iter = child_processes.begin(); child_iter != child_processes.end();
                child_iter++) {
            if (child_iter->pid == pid) {
                child_processes.erase(child_iter);
            }
        }
    }

    int fd = prepar_socket(port, 1024);
    if (fd < 0) {
        LOG_ERROR("prepar socket failed: %s", strerror(errno));
        return -1;
    }

    base = event_base_new();
    if (base == NULL) {
        LOG_ERROR("event_base_new failed");
        ret = -1;
        goto out;
    }

    httpd = evhttp_new(base);
    if (httpd == NULL) {
        LOG_ERROR("evhttp_new failed");
        event_base_free(base);
        base = NULL;
        ret = -1;
        goto out;
    }

    if (evhttp_accept_socket(httpd, fd) != 0) {
        LOG_ERROR("evhttp_accept_socket failed");
        evhttp_free(httpd);
        httpd = NULL;
        event_base_free(base);
        base = NULL;
        ret = -1;
        goto out;
    }

    evhttp_set_cb(httpd, "/stream_control", stream_control, NULL);
    //evhttp_set_gencb(httpd, gen_handler, NULL);

    event_base_dispatch(base);

out:
    for (child_iter = child_processes.begin(); child_iter != child_processes.end(); child_iter++) {
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
        fprintf(stderr, "cannot stat file %s: %s\n", config_file, strerror(errno));
        return 1;
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFREG) {
            fprintf(stderr, "%s is not a regular file\n", config_file);
            return 1;
        }
    }

    fd = fopen(config_file, "r");
    if (!fd) {
        fprintf(stderr, "open configure file failed: %s\n", strerror(errno));
        return 1;
    }

    buf = (char*)calloc(sb.st_size + 1, 1);
    if (!buf) {
        fprintf(stderr, "calloc failed: %s\n", strerror(errno));
        fclose(fd);
        return 1;
    }

    ret = 0;
    while (!feof(fd)) {
        ret = fread(&buf[ret], 1, sb.st_size - ret, fd);
        if (ferror(fd)) {
            fprintf(stderr, "something error when reading configure file: %s\n", strerror(errno));
            fclose(fd);
            free(buf);
            return 1;
        }
    }

    if (!reader.parse(buf, config)) {
        fprintf(stderr, "the configure file is not in a valid json format\n");
        fclose(fd);
        free(buf);
        return 1;
    }
    fclose(fd);
    free(buf);

    if (!config.isMember("log_file")) {
        fprintf(stderr, "not found config item 'log_file'\n");
        return 1;
    } else {
        if (!config["log_file"].isString()) {
            fprintf(stderr, "config item 'log_file' is not a string\n");
            return 1;
        }
    }
    if (!config.isMember("log_leave")) {
        fprintf(stderr, "not found config item 'log_leave'\n");
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
        fprintf(stderr, "my_log_init failed: %s\n", strerror(errno));
        unlink(config["log_file"].asString().c_str());
        unlink((config["log_file"].asString() + ".we").c_str());
        //unlink("log");
        return 1;
    }

    if (!freopen("/dev/null", "r", stdin)) {
        LOG_ERROR("failed to redirect STDIN to /dev/null");
    }
    if (!freopen((config["log_file"].asString() + ".stdout").c_str(), "w", stdout)) {
        LOG_ERROR("failed to redirect STDIN to %s",
                (config["log_file"].asString() + ".stdout").c_str());
    }
    if (!freopen((config["log_file"].asString() + ".stderr").c_str(), "w", stderr)) {
        LOG_ERROR("failed to redirect STDIN to %s",
                (config["log_file"].asString() + ".stderr").c_str());
    }

    if (!config.isMember("streams")) {
        LOG_ERROR("there is no item named 'streams' in configure file");
        ret = 1;
        goto end;
    }
    if (!config["streams"].isArray()) {
        LOG_ERROR("the item 'streams' is not a array");
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
            LOG_WARN("the %dth item of 'streams' is not a string", streams_count + 1);
        }
    }

    if (streams.size() == 0) {
        LOG_WARN("no stream found yet");
    }

    if (!config.isMember("port")) {
        LOG_ERROR("there is no item named 'port' in configure file");
        ret = 1;
        goto end;
    }
    if (!config["port"].isInt()) {
        LOG_ERROR("the item 'port' is not a integer");
        ret = 1;
        goto end;
    }

    if (start(config["port"].asInt(), &streams) < 0) {
        LOG_ERROR("start failed");
        ret = 1;
        goto end;
    }

    ret = 0;

end:
    my_log_close();
    return ret;
}
