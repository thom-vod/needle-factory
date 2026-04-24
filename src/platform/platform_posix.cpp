#ifndef _WIN32

#include "needle/platform/platform.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

namespace needle {
namespace platform {

bool is_absolute_path(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + '/' + b;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "";
}

std::string temp_dir() {
    const char* t = std::getenv("TMPDIR");
    return t ? std::string(t) : "/tmp";
}

std::string getcwd_str() {
    char buf[4096];
    if (::getcwd(buf, sizeof(buf))) {
        return std::string(buf);
    }
    return ".";
}

bool make_dir(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return true;
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool mkdir_p(const std::string& path) {
    if (path.empty()) return true;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return true;
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        mkdir_p(path.substr(0, pos));
    }
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> list_directory(const std::string& path) {
    std::vector<std::string> result;
    DIR* d = ::opendir(path.c_str());
    if (!d) return result;
    struct dirent* entry;
    while ((entry = ::readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            result.push_back(name);
        }
    }
    ::closedir(d);
    return result;
}

void list_files_recursive(const std::string& dir, const std::string& prefix,
                          std::vector<std::string>& out, int max_depth, int depth) {
    if (depth >= max_depth) return;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct dirent* entry;
    while ((entry = ::readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || name == ".git") continue;
        std::string full = dir + "/" + name;
        std::string rel = prefix.empty() ? name : prefix + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            list_files_recursive(full, rel, out, max_depth, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back(rel);
        }
    }
    ::closedir(d);
}

bool command_exists(const std::string& command) {
    std::string cmd = "command -v " + command + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string find_chrome() {
    // macOS
    const char* mac_path = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
    if (file_exists(mac_path)) return mac_path;

    // Linux common locations
    const char* linux_paths[] = {
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium-browser",
        "/usr/bin/chromium",
        nullptr
    };
    for (const char** p = linux_paths; *p; ++p) {
        if (file_exists(*p)) return *p;
    }
    return "";
}

std::string open_command() {
#ifdef __APPLE__
    return "open";
#else
    return "xdg-open";
#endif
}

std::string make_temp_file(const std::string& prefix) {
    std::string tmpl = temp_dir() + "/" + prefix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) return "";
    ::close(fd);
    return std::string(buf.data());
}

bool remove_file(const std::string& path) {
    return ::unlink(path.c_str()) == 0;
}

bool remove_dir(const std::string& path) {
    return ::rmdir(path.c_str()) == 0;
}

bool remove_recursive(const std::string& path) {
    if (!file_exists(path)) return true;
    if (is_regular_file(path)) return remove_file(path);
    if (!is_directory(path)) return false;
    auto entries = list_directory(path);
    for (const auto& entry : entries) {
        std::string child = path + "/" + entry;
        if (!remove_recursive(child)) return false;
    }
    return remove_dir(path);
}

} // namespace platform
} // namespace needle

#endif // !_WIN32
