#include "needle/util/curl_client.h"
#include "needle/util/logger.h"

#ifdef NEEDLE_HAS_CURL
#include <curl/curl.h>
#endif

namespace needle {

#ifdef NEEDLE_HAS_CURL

namespace {

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* response = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    response->append(ptr, total);
    return total;
}

} // anonymous namespace

CurlClient::CurlClient() {
    handle_ = curl_easy_init();
}

CurlClient::~CurlClient() {
    if (handle_) {
        curl_easy_cleanup(static_cast<CURL*>(handle_));
    }
}

Result<HttpResponse> CurlClient::post_json(const std::string& url,
                                            const std::vector<std::string>& headers,
                                            const std::string& body,
                                            long timeout_ms) const {
    if (!handle_) {
        return Result<HttpResponse>::failure("curl handle not initialized");
    }

    CURL* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Build headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    NEEDLE_LOG_DEBUG("curl", "POST %s (%ld bytes, timeout %ldms)",
                     url.c_str(), static_cast<long>(body.size()), timeout_ms);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        std::string err = std::string("curl POST failed: ") + curl_easy_strerror(res);
        NEEDLE_LOG_ERROR("curl", "%s", err.c_str());
        return Result<HttpResponse>::failure(err);
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    NEEDLE_LOG_DEBUG("curl", "POST response: HTTP %ld, %zu bytes",
                     http_code, response_body.size());

    HttpResponse resp;
    resp.status_code = http_code;
    resp.body = std::move(response_body);
    return Result<HttpResponse>::success(std::move(resp));
}

Result<HttpResponse> CurlClient::get(const std::string& url,
                                      const std::vector<std::string>& headers,
                                      long timeout_ms) const {
    if (!handle_) {
        return Result<HttpResponse>::failure("curl handle not initialized");
    }

    CURL* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Build headers
    struct curl_slist* header_list = nullptr;
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    NEEDLE_LOG_DEBUG("curl", "GET %s (timeout %ldms)", url.c_str(), timeout_ms);

    CURLcode res = curl_easy_perform(curl);
    if (header_list) curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        std::string err = std::string("curl GET failed: ") + curl_easy_strerror(res);
        NEEDLE_LOG_ERROR("curl", "%s", err.c_str());
        return Result<HttpResponse>::failure(err);
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    NEEDLE_LOG_DEBUG("curl", "GET response: HTTP %ld, %zu bytes",
                     http_code, response_body.size());

    HttpResponse resp;
    resp.status_code = http_code;
    resp.body = std::move(response_body);
    return Result<HttpResponse>::success(std::move(resp));
}

#endif // NEEDLE_HAS_CURL

} // namespace needle
