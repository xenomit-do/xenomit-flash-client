#include "as_bridge.h"

#include <cstdio>
#include <cstdlib>

namespace as_bridge {
namespace {

void append_utf8(std::string &out, unsigned code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

std::string xml_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(static_cast<char>(ch)); break;
        }
    }
    return out;
}

std::string xml_unescape(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t index = 0; index < value.size();) {
        if (value[index] != '&') {
            out.push_back(value[index++]);
            continue;
        }
        auto semi = value.find(';', index);
        if (semi == std::string::npos) {
            out.push_back(value[index++]);
            continue;
        }
        std::string entity = value.substr(index + 1, semi - index - 1);
        if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            char *end = nullptr;
            unsigned long code = 0;
            if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                code = std::strtoul(entity.c_str() + 2, &end, 16);
            else
                code = std::strtoul(entity.c_str() + 1, &end, 10);
            if (end && *end == '\0' && code)
                append_utf8(out, static_cast<unsigned>(code));
            else
                out.append(value, index, semi - index + 1);
        } else {
            out.append(value, index, semi - index + 1);
        }
        index = semi + 1;
    }
    return out;
}

} // namespace

std::string request_xml(const std::string &name, const std::vector<std::string> &args) {
    std::string xml = "<invoke name=\"" + xml_escape(name) + "\" returntype=\"xml\"><arguments>";
    for (const auto &arg : args)
        xml += "<string>" + xml_escape(arg) + "</string>";
    xml += "</arguments></invoke>";
    return xml;
}

std::string parse_response(const std::string &response) {
    auto open = response.find("<string");
    if (open == std::string::npos) return xml_unescape(response);
    auto start = response.find('>', open);
    auto close = response.find("</string>", start == std::string::npos ? open : start);
    if (start == std::string::npos || close == std::string::npos) return xml_unescape(response);
    return xml_unescape(response.substr(start + 1, close - start - 1));
}

std::vector<std::string> parse_arguments(const std::string &invoke) {
    std::vector<std::string> args;
    for (size_t at = invoke.find("<arguments>"); at != std::string::npos;) {
        auto open = invoke.find('<', at + 1);
        if (open == std::string::npos || invoke.compare(open, 12, "</arguments>") == 0) break;
        auto end = invoke.find('>', open);
        if (end == std::string::npos) break;
        if (invoke.compare(open, 7, "<string") == 0 && invoke[end - 1] != '/') {
            auto close = invoke.find("</string>", end);
            if (close == std::string::npos) break;
            args.push_back(xml_unescape(invoke.substr(end + 1, close - end - 1)));
            at = close + 8;
        } else if (invoke.compare(open, 7, "<number") == 0 && invoke[end - 1] != '/') {
            auto close = invoke.find("</number>", end);
            if (close == std::string::npos) break;
            args.push_back(invoke.substr(end + 1, close - end - 1));
            at = close + 8;
        } else {
            // Empty strings, null, undefined and other scalars.
            args.push_back(std::string());
            at = end;
        }
    }
    return args;
}

}
