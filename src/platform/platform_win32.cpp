#ifdef _WIN32

#include "needle/platform/platform.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace needle {
namespace platform {

bool is_absolute_path(const std::string& path) {
    if (path.empty()) return false;
    // UNC path: \\server\share
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return true;
    // Drive letter: C:\ or C:/
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0]))
        && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    // MSYS/MinGW style: /c/ or /d/
    if (path.size() >= 3 && path[0] == '/'
        && std::isalpha(static_cast<unsigned char>(path[1]))
        && path[2] == '/') {
        return true;
    }
    // Plain forward slash absolute (Git Bash)
    if (path[0] == '/') return true;
    return false;
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + '/' + b;
}

std::string home_dir() {
    // Try USERPROFILE first (native Windows), then HOME (MSYS2/Git Bash)
    const char* h = std::getenv("USERPROFILE");
    if (h) return std::string(h);
    h = std::getenv("HOME");
    return h ? std::string(h) : "";
}

std::string temp_dir() {
    char buf[MAX_PATH + 1];
    DWORD len = GetTempPathA(sizeof(buf), buf);
    if (len > 0 && len < sizeof(buf)) {
        std::string result(buf, len);
        // Remove trailing backslash
        while (!result.empty() && (result.back() == '\\' || result.back() == '/')) {
            result.pop_back();
        }
        return result;
    }
    const char* t = std::getenv("TEMP");
    if (t) return std::string(t);
    t = std::getenv("TMP");
    return t ? std::string(t) : "C:\\Windows\\Temp";
}

std::string getcwd_str() {
    char buf[4096];
    if (::_getcwd(buf, sizeof(buf))) {
        return std::string(buf);
    }
    return ".";
}

bool make_dir(const std::string& path) {
    struct _stat st;
    if (::_stat(path.c_str(), &st) == 0) return true;
    return ::_mkdir(path.c_str()) == 0;
}

bool mkdir_p(const std::string& path) {
    if (path.empty()) return true;
    struct _stat st;
    if (::_stat(path.c_str(), &st) == 0) return true;

    // Find last separator
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        // Don't recurse past drive letter (e.g., "C:")
        if (!(pos == 2 && path[1] == ':')) {
            mkdir_p(path.substr(0, pos));
        }
    }
    return ::_mkdir(path.c_str()) == 0 || errno == EEXIST;
}

bool file_exists(const std::string& path) {
    struct _stat st;
    return ::_stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct _stat st;
    return ::_stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
}

bool is_regular_file(const std::string& path) {
    struct _stat st;
    return ::_stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFREG);
}

std::vector<std::string> list_directory(const std::string& path) {
    std::vector<std::string> result;
    std::string search = path + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;
    do {
        std::string name = fd.cFileName;
        if (name != "." && name != "..") {
            result.push_back(name);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return result;
}

void list_files_recursive(const std::string& dir, const std::string& prefix,
                          std::vector<std::string>& out, int max_depth, int depth) {
    if (depth >= max_depth) return;
    std::string search = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == ".." || name == ".git") continue;
        std::string full = dir + "\\" + name;
        std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_files_recursive(full, rel, out, max_depth, depth + 1);
        } else {
            out.push_back(rel);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

bool command_exists(const std::string& command) {
    std::string cmd = "where " + command + " >nul 2>nul";
    return std::system(cmd.c_str()) == 0;
}

std::string find_chrome() {
    // Common Windows Chrome locations
    const char* paths[] = {
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        nullptr
    };
    for (const char** p = paths; *p; ++p) {
        if (file_exists(*p)) return *p;
    }

    // Try LOCALAPPDATA
    const char* local = std::getenv("LOCALAPPDATA");
    if (local) {
        std::string path = std::string(local) + "\\Google\\Chrome\\Application\\chrome.exe";
        if (file_exists(path)) return path;
    }

    return "";
}

std::string open_command() {
    return "start";
}

std::string make_temp_file(const std::string& prefix) {
    std::string dir = temp_dir();
    char tmp_name[MAX_PATH];
    // GetTempFileNameA creates a unique file
    if (GetTempFileNameA(dir.c_str(), prefix.c_str(), 0, tmp_name) == 0) {
        return "";
    }
    return std::string(tmp_name);
}

bool remove_file(const std::string& path) {
    return ::_unlink(path.c_str()) == 0 || DeleteFileA(path.c_str());
}

bool remove_dir(const std::string& path) {
    return ::_rmdir(path.c_str()) == 0 || RemoveDirectoryA(path.c_str());
}

bool remove_recursive(const std::string& path) {
    struct _stat st;
    if (::_stat(path.c_str(), &st) != 0) return true; // doesn't exist
    if (!(st.st_mode & _S_IFDIR)) {
        return remove_file(path);
    }
    // Recurse into directory
    auto entries = list_directory(path);
    for (const auto& e : entries) {
        std::string child = path + "\\" + e;
        if (!remove_recursive(child)) return false;
    }
    return remove_dir(path);
}

} // namespace platform
} // namespace needle

#endif // _WIN32
