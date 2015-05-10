#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "openssl/sha.h"
#include "json/json.h"
#include "curl/curl.h"
#include "evhttp.h"
#include "mylog.h"

#include "logic.h"

#include <string>
#include <vector>
#include <map>

//static pthread_mutex_t g_recv_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, std::string> g_refer_table;
//static char g_curl_errbuf[CURL_ERROR_SIZE];
static std::vector<pthread_t> g_threads;
static std::string g_recv_buf = "";
//static CURL *g_url;

/*
static size_t recv_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
    if (buffer) {
        pthread_mutex_lock(&g_recv_lock);
        g_recv_buf += (char*)buffer;
        pthread_mutex_unlock(&g_recv_lock);
    }
    return size * nmemb;
}

static int report_init()
{
    if (g_url) {
        return 0;
    }

    g_url = curl_easy_init();
    if (!g_url) {
        LOG_ERROR("[report init] curl_easy_init failed");
        return 1;
    }

    curl_easy_setopt(g_url, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_url, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(g_url, CURLOPT_USERAGENT, "tiny/op");
    curl_easy_setopt(g_url, CURLOPT_ERRORBUFFER, g_curl_errbuf);
    curl_easy_setopt(g_url, CURLOPT_WRITEFUNCTION, recv_data);

    return 0;
}

static int report_clean()
{
    curl_easy_cleanup(g_url);
    g_url = NULL;
    return 0;
}

static int report_send(std::string url, std::string post_data)
{
    int status = CURLE_OK;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: ");
    headers = curl_slist_append(headers, "Content-Type: ");

    curl_easy_setopt(g_url, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_url, CURLOPT_URL, url.c_str());
    curl_easy_setopt(g_url, CURLOPT_POSTFIELDS, post_data.c_str());
    status = curl_easy_perform(g_url);
    curl_slist_free_all(headers);

    if (status != CURLE_OK) {
        LOG_ERROR("[report send] curl_easy_perform failed: %s", g_curl_errbuf);
        return 1;
    }

    long resp_code;
    status = curl_easy_getinfo(g_url, CURLINFO_RESPONSE_CODE, &resp_code);
    if (resp_code != 200) {
        LOG_ERROR("[report send] request to \"%s\" failed, [HTTP/1.1 %ld]", url.c_str(), resp_code);
        return 1;
    }

    return 0;
}

static Json::Value do_query(std::string uri, std::string post_data)
{
    Json::Value value;
    Json::Reader reader;
    std::string url = "http://localhost:8888/";

    int ret = report_send(url + uri, post_data);
    if (ret != 0) {
        LOG_ERROR("[do query] request failed");
        return value;
    }

    time_t timeout = 10;
    time_t start_time = time(NULL);
    while (1) {
        if ((time(NULL) - start_time) > timeout) {
            break;
        }

        pthread_mutex_lock(&g_recv_lock);
        if (!reader.parse(g_recv_buf, value)) {
            pthread_mutex_unlock(&g_recv_lock);
            continue;
        }
        pthread_mutex_unlock(&g_recv_lock);
        break;
    }
    g_recv_buf = "";

    return value;
}
*/

void do_refer(std::string unit)
{
    std::map<std::string, std::string>::iterator table_iter;
    table_iter = g_refer_table.find(unit);
    if (table_iter == g_refer_table.end()) {
        LOG_INFO("[do refer] unit not hit a tiny, create tiny and add it to the refer table");
        Json::Value val = new_tiny("1");
        Json::Value empty;
        std::string name = "";
        name = val["names"].get((unsigned int)0, empty).asString();
        //TODO: do some thing ckeck
        g_refer_table.insert(std::map<std::string, std::string>::value_type(unit, name));
    }
}

void refer(unsigned int unit_len, std::string data)
{
    unsigned int round = data.size() / unit_len;
    unsigned int again;
    for (again = 0; again < round; again++) {
        std::string ss = data.substr(unit_len * again, unit_len);
        do_refer(ss);
    }
}

void statistic_handler(struct evhttp_request *req, void *arg)
{
    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[statistic handler] evbuffer_new() failed");
        return;
    }

    Json::Value value;
    value["g_refer_table"] = (unsigned int)g_refer_table.size();

    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    evbuffer_add_printf(rebuf, "%s", writer.write(value).c_str());
    evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
}

void receive_handler(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8886 -d '{ "unit_len" : n, "data" : xxxx }'
    size_t len = 0;
    struct evbuffer *evbuf = evhttp_request_get_input_buffer(req);
    if (!evbuf || (len = evbuffer_get_length(evbuf)) == 0) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[receive handler] no input data found");
        return;
    }

    char *buf = (char*)calloc(len+1, sizeof(size_t));
    evbuffer_remove(evbuf, buf, len);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[receive handler] parse input data to json format failed");
        free(buf);
        return;
    }
    free(buf);

    if (!value.isMember("unit_len") || !value.isMember("data")) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[receive handler] no item named \"unit_len\" or \"data\"");
        return;
    }
    if (!value["unit_len"].isUInt() || !value["data"].isString()) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[receive handler] item \"unit_len\" not type of uint or \"data\" not type of string");
        return;
    }
    refer(value["unit_len"].asUInt(), value["data"].asString());

    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[receive handler] evbuffer_new() failed");
        return;
    }

    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    evbuffer_add_printf(rebuf, "%s", writer.write(value).c_str());
    evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
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

        evhttp_set_cb(httpd, "/statistic", statistic_handler, NULL);
        evhttp_set_gencb(httpd, receive_handler, NULL);

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

    my_log_init(".", "log/reference.log", "log/reference.log.we", 16);

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
