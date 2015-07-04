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
#include <map>

#define DEQUE_MAX_SIZE 10000

#define msleep(x) \
    do { \
        struct timespec time_to_wait; \
        time_to_wait.tv_sec = 0; \
        time_to_wait.tv_nsec = 1000 * 1000 * x; \
        nanosleep(&time_to_wait, NULL); \
    } while(0)

typedef struct {
    AVFormatContext *fmt_ctx;
    const char *config_filename;
    const char *config;
    int stream_index;
} Trans_data;

typedef struct {
    std::string file;
    int index;
    int pid;
} Child_info;

std::vector<Child_info>::iterator child_iter;
std::vector<Child_info> child_processes;
int cur_rtsp_child = -1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::deque<AVPacket*> pkt_deque;

pthread_t rtsp_thread;
Trans_data trans_data;
std::string rtsp_config;

int need_delive_pkt = 0;
int want_video_stream_index = -1;
int want_audio_stream_index = -1;

AVFormatContext *fmt_ctx = NULL;
char temp_file[1024];

extern "C" void *rtsp_main(void *arg);
extern "C" void rtsp_close();

extern "C"
int pickup_pkt(AVPacket *pkt)
{
    if (!pkt) {
        LOG_WARN("pickup_pkt failed: invalied packet");
        return 1;
    }

    AVPacket *tmp = NULL;

    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;

    pthread_mutex_lock(&mutex);
    if (pkt_deque.empty()) {
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    tmp = pkt_deque.front();
    pkt_deque.pop_front();
    LOG_INFO("pickup <- nb: %d", pkt_deque.size());
    pthread_mutex_unlock(&mutex);
    av_copy_packet(pkt, tmp);
    av_free_packet(tmp);
    free(tmp);
    return 0;
}

int insert_pkt(AVPacket *pkt)
{
    if (!pkt) {
        LOG_WARN("insert_pkt failed: invalied packet");
        return 1;
    }

    AVPacket *new_pkt = NULL;
    new_pkt = (AVPacket*)malloc(sizeof(AVPacket));
    if (!new_pkt) {
        LOG_WARN("insert_pkt failed: alloc new_pkt failed: %s", strerror(errno));
        return 1;
    }
    av_init_packet(new_pkt);
    new_pkt->data = NULL;
    new_pkt->size = 0;
    av_copy_packet(new_pkt, pkt);

    pthread_mutex_lock(&mutex);
    if (pkt_deque.size() >= DEQUE_MAX_SIZE) {
        pthread_mutex_unlock(&mutex);
        LOG_WARN("insert_pkt failed: deque full");
        return 1;
    }
    pkt_deque.push_back(new_pkt);
    LOG_INFO("insert -> nb: %d", pkt_deque.size());
    pthread_mutex_unlock(&mutex);

    return 0;
}

void fun(AVFrame *frame, enum AVPixelFormat pix_fmt)
{
    if (pix_fmt != AV_PIX_FMT_YUV420P) {
        return;
    }
    int linesize = frame->linesize[0];
    uint8_t *data = frame->data[0];
    int w = frame->width;
    int h = frame->height;

    int i;

    for (i = 0; i < w; i++) {
        data[(h / 2) * linesize + i] = 0;
        data[(h / 2 - 1) * linesize + i] = 0;
        data[(h / 2 + 1) * linesize + i] = 0;
    }
}

int open_input_file(AVFormatContext **ifmt_ctx, char *filename)
{
    unsigned int i = 0;
    int ret = 0;

    AVFormatContext *p_fmt = NULL;
    //AVDictionary* options = NULL;
    //av_dict_set(&options, "rtsp_transport", "tcp", 0);

    ret = avformat_open_input(ifmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("open %s failed: %s", filename, av_err2str(ret));
        return 1;
    }

    p_fmt = *ifmt_ctx;

    ret = avformat_find_stream_info(p_fmt, NULL);
    if (ret < 0) {
        LOG_ERROR("find stream info failed for %s: %s", filename, av_err2str(ret));
        avformat_close_input(ifmt_ctx);
        return 1;
    }

    for (i = 0; i < p_fmt->nb_streams; i++) {
        AVStream *stream;
        AVCodecContext *codec_ctx;
        stream = p_fmt->streams[i];
        codec_ctx = stream->codec;
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL);
            if (ret < 0) {
                LOG_ERROR("open codec failed for stream %d in %s", i, filename);
                avformat_close_input(ifmt_ctx);
                return 1;
            }
        }
    }
    //av_dump_format(p_fmt, 0, filename, 0);

    return 0;
}

int open_temp_file(AVFormatContext *ifmt_ctx, AVFormatContext **ofmt_ctx, char *filename)
{
    unsigned int i = 0;
    int ret = 0;

    if (!ifmt_ctx || !ofmt_ctx) {
        LOG_ERROR("invalid paramters when open temp file for %s", filename);
        return 1;
    }

    avformat_alloc_output_context2(ofmt_ctx, NULL, NULL, filename);
    if (!(*ofmt_ctx)) {
        LOG_ERROR("alloc ofmt_ctx failed for %s: %s", filename, av_err2str(ret));
        return 1;
    }

    int is_there_video_stream = 0;
    int is_there_audio_stream = 0;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecContext *dec_ctx = NULL;
        AVCodecContext *enc_ctx = NULL;
        AVStream *out_stream = NULL;
        AVStream *in_stream = NULL;
        AVCodec *encoder = NULL;

        in_stream = ifmt_ctx->streams[i];
        dec_ctx = in_stream->codec;

        if ((is_there_video_stream && (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)) ||
            (is_there_audio_stream && (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO))) {
            continue;
        }

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            out_stream = avformat_new_stream(*ofmt_ctx, NULL);
            if (!out_stream) {
                LOG_ERROR("alloc new stream failed for %s", filename);
                avformat_close_input(ofmt_ctx);
                return 1;
            }
            enc_ctx = out_stream->codec;

            encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
            if (!encoder) {
                LOG_ERROR("find encoder %s faild for %s", avcodec_get_name(AV_CODEC_ID_MPEG4), filename);
                avformat_close_input(ofmt_ctx);
                return 1;
            }

            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->bit_rate = dec_ctx->bit_rate;
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                enc_ctx->time_base = dec_ctx->time_base;
                if (enc_ctx->time_base.den > ((1 << 16) - 1)) {
                    enc_ctx->time_base.den = (1 << 16) - 1;
                }

                if (dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ420P) {
                    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                    enc_ctx->color_range = AVCOL_RANGE_JPEG;
                } else {
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                }

                /*
                char pix_string[1024];
                memset(pix_string, 0, 1024);
                av_get_pix_fmt_string(pix_string, 1024, enc_ctx->pix_fmt);
                LOG_WARN("bit_rate:%d, height:%d, width:%d, sample_aspect_ratio:%d/%d, "
                        "pix_fmt:%s, time_base:%d/%d", enc_ctx->bit_rate, enc_ctx->height,
                        enc_ctx->width, enc_ctx->sample_aspect_ratio.num,
                        enc_ctx->sample_aspect_ratio.den, pix_string, enc_ctx->time_base.num,
                        enc_ctx->time_base.den);
                */
            } else {
                enc_ctx->bit_rate = dec_ctx->bit_rate;
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                LOG_ERROR("open %s encoder '%s' failed for %s: %s",
                        dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio",
                        encoder->name, filename, av_err2str(ret));
                avformat_close_input(ofmt_ctx);
                return 1;
            }
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                want_video_stream_index = i;
                is_there_video_stream = 1;
            } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                want_audio_stream_index = i;
                is_there_audio_stream = 1;
            }
            LOG_INFO("open encoder '%s' for stream %d success for %s", encoder->name, i, filename);
        }
    }

    if (want_video_stream_index == -1) {
        LOG_WARN("NOT found video stream in %s", filename);
        return 1;
    }
    //av_dump_format(*ofmt_ctx, 0, temp_file, 1);

    ret = avio_open(&(*ofmt_ctx)->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        LOG_ERROR("avio open failed for %s: %s", filename, av_err2str(ret));
        avformat_close_input(ofmt_ctx);
        return 1;
    }
    ret = avformat_write_header(*ofmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("write output header failed %s: %s", filename, av_err2str(ret));
        avformat_close_input(ofmt_ctx);
        return 1;
    }
    return 0;
}

int add_to_show_stream(AVCodecContext *codec, const AVFrame *frame, 
        int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *),
        int64_t pts, int duration)
{
    int got_packet = 0;
    int ret = 0;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    ret = enc_func(codec, &pkt, frame, &got_packet);
    if (ret < 0) {
        LOG_ERROR("encode failed: %s", av_err2str(ret));
        return 1;
    }

    if (got_packet) {
        pkt.pts = pts;
        pkt.duration = duration;
        if (insert_pkt(&pkt) != 0) {
            LOG_ERROR("insert packet failed");
            av_free_packet(&pkt);
            return 1;
        }
        av_free_packet(&pkt);
        return 0;
    }
    return 1;
}

int stream_process(char *filename)
{
    int ret = 0;

    memset(temp_file, 0, 1024);
    snprintf(temp_file, 1024, "tmp/temp_%d.avi", getpid());

    av_register_all();
    avcodec_register_all();
    avformat_network_init();

    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;

    ret = open_input_file(&ifmt_ctx, filename);
    if (ret != 0) {
        avformat_network_deinit();
        return 1;
    }

    ret = open_temp_file(ifmt_ctx, &ofmt_ctx, temp_file);
    if (ret != 0) {
        avformat_close_input(&ifmt_ctx);
        avformat_network_deinit();
        return 1;
    }

    int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *);
    dec_func = NULL;
    enc_func = NULL;

    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("alloc frame failed for %s: %s", filename, av_err2str(ret));
        avformat_close_input(&ifmt_ctx);
        avformat_close_input(&ofmt_ctx);
        avformat_network_deinit();
        return 1;
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int64_t tt = 0;
    int got_frame = 0;
    int stream_index = 0;
    int read_error_counter = 0;
    while (1) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) {
            if (read_error_counter++ < 10) {
                msleep(100);
                continue;
            } else {
                break;
            }
        }
        read_error_counter = 0;
        stream_index = pkt.stream_index;
        enum AVMediaType type = ifmt_ctx->streams[stream_index]->codec->codec_type;
        //TODO drop audio packet
        if (stream_index != want_video_stream_index) {
            continue;
        }

        av_packet_rescale_ts(&pkt, ifmt_ctx->streams[stream_index]->time_base,
                ifmt_ctx->streams[stream_index]->codec->time_base);

        dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 : avcodec_decode_audio4;
        enc_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

        do {
            got_frame = 0;
            ret = dec_func(ifmt_ctx->streams[stream_index]->codec, frame, &got_frame, &pkt);
            if (ret < 0) {
                LOG_ERROR("decode packet failed for %s: %s", filename, av_err2str(ret));
                avformat_close_input(&ifmt_ctx);
                avformat_close_input(&ofmt_ctx);
                avformat_network_deinit();
                return 1;
            }
            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
        if (got_frame) {
            if (frame->pts == AV_NOPTS_VALUE) {
                frame->pts = tt++;
            } else {
                frame->pts = av_frame_get_best_effort_timestamp(frame);
            }

            if (type == AVMEDIA_TYPE_VIDEO) {
                fun(frame, ifmt_ctx->streams[stream_index]->codec->pix_fmt);
                if (need_delive_pkt) {
                    ret = add_to_show_stream(ofmt_ctx->streams[stream_index]->codec, frame, enc_func, pkt.pts, pkt.duration);
                    if (ret != 0) {
                        LOG_ERROR("failed adding frame to show stream");
                    }
                }
            }

            av_frame_unref(frame);
        }
        av_frame_unref(frame);
        av_free_packet(&pkt);
        continue;
    }

    return 0;
}

void *process_thread(void *arg)
{
    stream_process((char*)arg);
    return NULL;
}

void sigusr1_func(int signo)
{
    int ret = 0;

    ret = avformat_open_input(&fmt_ctx, temp_file, NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("open %s failed: %s", temp_file, av_err2str(ret));
        return;
    }

    trans_data.fmt_ctx = fmt_ctx;
    trans_data.config_filename = "decoder.conf";
    trans_data.config = rtsp_config.c_str();
    trans_data.stream_index = want_video_stream_index;

    pkt_deque.clear();
    need_delive_pkt = 1;
    if ((ret = pthread_create(&rtsp_thread, NULL, rtsp_main, &trans_data)) != 0) {
        LOG_ERROR("create rtsp_server thread failed");
        need_delive_pkt = 0;
        pkt_deque.clear();
    }
}

void sigusr2_func(int signo)
{
    need_delive_pkt = 0;
    rtsp_close();

    avformat_close_input(&fmt_ctx);
    fmt_ctx = NULL;
    pkt_deque.clear();
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

    child_processes.clear();

    signal(SIGUSR1, sigusr1_func);
    signal(SIGUSR2, sigusr2_func);

    pthread_t thread;
    if ((ret = pthread_create(&thread, NULL, process_thread, (void*)url.c_str())) != 0) {
        LOG_WARN("failed to create receiving thread, for file %s : %s",
                url.c_str(), strerror(ret));
    }
    pthread_join(thread, NULL);

    LOG_WARN("process for %s exited", url.c_str());

    return 0;
}

void query_streams(struct evhttp_request *req, void *arg)
{
    if (!req) {
        return;
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "INTERNAL", NULL);
        return;
    }

    evbuffer_add_printf(buf, "Index\tPID\tSource\n");
    for (child_iter = child_processes.begin(); child_iter != child_processes.end();
                child_iter++) {
        evbuffer_add_printf(buf, "%d\t%d\t%s\n", child_iter->index, child_iter->pid, child_iter->file.c_str());
    }

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain; charset=utf-8");
    evhttp_add_header(req->output_headers, "Connection", "Close");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
    evbuffer_free(buf);
}

void select_stream(struct evhttp_request *req, void *arg)
{
    const char *value = NULL;
    struct evkeyvalq res;
    int target_index = -1;

    if (!req) {
        return;
    }

    evhttp_parse_query(req->uri, &res);
    if ((value = evhttp_find_header(&res, "index")) != NULL) {
        target_index = atoi(value);
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "BADREQUEST", NULL);
        return;
    }

    if (target_index == -1) {
        evhttp_send_reply(req, HTTP_INTERNAL, "INTERNAL", NULL);
        return;
    }

    for (child_iter = child_processes.begin(); child_iter != child_processes.end();
                child_iter++) {
        if (child_iter->index == target_index) {
            cur_rtsp_child = child_iter->pid;
            break;
        }
    }

    if (cur_rtsp_child == -1) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "BADREQUEST", NULL);
        return;
    } else {
        evhttp_send_reply(req, HTTP_OK, "OK", NULL);
    }

    // start up rtsp server
    kill(cur_rtsp_child, SIGUSR1);
}

void kill_rtsp_serv(struct evhttp_request *req, void *arg)
{
    if (cur_rtsp_child == -1) {
        evhttp_send_reply(req, HTTP_OK, "OK", NULL);
        return;
    }

    // stop rtsp server
    kill(cur_rtsp_child, SIGUSR2);
    cur_rtsp_child = -1;
    evhttp_send_reply(req, HTTP_OK, "OK", NULL);
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

void *req_serv(void *arg)
{
    struct event_base *base = NULL;
    struct evhttp *httpd = NULL;

    if (!arg) {
        LOG_ERROR("create request service failed");
        return NULL;
    }

    int port = *(int*)arg;
    int fd = prepar_socket(port, 1024);
    if (fd < 0) {
        LOG_ERROR("prepar socket failed: %s", strerror(errno));
        return NULL;
    }

    base = event_base_new();
    if (!base) {
        LOG_ERROR("event_base_new failed");
        return NULL;
    }

    httpd = evhttp_new(base);
    if (!httpd) {
        LOG_ERROR("evhttp_new failed");
        event_base_free(base);
        return NULL;
    }

    if (evhttp_accept_socket(httpd, fd) != 0) {
        LOG_ERROR("evhttp_accept_socket failed");
        evhttp_free(httpd);
        event_base_free(base);
        return NULL;
    }

    evhttp_set_cb(httpd, "/query_streams", query_streams, NULL);
    evhttp_set_cb(httpd, "/select_stream", select_stream, NULL);
    evhttp_set_cb(httpd, "/kill_rtsp", kill_rtsp_serv, NULL);
    //evhttp_set_gencb(httpd, gen_handler, NULL);

    event_base_dispatch(base);

    return NULL;
}

int start(int port, std::vector<std::string> *files)
{
    std::vector<std::string>::iterator file_iter;
    int index = 0;
    int ret = 0;

    pthread_t thread;
    if ((ret = pthread_create(&thread, NULL, req_serv, (void*)&port)) != 0) {
        LOG_WARN("failed creating request service thread: %s", strerror(ret));
        return 1;
    }

    child_processes.clear();
    for (file_iter = files->begin(); file_iter != files->end(); file_iter++) {
        int pid = new_file(*file_iter);
        if (pid < 0) {
            LOG_ERROR("fork failed, for file %s", file_iter->c_str());
            continue;
        } else if (pid == 0) {
            exit(0);
        } else {
            Child_info tt;
            tt.file = *file_iter;
            tt.index = index++;
            tt.pid = pid;
            child_processes.push_back(tt);
        }
    }

    while (child_processes.size() != 0) {
        pid_t pid = wait(NULL);
        for (child_iter = child_processes.begin(); child_iter != child_processes.end();
                child_iter++) {
            if (child_iter->pid == pid) {
                child_processes.erase(child_iter);
                break;
            }
        }
    }

    LOG_WARN("proc exit");
    return 0;
}

int get_rtsp_config(Json::Value config)
{
    std::map<std::string, std::string> stream;
    Json::Value::Members::iterator iter;
    Json::Value::Members members;

    if (!config.isMember("RTSPPort") || !config.isMember("RTSPBindAddress") || !config.isMember("Streams")) {
        LOG_ERROR("not found item named 'RTSPPort' or 'RTSPBindAddress' or 'Streams' in rtsp_config");
        return 1;
    } else {
        if (!config["RTSPPort"].isString() || !config["RTSPBindAddress"].isString()) {
            LOG_ERROR("item 'RTSPPort' or 'RTSPBindAddress' is not a string");
            return 1;
        }
        if (!config["Streams"].isObject()) {
            LOG_ERROR("item 'Streams' is not an object");
            return 1;
        }
    }

    rtsp_config = "";

    members = config.getMemberNames();
    for (iter = members.begin(); iter != members.end(); iter++) {
        if (*iter == "Streams") {
            continue;
        } else {
            if (config[*iter].isString()) {
                rtsp_config += *iter + " " + config[*iter].asString() + "\n";
            }
        }
    }
    if (config.isMember("Streams")) {
        if (config["Streams"].isObject()) {
            Json::Value streams = config["Streams"];
            Json::Value::Members streams_mb;
            Json::Value::Members::iterator streams_iter;
            streams_mb = streams.getMemberNames();

            for (streams_iter = streams_mb.begin(); streams_iter != streams_mb.end();
                    streams_iter++) {
                Json::Value stream = streams[*streams_iter];
                if (stream.empty()) {
                    continue;
                }
                if (stream.isObject()) {
                    Json::Value::Members stream_mb;
                    Json::Value::Members::iterator stream_iter;
                    stream_mb = stream.getMemberNames();
                    std::string stream_val = "";
                    for (stream_iter = stream_mb.begin(); stream_iter != stream_mb.end();
                            stream_iter++) {
                        if (stream[*stream_iter].isString()) {
                            stream_val += *stream_iter + " " + stream[*stream_iter].asString() + "\n";
                        }
                    }
                    if (stream_val != "") {
                        rtsp_config += "<Stream " + *streams_iter + ">\n" + stream_val + "</Stream>\n";
                    }
                }
            }
        }
    }
    return 0;
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

    if (stat("tmp", &sb) == -1) {
        if (mkdir("tmp", 0755) < 0) {
            fprintf(stderr, "[main] mkdir tmp failed\n");
            return 1;
        }
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFDIR) {
            if (unlink("tmp") != 0) {
                fprintf(stderr, "[main] ./tmp need to be a directory, but not, and rm failed\n");
                return 1;
            }
        }
    }

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

    if (!config.isMember("rtsp_config")) {
        LOG_ERROR("there is no item named 'rtsp_config' in configure file");
        ret = 1;
        goto end;
    }
    if (!config["rtsp_config"].isObject()) {
        LOG_ERROR("the item 'port' is not an object");
        ret = 1;
        goto end;
    }
    if (get_rtsp_config(config["rtsp_config"]) != 0) {
        LOG_ERROR("get rtsp config failed");
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
