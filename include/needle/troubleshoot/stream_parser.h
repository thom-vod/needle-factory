#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace needle {

struct TroubleshootStreamEvent {
    std::string type;
    nlohmann::json payload;
    std::string raw_line;
};

class TroubleshootStreamParser {
public:
    std::vector<TroubleshootStreamEvent> parse_line(const std::string& line);

private:
    bool emitted_session_started_ = false;
};

} // namespace needle
