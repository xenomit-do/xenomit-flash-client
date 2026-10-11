#pragma once
#include <cstdint>
#include <memory>
#include <string>

// STA-only decoder: never retains unrooted entity pointers between frames.
class NativeState {
public:
    NativeState();
    ~NativeState();
    bool capture(const std::string& navigation, uint64_t navigation_at);
    void invalidate();
    uint32_t capture_us() const;
    const char* error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
