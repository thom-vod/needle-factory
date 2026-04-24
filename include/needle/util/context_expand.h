#pragma once

#include <string>
#include "needle/model/context.h"

namespace needle {

/// Expand $context.key and ${context.key} references in `input` against `ctx`.
/// Unknown keys are left untouched. `$var.*` is not handled here — those are
/// substituted at graph-build time.
std::string expand_context_refs(const std::string& input, const Context& ctx);

} // namespace needle
