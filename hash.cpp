#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "event2/event.h"
#include "openssl/sha.h"
#include "json/json.h"
#include "evhttp.h"
#include "mylog.h"

#include <string>
#include <vector>
#include <map>

pthread_mutex_t g_seed_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_time_now;
unsigned long long g_seed_ct = 0;

#define MAKE_SEED(seed) \
    do { \
        pthread_mutex_lock(&g_seed_lock); \
        char tt[32]; \
        snprintf(tt, 17, "%.16llu", g_seed_ct); \
        seed += g_time_now + tt; \
        g_seed_ct++; \
        pthread_mutex_unlock(&g_seed_lock); \
    } while (0)

struct tiny {
    std::string tag;
    std::vector<std::string> other_tags;
};

std::map<std::string, struct tiny> g_tiny_root;

std::vector<pthread_t> g_threads;

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

void new_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/new_tiny?seed=xxxx
    // ret : { "tag" : "xxxxxxxxxxxxxxxxxxxxxxxxxx" }

    std::string seed = "";
    std::string tag = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *value = NULL;
    if ((value = evhttp_find_header(&res, "seed")) != NULL) {
        seed = value;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        return;
    }

    MAKE_SEED(seed);

    char md_v[129];
    unsigned char md[1024];
    memset(md_v, 0, 129);
    SHA512((unsigned char*)seed.c_str(), seed.length(), md);
    int n = 64;
    while (n--) {
        snprintf(&md_v[126-2*n], 3, "%.2x", md[63-n]);
    }
    md_v[128] = 0;

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        return;
    }

    evbuffer_add_printf(buf, "{\"tag\":\"%s\"}", md_v);
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void get_tiny(struct evhttp_request *req, void *arg)
{
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add_printf(buf, "%s\n", "sha512");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void statistic_handler(struct evhttp_request *req, void *arg)
{
    ;
}

void *dispatch(void *arg)
{
    event_base_dispatch((struct event_base*)arg);
    event_base_free((struct event_base*)arg);
    return NULL;
}

int start(int port, int thread_num)
{
    int fd = prepar_socket(port, 1024);
    if (fd < 0) {
        LOG_ERROR("prepar socket failed: %s", strerror(errno));
        return -1;
    }

    while (thread_num--) {
        struct event_base *base = event_base_new();
        if (base == NULL) {
            LOG_ERROR("event_base_new failed");
            continue;
        }

        struct evhttp *httpd = evhttp_new(base);
        if (httpd == NULL) {
            LOG_ERROR("evhttp_new failed");
            event_base_free(base);
            continue;
        }

        if (evhttp_accept_socket(httpd, fd) != 0) {
            LOG_ERROR("evhttp_accept_socket failed");
            evhttp_free(httpd);
            event_base_free(base);
            continue;
        }

        evhttp_set_cb(httpd, "/new_tiny", new_tiny, NULL);
        evhttp_set_cb(httpd, "/get_tiny", get_tiny, NULL);
        evhttp_set_cb(httpd, "/statistic", statistic_handler, NULL);
        //evhttp_set_gencb(httpd, gen_handler, NULL);

        pthread_t thread;
        if (pthread_create(&thread, NULL, dispatch, base) != 0) {
            LOG_ERROR("create recieving thread failed");
            evhttp_free(httpd);
            event_base_free(base);
            continue;
        }

        g_threads.push_back(thread);
    }

    return (int)(g_threads.size());
}

int main(int argc, char **argv)
{
    int ret = 0;
    int port = 0;
    int thread_num = 4;

    int c = 0;
    while ((c=getopt(argc, argv, "t:p:")) != EOF) {
        switch (c) {
        case 'p':
            port = atoi(optarg);
            break;
        case 't':
            thread_num = atoi(optarg);
            break;
        case '?':
            return 1;
        }
    }

    my_log_init(".", "hash.log", "hash.log.we", 16);

    if (port <= 1024) {
        LOG_ERROR("Invalid port number: %d, should >1024", port);
        my_log_close();
        return 1;
    }

    if (thread_num < 1) {
        thread_num = 1;
    }

    ret = start(port, thread_num);
    if (ret < 1) {
        LOG_ERROR("start failed");
        my_log_close();
        return 1;
    }

    LOG_INFO("start %d recieving threads", ret);

    std::vector<pthread_t>::iterator it;
    for (it = g_threads.begin(); it != g_threads.end(); it++) {
        pthread_join(*it, NULL);
    }

    my_log_close();

    return 0;
}
