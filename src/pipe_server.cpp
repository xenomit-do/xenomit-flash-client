// Out-of-process fallback: the legacy one-command-per-connection pipe, now a
// thin text front end over the same exports an in-process caller uses.
#include "host.h"

#include <cstdlib>
#include <sstream>

namespace xf {
namespace {

std::string take(XfBuf &buffer) {
    std::string value(buffer.data ? buffer.data : "", buffer.len);
    xf_free(&buffer);
    return value;
}

std::string reply(XfCode code, const std::string &text) {
    return (code == XF_OK ? "OK " : "ERR ") + text + "\n";
}

std::string code_reply(XfCode code, const std::string &ok) {
    if (code == XF_OK) return "OK " + ok + "\n";
    switch (code) {
        case XF_INVALID: return "ERR invalid arguments\n";
        case XF_NOT_READY: return "ERR player or game not ready\n";
        case XF_TIMEOUT_UNKNOWN: return "ERR call timed out; execution may have started; do not retry automatically\n";
        default: return "ERR request failed (" + std::to_string(code) + ")\n";
    }
}

std::vector<std::string> split(const std::string &text, char separator) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (;;) {
        auto next = text.find(separator, at);
        parts.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return parts;
}

bool integer(const std::string &text, long long &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    value = std::strtoll(text.c_str(), &end, 10);
    return end && *end == '\0';
}

// Commands that run in the game and support ASYNC.
std::string game_call(const std::vector<std::string> &words, const std::string &command, bool async) {
    const uint32_t flags = async ? XF_ASYNC : 0;
    XfBuf result{};
    uint64_t ticket = 0;
    XfCode code = XF_INVALID;
    const auto &verb = words[0];
    long long a = 0, b = 0;
    if (verb == "FINVOKE" || verb == "INVOKE") {
        auto fields = split(command.substr(verb.size() + 1), '\t');
        if (fields.size() < 3) return "ERR expected target, method, JSON arguments\n";
        auto json = command.substr(verb.size() + 1 + fields[0].size() + 1 + fields[1].size() + 1);
        code = xf_invoke(fields[0].c_str(), fields[1].c_str(), json.c_str(), flags | (verb == "FINVOKE" ? XF_FRAME : 0), 0, &result, &ticket);
    } else if (verb == "MOVE" && words.size() == 3 && integer(words[1], a) && integer(words[2], b)) {
        code = xf_move(static_cast<int32_t>(a), static_cast<int32_t>(b), flags, &result, &ticket);
    } else if (verb == "STOP" && words.size() == 1) {
        code = xf_stop_hero(flags, &result, &ticket);
    } else if (verb == "SELECT" && (words.size() == 2 || words.size() == 3) && integer(words[1], a)) {
        code = xf_select(static_cast<int32_t>(a), words.size() == 3 && words[2] == "1", flags, &result, &ticket);
    } else if (verb == "DESELECT" && words.size() == 1) {
        code = xf_deselect(flags, &result, &ticket);
    } else if (verb == "COLLECT" && (words.size() == 2 || words.size() == 4) && integer(words[1], a)) {
        long long x = XF_AT_ENTITY, y = XF_AT_ENTITY;
        if (words.size() == 4 && !(integer(words[2], x) && integer(words[3], y))) return "ERR invalid arguments\n";
        code = xf_collect(static_cast<int32_t>(a), static_cast<int32_t>(x), static_cast<int32_t>(y), flags, &result, &ticket);
    } else if (verb == "USE" && (words.size() == 2 || words.size() == 3)) {
        code = xf_use_item(words[1].c_str(), words.size() == 3 && words[2] == "1", flags, &result, &ticket);
    } else if (verb == "REFINE" && words.size() == 3 && integer(words[1], a) && integer(words[2], b) && b > 0) {
        code = xf_refine(static_cast<int32_t>(a), static_cast<uint64_t>(b), flags, &result, &ticket);
    } else if (verb == "NOTIFY") {
        auto fields = split(command, '\t');
        if (fields.size() < 2 || fields.size() > 3) return "ERR expected NOTIFY<TAB>name[<TAB>JSON arguments]\n";
        code = xf_notify(fields[1].c_str(), fields.size() == 3 ? fields[2].c_str() : nullptr, flags, &result, &ticket);
    } else {
        return "ERR invalid arguments\n";
    }
    if (async && code == XF_OK) return "OK ticket=" + std::to_string(ticket) + "\n";
    return reply(code, take(result));
}

} // namespace

std::string dispatch_command(const std::string &raw) {
    std::string command = raw;
    while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) command.pop_back();
    if (command.find('\0') != std::string::npos) return "ERR embedded NUL\n";
    if (command == "STATUS") return host_status_line();
    if (command == "VERSION") return "OK host=1 api=" + std::to_string(xf_api_version()) + "\n";
    // Words separate on spaces or a tab, matching the legacy verbs.
    auto first = command.find_first_of(" \t");
    auto verb = command.substr(0, first);
    std::vector<std::string> words;
    {
        std::istringstream stream(command);
        for (std::string word; stream >> word;) words.push_back(word);
    }
    if (words.empty()) return "ERR unknown command\n";
    long long a = 0, b = 0;

    if (verb == "QUALITY" && words.size() == 2) {
        auto code = xf_set_quality(words[1].c_str());
        return code_reply(code, "quality=" + words[1]);
    }
    if (verb == "BACKEND" && words.size() == 2) {
        XfBuf error{};
        auto code = xf_set_backend(words[1].c_str(), &error);
        auto text = take(error);
        return code == XF_OK ? "OK " + words[1] + "\n" : "ERR " + (text.empty() ? std::string("backend change failed") : text) + "\n";
    }
    if (command == "CAPTURE 0" || command == "CAPTURE 1") {
        auto value = command.substr(8);
        auto out = run_ui([value] {
            std::string parsed, error;
            return flash_call("configureAS", {value}, parsed, error) ? Outcome{XF_OK, parsed} : Outcome{XF_FLASH_ERROR, error};
        }, 5000);
        return reply(out.code, out.text);
    }
    if (verb == "WINDOW") {
        if (command == "WINDOW 0" || command == "WINDOW 1")
            return code_reply(xf_set_visible(command[7] == '1'), "visible=" + command.substr(7));
        if (words.size() == 4 && integer(words[2], a) && integer(words[3], b)) {
            auto x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
            if (words[1] == "pos") return code_reply(xf_set_position(x, y), "position");
            if (words[1] == "size") return code_reply(xf_set_size(x, y), "size");
            if (words[1] == "client") return code_reply(xf_set_client_size(x, y), "client");
            if (words[1] == "minclient") return code_reply(xf_set_min_client_size(x, y), "minclient");
        }
        if (words.size() == 3 && integer(words[2], a)) {
            if (words[1] == "minimize") return code_reply(xf_set_minimized(a != 0), "minimized=" + words[2]);
            if (words[1] == "alpha") return code_reply(xf_set_transparency(static_cast<int32_t>(a)), "alpha=" + words[2]);
        }
        return "ERR invalid arguments\n";
    }
    if (verb == "FPS" && words.size() == 2 && integer(words[1], a)) return code_reply(xf_set_max_fps(static_cast<int32_t>(a)), "fps=" + words[1]);
    if (verb == "VOLUME" && words.size() == 2 && integer(words[1], a)) return code_reply(xf_set_volume(static_cast<int32_t>(a)), "volume=" + words[1]);
    if (verb == "INPUT" && words.size() == 2) return code_reply(xf_set_user_input(words[1] == "1"), "user_input=" + words[1]);
    if (verb == "KEY" && (words.size() == 2 || words.size() == 3) && integer(words[1], a)) {
        int kind = words.size() == 2 || words[2] == "click" ? XF_KEY_CLICK : words[2] == "down" ? XF_KEY_DOWN : words[2] == "up" ? XF_KEY_UP : -1;
        return code_reply(xf_key(static_cast<uint32_t>(a), kind), "key");
    }
    if (verb == "MOUSE" && words.size() == 4 && integer(words[2], a) && integer(words[3], b)) {
        int kind = words[1] == "move" ? XF_MOUSE_MOVE : words[1] == "down" ? XF_MOUSE_DOWN : words[1] == "up" ? XF_MOUSE_UP : words[1] == "click" ? XF_MOUSE_CLICK : -1;
        return code_reply(xf_mouse(kind, static_cast<int32_t>(a), static_cast<int32_t>(b)), "mouse");
    }
    if (verb == "TEXT" && command.size() > 5) return code_reply(xf_text(command.substr(5).c_str()), "text");
    if (verb == "PASTE" && command.size() > 6) return code_reply(xf_paste(command.substr(6).c_str()), "paste");
    if (verb == "BLOCK") {
        auto fields = split(command, '\t');
        std::vector<const char *> patterns;
        for (size_t i = 1; i < fields.size(); ++i) if (!fields[i].empty()) patterns.push_back(fields[i].c_str());
        if (command != "BLOCK clear" && patterns.empty()) return "ERR expected BLOCK clear or BLOCK<TAB>pattern...\n";
        return code_reply(xf_set_blocking_patterns(patterns.data(), patterns.size()), "patterns=" + std::to_string(patterns.size()));
    }
    if (verb == "CACHE" && command.size() > 6) {
        uint32_t deleted = 0;
        auto code = xf_clear_cache(command.substr(6).c_str(), &deleted);
        return code_reply(code, "deleted=" + std::to_string(deleted));
    }
    if (command == "RELOAD") {
        XfBuf error{};
        auto code = xf_reload(&error);
        auto text = take(error);
        return code == XF_OK ? "OK reloaded\n" : "ERR " + text + "\n";
    }
    if (command == "TRIM") return code_reply(xf_trim(), "trimmed");
    if (command == "CAPS") {
        XfBuf result{};
        auto code = xf_caps(&result);
        return reply(code, take(result));
    }
    if (verb == "RESULT" && words.size() == 2 && integer(words[1], a)) {
        XfBuf result{};
        auto code = xf_take_result(static_cast<uint64_t>(a), &result);
        if (code == XF_PENDING) return "OK pending\n";
        if (code == XF_UNKNOWN_TICKET) return "ERR unknown or expired ticket\n";
        return reply(code, take(result));
    }
    if (command == "BRIDGE" || command == "REFERENCE") {
        auto name = command == "BRIDGE" ? "pingBridge" : "referenceAS";
        auto out = run_ui([name] {
            std::string parsed, error;
            return flash_call(name, {}, parsed, error) ? Outcome{XF_OK, parsed} : Outcome{XF_FLASH_ERROR, error};
        }, 5000);
        return reply(out.code, out.text);
    }
    if (command == "STATE" || command == "STATE\tstage") {
        XfBuf result{};
        auto code = xf_state("stage", &result);
        return reply(code, take(result));
    }
    if (command.rfind("STATE\t", 0) == 0) return "ERR cached state supports only stage\n";
    if (command.rfind("INSPECT\t", 0) == 0) {
        XfBuf result{};
        auto code = xf_inspect(command.substr(8).c_str(), &result);
        return reply(code, take(result));
    }
    bool async = verb == "ASYNC";
    if (async) {
        auto rest = command.substr(first == std::string::npos ? command.size() : first + 1);
        auto inner = rest.substr(0, rest.find_first_of(" \t"));
        std::vector<std::string> inner_words(words.begin() + 1, words.end());
        if (inner_words.empty()) return "ERR unknown command\n";
        inner_words[0] = inner;
        return game_call(inner_words, rest, true);
    }
    static const char *game_verbs[] = {"INVOKE", "FINVOKE", "MOVE", "STOP", "SELECT", "DESELECT", "COLLECT", "USE", "REFINE", "NOTIFY"};
    for (auto name : game_verbs) if (verb == name) return game_call(words, command, false);
    return "ERR unknown command\n";
}

namespace {

// Overlapped pipe I/O has a bounded wait. A client that connects and disappears
// cannot occupy the only pipe forever. Never wait for FlushFileBuffers.
bool finish_io(HANDLE pipe, OVERLAPPED &overlap, BOOL started, DWORD &bytes, DWORD timeout) {
    if (started) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    auto wait = WaitForSingleObject(overlap.hEvent, timeout);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlap);
        GetOverlappedResult(pipe, &overlap, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &overlap, &bytes, FALSE) != FALSE;
}

DWORD WINAPI control_thread(void *) {
    wchar_t name[96]{};
    swprintf_s(name, L"\\\\.\\pipe\\xenomit_flash_%lu", GetCurrentProcessId());
    // Keep the owned endpoint alive across connections. Recreating a FIRST_PIPE
    // instance races the reader's closing handle after its receipt byte.
    HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 1 << 20, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { log(2, "Control pipe creation failed: %lu", GetLastError()); return 1; }
    log(0, "Control pipe: %ls", name);
    OVERLAPPED overlap{}; overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlap.hEvent) { CloseHandle(pipe); return 1; }
    while (!host_stopping()) {
        ResetEvent(overlap.hEvent);
        DWORD bytes = 0;
        BOOL connected = ConnectNamedPipe(pipe, &overlap);
        DWORD err = connected ? ERROR_SUCCESS : GetLastError();
        bool ok = connected || err == ERROR_PIPE_CONNECTED;
        if (!ok && err == ERROR_IO_PENDING) {
            while (!host_stopping() && WaitForSingleObject(overlap.hEvent, 100) == WAIT_TIMEOUT) {}
            if (host_stopping()) { CancelIoEx(pipe, &overlap); GetOverlappedResult(pipe, &overlap, &bytes, TRUE); }
            else ok = GetOverlappedResult(pipe, &overlap, &bytes, FALSE) != FALSE;
        }
        if (ok && !host_stopping()) {
            ResetEvent(overlap.hEvent);
            std::vector<char> input(65536);
            if (finish_io(pipe, overlap, ReadFile(pipe, input.data(), 65535, &bytes, &overlap), bytes, 5000) && bytes) {
                auto reply_text = dispatch_command(std::string(input.data(), bytes));
                ResetEvent(overlap.hEvent);
                if (finish_io(pipe, overlap, WriteFile(pipe, reply_text.data(), static_cast<DWORD>(reply_text.size()), &bytes, &overlap), bytes, 5000)) {
                    ResetEvent(overlap.hEvent);
                    char receipt = 0;
                    finish_io(pipe, overlap, ReadFile(pipe, &receipt, 1, &bytes, &overlap), bytes, 2000);
                }
            }
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(overlap.hEvent); CloseHandle(pipe);
    return 0;
}

} // namespace

HANDLE pipe_server_start() {
    return CreateThread(nullptr, 0, control_thread, nullptr, 0, nullptr);
}

} // namespace xf
