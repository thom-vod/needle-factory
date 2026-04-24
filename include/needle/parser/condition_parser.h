#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/model/condition.h"

namespace needle {

class ConditionParser {
public:
    static Result<ConditionExpr> parse(const std::string& expr);
};

} // namespace needle
