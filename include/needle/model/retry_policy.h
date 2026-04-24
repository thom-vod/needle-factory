#pragma once

#include "needle/model/attribute_map.h"

namespace needle {

enum class RetryPreset {
    STANDARD,
    AGGRESSIVE,
    LINEAR,
    PATIENT
};

struct RetryPolicy {
    int max_retries;
    int base_delay_ms;
    double multiplier;
    int max_delay_ms;
    bool jitter;

    int delay_for_attempt(int attempt) const;

    static RetryPolicy from_preset(RetryPreset preset);
    static RetryPolicy from_attributes(const AttributeMap& attrs);
    static RetryPolicy default_policy();
};

} // namespace needle
