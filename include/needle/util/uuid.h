#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <random>

namespace needle {

// Generate a random v4 UUID.
inline std::string random_uuid() {
    static std::mt19937_64 rng(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint64_t h1 = rng();
    uint64_t h2 = rng();

    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((h1 >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; ++i) bytes[8 + i] = static_cast<unsigned char>((h2 >> (i * 8)) & 0xFF);

    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

// Generate a deterministic UUID from a string using FNV-1a hash.
// Same input always produces the same UUID. Format: v4-style UUID.
inline std::string deterministic_uuid(const std::string& input) {
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 14695981039346656037ULL;
    for (size_t i = 0; i < input.size(); ++i) {
        h1 ^= static_cast<uint64_t>(static_cast<unsigned char>(input[i]));
        h1 *= 1099511628211ULL;
    }
    for (size_t i = 0; i < input.size(); ++i) {
        h2 ^= static_cast<uint64_t>(static_cast<unsigned char>(input[i])) + 0x9e;
        h2 *= 1099511628211ULL;
    }

    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((h1 >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; ++i) bytes[8 + i] = static_cast<unsigned char>((h2 >> (i * 8)) & 0xFF);

    bytes[6] = (bytes[6] & 0x0F) | 0x40;  // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80;  // variant 1

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

} // namespace needle
