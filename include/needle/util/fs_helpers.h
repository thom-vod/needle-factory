#pragma once

#include <string>

namespace needle {

/// Recursively create directories (like `mkdir -p`).
/// Returns true if the directory exists after the call.
bool mkdir_p(const std::string& path);

/// Returns true if the path exists (file or directory).
bool path_exists(const std::string& path);

/// Returns true if the path exists and is a regular file.
bool is_file(const std::string& path);

/// Returns true if the path exists and is a directory.
bool is_directory(const std::string& path);

/// Recursively remove a directory and all its contents.
bool remove_recursive(const std::string& path);

/// Extract a filesystem-safe stem from DOT source (via graph label attribute).
/// "Weather & Air Quality Dashboard" → "weather__air_quality_dashboard"
/// Falls back to "design" if no label found.
std::string dot_stem_from_source(const std::string& dot_source);

/// Extract stem from a DOT filename: "/path/to/ci_pipeline.dot" → "ci_pipeline"
std::string dot_stem_from_filename(const std::string& filename);

/// Sanitize a string for use as a directory/filename component.
/// Lowercase, alnum kept, spaces/dashes → underscore, other chars dropped.
std::string sanitize_for_filename(const std::string& input);

/// Rename existing stage files (prompt.md, response.md, status.json, debug.log)
/// by appending their last-modified timestamp (MMDDHHMM) so they aren't overwritten
/// when a node reruns on resume.
void archive_stage_files(const std::string& stage_dir);

} // namespace needle
