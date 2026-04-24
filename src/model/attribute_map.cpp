#include "needle/model/attribute_map.h"

#include <cstdlib>
#include <stdexcept>

namespace needle {

void AttributeMap::set(const std::string& key, const std::string& value) {
    attrs_[key] = value;
}

std::string AttributeMap::get(const std::string& key, const std::string& default_val) const {
    auto it = attrs_.find(key);
    if (it != attrs_.end()) {
        return it->second;
    }
    return default_val;
}

Maybe<int> AttributeMap::get_int(const std::string& key) const {
    auto it = attrs_.find(key);
    if (it == attrs_.end()) {
        return Maybe<int>();
    }
    try {
        size_t pos = 0;
        int val = std::stoi(it->second, &pos);
        if (pos == it->second.size()) {
            return Maybe<int>(val);
        }
        return Maybe<int>();
    } catch (...) {
        return Maybe<int>();
    }
}

Maybe<double> AttributeMap::get_double(const std::string& key) const {
    auto it = attrs_.find(key);
    if (it == attrs_.end()) {
        return Maybe<double>();
    }
    try {
        size_t pos = 0;
        double val = std::stod(it->second, &pos);
        if (pos == it->second.size()) {
            return Maybe<double>(val);
        }
        return Maybe<double>();
    } catch (...) {
        return Maybe<double>();
    }
}

Maybe<bool> AttributeMap::get_bool(const std::string& key) const {
    auto it = attrs_.find(key);
    if (it == attrs_.end()) {
        return Maybe<bool>();
    }
    const std::string& s = it->second;
    if (s == "true" || s == "1" || s == "yes") {
        return Maybe<bool>(true);
    }
    if (s == "false" || s == "0" || s == "no") {
        return Maybe<bool>(false);
    }
    return Maybe<bool>();
}

Maybe<int> AttributeMap::get_duration_ms(const std::string& key) const {
    auto it = attrs_.find(key);
    if (it == attrs_.end()) {
        return Maybe<int>();
    }
    const std::string& s = it->second;
    if (s.empty()) {
        return Maybe<int>();
    }

    // Try to parse as integer with optional suffix
    // Supported: "500" (ms), "500ms", "2s", "1m"
    try {
        size_t pos = 0;
        int val = std::stoi(s, &pos);
        if (pos == s.size()) {
            // Plain number, assume milliseconds
            return Maybe<int>(val);
        }
        std::string suffix = s.substr(pos);
        if (suffix == "ms") {
            return Maybe<int>(val);
        } else if (suffix == "s") {
            return Maybe<int>(val * 1000);
        } else if (suffix == "m") {
            return Maybe<int>(val * 60 * 1000);
        }
        return Maybe<int>();
    } catch (...) {
        return Maybe<int>();
    }
}

bool AttributeMap::has(const std::string& key) const {
    return attrs_.find(key) != attrs_.end();
}

const std::map<std::string, std::string>& AttributeMap::raw() const {
    return attrs_;
}

} // namespace needle
