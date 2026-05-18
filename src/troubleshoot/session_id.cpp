#include "needle/troubleshoot/session_id.h"

#include <mutex>
#include <random>
#include <set>

#include "needle/util/timestamp.h"

namespace needle {

namespace {

std::string random_hex_suffix() {
    static const char* digits = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string suffix;
    suffix.reserve(4);
    for (int i = 0; i < 4; ++i) {
        suffix.push_back(digits[dist(rd)]);
    }
    return suffix;
}

} // namespace

std::string make_troubleshoot_session_id() {
    static std::mutex mutex;
    static std::set<std::string> generated;

    std::lock_guard<std::mutex> lock(mutex);
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::string session_id = utc_timestamp_now_dashes() + "-" + random_hex_suffix();
        if (generated.insert(session_id).second) {
            return session_id;
        }
    }
    return utc_timestamp_now_dashes() + "-" + random_hex_suffix();
}

} // namespace needle
