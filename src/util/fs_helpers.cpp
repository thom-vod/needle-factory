#include "needle/util/fs_helpers.h"
#include "needle/platform/platform.h"
#include "needle/platform/portable_time.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace needle {

bool mkdir_p(const std::string& path) {
    return platform::mkdir_p(path);
}

bool path_exists(const std::string& path) {
    return platform::file_exists(path);
}

bool is_file(const std::string& path) {
    return platform::is_regular_file(path);
}

bool is_directory(const std::string& path) {
    return platform::is_directory(path);
}

bool remove_recursive(const std::string& path) {
    return platform::remove_recursive(path);
}

std::string sanitize_for_filename(const std::string& input) {
    std::string result;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == ' ' || c == '-') {
            result += '_';
        }
        // Other characters are dropped
    }
    return result;
}

std::string dot_stem_from_source(const std::string& dot_source) {
    // Extract graph label attribute: label="..."
    std::string label;
    auto pos = dot_source.find("label=");
    if (pos != std::string::npos) {
        pos = dot_source.find('"', pos);
        if (pos != std::string::npos) {
            auto end = dot_source.find('"', pos + 1);
            if (end != std::string::npos) {
                label = dot_source.substr(pos + 1, end - pos - 1);
            }
        }
    }
    std::string stem = sanitize_for_filename(label);
    return stem.empty() ? "design" : stem;
}

std::string dot_stem_from_filename(const std::string& filename) {
    // Strip directory prefix
    std::string base = filename;
    auto sep = base.rfind('/');
    if (sep != std::string::npos) base = base.substr(sep + 1);
#ifdef _WIN32
    sep = base.rfind('\\');
    if (sep != std::string::npos) base = base.substr(sep + 1);
#endif
    // Strip .dot or .gv extension
    auto dot = base.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = base.substr(dot);
        if (ext == ".dot" || ext == ".gv") {
            base = base.substr(0, dot);
        }
    }
    return base.empty() ? "design" : base;
}

void archive_stage_files(const std::string& stage_dir) {
    static const char* names[] = {
        "prompt.md", "response.md", "status.json", "debug.log"
    };
    for (const char* name : names) {
        std::string path = stage_dir + "/" + name;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;

        // Format mtime as MMDDHHMM
        struct tm tm_buf;
        localtime_r(&st.st_mtime, &tm_buf);
        char ts[16];
        std::strftime(ts, sizeof(ts), "%m%d%H%M", &tm_buf);

        // Split name into base and extension
        std::string sname(name);
        std::string base, ext;
        auto dot = sname.rfind('.');
        if (dot != std::string::npos) {
            base = sname.substr(0, dot);
            ext = sname.substr(dot);
        } else {
            base = sname;
        }

        // Rename to base-MMDDHHMM.ext (append -2, -3 if collision)
        std::string dest = stage_dir + "/" + base + "-" + ts + ext;
        if (platform::file_exists(dest)) {
            for (int i = 2; i <= 99; ++i) {
                dest = stage_dir + "/" + base + "-" + ts + "-" + std::to_string(i) + ext;
                if (!platform::file_exists(dest)) break;
            }
        }
        std::rename(path.c_str(), dest.c_str());
    }
}

} // namespace needle
