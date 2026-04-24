#pragma once

#include <ctime>
#include <cstring>

#ifdef _WIN32

// Windows doesn't provide localtime_r; localtime_s has swapped arguments.
inline struct tm* localtime_r(const time_t* timep, struct tm* result) {
    if (localtime_s(result, timep) == 0) return result;
    return nullptr;
}

// timegm: convert struct tm (UTC) to time_t.  Windows provides _mkgmtime.
inline time_t timegm(struct tm* tm) {
    return _mkgmtime(tm);
}

// Minimal strptime for the subset of format specifiers needle uses.
// Supports: %Y %m %d %H %M %S and literal characters.
inline char* strptime(const char* s, const char* fmt, struct tm* tm) {
    std::memset(tm, 0, sizeof(*tm));
    while (*fmt) {
        if (*fmt == '%') {
            ++fmt;
            int* field = nullptr;
            int width = 0;
            int offset = 0;
            switch (*fmt) {
                case 'Y': field = &tm->tm_year; width = 4; offset = -1900; break;
                case 'm': field = &tm->tm_mon;  width = 2; offset = -1;    break;
                case 'd': field = &tm->tm_mday; width = 2; offset = 0;     break;
                case 'H': field = &tm->tm_hour; width = 2; offset = 0;     break;
                case 'M': field = &tm->tm_min;  width = 2; offset = 0;     break;
                case 'S': field = &tm->tm_sec;  width = 2; offset = 0;     break;
                default: return nullptr;
            }
            int val = 0;
            for (int i = 0; i < width; ++i) {
                if (*s < '0' || *s > '9') return nullptr;
                val = val * 10 + (*s - '0');
                ++s;
            }
            *field = val + offset;
            ++fmt;
        } else {
            if (*s != *fmt) return nullptr;
            ++s; ++fmt;
        }
    }
    return const_cast<char*>(s);
}

#endif
