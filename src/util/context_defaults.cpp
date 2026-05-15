#include "needle/util/context_defaults.h"

namespace needle {

void inject_config_defaults(Context& ctx,
                            const NeedleConfig& cfg,
                            bool overwrite_existing) {
    nlohmann::json defaults_json =
        cfg.to_json().value("defaults", nlohmann::json::object());

    if (!defaults_json.is_object()) {
        return;
    }

    for (auto it = defaults_json.begin(); it != defaults_json.end(); ++it) {
        std::string key = "config.defaults." + it.key();
        if (!overwrite_existing && ctx.has(key)) {
            continue;
        }

        const auto& v = it.value();
        if (v.is_string()) {
            ctx.set(key, v.get<std::string>());
        } else if (v.is_number_integer()) {
            ctx.set(key, std::to_string(v.get<long long>()));
        } else if (v.is_number_float()) {
            ctx.set(key, std::to_string(v.get<double>()));
        } else if (v.is_boolean()) {
            ctx.set(key, v.get<bool>() ? "true" : "false");
        }
        // Skip nested objects, arrays, and nulls; they are not scalar defaults.
    }
}

} // namespace needle
