#pragma once

#include <string>

namespace needle {

struct WorktreeReadyEvent {
    std::string branch_id;
    std::string path;
    std::string branch;
    bool created_now = false;
};

} // namespace needle
