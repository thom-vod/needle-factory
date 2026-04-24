#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace needle {

class Context {
public:
    void set(const std::string& key, const std::string& value);
    std::string get(const std::string& key) const;
    bool has(const std::string& key) const;
    void apply_updates(const std::map<std::string, std::string>& updates);
    const std::map<std::string, std::string>& all() const;
    Context clone() const;

    nlohmann::json to_json() const;
    static Context from_json(const nlohmann::json& j);

    /// Set the global maximum size for context values (bytes). 0 = unlimited.
    /// Values exceeding this are truncated with a marker.
    static void set_max_value_size(size_t bytes) { max_value_size_ = bytes; }
    static size_t max_value_size() { return max_value_size_; }

private:
    std::map<std::string, std::string> data_;
    static size_t max_value_size_;  // 0 = unlimited, default 100KB
};

} // namespace needle
