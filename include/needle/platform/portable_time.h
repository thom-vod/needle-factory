#pragma once

#include <ctime>
#include <cstring>
#include <cctype>

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
// Supports: %Y %m %d %H %M %S (numeric), %I (12-hour), %b/%h (abbreviated
// month name, case-insensitive), %p/%P (AM/PM, case-insensitive, adjusts the
// hour set by %I), and literal characters. A space in the format matches zero
// or more whitespace, as POSIX strptime does. Numeric fields accept 1..width
// digits so unpadded values ("Jan 1, 2099") parse as well as zero-padded ones.
inline char* strptime(const char* s, const char* fmt, struct tm* tm) {
    static const char* const k_months[12] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"};
    std::memset(tm, 0, sizeof(*tm));
    while (*fmt) {
        if (*fmt == '%') {
            ++fmt;
            switch (*fmt) {
                case 'Y': case 'm': case 'd':
                case 'H': case 'I': case 'M': case 'S': {
                    int width = (*fmt == 'Y') ? 4 : 2;
                    int offset = 0;
                    int* field = nullptr;
                    switch (*fmt) {
                        case 'Y': field = &tm->tm_year; offset = -1900; break;
                        case 'm': field = &tm->tm_mon;  offset = -1;    break;
                        case 'd': field = &tm->tm_mday; break;
                        case 'H': case 'I': field = &tm->tm_hour; break;
                        case 'M': field = &tm->tm_min;  break;
                        default:  field = &tm->tm_sec;  break;  // 'S'
                    }
                    if (*s < '0' || *s > '9') return nullptr;
                    int val = 0;
                    for (int i = 0; i < width && *s >= '0' && *s <= '9'; ++i) {
                        val = val * 10 + (*s - '0');
                        ++s;
                    }
                    *field = val + offset;
                    break;
                }
                case 'b': case 'h': {  // abbreviated month name
                    char abbr[3];
                    for (int i = 0; i < 3; ++i) {
                        if (!s[i]) return nullptr;
                        abbr[i] = (char)std::tolower((unsigned char)s[i]);
                    }
                    int mon = -1;
                    for (int i = 0; i < 12; ++i) {
                        if (std::strncmp(abbr, k_months[i], 3) == 0) { mon = i; break; }
                    }
                    if (mon < 0) return nullptr;
                    tm->tm_mon = mon;
                    s += 3;
                    break;
                }
                case 'p': case 'P': {  // AM/PM, adjusts hour parsed by %I
                    char a = (char)std::tolower((unsigned char)s[0]);
                    char m = a ? (char)std::tolower((unsigned char)s[1]) : 0;
                    if (m != 'm') return nullptr;
                    if (a == 'p') { if (tm->tm_hour != 12) tm->tm_hour += 12; }
                    else if (a == 'a') { if (tm->tm_hour == 12) tm->tm_hour = 0; }
                    else return nullptr;
                    s += 2;
                    break;
                }
                default: return nullptr;
            }
            ++fmt;
        } else if (*fmt == ' ') {
            while (*s == ' ' || *s == '\t') ++s;
            ++fmt;
        } else {
            if (*s != *fmt) return nullptr;
            ++s; ++fmt;
        }
    }
    return const_cast<char*>(s);
}

#endif
