#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "event2/event.h"
#include "json/json.h"
#include "evhttp.h"
#include "mylog.h"
#include "curl/curl.h"

#include "logic.h"

std::vector<pthread_t> g_threads;

void control_handler(struct evhttp_request *req, void *arg)
{
    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[control handler] evbuffer_new() failed");
        return;
    }

    if (do_logic() != 0) {
        evbuffer_add_printf(rebuf, "failed\n");
        evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
    } else {
        evbuffer_add_printf(rebuf, "success\n");
        evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
    }
}

void *dispatch(void *arg)
{
    event_base_dispatch((struct event_base*)arg);
    event_base_free((struct event_base*)arg);
    return NULL;
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

        evhttp_set_cb(httpd, "/control", control_handler, NULL);
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

    report_init();

    return (int)(g_threads.size());
}

int main(int argc, char **argv)
{
    int ret = 0;
    int port = 0;
    int thread_num = 1;

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

    struct stat sb;
    if (stat("log", &sb) == -1) {
        if (mkdir("log", 0755) < 0) {
            printf("mkdir log failed\n");
            return 1;
        }
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFDIR) {
            if (unlink("log") != 0) {
                printf("I need ./log to be a directory, but it does not, and rm failed");
                return 1;
            }
        }
    }

    my_log_init(".", "log/case_op.log", "log/case_op.log.we", 16);

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

    LOG_INFO("start %d controlling threads", ret);

    std::vector<pthread_t>::iterator it;
    for (it = g_threads.begin(); it != g_threads.end(); it++) {
        pthread_join(*it, NULL);
    }

    report_clean();

    LOG_INFO("tiny_op stop");

    my_log_close();

    return 0;
}
