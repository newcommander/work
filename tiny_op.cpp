#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
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
pthread_mutex_t g_tiny_root_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_time_now;
unsigned long long g_seed_ct = 0;

std::map<std::string, Tiny> g_tiny_root_master;
std::map<std::string, Tiny> g_tiny_root_slaver;
std::map<std::string, Tiny> *g_tiny_root_master_p;
std::map<std::string, Tiny> *g_tiny_root_slaver_p;

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
    // url : http://localhost:8888/create_tiny?num=n&seed=xxxx
    // ret : { "names" : ["xxxx", "xxxx", "xxxx"] }
    Json::Value value;
    std::string seed = "";
    unsigned int num = 1;
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;
    if ((val = evhttp_find_header(&res, "seed")) != NULL) {
        seed = val;
    }
    if ((val = evhttp_find_header(&res, "num")) != NULL) {
        num = atoi(val);
    }

    char md_v[57];
    unsigned char md[1024];
    int n;

    value["names"].resize(0);

    while (num--) {
        memset(md_v, 0, 57);

        MAKE_SEED(seed);
        SHA224((unsigned char*)seed.c_str(), seed.length(), md);

        n = 28;
        while (n--) {
            snprintf(&md_v[54-2*n], 3, "%.2x", md[27-n]);
        }
        md_v[56] = 0;

        Tiny *tiny = new Tiny();
        tiny->name = md_v;
        tiny->tags.clear();

        pthread_mutex_lock(&g_tiny_root_lock);
        g_tiny_root_master_p->insert(std::map<std::string, Tiny>::value_type(tiny->name, *tiny));
        pthread_mutex_unlock(&g_tiny_root_lock);

        value["names"].append(md_v);
        delete tiny;
    }

    LOG_DEBUG("[create tiny] created %ld tiny success", value["names"].size());

    Json::FastWriter writer;
    writer.omitEndingLineFeed();

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[create tiny] evbuffer_new() failed");
        return;
    }

    evbuffer_add_printf(buf, "%s", writer.write(value).c_str());
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void destroy_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/destroy_tiny?name=xxxx
    // ret : { "status" : 0/1 }
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *value = NULL;
    if ((value = evhttp_find_header(&res, "name")) != NULL) {
        name = value;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_ERROR("[destroy tiny] recieved a destroy_tiny request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;

    pthread_mutex_lock(&g_tiny_root_lock);
    it = g_tiny_root_master_p->find(name);
    if (it != g_tiny_root_master_p->end()) {
        LOG_DEBUG("[destroy tiny] destroied a tiny success");
        g_tiny_root_master_p->erase(it);
    }
    pthread_mutex_unlock(&g_tiny_root_lock);
    
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[destroy tiny] evbuffer_new() failed");
        return;
    }

    evbuffer_add_printf(buf, "{\"status\":0}");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void query_tiny(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/query_tiny?deep=n&head=xxxx
    // ret : { "head" : "xxxxxxxxxxxxxx",
    //         "deep" : real_deep,
    //         "message": [
    //                      { "name" : "xxxxx", "tags" : [ "xxxx", "xxxx", .... ] },
    //                      { "name" : "xxxxx", "tags" : [ "xxxx", "xxxx", .... ] },
    //                      ......
    //                    ]
    //       }
    int deep = 1;
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;

    if ((val = evhttp_find_header(&res, "head")) != NULL) {
        name = val;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_ERROR("[query tiny] recieved a query_tiny request WITHOUT name parameter");
        return;
    }

    if ((val = evhttp_find_header(&res, "deep")) != NULL) {
        deep = atoi(val);
    }

    int deep_i;
    std::set<std::string> layer_info;
    std::set<std::string>::iterator layer_iter;
    std::set<std::string> tags_set;
    std::set<std::string>::iterator tags_iter;

    Json::Value value;

    value["head"] = name;
    value["message"].resize(0);

    pthread_mutex_lock(&g_tiny_root_lock);
    //get all names of "deep" layers
    layer_info.insert(name);
    unsigned int deep_count = 0;
    for (deep_i = 2; deep_i <= deep; deep_i++) {
        for (layer_iter = layer_info.begin(); layer_iter != layer_info.end(); layer_iter++) {
            std::map<std::string, Tiny>::iterator it;
            it = g_tiny_root_master_p->find(*layer_iter);
            if (it == g_tiny_root_master_p->end()) {
                continue;
            } else {
                Json::Value item;
                item[*layer_iter].resize(0);
                for (tags_iter = it->second.tags.begin(); tags_iter != it->second.tags.end(); tags_iter++) {
                    item[*layer_iter].append(*tags_iter);
                    tags_set.insert(*tags_iter);
                }
                value["message"].append(item);
            }
        }
        if (tags_set.empty()) {
            break;
        }
        deep_count++;
        layer_info.clear();
        layer_info = tags_set;
        tags_set.clear();
    }
    pthread_mutex_unlock(&g_tiny_root_lock);

    value["deep"] = deep_count;

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_DEBUG("[query tiny] evbuffer_new failed");
        return;
    }

    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    evbuffer_add_printf(buf, "%s", writer.write(value).c_str());

    evhttp_send_reply(req, HTTP_OK, "OK", buf);

    LOG_DEBUG("[query tiny] queried a tiny with %d deep success", deep_count);
}

void add_tags(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/add_tags?name=xxxx -d '{ "tags" : [ "xxxx", "xxxx", .... ] }'
    // ret : { "status" : 0\1 }
    std::string name = "";
    struct evkeyvalq res;

    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;
    if ((val = evhttp_find_header(&res, "name")) != NULL) {
        name = val;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_ERROR("[add tags] recieved a add_tags request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;

    pthread_mutex_lock(&g_tiny_root_lock);
    it = g_tiny_root_master_p->find(name);
    if (it == g_tiny_root_master_p->end()) {
        pthread_mutex_unlock(&g_tiny_root_lock);
        evhttp_send_reply(req, HTTP_NOTFOUND, "could not find the tiny", NULL);
        LOG_ERROR("[add tags] add tags into a tiny, but not found this tiny");
        return;
    }
    pthread_mutex_unlock(&g_tiny_root_lock);

    Tiny &tiny = it->second;

    size_t len = 0;
    struct evbuffer *evbuf = evhttp_request_get_input_buffer(req);
    if (!evbuf || (len = evbuffer_get_length(evbuf)) == 0) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[add tags] no input data(tags) found");
        return;
    }

    char *buf = (char*)calloc(len+1, sizeof(size_t));
    evbuffer_remove(evbuf, buf, len);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[add tags] parse input data to json format failed");
        return;
    }

    if (!value.isMember("tags")) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_ERROR("[add tags] there's no \"tags\" json item within the input data");
        return;
    }

    if (!value["tags"].isArray()) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_ERROR("[add tags] the tags item is not an array");
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

    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[add tags] evbuffer_new() failed");
        return;
    }

    evbuffer_add_printf(rebuf, "{\"status\":0}");
    evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
}

void del_tags(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/del_tags?name=xxxx -d '{ "tags" : [ "xxxx", "xxxx", .... ] }'
    // ret : { "status" : 0/1 }
    std::string name = "";
    struct evkeyvalq res;
    evhttp_parse_query(req->uri, &res);
    const char *val = NULL;
    if ((val = evhttp_find_header(&res, "name")) != NULL) {
        name = val;
    } else {
        evhttp_send_reply(req, HTTP_BADREQUEST, "invalid http request was made", NULL);
        LOG_ERROR("[del tags] recieved a del_tags request WITHOUT name parameter");
        return;
    }

    std::map<std::string, Tiny>::iterator it;

    pthread_mutex_lock(&g_tiny_root_lock);
    it = g_tiny_root_master_p->find(name);
    if (it == g_tiny_root_master_p->end()) {
        pthread_mutex_unlock(&g_tiny_root_lock);
        evhttp_send_reply(req, HTTP_NOTFOUND, "could not find the tiny", NULL);
        LOG_ERROR("[del tags] delete tags of a tiny, but not found this tiny");
        return;
    }
    pthread_mutex_unlock(&g_tiny_root_lock);

    Tiny &tiny = it->second;

    size_t len = 0;
    struct evbuffer *evbuf = evhttp_request_get_input_buffer(req);
    if (!evbuf || (len = evbuffer_get_length(evbuf)) == 0) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[del tags] no input data(tags) found");
        return;
    }

    char *buf = (char*)calloc(len+1, sizeof(size_t));
    evbuffer_remove(evbuf, buf, len);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[del tags] parse input data to json format failed");
        return;
    }

    if (!value.isMember("tags")) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_ERROR("[del tags] there's no \"tags\" json item within the input data");
        return;
    }

    if (!value["tags"].isArray()) {
        evhttp_send_reply(req, HTTP_BADREQUEST, "post data format error", NULL);
        LOG_ERROR("[del tags] the tags item is not an array");
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

    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[del tags] evbuffer_new() failed");
        return;
    }

    evbuffer_add_printf(rebuf, "{\"status\":0}");
    evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
}

void statistic_handler(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/statistic
    // ret : { "master root count" : n, "slaver root count" : m }
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[statistic handler] evbuffer_new failed");
        return;
    }

    Json::Value value;
    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    value["master root count"] = (unsigned int)g_tiny_root_master_p->size();
    value["slaver root count"] = (unsigned int)g_tiny_root_slaver_p->size();

    pthread_mutex_lock(&g_tiny_root_lock);
    evbuffer_add_printf(buf, "%s", writer.write(value).c_str());
    pthread_mutex_unlock(&g_tiny_root_lock);

    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void dump_handler(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/dump
    // ret : { "status" : 0/1 }
    Json::Value value;
    std::map<std::string, Tiny>::iterator it;
    std::set<std::string>::iterator iter;

    pthread_mutex_lock(&g_tiny_root_lock);
    for (it = g_tiny_root_master_p->begin(); it != g_tiny_root_master_p->end(); it++) {
        value[it->first].resize(0);
        for (iter = it->second.tags.begin(); iter != it->second.tags.end(); iter++) {
            value[it->first].append(*iter);
        }
    }
    pthread_mutex_unlock(&g_tiny_root_lock);

    Json::FastWriter writer;
    writer.omitEndingLineFeed();

    FILE *fd = fopen("log/dump", "w");
    if (!fd) {
        LOG_ERROR("[dump handler] fopen log/dump failed: %s", strerror(errno));
        return;
    }
    fwrite(writer.write(value).c_str(), writer.write(value).size(), 1, fd);
    fclose(fd);

    LOG_DEBUG("[dump handler] dump tiny success");

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[dump handler] evbuffer_new() failed");
        return;
    }

    evbuffer_add_printf(buf, "{\"status\":0}");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

void load_handler(struct evhttp_request *req, void *arg)
{
    // url : http://localhost:8888/load
    // ret : { "status" : 0/1 }
    struct stat sb;
    if (stat("log/dump", &sb) == -1) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] stat log/dump failed, cannot load it");
        return;
    }

    char *buf = (char*)calloc(1, sb.st_size+1);
    if (!buf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] calloc(size: %ld) failed, cannot load it", sb.st_size+1);
        return;
    }

    FILE *fd = fopen("log/dump", "r");
    if (!fd) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] fopen log/dump failed: %s", strerror(errno));
        free(buf);
        return;
    }

    time_t start_point = time(NULL);
    off_t ret = 0;
    while (1) {
        if (time(NULL) - start_point > 10) {
            evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
            LOG_ERROR("[load handler] load failed, timeout");
            fclose(fd);
            free(buf);
            return;
        }
        ret += fread(&(buf[ret * sb.st_size]), sb.st_size - ret, 1, fd);
        //TODO: dose file read complete???
        break;
    }
    fclose(fd);

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(buf, value)) {
        LOG_ERROR("[load handler] json pares failed");
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        return;
    }
    free(buf);

    if (value.empty()) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] json value empty, load failed");
        return;
    }

    Json::Value::Members root_members;
    Json::Value::Members::iterator root_mem;
    unsigned int tags_num = 0;
    if (!value.isObject()) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] the json root is not an object, load failed");
        return;
    }
    root_members = value.getMemberNames();
    for (root_mem = root_members.begin(); root_mem != root_members.end(); root_mem++) {
        Tiny tiny;
        tiny.name = *root_mem;
        if (!value[tiny.name].isArray()) {
            evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
            LOG_ERROR("[load handler] the json data is not an array, load failed");
            g_tiny_root_slaver_p->clear();
            return;
        }
        tags_num = value[tiny.name].size();
        Json::Value empty;
        while (tags_num--) {
            tiny.tags.insert(value[tiny.name].get(tags_num, empty).asString());
        }
        g_tiny_root_slaver_p->insert(std::map<std::string, Tiny>::value_type(tiny.name, tiny));
    }

    std::map<std::string, Tiny> *g_tiny_root_temp_p;

    pthread_mutex_lock(&g_tiny_root_lock);
    g_tiny_root_temp_p = g_tiny_root_master_p;
    g_tiny_root_master_p = g_tiny_root_slaver_p;
    g_tiny_root_slaver_p = g_tiny_root_temp_p;
    g_tiny_root_slaver_p->clear();
    pthread_mutex_unlock(&g_tiny_root_lock);

    LOG_DEBUG("[load handler] load tiny success");

    struct evbuffer *rebuf = evbuffer_new();
    if (!rebuf) {
        evhttp_send_reply(req, HTTP_INTERNAL, "internal error", NULL);
        LOG_ERROR("[load handler] evbuffer_new() failed");
        return;
    }
    evbuffer_add_printf(rebuf, "{\"status\":0}");
    evhttp_send_reply(req, HTTP_OK, "OK", rebuf);
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

    g_tiny_root_master_p = &g_tiny_root_master;
    g_tiny_root_slaver_p = &g_tiny_root_slaver;

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
        evhttp_set_cb(httpd, "/dump", dump_handler, NULL);
        evhttp_set_cb(httpd, "/load", load_handler, NULL);
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

    struct stat sb;
    if (stat("log", &sb) == -1) {
        if (mkdir("log", 0755) < 0) {
            printf("mkdir log failed\n");
            return 1;
        }
    } else {
        if ((sb.st_mode & S_IFMT) != S_IFDIR) {
            if (unlink("log") != 0) {
                printf("I need ./log to be a directory, but it dose not, and rm failed");
                return 1;
            }
        }
    }

    my_log_init(".", "log/tiny_op.log", "log/tiny_op.log.we", 16);

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
