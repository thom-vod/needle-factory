#include <catch2/catch.hpp>
#include "needle/config/needle_config.h"

#include "needle/platform/platform.h"
#include "support/temp_needle_state.h"
#include <cstdlib>

#ifdef _WIN32
#include <cstdio>
#include <direct.h>
#include <process.h>
static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#define getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace needle;

namespace {

/// Helper: create a unique temp directory for each test section.
struct TempDir {
    needle::tests::TempNeedleState state;
    std::string path;

    TempDir() {
        path = state.root();
    }

    std::string file(const std::string& name) const {
        return state.path(name);
    }
};

} // anonymous namespace

// -----------------------------------------------------------------------
// Config loads defaults when no file exists
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig loads defaults when no file exists", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));

    auto r = cfg.load();
    REQUIRE(r.ok());

    // Should have default values
    REQUIRE(cfg.get_string("defaults.chat_agent") == "claude");
    REQUIRE(cfg.get_int("server.port") == 8080);
    REQUIRE(cfg.get_bool("server.auto_approve") == false);
    REQUIRE(cfg.get_string("ui.theme") == "dark");
    REQUIRE(cfg.get_string("logging.level") == "info");
    REQUIRE(cfg.get_int("schema_version") == 1);
}

// -----------------------------------------------------------------------
// Config saves and loads round-trip
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig saves and loads round-trip", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));

    cfg.load();
    auto sr = cfg.set("providers.openai.api_key", "sk-test12345");
    REQUIRE(sr.ok());

    // Load into a fresh instance
    NeedleConfig cfg2;
    cfg2.set_config_path(tmp.file("config.json"));
    auto lr = cfg2.load();
    REQUIRE(lr.ok());

    REQUIRE(cfg2.get_string("providers.openai.api_key") == "sk-test12345");
    // Defaults should also be present
    REQUIRE(cfg2.get_string("defaults.chat_agent") == "claude");
}

// -----------------------------------------------------------------------
// Dot-notation get/set works (nested paths)
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig dot-notation get/set nested paths", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Set a deeply nested value
    auto r = cfg.set("providers.openai.default_model", "gpt-4");
    REQUIRE(r.ok());

    REQUIRE(cfg.get_string("providers.openai.default_model") == "gpt-4");

    // Set creates intermediate objects
    auto r2 = cfg.set("custom.deeply.nested.key", "value123");
    REQUIRE(r2.ok());
    REQUIRE(cfg.get_string("custom.deeply.nested.key") == "value123");
}

// -----------------------------------------------------------------------
// Env var takes precedence over config file value
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig env var takes precedence over config value", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("providers.openai.api_key", "sk-from-config");

    // Set env var
    setenv("NEEDLE_TEST_ENV_VAR_12345", "from-env", 1);

    std::string val = cfg.get_string("providers.openai.api_key",
                                     "NEEDLE_TEST_ENV_VAR_12345");
    REQUIRE(val == "from-env");

    // Without env var, should return config value
    unsetenv("NEEDLE_TEST_ENV_VAR_12345");
    std::string val2 = cfg.get_string("providers.openai.api_key",
                                      "NEEDLE_TEST_ENV_VAR_12345");
    REQUIRE(val2 == "sk-from-config");
}

// -----------------------------------------------------------------------
// Redaction hides API keys (shows first 5 chars + ***)
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig redaction hides API keys", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("providers.openai.api_key", "sk-abcdef123456789");

    auto redacted = cfg.to_json_redacted();
    std::string redacted_key = redacted["providers"]["openai"]["api_key"].get<std::string>();
    REQUIRE(redacted_key == "sk-ab***");

    // Non-api_key values should not be redacted
    std::string model = redacted["providers"]["openai"]["default_model"].get<std::string>();
    REQUIRE(model == "gpt-4o");
}

// -----------------------------------------------------------------------
// Merge adds new keys without removing existing
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig merge adds new keys without removing existing", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Set an initial value
    cfg.set("providers.openai.api_key", "sk-original");

    // Merge partial JSON
    nlohmann::json partial = {
        {"providers", {
            {"openai", {{"custom_field", "custom_value"}}}
        }},
        {"new_section", {{"key1", "val1"}}}
    };
    auto r = cfg.merge(partial);
    REQUIRE(r.ok());

    // Original key should still be there
    REQUIRE(cfg.get_string("providers.openai.api_key") == "sk-original");
    // New keys should be present
    REQUIRE(cfg.get_string("providers.openai.custom_field") == "custom_value");
    REQUIRE(cfg.get_string("new_section.key1") == "val1");
    // Defaults should still be present
    REQUIRE(cfg.get_string("defaults.chat_agent") == "claude");
}

// -----------------------------------------------------------------------
// Unset removes a key
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig unset removes a key", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("custom.key_to_remove", "temporary");
    REQUIRE(cfg.get_string("custom.key_to_remove") == "temporary");

    auto r = cfg.unset("custom.key_to_remove");
    REQUIRE(r.ok());

    REQUIRE(cfg.get_string("custom.key_to_remove") == "");
}

// -----------------------------------------------------------------------
// Empty api_key shows "not set" in redacted output
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig empty api_key shows 'not set' in redacted output", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Default api_keys are empty strings
    auto redacted = cfg.to_json_redacted();
    std::string openai_key = redacted["providers"]["openai"]["api_key"].get<std::string>();
    REQUIRE(openai_key == "not set");

    std::string anthropic_key = redacted["providers"]["anthropic"]["api_key"].get<std::string>();
    REQUIRE(anthropic_key == "not set");
}

// -----------------------------------------------------------------------
// resolve_api_key returns env var when set, config value when not
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig resolve_api_key prefers env var", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("providers.openai.api_key", "sk-from-config-file");

    // Set env var
    setenv("OPENAI_API_KEY", "sk-from-env", 1);

    REQUIRE(cfg.resolve_api_key("openai") == "sk-from-env");

    unsetenv("OPENAI_API_KEY");

    REQUIRE(cfg.resolve_api_key("openai") == "sk-from-config-file");
}

TEST_CASE("NeedleConfig resolve_api_key works for all providers", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("providers.anthropic.api_key", "sk-ant-config");
    cfg.set("providers.gemini.api_key", "AIza-config");
    cfg.set("providers.tavily.api_key", "tvly-config");

    // Without env vars, should return config values
    // (Make sure env vars are not set from a previous test)
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("GEMINI_API_KEY");
    unsetenv("TAVILY_API_KEY");

    REQUIRE(cfg.resolve_api_key("anthropic") == "sk-ant-config");
    REQUIRE(cfg.resolve_api_key("gemini") == "AIza-config");
    REQUIRE(cfg.resolve_api_key("tavily") == "tvly-config");

    // Unknown provider returns empty
    REQUIRE(cfg.resolve_api_key("unknown_provider") == "");
}

// -----------------------------------------------------------------------
// config_path returns ~/.needle/config.json
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig config_path returns expected path", "[config]") {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    REQUIRE(home != nullptr);

    std::string expected = std::string(home) + "/.needle/config.json";
    REQUIRE(NeedleConfig::config_path() == expected);
}

// -----------------------------------------------------------------------
// File permissions are 0600 after save
// -----------------------------------------------------------------------
// Windows does not support POSIX file permission bits, so skip this test there.
#ifndef _WIN32
TEST_CASE("NeedleConfig file permissions are 0600 after save", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Trigger a save by setting a value
    auto r = cfg.set("test.perm_check", "value");
    REQUIRE(r.ok());

    struct stat st;
    int rc = stat(tmp.file("config.json").c_str(), &st);
    REQUIRE(rc == 0);
    // Check permission bits (mask out file type)
    REQUIRE((st.st_mode & 0777) == 0600);
}
#endif

// -----------------------------------------------------------------------
// get_bool and set_bool
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig get_bool and set_bool", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    REQUIRE(cfg.get_bool("server.auto_approve") == false);

    auto r = cfg.set_bool("server.auto_approve", true);
    REQUIRE(r.ok());
    REQUIRE(cfg.get_bool("server.auto_approve") == true);
}

// -----------------------------------------------------------------------
// get_string returns fallback for missing path
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig get_string returns fallback for missing path", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    REQUIRE(cfg.get_string("nonexistent.path", "", "my_fallback") == "my_fallback");
    REQUIRE(cfg.get_string("nonexistent.path") == "");
}

// -----------------------------------------------------------------------
// get_string with env_var checks env first, then config, then fallback
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig get_string env > config > fallback precedence", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // 1. When env is set, config is set, and fallback is provided: env wins
    cfg.set("defaults.chat_agent", "claude");
    setenv("NEEDLE_TEST_CHAT_AGENT", "codex", 1);
    REQUIRE(cfg.get_string("defaults.chat_agent",
                           "NEEDLE_TEST_CHAT_AGENT", "gemini") == "codex");

    // 2. When env is unset, config value wins over fallback
    unsetenv("NEEDLE_TEST_CHAT_AGENT");
    REQUIRE(cfg.get_string("defaults.chat_agent",
                           "NEEDLE_TEST_CHAT_AGENT", "gemini") == "claude");

    // 3. When env is unset and config key is missing, fallback wins
    cfg.unset("defaults.chat_agent");
    REQUIRE(cfg.get_string("defaults.chat_agent",
                           "NEEDLE_TEST_CHAT_AGENT", "gemini") == "gemini");
}

// -----------------------------------------------------------------------
// resolve_api_key returns env var when both env and config are set
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig resolve_api_key env overrides config consistently", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Set both config and env for openai
    cfg.set("providers.openai.api_key", "sk-from-config");
    setenv("OPENAI_API_KEY", "sk-from-environment", 1);

    // Env should always win
    REQUIRE(cfg.resolve_api_key("openai") == "sk-from-environment");

    // Unset env -- config value should be returned
    unsetenv("OPENAI_API_KEY");
    REQUIRE(cfg.resolve_api_key("openai") == "sk-from-config");

    // Unset config -- should return empty
    cfg.unset("providers.openai.api_key");
    REQUIRE(cfg.resolve_api_key("openai") == "");
}

// -----------------------------------------------------------------------
// get_int returns config value, then fallback
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig get_int returns config value then fallback", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    // Default server.port is 8080
    REQUIRE(cfg.get_int("server.port", 9999) == 8080);

    // Missing key returns fallback
    REQUIRE(cfg.get_int("nonexistent.port", 3000) == 3000);
}

// -----------------------------------------------------------------------
// to_json returns raw data
// -----------------------------------------------------------------------
TEST_CASE("NeedleConfig to_json returns raw data", "[config]") {
    TempDir tmp;
    NeedleConfig cfg;
    cfg.set_config_path(tmp.file("config.json"));
    cfg.load();

    cfg.set("providers.openai.api_key", "sk-secret123");

    auto j = cfg.to_json();
    REQUIRE(j["providers"]["openai"]["api_key"].get<std::string>() == "sk-secret123");
}
