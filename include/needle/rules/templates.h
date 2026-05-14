#pragma once

#include <string>
#include <vector>

namespace needle {
namespace templates {

// Names of all bundled sample DOTs (alphabetical). Backed by
// sample_dots/*.dot at build time via scripts/embed_templates.py.
std::vector<std::string> list_names();

// Full DOT source for the named template, or nullptr if no such name.
// The pointer is to a static string baked into the binary; do not free.
const char* get(const std::string& name);

} // namespace templates
} // namespace needle
