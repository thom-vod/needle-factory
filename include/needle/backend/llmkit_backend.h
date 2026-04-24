#pragma once

#include <string>
#include <map>
#include "needle/backend/backend.h"

namespace needle {

struct ProviderConfig {
    std::string name;           // "anthropic", "openai", "google"
    std::string base_url;
    std::string api_key_env;    // environment variable name, NOT the key itself
    std::string default_model;
};

class LLMKitBackend : public Backend {
public:
    explicit LLMKitBackend(std::map<std::string, ProviderConfig> providers);

    std::string name() const override;
    Result<Outcome> execute(const Node& node, Context& ctx,
                            const std::string& stage_dir) override;

    // Exposed for testing: build request body without making HTTP calls
    std::string build_anthropic_request(const std::string& prompt,
                                        const std::string& model) const;
    std::string build_openai_request(const std::string& prompt,
                                     const std::string& model) const;
    std::string build_google_request(const std::string& prompt) const;

private:
    Result<std::string> call_anthropic(const std::string& prompt,
                                       const std::string& model,
                                       const std::string& api_key);
    Result<std::string> call_openai(const std::string& prompt,
                                    const std::string& model,
                                    const std::string& api_key);
    Result<std::string> call_google(const std::string& prompt,
                                    const std::string& model,
                                    const std::string& api_key);

    StageStatus classify_http_error(int status_code);

    std::map<std::string, ProviderConfig> providers_;
};

} // namespace needle
