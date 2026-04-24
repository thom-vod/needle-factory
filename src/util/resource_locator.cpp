#include "needle/util/resource_locator.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"

#include <climits>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace needle {

ResourceLocator::ResourceLocator()
    : exe_dir_(detect_exe_dir())
    , exe_path_(detect_exe_path())
{
    // Resolve prefix: parent of exe_dir (e.g., build/ -> project root)
    std::string candidate = exe_dir_ + "/..";
    char resolved[PATH_MAX];
#ifdef _WIN32
    if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
#else
    if (realpath(candidate.c_str(), resolved)) {
#endif
        prefix_ = std::string(resolved);
    } else {
        prefix_ = candidate;
    }
    build_search_roots();
}

ResourceLocator::ResourceLocator(const std::string& exe_dir)
    : exe_dir_(exe_dir)
    , exe_path_(detect_exe_path())
{
    std::string candidate = exe_dir_ + "/..";
    char resolved[PATH_MAX];
#ifdef _WIN32
    if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
#else
    if (realpath(candidate.c_str(), resolved)) {
#endif
        prefix_ = std::string(resolved);
    } else {
        prefix_ = candidate;
    }
    build_search_roots();
}

void ResourceLocator::build_search_roots() {
    // 1. Install prefix: <prefix>/share/needle/
    search_roots_.push_back(prefix_ + "/share/needle");
    // 2. Build tree: <prefix>/ (exe is in build/, prefix is project root)
    search_roots_.push_back(prefix_);
    // 3. CWD fallback
    search_roots_.push_back(".");
}

Result<std::string> ResourceLocator::find(const std::string& category,
                                           const std::string& filename) const {
    std::string tried;
    for (const auto& root : search_roots_) {
        std::string candidate = root + "/" + category + "/" + filename;
        NEEDLE_LOG_DEBUG("resource", "searching %s", candidate.c_str());
        if (path_exists(candidate)) {
            if (root == ".") {
                NEEDLE_LOG_WARN("resource", "falling back to CWD for %s/%s", category.c_str(), filename.c_str());
            }
            // Resolve to absolute path
#ifdef _WIN32
            char resolved[PATH_MAX];
            if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
                NEEDLE_LOG_INFO("resource", "found %s/%s at %s", category.c_str(), filename.c_str(), resolved);
                return Result<std::string>::success(std::string(resolved));
            }
#else
            char resolved[PATH_MAX];
        #ifdef _WIN32
    if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
#else
    if (realpath(candidate.c_str(), resolved)) {
#endif
                NEEDLE_LOG_INFO("resource", "found %s/%s at %s", category.c_str(), filename.c_str(), resolved);
                return Result<std::string>::success(std::string(resolved));
            }
#endif
            NEEDLE_LOG_INFO("resource", "found %s/%s at %s", category.c_str(), filename.c_str(), candidate.c_str());
            return Result<std::string>::success(candidate);
        }
        if (!tried.empty()) tried += ", ";
        tried += candidate;
    }
    return Result<std::string>::failure(
        "resource not found: " + category + "/" + filename + " (tried: " + tried + ")");
}

Result<std::string> ResourceLocator::find_dir(const std::string& category) const {
    std::string tried;
    for (const auto& root : search_roots_) {
        std::string candidate = root + "/" + category;
        if (is_directory(candidate)) {
#ifdef _WIN32
            char resolved[PATH_MAX];
            if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
                return Result<std::string>::success(std::string(resolved));
            }
#else
            char resolved[PATH_MAX];
        #ifdef _WIN32
    if (GetFullPathNameA(candidate.c_str(), PATH_MAX, resolved, nullptr)) {
#else
    if (realpath(candidate.c_str(), resolved)) {
#endif
                return Result<std::string>::success(std::string(resolved));
            }
#endif
            return Result<std::string>::success(candidate);
        }
        if (!tried.empty()) tried += ", ";
        tried += candidate;
    }
    return Result<std::string>::failure(
        "directory not found: " + category + " (tried: " + tried + ")");
}

Result<std::string> ResourceLocator::find_executable() const {
    if (!exe_path_.empty() && exe_path_ != ".") {
        return Result<std::string>::success(exe_path_);
    }
    return Result<std::string>::failure("could not determine executable path");
}

const std::string& ResourceLocator::exe_dir() const {
    return exe_dir_;
}

const std::string& ResourceLocator::prefix() const {
    return prefix_;
}

const std::vector<std::string>& ResourceLocator::search_roots() const {
    return search_roots_;
}

std::string ResourceLocator::detect_exe_dir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved)) {
            std::string p(resolved);
            auto slash = p.find_last_of('/');
            if (slash != std::string::npos) return p.substr(0, slash);
        }
    }
    // Retry with dynamic allocation if buffer was too small
    if (size > sizeof(buf)) {
        std::vector<char> dynbuf(size);
        if (_NSGetExecutablePath(dynbuf.data(), &size) == 0) {
            char resolved[PATH_MAX];
            if (realpath(dynbuf.data(), resolved)) {
                std::string p(resolved);
                auto slash = p.find_last_of('/');
                if (slash != std::string::npos) return p.substr(0, slash);
            }
        }
    }
#elif defined(__linux__)
    char resolved[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (len > 0) {
        resolved[len] = '\0';
        std::string p(resolved);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#elif defined(_WIN32)
    char resolved[PATH_MAX];
    DWORD len = GetModuleFileNameA(nullptr, resolved, PATH_MAX);
    if (len > 0 && len < PATH_MAX) {
        std::string p(resolved);
        auto slash = p.find_last_of("\\/");
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#endif
    return ".";
}

std::string ResourceLocator::detect_exe_path() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved)) {
            return std::string(resolved);
        }
    }
    if (size > sizeof(buf)) {
        std::vector<char> dynbuf(size);
        if (_NSGetExecutablePath(dynbuf.data(), &size) == 0) {
            char resolved[PATH_MAX];
            if (realpath(dynbuf.data(), resolved)) {
                return std::string(resolved);
            }
        }
    }
#elif defined(__linux__)
    char resolved[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (len > 0) {
        resolved[len] = '\0';
        return std::string(resolved);
    }
#elif defined(_WIN32)
    char resolved[PATH_MAX];
    DWORD len = GetModuleFileNameA(nullptr, resolved, PATH_MAX);
    if (len > 0 && len < PATH_MAX) {
        return std::string(resolved);
    }
#endif
    return ".";
}

} // namespace needle
