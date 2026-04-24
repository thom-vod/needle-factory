#include "needle/model/context.h"

namespace needle {

size_t Context::max_value_size_ = 100 * 1024;  // 100 KB default

void Context::set(const std::string& key, const std::string& value) {
    if (max_value_size_ > 0 && value.size() > max_value_size_) {
        data_[key] = value.substr(0, max_value_size_) +
            "\n\n[truncated at " + std::to_string(max_value_size_ / 1024) + " KB]";
    } else {
        data_[key] = value;
    }
}

std::string Context::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it != data_.end()) {
        return it->second;
    }
    return "";
}

bool Context::has(const std::string& key) const {
    return data_.find(key) != data_.end();
}

void Context::apply_updates(const std::map<std::string, std::string>& updates) {
    for (const auto& kv : updates) {
        set(kv.first, kv.second);
    }
}

const std::map<std::string, std::string>& Context::all() const {
    return data_;
}

Context Context::clone() const {
    Context c;
    c.data_ = data_;
    return c;
}

nlohmann::json Context::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : data_) {
        j[kv.first] = kv.second;
    }
    return j;
}

Context Context::from_json(const nlohmann::json& j) {
    Context c;
    for (auto it = j.begin(); it != j.end(); ++it) {
        c.data_[it.key()] = it.value().get<std::string>();
    }
    return c;
}

} // namespace needle
