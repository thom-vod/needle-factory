#pragma once

#ifdef NEEDLE_ENABLE_SERVER

#include <string>
#include <vector>
#include "needle/model/result.h"

namespace needle {

struct ChatMessage {
    std::string role;     // "user" or "assistant"
    std::string content;
};

class DotGenerator {
public:
    // Call the LLM with conversation history to generate/refine DOT source.
    // provider: "anthropic", "openai", or "google"
    // Returns the assistant's response text.
    Result<std::string> generate(const std::string& provider,
                                 const std::vector<ChatMessage>& messages);

    // Call the LLM with a custom system prompt and conversation messages.
    // Uses the default provider (anthropic).
    Result<std::string> chat(const std::string& system_prompt,
                             const std::vector<ChatMessage>& messages);

private:
    Result<std::string> call_llm(const std::string& provider,
                                 const std::string& sys_prompt,
                                 const std::vector<ChatMessage>& messages);

    std::string build_anthropic_body(const std::string& sys_prompt,
                                     const std::vector<ChatMessage>& messages,
                                     const std::string& model) const;
    std::string build_openai_body(const std::string& sys_prompt,
                                  const std::vector<ChatMessage>& messages,
                                  const std::string& model) const;
    std::string build_google_body(const std::string& sys_prompt,
                                  const std::vector<ChatMessage>& messages) const;

    Result<std::string> http_post(const std::string& url,
                                   const std::string& body,
                                   const std::vector<std::pair<std::string, std::string>>& headers);

    static std::string system_prompt();
};

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
