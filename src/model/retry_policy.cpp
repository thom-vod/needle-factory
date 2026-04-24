#include "needle/model/retry_policy.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace needle {

int RetryPolicy::delay_for_attempt(int attempt) const {
    double delay = base_delay_ms * std::pow(multiplier, attempt);
    int delay_ms = static_cast<int>(delay);
    delay_ms = std::min(delay_ms, max_delay_ms);

    if (jitter) {
        // Add random jitter up to 25% of the computed delay
        static thread_local std::mt19937 gen(std::random_device{}());
        int jitter_range = std::max(1, delay_ms / 4);
        std::uniform_int_distribution<int> dist(0, jitter_range);
        delay_ms += dist(gen);
    }

    return delay_ms;
}

RetryPolicy RetryPolicy::from_preset(RetryPreset preset) {
    switch (preset) {
        case RetryPreset::STANDARD:
            return RetryPolicy{5, 200, 2.0, 30000, true};
        case RetryPreset::AGGRESSIVE:
            return RetryPolicy{10, 100, 1.5, 30000, true};
        case RetryPreset::LINEAR:
            return RetryPolicy{5, 1000, 1.0, 30000, false};
        case RetryPreset::PATIENT:
            return RetryPolicy{3, 5000, 3.0, 60000, true};
    }
    return default_policy();
}

RetryPolicy RetryPolicy::from_attributes(const AttributeMap& attrs) {
    // Check for preset first
    std::string preset_str = attrs.get("retry_preset");
    if (!preset_str.empty()) {
        RetryPreset preset = RetryPreset::STANDARD;
        if (preset_str == "aggressive") preset = RetryPreset::AGGRESSIVE;
        else if (preset_str == "linear") preset = RetryPreset::LINEAR;
        else if (preset_str == "patient") preset = RetryPreset::PATIENT;
        return from_preset(preset);
    }

    // Build from individual attributes, using STANDARD defaults
    RetryPolicy policy = default_policy();

    Maybe<int> mr = attrs.get_int("max_retries");
    if (mr.has_value()) policy.max_retries = *mr;

    Maybe<int> bd = attrs.get_duration_ms("base_delay");
    if (bd.has_value()) policy.base_delay_ms = *bd;

    Maybe<double> mult = attrs.get_double("retry_multiplier");
    if (mult.has_value()) policy.multiplier = *mult;

    Maybe<int> md = attrs.get_duration_ms("max_delay");
    if (md.has_value()) policy.max_delay_ms = *md;

    Maybe<bool> j = attrs.get_bool("retry_jitter");
    if (j.has_value()) policy.jitter = *j;

    return policy;
}

RetryPolicy RetryPolicy::default_policy() {
    return from_preset(RetryPreset::STANDARD);
}

} // namespace needle
