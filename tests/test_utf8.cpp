#include <catch2/catch.hpp>
#include "needle/util/utf8.h"
#include "needle/model/context.h"

using namespace needle;

// The ellipsis U+2026 encodes to the three bytes E2 80 A6. Cutting at byte
// offset 1 or 2 inside it is exactly the bug this guards against.
static const std::string kEllipsis = "\xE2\x80\xA6";  // …
static const std::string kEmDash = "\xE2\x80\x94";    // —

static bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;  // stray continuation or invalid lead
        for (size_t k = 1; k <= need; ++k) {
            if (i + k >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

TEST_CASE("utf8::truncate_front never splits a multibyte char", "[utf8]") {
    // "ab" + "…"  =>  bytes: 61 62 E2 80 A6
    std::string s = "ab" + kEllipsis;
    REQUIRE(s.size() == 5);

    // Cutting at byte 3 lands on the ellipsis lead byte: keep "ab".
    REQUIRE(utf8::truncate_front(s, 3) == "ab");
    // Cutting at byte 4 is mid-ellipsis: must back off to "ab", not "ab\xE2".
    REQUIRE(utf8::truncate_front(s, 4) == "ab");
    // Cutting at byte 5 is mid-ellipsis: must back off to "ab".
    REQUIRE(utf8::truncate_front(s, 5) == s);  // size==max, returned whole
    REQUIRE(is_valid_utf8(utf8::truncate_front(s, 4)));
}

TEST_CASE("utf8::truncate_front keeps complete chars up to the limit", "[utf8]") {
    std::string s = kEllipsis + kEmDash + "x";  // 7 bytes: E2 80 A6 E2 80 94 78
    REQUIRE(utf8::truncate_front(s, 6) == kEllipsis + kEmDash);
    REQUIRE(utf8::truncate_front(s, 4) == kEllipsis);  // back off mid em-dash
    REQUIRE(utf8::truncate_front(s, 2) == "");         // back off mid ellipsis
    REQUIRE(is_valid_utf8(utf8::truncate_front(s, 2)));
    REQUIRE(is_valid_utf8(utf8::truncate_front(s, 4)));
}

TEST_CASE("utf8::truncate_front leaves ASCII untouched", "[utf8]") {
    REQUIRE(utf8::truncate_front("hello", 3) == "hel");
    REQUIRE(utf8::truncate_front("hi", 10) == "hi");
}

TEST_CASE("utf8::truncate_back drops a partial leading char", "[utf8]") {
    std::string s = kEllipsis + "tail";  // E2 80 A6 74 61 69 6C, 7 bytes
    // Keeping the last 6 bytes would start mid-ellipsis (80 A6 ...) -> advance
    // forward to the 't', yielding "tail".
    REQUIRE(utf8::truncate_back(s, 6) == "tail");
    REQUIRE(is_valid_utf8(utf8::truncate_back(s, 6)));
    REQUIRE(utf8::truncate_back(s, 4) == "tail");
    REQUIRE(utf8::truncate_back("hello", 3) == "llo");
    REQUIRE(utf8::truncate_back("hi", 10) == "hi");
}

TEST_CASE("Context truncation produces valid UTF-8 at the boundary", "[utf8][context]") {
    size_t saved = Context::max_value_size();
    // Force the cut to land inside a multibyte sequence: pad so that the byte
    // at the limit is a continuation byte of an ellipsis.
    Context::set_max_value_size(4);
    Context ctx;
    ctx.set("k", "ab" + kEllipsis + "cd");  // limit 4 lands mid-ellipsis
    std::string v = ctx.get("k");
    // The kept prefix must be "ab" (ellipsis dropped), then the marker.
    REQUIRE(v.rfind("ab\n\n[truncated", 0) == 0);
    // The kept content before the marker must be valid UTF-8.
    std::string content = v.substr(0, v.find("\n\n[truncated"));
    REQUIRE(is_valid_utf8(content));
    Context::set_max_value_size(saved);
}
