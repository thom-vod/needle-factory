#pragma once

#ifdef NEEDLE_ENABLE_SERVER

#include <string>

namespace needle { namespace dashboard {

extern const char* const CSS;
extern const char* const HTML;
extern const char* const JS;

std::string assemble_page(const std::string& graph_svg,
                          const std::string& graph_name);

}} // namespace needle::dashboard

#endif // NEEDLE_ENABLE_SERVER
