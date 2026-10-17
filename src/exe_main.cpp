// xenomit_flash_client.exe: the out-of-process fallback host. It keeps the
// original command line and stdin bootstrap contract, starts xenomit_flash.dll
// with the named LiveFrame mapping and the control pipe, and waits for it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <map>
#include <string>

#include "xenomit_flash.h"

// Request the discrete GPU for the Flash renderer (InProcessPlayer exports these
// too; drivers only honour them on the process executable).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::map<std::wstring, std::wstring> parse_args() {
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::map<std::wstring, std::wstring> options;
    for (int index = 1; argv && index < count; ++index) {
        std::wstring key = argv[index];
        if (key.rfind(L"--", 0) != 0) continue;
        if (index + 1 < count && std::wstring(argv[index + 1]).rfind(L"--", 0) != 0)
            options[key] = argv[++index];
        else
            options[key] = L"1";
    }
    if (argv) LocalFree(argv);
    return options;
}

} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto options = parse_args();
    // Session records stay as UTF-8 bytes in memory; they never reach argv.
    std::map<std::wstring, std::string> values;
    for (auto &option : options) values[option.first] = utf8(option.second);
    if (options.count(L"--launch-stdin")) {
        for (const auto *key : {L"--movie", L"--url", L"--sid", L"--vars", L"--width", L"--height"}) {
            std::string line;
            for (int ch; (ch = std::getchar()) != EOF && ch != '\n';) {
                if (line.size() >= 65535) { std::fprintf(stderr, "bootstrap record too large\n"); return 2; }
                if (ch != '\r') line.push_back(static_cast<char>(ch));
            }
            if (line.empty()) { std::fprintf(stderr, "missing bootstrap record\n"); return 2; }
            values[key] = line;
        }
    }
    if (!values.count(L"--movie")) { std::fprintf(stderr, "--movie is required\n"); return 2; }
    if (!values.count(L"--flash")) {
        wchar_t env[32768]{};
        if (GetEnvironmentVariableW(L"XENOMIT_FLASH_OCX_PATH", env, 32768)) values[L"--flash"] = utf8(env);
    }
    if (values.count(L"--hidden-bootstrap") && values[L"--hidden-bootstrap"] != "draw") {
        std::fprintf(stderr, "Hidden bootstrap must be draw\n");
        return 2;
    }
    auto get = [&](const wchar_t *key) -> const char * {
        auto it = values.find(key);
        return it == values.end() ? nullptr : it->second.c_str();
    };
    auto number = [&](const wchar_t *key) { auto value = get(key); return value ? std::atoi(value) : 0; };

    XfConfig config{};
    config.size = sizeof(config);
    config.movie = get(L"--movie");
    config.base_url = get(L"--url");
    config.sid = get(L"--sid");
    config.flash_vars = get(L"--vars");
    config.width = number(L"--width");
    config.height = number(L"--height");
    config.backend = get(L"--backend");
    config.native_discovery = get(L"--native-discovery");
    config.quality = get(L"--quality");
    config.wmode = get(L"--wmode");
    config.ocx_path = get(L"--flash");
    config.user_agent = get(L"--user-agent");
    config.title = get(L"--name");
    config.hidden = options.count(L"--hidden") ? 1 : 0;
    config.hidden_draw_bootstrap = options.count(L"--hidden-bootstrap") ? 1 : 0;
    config.max_fps = number(L"--max-fps");
    config.local_proxy_port = number(L"--local-proxy");
    config.clear_cache = get(L"--clear-cache");
    config.game_ports = get(L"--game-ports");
    config.publish_named_frame = 1;
    config.pipe_server = 1;

    XfBuf error{};
    XfCode code = xf_start(&config, &error);
    // Session values are no longer needed in this process.
    SecureZeroMemory(values[L"--sid"].data(), values[L"--sid"].size());
    if (code != XF_OK) {
        if (error.data) std::fprintf(stderr, "%s\n", error.data);
        xf_free(&error);
        int32_t exit_code = 0;
        xf_wait(0, &exit_code);
        return exit_code ? exit_code : 3;
    }
    int32_t exit_code = 0;
    while (xf_wait(INFINITE, &exit_code) == XF_PENDING) {}
    return exit_code;
}
