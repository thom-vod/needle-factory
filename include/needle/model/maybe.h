#pragma once

#include <cassert>

namespace needle {

template <typename T>
class Maybe {
public:
    Maybe() : has_value_(false), value_() {}
    Maybe(T value) : has_value_(true), value_(std::move(value)) {}

    bool has_value() const { return has_value_; }
    operator bool() const { return has_value_; }

    const T& operator*() const {
        assert(has_value_);
        return value_;
    }

    T& operator*() {
        assert(has_value_);
        return value_;
    }

private:
    bool has_value_;
    T value_;
};

} // namespace needle
