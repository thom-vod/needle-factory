#pragma once

#include <string>
#include <vector>

namespace needle {
namespace platform {

// Path separator for the current platform
#ifdef _WIN32
constexpr char path_separator = '\\';
constexpr char path_separator_alt = '/';
#else
constexpr char path_separator = '/';
#endif

// Returns true if the given path is absolute (handles drive letters on Windows)
bool is_absolute_path(const std::string& path);

// Join two path components with the correct separator
std::string path_join(const std::string& a, const std::string& b);

// Get the user's home directory (HOME on POSIX, USERPROFILE on Windows)
std::string home_dir();

// Get the system temp directory (/tmp on POSIX, %TEMP% on Windows)
std::string temp_dir();

// Get the current working directory
std::string getcwd_str();

// Create a directory (single level). Returns true on success or if it already exists.
bool make_dir(const std::string& path);

// Create a directory and all parent directories. Returns true on success.
bool mkdir_p(const std::string& path);

// Check if a file or directory exists
bool file_exists(const std::string& path);

// Check if path is a directory
bool is_directory(const std::string& path);

// Check if path is a regular file
bool is_regular_file(const std::string& path);

// List entries in a directory (non-recursive, excludes "." and "..")
std::vector<std::string> list_directory(const std::string& path);

// Recursive file listing (returns relative paths)
void list_files_recursive(const std::string& dir, const std::string& prefix,
                          std::vector<std::string>& out, int max_depth, int depth = 0);

// Check if a command/executable exists on PATH
// Uses "which" on POSIX, "where" on Windows
bool command_exists(const std::string& command);

// Get the path to the system Chrome browser, or empty string if not found
std::string find_chrome();

// Open a URL or file with the system default handler
// Uses "open" on macOS, "xdg-open" on Linux, "start" on Windows
std::string open_command();

// Create a temporary file and return its path. The file is created but closed.
// The caller is responsible for cleanup.
std::string make_temp_file(const std::string& prefix);

// Delete a file
bool remove_file(const std::string& path);

// Remove an empty directory
bool remove_dir(const std::string& path);

// Recursively remove a directory and all its contents
bool remove_recursive(const std::string& path);

} // namespace platform
} // namespace needle
