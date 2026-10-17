#pragma once
#include <cstdint>
#include <memory>
#include <string>

// How the ScreenManager root is found. Anchor: the bridge's NativeAnchor
// object (game-independent). Pattern: the legacy MainClientApplication value
// fingerprint from generated/game_offsets.h. Auto prefers the anchor and falls
// back to the pattern after a full scan. Parity requires both and compares
// them on every capture (diagnostic proof that the anchor is equivalent).
enum class NativeDiscovery { Auto, Anchor, Pattern, Parity };

// STA-only decoder: never retains unrooted entity pointers between frames.
class NativeState {
public:
    // named: publish as Local\xenomit_state_<pid> for out-of-process readers.
    explicit NativeState(NativeDiscovery discovery=NativeDiscovery::Auto,bool named=true);
    ~NativeState();
    bool capture(const std::string& navigation, uint64_t navigation_at);
    void invalidate();
    uint32_t capture_us() const;
    const char* error() const;
    // "<tag0>,<tag1>": integers below 2^53, exact as AS3 Numbers.
    const std::string& anchor_nonce() const;
    // Root currently in use: "anchor", "pattern" or "none".
    const char* root() const;
    uint64_t parity_checks() const;
    uint64_t parity_mismatches() const;
    // The version 2 LiveFrame bytes (seqlock), or null if allocation failed.
    const uint8_t* frame(size_t& size) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
