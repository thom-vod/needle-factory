#ifndef NEEDLE_UTIL_UTF8_H
#define NEEDLE_UTIL_UTF8_H

#include <string>

namespace needle {
namespace utf8 {

// A UTF-8 continuation byte has the bit pattern 10xxxxxx. Lead bytes and
// ASCII bytes never match this, so a position whose byte is NOT a
// continuation byte is a valid character boundary.
inline bool is_continuation_byte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// Largest index <= byte_index that lands on a UTF-8 character boundary.
// Assumes the input up to byte_index was itself valid UTF-8 (the only thing
// we can be cut mid-way through is a continuation sequence). Backs up over
// continuation bytes until a lead/ASCII byte (or the start) is reached.
inline size_t boundary_before(const std::string& s, size_t byte_index) {
    if (byte_index > s.size()) byte_index = s.size();
    while (byte_index > 0 &&
           is_continuation_byte(static_cast<unsigned char>(s[byte_index]))) {
        --byte_index;
    }
    return byte_index;
}

// Smallest index >= byte_index that lands on a UTF-8 character boundary.
inline size_t boundary_after(const std::string& s, size_t byte_index) {
    while (byte_index < s.size() &&
           is_continuation_byte(static_cast<unsigned char>(s[byte_index]))) {
        ++byte_index;
    }
    return byte_index;
}

// Keep at most `max_bytes` bytes from the front, never splitting a multibyte
// character. The result is always valid UTF-8 (given valid input) and never
// longer than `max_bytes`.
inline std::string truncate_front(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    return s.substr(0, boundary_before(s, max_bytes));
}

// Keep at most `max_bytes` bytes from the end, dropping any partial leading
// character so the result starts on a boundary.
inline std::string truncate_back(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t start = boundary_after(s, s.size() - max_bytes);
    return s.substr(start);
}

} // namespace utf8
} // namespace needle

#endif // NEEDLE_UTIL_UTF8_H
