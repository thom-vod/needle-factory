#pragma once

#include <string>
#include <cassert>

namespace needle {

template <typename T>
class Result {
public:
    static Result<T> success(T value) {
        Result r;
        r.ok_ = true;
        r.value_ = std::move(value);
        return r;
    }

    static Result<T> failure(std::string error) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const { return ok_; }
    operator bool() const { return ok_; }

    const T& value() const {
        assert(ok_);
        return value_;
    }

    T& value() {
        assert(ok_);
        return value_;
    }

    const std::string& error() const {
        assert(!ok_);
        return error_;
    }

private:
    Result() : ok_(false) {}

    bool ok_;
    T value_;
    std::string error_;
};

template <>
class Result<void> {
public:
    static Result<void> success() {
        Result r;
        r.ok_ = true;
        return r;
    }

    static Result<void> failure(std::string error) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const { return ok_; }
    operator bool() const { return ok_; }

    const std::string& error() const {
        assert(!ok_);
        return error_;
    }

private:
    Result() : ok_(false) {}

    bool ok_;
    std::string error_;
};

} // namespace needle
