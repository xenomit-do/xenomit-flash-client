#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swf_bridge {

struct Result {
    bool ok = false;
    std::string error;
    std::string compression;
    std::vector<uint8_t> swf;
};

// Splice the precompiled reflection bridge into an in-memory SWF.
// The original bytes are not modified. FWS, CWS, and ZWS are accepted.
Result inject(const uint8_t *data, size_t size);


}
