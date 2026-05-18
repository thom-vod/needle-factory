#include "needle/troubleshoot/write_hook.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "needle/platform/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <limits.h>
#include <stdlib.h>
#endif

namespace needle {

namespace {

std::string normalize_separators(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string trim_trailing_separators(std::string path) {
    path = normalize_separators(path);
    while (path.size() > 1 && path.back() == '/') {
        if (path.size() == 3 && path[1] == ':') break;
        path.pop_back();
    }
    return path;
}

std::string lexical_normalize(const std::string& input) {
    std::string path = normalize_separators(input);
    std::string prefix;
    size_t pos = 0;

    if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
        prefix = "//";
        pos = 2;
    } else if (path.size() >= 3 && path[1] == ':' && path[2] == '/') {
        prefix = path.substr(0, 3);
        pos = 3;
    } else if (!path.empty() && path[0] == '/') {
        prefix = "/";
        pos = 1;
    }

    std::vector<std::string> parts;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        std::string part = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (part.empty() || part == ".") {
            // Skip.
        } else if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            } else if (prefix.empty()) {
                parts.push_back(part);
            }
        } else {
            parts.push_back(part);
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    std::ostringstream out;
    out << prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0 || (!prefix.empty() && prefix.back() != '/')) out << "/";
        out << parts[i];
    }
    std::string result = out.str();
    if (result.empty()) return prefix.empty() ? "." : prefix;
    return trim_trailing_separators(result);
}

#ifndef _WIN32
bool real_path(const std::string& path, std::string& out) {
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr) return false;
    out = trim_trailing_separators(resolved);
    return true;
}
#endif

std::string canonical_path_tolerant(const std::string& path) {
    if (path.empty()) return "";

#ifdef _WIN32
    char buf[MAX_PATH + 1];
    DWORD len = GetFullPathNameA(path.c_str(), static_cast<DWORD>(sizeof(buf)), buf, nullptr);
    if (len == 0 || len >= sizeof(buf)) {
        return lexical_normalize(path);
    }
    return lexical_normalize(buf);
#else
    std::string resolved;
    if (real_path(path, resolved)) return resolved;

    std::string normalized = normalize_separators(path);
    if (!platform::is_absolute_path(normalized)) {
        normalized = normalize_separators(platform::path_join(platform::getcwd_str(), normalized));
    }

    size_t cut = normalized.size();
    while (true) {
        std::string candidate = normalized.substr(0, cut);
        if (candidate.empty()) candidate = "/";
        candidate = trim_trailing_separators(candidate);
        if (real_path(candidate, resolved)) {
            std::string remainder = normalized.substr(cut);
            while (!remainder.empty() && remainder.front() == '/') {
                remainder.erase(remainder.begin());
            }
            if (!remainder.empty()) {
                resolved = trim_trailing_separators(resolved) + "/" + remainder;
            }
            return lexical_normalize(resolved);
        }
        if (cut == 0) break;
        size_t slash = normalized.find_last_of('/', cut == normalized.size() ? std::string::npos : cut - 1);
        if (slash == std::string::npos) break;
        cut = slash;
    }

    return lexical_normalize(normalized);
#endif
}

bool same_or_child(const std::string& path, const std::string& root) {
    if (path.empty() || root.empty()) return false;
    if (path == root) return true;
    if (root == "/") return !path.empty() && path[0] == '/';
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path[root.size()] == '/';
}

bool is_write_tool(const std::string& name) {
    return name == "Edit" || name == "Write";
}

} // namespace

// m-c: reject any path containing a literal `~` segment. If a shell or agent
// expands `~` before writing, the canonicaliser sees the expanded form and
// happily clears it — but the on-disk write may land under $HOME. The audit
// should refuse the call rather than wave it through.
bool path_contains_tilde_segment(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty()) return false;
    if (normalized == "~") return true;
    if (normalized.size() >= 2 && normalized.compare(0, 2, "~/") == 0) return true;
    // Check for `/~/` or trailing `/~`.
    for (size_t i = 0; i + 1 < normalized.size(); ++i) {
        if (normalized[i] == '/' && normalized[i + 1] == '~') {
            // Either `/~` at end, or `/~/...`.
            if (i + 2 == normalized.size() || normalized[i + 2] == '/') return true;
        }
    }
    return false;
}

bool file_write_allowed(const std::string& abs_path,
                        const std::string& project_dir,
                        const std::string& session_dir) {
    // m-c: literal `~` segments are rejected outright — we can't know
    // whether the caller intended a literal file named `~` (rare) or an
    // unexpanded home-dir reference (likely). Refuse and let the operator
    // disambiguate.
    if (path_contains_tilde_segment(abs_path)) return false;
    const std::string path = canonical_path_tolerant(abs_path);
    const std::string project = canonical_path_tolerant(project_dir);
    const std::string session = canonical_path_tolerant(session_dir);
    return same_or_child(path, project) || same_or_child(path, session);
}

std::vector<HookViolation> audit_events_ndjson(const std::string& events_ndjson_path,
                                               const std::string& project_dir,
                                               const std::string& session_dir) {
    std::vector<HookViolation> violations;
    std::ifstream in(events_ndjson_path);
    if (!in.is_open()) return violations;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (const std::exception&) {
            continue;
        }

        if (!j.is_object() || j.value("type", "") != "assistant") continue;

        // m-b: Claude's stream-json can deliver the assistant block content
        // array as either `j.message.content` (current shape) or `j.content`
        // (alternate shape the stream parser also tolerates). Mirror the
        // parser's tolerance so a shape drift doesn't silently disable the
        // post-run audit while SSE continues to surface tool calls.
        const nlohmann::json* content_ptr = nullptr;
        if (j.contains("message") && j["message"].is_object() &&
            j["message"].contains("content") && j["message"]["content"].is_array()) {
            content_ptr = &j["message"]["content"];
        } else if (j.contains("content") && j["content"].is_array()) {
            content_ptr = &j["content"];
        }
        if (!content_ptr) continue;

        for (const auto& block : *content_ptr) {
            if (!block.is_object() || block.value("type", "") != "tool_use") continue;
            const std::string tool = block.value("name", "");
            if (!is_write_tool(tool)) continue;
            if (!block.contains("input") || !block["input"].is_object()) continue;
            const auto& input = block["input"];
            if (!input.contains("file_path") || !input["file_path"].is_string()) continue;

            const std::string file_path = input["file_path"].get<std::string>();
            const std::string abs_path = platform::is_absolute_path(file_path)
                ? file_path
                : platform::path_join(project_dir, file_path);
            if (!file_write_allowed(abs_path, project_dir, session_dir)) {
                HookViolation violation;
                violation.tool = tool;
                violation.file_path = file_path;
                violation.tool_use_id = block.value("id", "");
                violations.push_back(std::move(violation));
            }
        }
    }

    return violations;
}

} // namespace needle
