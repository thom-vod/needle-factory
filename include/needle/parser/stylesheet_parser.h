#pragma once

#include <string>
#include <vector>
#include "needle/model/result.h"
#include "needle/model/attribute_map.h"

namespace needle {

struct StyleRule {
    enum SelectorType { UNIVERSAL, CLASS, ID };
    SelectorType selector_type;
    std::string selector;
    AttributeMap properties;
};

struct Stylesheet {
    std::vector<StyleRule> rules;
};

class StylesheetParser {
public:
    static Result<Stylesheet> parse(const std::string& source);
};

} // namespace needle
