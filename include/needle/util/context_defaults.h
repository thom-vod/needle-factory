#pragma once

#include "needle/config/needle_config.h"
#include "needle/model/context.h"

namespace needle {

/// Inject scalar entries from cfg.to_json()["defaults"] into ctx as
/// config.defaults.<key> strings.
///
/// String values are copied directly, integer and float values use
/// std::to_string, and booleans become "true" or "false". Objects, arrays,
/// and null values are ignored. When overwrite_existing is false, existing
/// context keys are left unchanged.
void inject_config_defaults(Context& ctx,
                            const NeedleConfig& cfg,
                            bool overwrite_existing);

} // namespace needle
