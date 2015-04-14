#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#include "event2/event.h"
#include "openssl/sha.h"
#include "json/json.h"
#include "evhttp.h"
#include "mylog.h"

#include <string>
#include <vector>
#include <set>
#include <map>

class Tiny {
public:
    std::string name;
    std::set<std::string> tags;
};

pthread_mutex_t g_seed_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_time_now;
unsigned long long g_seed_ct = 0;

std::map<std::string, Tiny> g_tiny_root;

std::vector<pthread_t> g_threads;

#define MAKE_SEED(seed) \
    do { \
        pthread_mutex_lock(&g_seed_lock); \
        char tt[32]; \
        snprintf(tt, 17, "%.16llu", g_seed_ct); \
        seed += g_time_now + tt; \
        g_seed_ct++; \
        pthread_mutex_unlock(&g_seed_lock); \
    } while (0)

#define MAKE_TRIGGER_TO_JSON(tiny, json) \
    do { \
        json["name"] = tiny.name; \
        json; \
    } while (0)

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

void create_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/create_tiny?seed=xxxx
    // ret : { "name" : "xxxxxxxxxxxxxxxxxxxxxxxxxx" }
    std::string seed = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *value = NULL;
    if ((value = evhttp_find_header(&res, "seed")) != NULL) {
        seed = value;
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
        LOG_DEBUG("[create tiny] evbuffer_new() failed");
        return;
    }

    Tiny *tiny = new Tiny();
    tiny->name = md_v;
    tiny->tags.clear();
    g_tiny_root.insert(std::map<std::string, Tiny>::value_type(tiny->name, *tiny));
    delete tiny;

    LOG_DEBUG("[create tiny] created a tiny success, yes, a tiny, one step of which rise you UP ! YOU WIN !");

    evbuffer_add_printf(buf, "{\"name\":\"%s\"}", md_v);
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void destroy_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/destroy_tiny?name=xxxx
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *value = NULL;
    if ((value = evhttp_find_header(&res, "name")) != NULL) {
        name = value;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_DEBUG("[destroy tiny] recieved a destroy_tiny request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;
    it = g_tiny_root.find(name);
    if (it != g_tiny_root.end()) {
        LOG_DEBUG("[destroy tiny] destroied a tiny success");
        g_tiny_root.erase(it);
    }
    
    evhttp_send_reply(req, HTTP_OK, "OK", NULL);
}

void query_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/query_tiny?name=xxxx
    // ret : { "name" : "xxxxx", "tags" : [ "xxxx", "xxxx", .... ] }
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *value = NULL;

    if ((value = evhttp_find_header(&res, "name")) != NULL) {
        name = value;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_DEBUG("[query tiny] recieved a query_tiny request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator iter;
    iter = g_tiny_root.find(name);
    if (iter == g_tiny_root.end()) {
        evhttp_send_reply(req, HTTP_NOTFOUND, "could not find the tiny", NULL);
        LOG_DEBUG("[query tiny] query a tiny, but not found it");
        return;
    }

    Tiny &tiny = iter->second;
    Json::Value tags;
    std::set<std::string>::iterator it;
    for (it = tiny.tags.begin(); it != tiny.tags.end(); it++) {
        tags.append(*it);
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[query tiny] evbuffer_new failed");
        return;
    }

    if (tags.empty()) {
        evbuffer_add_printf(buf, "{\"name\":\"%s\",\"tags\":[]}", name.c_str());
    } else {
        Json::FastWriter writer;
        writer.omitEndingLineFeed();
        evbuffer_add_printf(buf, "{\"name\":\"%s\",\"tags\":%s}", name.c_str(), writer.write(tags).c_str());
    }

    evhttp_send_reply(req, HTTP_OK, "OK", buf);

    LOG_DEBUG("[query tiny] queried a tiny success");
}

void add_tags(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/add_tags?name=xxxx -d '{ "tags" : [ "xxxx", "xxxx", .... ] }'
    std::string name = "";
    struct evkeyvalq res;

    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;
    if ((val = evhttp_find_header(&res, "name")) != NULL) {
        name = val;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_DEBUG("[add tags] recieved a add_tags request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;
    it = g_tiny_root.find(name);
    if (it == g_tiny_root.end()) {
        evhttp_send_reply(req, HTTP_NOTFOUND, "could not find the tiny", NULL);
        LOG_DEBUG("[add tags] add tags into a tiny, but not found this tiny");
        return;
    }

    Tiny &tiny = it->second;

    size_t len = 0;
    struct evbuffer *evbuf = evhttp_request_get_input_buffer(req);
    if (!evbuf || (len = evbuffer_get_length(evbuf)) == 0) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[add tags] no input data(tags) found");
        return;
    }

    char *buf = (char*)calloc(len+1, sizeof(size_t));
    evbuffer_remove(evbuf, buf, len);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[add tags] parse input data to json format failed");
        return;
    }

    if (!value.isMember("tags")) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_DEBUG("[add tags] there's no \"tags\" json item within the input data");
        return;
    }

    if (!value["tags"].isArray()) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_DEBUG("[add tags] the tags item is not an array");
        return;
    }

    unsigned int tags_count = value["tags"].size();
    Json::Value empty;
    while (tags_count--) {
        Json::Value temp = value["tags"].get(tags_count, empty);
        if (temp.empty()) {
            continue;
        }
        tiny.tags.insert(temp.asString());
        LOG_DEBUG("[add tags] add a tag into a tiny success");
    }
    evhttp_send_reply(req, HTTP_OK, "OK", NULL);
}

void del_tags(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/del_tags?name=xxxx -d '{ "tags" : [ "xxxx", "xxxx", .... ] }'
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;
    if ((val = evhttp_find_header(&res, "name")) != NULL) {
        name = val;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_DEBUG("[del tags] recieved a del_tags request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;
    it = g_tiny_root.find(name);
    if (it == g_tiny_root.end()) {
        evhttp_send_reply(req, HTTP_NOTFOUND, "could not find the tiny", NULL);
        LOG_DEBUG("[del tags] delete tags of a tiny, but not found this tiny");
        return;
    }

    Tiny &tiny = it->second;

    size_t len = 0;
    struct evbuffer *evbuf = evhttp_request_get_input_buffer(req);
    if (!evbuf || (len = evbuffer_get_length(evbuf)) == 0) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[del tags] no input data(tags) found");
        return;
    }

    char *buf = (char*)calloc(len+1, sizeof(size_t));
    evbuffer_remove(evbuf, buf, len);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[del tags] parse input data to json format failed");
        return;
    }

    if (!value.isMember("tags")) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_DEBUG("[del tags] there's no \"tags\" json item within the input data");
        return;
    }

    if (!value["tags"].isArray()) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_DEBUG("[del tags] the tags item is not an array");
        return;
    }

    unsigned int tags_count = value["tags"].size();
    Json::Value empty;
    while (tags_count--) {
        Json::Value temp = value["tags"].get(tags_count, empty);
        if (temp.empty()) {
            continue;
        }
        tiny.tags.erase(temp.asString());
        LOG_DEBUG("[del tags] delete a tag from a tiny success");
    }

    evhttp_send_reply(req, HTTP_OK, "OK", NULL);
}

void statistic_handler(struct evhttp_request *req, void *arg)
{
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[statistic handler] evbuffer_new failed");
        return;
    }
    evbuffer_add_printf(buf, "tiny count: %ld\n", g_tiny_root.size());
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
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

        evhttp_set_cb(httpd, "/create_tiny", create_tiny, NULL);
        evhttp_set_cb(httpd, "/destroy_tiny", destroy_tiny, NULL);
        evhttp_set_cb(httpd, "/query_tiny", query_tiny, NULL);
        evhttp_set_cb(httpd, "/add_tags", add_tags, NULL);
        evhttp_set_cb(httpd, "/del_tags", del_tags, NULL);
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

static void sig_alrm(int signo)
{
    time_t tt = time(NULL);
    char buf[16];
    snprintf(buf, 11, "%lld", (long long)tt);
    std::string tt_buf = buf;

    if (g_time_now != tt_buf) {
        pthread_mutex_lock(&g_seed_lock);
        g_time_now = buf;
        g_seed_ct = 0;
        pthread_mutex_unlock(&g_seed_lock);
    }

    alarm(2);
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

    if (mkdir("log", 0755) < 0) {
        printf("mkdir log failed\n");
        return 1;
    }

    my_log_init(".", "log/hash.log", "log/hash.log.we", 16);

    if (port <= 1024) {
        LOG_ERROR("Invalid port number: %d, should >1024", port);
        my_log_close();
        return 1;
    }

    if (thread_num < 1) {
        thread_num = 1;
    }

    if (signal(SIGALRM, sig_alrm) == SIG_ERR) {
        LOG_ERROR();
        my_log_close();
        return 1;
    }
    alarm(2);

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
