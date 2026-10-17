// Flash Player configuration served through the virtual-file hooks (see
// InProcessPlayer §7.1). These are our own payloads, not copies of InProcess's.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace xf {
namespace flash_config {

// mms.cfg policy. No AllowList directives: the root movie is our staged local
// file, which an https-only allow list would refuse.
inline std::string mms_cfg() {
    return
        "SilentAutoUpdateEnable=0\r\n"
        "AutoUpdateDisable=1\r\n"
        "EOLUninstallDisable=1\r\n"
        "ProtectedMode=0\r\n"
        "TimerJitterMicroseconds=0\r\n"
        "EventJitterMicroseconds=0\r\n"
        "AVHardwareDisable=1\r\n"
        "OverrideGPUValidation=1\r\n"
        "UseWAVPlayer=1\r\n";
}

namespace amf0 {
inline void u16(std::string &out, uint16_t value) {
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value & 0xff));
}
inline void name(std::string &out, const char *text) {
    u16(out, static_cast<uint16_t>(std::strlen(text)));
    out += text;
}
inline void boolean(std::string &out, const char *key, bool value) {
    name(out, key);
    out.push_back(0x01);
    out.push_back(value ? 1 : 0);
    out.push_back(0x00);
}
inline void number(std::string &out, const char *key, double value) {
    name(out, key);
    out.push_back(0x00);
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((bits >> shift) & 0xff));
    out.push_back(0x00);
}
inline void text(std::string &out, const char *key, const char *value) {
    name(out, key);
    out.push_back(0x02);
    name(out, value);
    out.push_back(0x00);
}
} // namespace amf0

// Flash Player settings Local Shared Object (AMF0 "TCSO" container) with the
// keys InProcessPlayer seeds: no update checks, cross-domain allowed, no P2P
// uplink, default device choices.
inline std::string settings_sol() {
    std::string body;
    body += "TCSO";
    body += std::string("\x00\x04\x00\x00\x00\x00", 6);
    amf0::name(body, "settings");
    body += std::string("\x00\x00\x00\x00", 4); // AMF0 encoding
    amf0::number(body, "gain", 50);
    amf0::boolean(body, "echosuppression", false);
    amf0::text(body, "defaultmicrophone", "");
    amf0::text(body, "defaultcamera", "");
    amf0::number(body, "defaultklimit", 100);
    amf0::boolean(body, "defaultalways", false);
    amf0::boolean(body, "windowlessDisable", false);
    amf0::boolean(body, "autoUpdateDisabled", true);
    amf0::boolean(body, "crossdomainAllow", true);
    amf0::boolean(body, "crossdomainAlways", true);
    amf0::boolean(body, "allowThirdPartyLSOAccess", true);
    amf0::boolean(body, "safefullscreen", false);
    amf0::boolean(body, "disallowP2PUplink", true);
    std::string file("\x00\xbf", 2);
    auto size = static_cast<uint32_t>(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) file.push_back(static_cast<char>((size >> shift) & 0xff));
    return file + body;
}

} // namespace flash_config
} // namespace xf
