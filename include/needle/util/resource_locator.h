#pragma once

#include <string>
#include <vector>
#include "needle/model/result.h"

namespace needle {

class ResourceLocator {
public:
    ResourceLocator();
    explicit ResourceLocator(const std::string& exe_dir);

    /// Find a file by category and filename across the search roots.
    /// Search order:
    ///   1. <prefix>/share/needle/<category>/<filename>  (install prefix)
    ///   2. <prefix>/<category>/<filename>               (build tree)
    ///   3. ./<category>/<filename>                      (CWD fallback)
    Result<std::string> find(const std::string& category, const std::string& filename) const;

    /// Find a directory by category name across the search roots.
    Result<std::string> find_dir(const std::string& category) const;

    /// Return the path to the running executable.
    Result<std::string> find_executable() const;

    const std::string& exe_dir() const;
    const std::string& prefix() const;
    const std::vector<std::string>& search_roots() const;

private:
    static std::string detect_exe_dir();
    static std::string detect_exe_path();
    void build_search_roots();

    std::string exe_dir_;
    std::string prefix_;
    std::string exe_path_;
    std::vector<std::string> search_roots_;
};

} // namespace needle
