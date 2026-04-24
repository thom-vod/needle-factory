#pragma once

#include <string>
#include <map>
#include "needle/model/maybe.h"

namespace needle {

class AttributeMap {
public:
    void set(const std::string& key, const std::string& value);
    std::string get(const std::string& key, const std::string& default_val = "") const;
    Maybe<int> get_int(const std::string& key) const;
    Maybe<double> get_double(const std::string& key) const;
    Maybe<bool> get_bool(const std::string& key) const;
    Maybe<int> get_duration_ms(const std::string& key) const;
    bool has(const std::string& key) const;
    const std::map<std::string, std::string>& raw() const;

private:
    std::map<std::string, std::string> attrs_;
};

} // namespace needle
