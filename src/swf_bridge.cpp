#include "swf_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "LzmaDec.h"

static void *sz_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
static void sz_free(ISzAllocPtr, void *address) { std::free(address); }
}

#include "miniz.h"
#include "bridge_abc.inc"

namespace swf_bridge {
namespace {

constexpr uint32_t kMaxSwfBytes = 64u * 1024u * 1024u;
constexpr uint16_t kShowFrame = 1;
constexpr uint16_t kEnd = 0;
constexpr uint16_t kDoAbc = 82;

void write_u16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void write_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

uint16_t read_u16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

void store_u32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

void append_tag(std::vector<uint8_t> &out, uint16_t code, const uint8_t *payload, uint32_t length) {
    if (length < 0x3f) {
        write_u16(out, static_cast<uint16_t>((code << 6) | length));
    } else {
        write_u16(out, static_cast<uint16_t>((code << 6) | 0x3f));
        write_u32(out, length);
    }
    if (length && payload) out.insert(out.end(), payload, payload + length);
}

struct BitWriter {
    std::vector<uint8_t> bytes;
    int bit = 0;
    void put(uint32_t value, int width) {
        for (int index = width - 1; index >= 0; --index) {
            if (bit == 0) bytes.push_back(0);
            if (value & (1u << index)) bytes.back() |= static_cast<uint8_t>(1u << (7 - bit));
            bit = (bit + 1) & 7;
        }
    }
};

size_t tag_stream_offset(const std::vector<uint8_t> &swf, std::string &error) {
    if (swf.size() < 9) {
        error = "SWF header is truncated";
        return 0;
    }
    unsigned nbits = swf[8] >> 3;
    size_t rect_bytes = (5u + nbits * 4u + 7u) / 8u;
    size_t tags = 8 + rect_bytes + 4;
    if (tags > swf.size()) {
        error = "SWF rectangle is truncated";
        return 0;
    }
    return tags;
}

bool decompress_cws(const uint8_t *data, size_t size, std::vector<uint8_t> &out, std::string &error) {
    uint32_t declared = read_u32(data + 4);
    if (declared <= 8 || declared > kMaxSwfBytes) {
        error = "CWS length is invalid";
        return false;
    }
    out.assign(declared, 0);
    std::memcpy(out.data(), data, 8);
    mz_ulong dest = declared - 8;
    int status = mz_uncompress(out.data() + 8, &dest, data + 8, static_cast<mz_ulong>(size - 8));
    if (status != MZ_OK) {
        error = "CWS inflate failed";
        return false;
    }
    out.resize(8 + dest);
    out[0] = 'F';
    out[1] = 'W';
    out[2] = 'S';
    store_u32(out.data() + 4, static_cast<uint32_t>(out.size()));
    return true;
}

bool lzma_decode(const uint8_t *props, const uint8_t *src, size_t src_len,
                 uint8_t *dest, size_t dest_len) {
    if (src_len == 0 || dest_len == 0) return false;
    SizeT out_len = dest_len;
    SizeT in_len = src_len;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    ISzAlloc alloc{sz_alloc, sz_free};
    SRes result = LzmaDecode(dest, &out_len, src, &in_len, props, 5,
                             LZMA_FINISH_END, &status, &alloc);
    return result == SZ_OK && out_len == dest_len;
}

bool decompress_zws(const uint8_t *data, size_t size, std::vector<uint8_t> &out, std::string &error) {
    if (size < 17) {
        error = "ZWS header is truncated";
        return false;
    }
    uint32_t declared = read_u32(data + 4);
    uint32_t compressed_field = read_u32(data + 8);
    if (declared <= 8 || declared > kMaxSwfBytes) {
        error = "ZWS length is invalid";
        return false;
    }
    const uint8_t *props = data + 12;
    size_t available = size - 17;
    size_t candidates[3] = {available, available, available};
    int count = 1;
    if (compressed_field >= 5 && compressed_field - 5 <= available)
        candidates[count++] = compressed_field - 5;
    if (compressed_field <= available && compressed_field != candidates[1])
        candidates[count++] = compressed_field;

    out.assign(declared, 0);
    std::memcpy(out.data(), data, 8);
    for (int index = 0; index < count; ++index) {
        if (lzma_decode(props, data + 17, candidates[index], out.data() + 8, declared - 8)) {
            out[0] = 'F';
            out[1] = 'W';
            out[2] = 'S';
            return true;
        }
    }
    error = "ZWS inflate failed";
    return false;
}

bool tag_defines_character(uint16_t code) {
    switch (code) {
        case 2: case 6: case 7: case 10: case 11: case 14:
        case 20: case 21: case 22: case 32: case 33: case 34: case 35: case 36:
        case 37: case 39: case 46: case 48: case 60: case 75: case 83: case 84:
        case 87: case 90: case 91:
            return true;
        default:
            return false;
    }
}

size_t skip_swf_string(const uint8_t *data, size_t size, size_t pos) {
    while (pos < size && data[pos] != 0) ++pos;
    return pos < size ? pos + 1 : pos;
}

void note_id(uint16_t id, uint16_t &highest) {
    if (id > highest) highest = id;
}

void note_symbol_list(const uint8_t *payload, uint32_t length, uint16_t &highest) {
    if (length < 2) return;
    uint16_t count = read_u16(payload);
    size_t pos = 2;
    for (uint16_t index = 0; index < count && pos + 2 <= length; ++index) {
        note_id(read_u16(payload + pos), highest);
        pos = skip_swf_string(payload, length, pos + 2);
    }
}

std::vector<uint8_t> bridge_tags(uint16_t character_id, uint16_t depth) {
    std::vector<uint8_t> tags;
    std::vector<uint8_t> abc_payload;
    write_u32(abc_payload, 0);
    const char abc_name[] = "DobotBridge";
    abc_payload.insert(abc_payload.end(), abc_name, abc_name + sizeof(abc_name));
    abc_payload.insert(abc_payload.end(), kBridgeAbc, kBridgeAbc + kBridgeAbcSize);
    append_tag(tags, kDoAbc, abc_payload.data(), static_cast<uint32_t>(abc_payload.size()));

    // Empty sprite. SymbolClass turns its instances into DobotBridge.
    uint8_t sprite[8]{};
    sprite[0] = static_cast<uint8_t>(character_id);
    sprite[1] = static_cast<uint8_t>(character_id >> 8);
    sprite[2] = 1; // one frame
    sprite[4] = 0x40; // ShowFrame
    append_tag(tags, 39, sprite, sizeof(sprite));

    const char class_name[] = "DobotBridge";
    std::vector<uint8_t> symbols;
    write_u16(symbols, 1);
    write_u16(symbols, character_id);
    symbols.insert(symbols.end(), class_name, class_name + sizeof(class_name));
    append_tag(tags, 76, symbols.data(), static_cast<uint32_t>(symbols.size()));

    // PlaceFlagHasMatrix | PlaceFlagHasCharacter, identity matrix, above the game.
    uint8_t place[6] = {
        0x06,
        static_cast<uint8_t>(depth), static_cast<uint8_t>(depth >> 8),
        static_cast<uint8_t>(character_id), static_cast<uint8_t>(character_id >> 8),
        0x00,
    };
    append_tag(tags, 26, place, sizeof(place));
    return tags;
}

} // namespace

Result inject(const uint8_t *data, size_t size) {
    Result result;
    if (!data || size < 8) {
        result.error = "SWF is too small";
        return result;
    }
    if (std::memcmp(data, "FWS", 3) == 0) result.compression = "FWS";
    else if (std::memcmp(data, "CWS", 3) == 0) result.compression = "CWS";
    else if (std::memcmp(data, "ZWS", 3) == 0) result.compression = "ZWS";
    else {
        result.error = "not a SWF";
        return result;
    }
    if constexpr (kBridgeAbcSize < 64) {
        result.error = "reflection bridge ABC is missing";
        return result;
    }

    std::vector<uint8_t> swf;
    if (result.compression == "FWS") {
        if (size > kMaxSwfBytes) {
            result.error = "SWF is too large";
            return result;
        }
        swf.assign(data, data + size);
    } else if (result.compression == "CWS") {
        if (!decompress_cws(data, size, swf, result.error)) return result;
    } else if (!decompress_zws(data, size, swf, result.error)) {
        return result;
    }

    const char marker[] = "DobotBridge";
    if (std::search(swf.begin(), swf.end(), marker, marker + sizeof(marker) - 1) != swf.end()) {
        result.ok = true;
        result.swf = std::move(swf);
        return result;
    }

    std::string error;
    size_t tags = tag_stream_offset(swf, error);
    if (!tags) {
        result.error = error;
        return result;
    }
    size_t insert_at = swf.size();
    bool found = false;
    uint16_t highest_id = 0;
    uint16_t highest_depth = 0;
    size_t pos = tags;
    while (pos + 2 <= swf.size()) {
        size_t tag_start = pos;
        uint16_t header = read_u16(swf.data() + pos);
        uint16_t code = header >> 6;
        uint32_t length = header & 0x3f;
        pos += 2;
        if (length == 0x3f) {
            if (pos + 4 > swf.size()) break;
            length = read_u32(swf.data() + pos);
            pos += 4;
        }
        if (pos + length > swf.size()) break;
        const uint8_t *payload = swf.data() + pos;
        if (tag_defines_character(code) && length >= 2)
            note_id(read_u16(payload), highest_id);
        if ((code == 56 || code == 76) && length >= 2)
            note_symbol_list(payload, length, highest_id);
        if (code == 4 && length >= 4) {
            note_id(read_u16(payload), highest_id);
            note_id(read_u16(payload + 2), highest_depth);
        } else if ((code == 26 || code == 70) && length >= 3) {
            size_t cursor = code == 70 ? 2u : 1u;
            if (cursor + 2 <= length) {
                uint16_t depth = read_u16(payload + cursor);
                note_id(depth, highest_depth);
                if ((payload[0] & 0x02) && cursor + 4 <= length)
                    note_id(read_u16(payload + cursor + 2), highest_id);
            }
        }
        if (!found && (code == kShowFrame || code == kEnd)) {
            insert_at = tag_start;
            found = true;
        }
        pos += length;
    }
    if (!found) {
        result.error = "SWF has no ShowFrame tag";
        return result;
    }
    if (highest_id == 65535) {
        result.error = "SWF has no free character id";
        return result;
    }

    auto tag = bridge_tags(static_cast<uint16_t>(highest_id + 1),
                           static_cast<uint16_t>(highest_depth == 65535 ? 65535 : highest_depth + 1));
    std::vector<uint8_t> patched;
    patched.reserve(swf.size() + tag.size());
    patched.insert(patched.end(), swf.begin(), swf.begin() + static_cast<std::ptrdiff_t>(insert_at));
    patched.insert(patched.end(), tag.begin(), tag.end());
    patched.insert(patched.end(), swf.begin() + static_cast<std::ptrdiff_t>(insert_at), swf.end());
    if (patched[3] < 17) patched[3] = 17;
    store_u32(patched.data() + 4, static_cast<uint32_t>(patched.size()));
    patched[0] = 'F';
    patched[1] = 'W';
    patched[2] = 'S';
    result.ok = true;
    result.swf = std::move(patched);
    return result;
}

} // namespace swf_bridge
