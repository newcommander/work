#include "logic.h"

CURL *g_url;
char g_curl_errbuf[CURL_ERROR_SIZE];
pthread_mutex_t g_recv_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_recv_buf = "";

static size_t recv_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
    if (buffer) {
        pthread_mutex_lock(&g_recv_lock);
        g_recv_buf += (char*)buffer;
        pthread_mutex_unlock(&g_recv_lock);
    }
    return size * nmemb;
}

int report_init()
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

int report_clean()
{
    curl_easy_cleanup(g_url);
    g_url = NULL;
    return 0;
}

static int report_send(std::string url, std::string post_data)
{
    int status = 0;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: ");
    headers = curl_slist_append(headers, "Content-Type: ");

    curl_easy_setopt(g_url, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_url, CURLOPT_URL, url.c_str());
    curl_easy_setopt(g_url, CURLOPT_POSTFIELDS, post_data.c_str());
    status = curl_easy_perform(g_url);
    curl_slist_free_all(headers);

    if (status != CURLE_OK) {
        LOG_ERROR("[report send] repotr failed: %s", g_curl_errbuf);
        return 1;
    }

    return 0;
}

static Json::Value do_query(std::string uri, std::string post_data)
{
    Json::Value value;
    Json::Reader reader;
    std::string url = "http://localhost:8888/";

    int status = report_send(url + uri, post_data);
    if (status != CURLE_OK) {
        LOG_ERROR("[do query] request failed, ret code: %d", status);
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

int do_logic()
{
    Json::Value value;
    value = do_query("create_tiny?seed=srog", "");
    if (!value.isMember("names")) {
        LOG_ERROR("[do_logic] request for create_tiny failed, no \"names\" return");
        return 1;
    }
    if (!value["names"].isArray()) {
        LOG_ERROR("[do_logic] request for create_tiny failed, item \"names\" is not a string");
        return 1;
    }
    if (value["names"].size() == 0) {
        LOG_ERROR("[do_logic] request for create_tiny failed, array \"names\" has no item");
        return 1;
    }
    return 0;
}
