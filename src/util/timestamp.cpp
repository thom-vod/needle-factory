#include "needle/util/timestamp.h"

#include <ctime>
#include <cstring>

namespace needle {

std::string utc_timestamp_now_dashes() {
    std::time_t now = std::time(nullptr);
    std::tm utc_tm;
    std::memset(&utc_tm, 0, sizeof(utc_tm));
#ifdef _WIN32
    gmtime_s(&utc_tm, &now);
#else
    gmtime_r(&now, &utc_tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%SZ", &utc_tm);
    return std::string(buf);
}

} // namespace needle
