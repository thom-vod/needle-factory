#pragma once

#include <string>
#include <vector>
#include "needle/model/result.h"

namespace needle {

struct HttpResponse {
    long status_code;
    std::string body;
};

#ifdef NEEDLE_HAS_CURL

class CurlClient {
public:
    CurlClient();
    ~CurlClient();

    // Disable copy (handle is unique)
    CurlClient(const CurlClient&) = delete;
    CurlClient& operator=(const CurlClient&) = delete;

    // POST JSON body, return response
    Result<HttpResponse> post_json(const std::string& url,
                                    const std::vector<std::string>& headers,
                                    const std::string& body,
                                    long timeout_ms = 60000) const;

    // GET request, return response
    Result<HttpResponse> get(const std::string& url,
                              const std::vector<std::string>& headers,
                              long timeout_ms = 60000) const;

private:
    void* handle_;  // CURL* stored as void* to avoid leaking curl.h
};

#else

// Stub when libcurl not available
class CurlClient {
public:
    Result<HttpResponse> post_json(const std::string&, const std::vector<std::string>&,
                                    const std::string&, long = 60000) const {
        return Result<HttpResponse>::failure("deep research API requires libcurl (not found at build time)");
    }
    Result<HttpResponse> get(const std::string&, const std::vector<std::string>&,
                              long = 60000) const {
        return Result<HttpResponse>::failure("deep research API requires libcurl (not found at build time)");
    }
};

#endif

} // namespace needle
