#include <catch2/catch.hpp>

#include "needle/config/needle_config.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/model/context.h"
#include "needle/util/context_defaults.h"

#include "support/temp_needle_state.h"

using namespace needle;

namespace {

void load_config_with_defaults(NeedleConfig& cfg,
                               const std::string& path,
                               const nlohmann::json& defaults) {
    cfg.set_config_path(path);
    REQUIRE(cfg.merge({{"defaults", defaults}}).ok());
}

} // namespace

TEST_CASE("config defaults injection stringifies scalar defaults", "[resume][config]") {
    needle::tests::TempNeedleState state;
    NeedleConfig cfg;
    load_config_with_defaults(cfg, state.path("config.json"), {
        {"string_key", "value"},
        {"int_key", 42},
        {"float_key", 1.25},
        {"bool_key", true},
        {"object_key", {{"nested", "ignored"}}},
        {"array_key", nlohmann::json::array({1, 2})},
        {"null_key", nullptr}
    });

    Context ctx;
    inject_config_defaults(ctx, cfg, true);

    REQUIRE(ctx.get("config.defaults.string_key") == "value");
    REQUIRE(ctx.get("config.defaults.int_key") == "42");
    REQUIRE(ctx.get("config.defaults.float_key") == std::to_string(1.25));
    REQUIRE(ctx.get("config.defaults.bool_key") == "true");
    REQUIRE_FALSE(ctx.has("config.defaults.object_key"));
    REQUIRE_FALSE(ctx.has("config.defaults.array_key"));
    REQUIRE_FALSE(ctx.has("config.defaults.null_key"));
}

TEST_CASE("resume default live config overwrites checkpoint defaults", "[resume][config]") {
    needle::tests::TempNeedleState state;
    NeedleConfig cfg_a;
    load_config_with_defaults(cfg_a, state.path("config-a.json"), {
        {"critique_model", "model-a"}
    });

    Checkpoint cp;
    inject_config_defaults(cp.context, cfg_a, true);
    REQUIRE(cp.context.get("config.defaults.critique_model") == "model-a");

    NeedleConfig cfg_b;
    load_config_with_defaults(cfg_b, state.path("config-b.json"), {
        {"critique_model", "model-b"}
    });
    inject_config_defaults(cp.context, cfg_b, true);

    REQUIRE(cp.context.get("config.defaults.critique_model") == "model-b");
}

TEST_CASE("resume frozen config preserves checkpoint defaults", "[resume][config]") {
    needle::tests::TempNeedleState state;
    NeedleConfig cfg_a;
    load_config_with_defaults(cfg_a, state.path("config-a.json"), {
        {"critique_model", "model-a"}
    });

    Checkpoint cp;
    inject_config_defaults(cp.context, cfg_a, true);
    REQUIRE(cp.context.get("config.defaults.critique_model") == "model-a");

    NeedleConfig cfg_b;
    load_config_with_defaults(cfg_b, state.path("config-b.json"), {
        {"critique_model", "model-b"}
    });
    inject_config_defaults(cp.context, cfg_b, false);

    REQUIRE(cp.context.get("config.defaults.critique_model") == "model-a");
}

TEST_CASE("empty defaults injection creates no config defaults keys", "[resume][config]") {
    needle::tests::TempNeedleState state;
    NeedleConfig cfg;
    load_config_with_defaults(cfg, state.path("config.json"), nlohmann::json::object());
    Context ctx;

    inject_config_defaults(ctx, cfg, true);

    for (const auto& kv : ctx.all()) {
        REQUIRE(kv.first.find("config.defaults.") != 0);
    }
    REQUIRE(ctx.all().empty());
}
