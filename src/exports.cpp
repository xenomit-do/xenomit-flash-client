// The xenomit_flash.dll C ABI. Every export converts its arguments to owned
// values, runs Flash work through the task engine and never lets a C++
// exception cross the boundary.
#include "host.h"

#include <cstdlib>
#include <cstring>

namespace {

using namespace xf;

constexpr DWORD FrameTimeout = 4500;    // former FINVOKE bound
constexpr DWORD ImmediateTimeout = 5000; // former INVOKE bound
constexpr DWORD HostTimeout = 1000;      // InProcessPlayer sync engine default

void fill(XfBuf *buffer, const std::string &text) {
    if (!buffer) return;
    buffer->data = static_cast<char *>(std::malloc(text.size() + 1));
    buffer->len = buffer->data ? text.size() : 0;
    if (buffer->data) {
        std::memcpy(buffer->data, text.data(), text.size());
        buffer->data[text.size()] = '\0';
    }
}

std::string text(const char *value) { return value ? std::string(value) : std::string(); }
std::wstring wide(const char *value) { return utf8_to_wide(text(value)); }

template <class F>
XfCode guarded(F &&body) {
    try {
        return body();
    } catch (const std::exception &error) {
        log(2, "xenomit_flash: %s", error.what());
        return XF_INVALID;
    } catch (...) {
        log(2, "xenomit_flash: unknown error");
        return XF_INVALID;
    }
}

XfCode call(const std::string &target, const std::string &method, const std::string &json, uint32_t flags,
            uint32_t timeout_ms, XfBuf *result, uint64_t *ticket) {
    if (target.empty() || method.empty()) return XF_INVALID;
    if (!host_running()) { fill(result, "player is not running"); return XF_NOT_READY; }
    auto args = json.empty() ? std::string("[]") : json;
    CompletionPtr done;
    DWORD timeout;
    if (flags & XF_FRAME) {
        timeout = timeout_ms ? timeout_ms : FrameTimeout;
        done = begin_frame_call(target, method, args, timeout);
    } else {
        timeout = timeout_ms ? timeout_ms : ImmediateTimeout;
        done = begin_ui_call([target, method, args] {
            std::string parsed, error;
            if (flash_call("invokeAS", {target, method, args}, parsed, error)) return Outcome{XF_OK, parsed};
            return Outcome{XF_FLASH_ERROR, error};
        }, timeout);
    }
    Outcome out;
    auto code = finish(done, flags, timeout, out, ticket);
    if (!(flags & XF_ASYNC) || code != XF_OK) fill(result, out.text);
    return code;
}

XfCode direct(const std::string &json, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return call("xenomit", "direct", json, flags | XF_FRAME, 0, result, ticket);
}

XfCode game_command(const std::string &json, uint32_t flags, uint32_t timeout_ms, XfBuf *result, uint64_t *ticket) {
    return call("xenomit", "gameCommand", "[" + json + "]", flags | XF_FRAME, timeout_ms, result, ticket);
}

XfCode ui(std::function<std::string()> fn, DWORD timeout = HostTimeout, XfBuf *error = nullptr) {
    if (!host_running()) { fill(error, "player is not running"); return XF_NOT_READY; }
    auto out = run_ui([fn = std::move(fn)] {
        auto failure = fn();
        return failure.empty() ? Outcome{XF_OK, ""} : Outcome{XF_FLASH_ERROR, failure};
    }, timeout);
    if (out.code != XF_OK) {
        fill(error, out.text);
        if (!error && !out.text.empty()) log(1, "%s", out.text.c_str());
    }
    return out.code;
}

XfCode window_pos(int x, int y, int w, int h, UINT flags) {
    return ui([=] {
        HWND window = host_window();
        if (!window) return std::string("player window unavailable");
        return SetWindowPos(window, nullptr, x, y, w, h, flags | SWP_NOZORDER | SWP_NOACTIVATE) ? std::string() : std::string("SetWindowPos failed");
    });
}

std::string number(int64_t value) { return std::to_string(value); }

} // namespace

extern "C" {

XF_API uint32_t XF_CALL xf_api_version(void) { return XF_API_VERSION; }

XF_API void XF_CALL xf_free(XfBuf *buffer) {
    if (!buffer) return;
    std::free(buffer->data);
    buffer->data = nullptr;
    buffer->len = 0;
}

XF_API XfCode XF_CALL xf_start(const XfConfig *config, XfBuf *error) {
    return guarded([&] {
        if (!config || config->size < sizeof(XfConfig)) { fill(error, "XfConfig size mismatch"); return XF_INVALID; }
        set_log_sink(config->log, config->log_context);
        Settings settings;
        settings.movie = wide(config->movie);
        settings.base_url = wide(config->base_url);
        settings.sid = wide(config->sid);
        settings.flash_vars = wide(config->flash_vars);
        settings.width = config->width;
        settings.height = config->height;
        settings.backend = wide(config->backend);
        settings.discovery = wide(config->native_discovery);
        settings.quality = wide(config->quality);
        settings.wmode = wide(config->wmode);
        settings.ocx = wide(config->ocx_path);
        settings.user_agent = wide(config->user_agent);
        settings.title = wide(config->title);
        settings.hidden = config->hidden != 0;
        settings.hidden_draw = config->hidden_draw_bootstrap != 0;
        settings.max_fps = config->max_fps;
        settings.local_proxy_port = config->local_proxy_port;
        settings.clear_cache = wide(config->clear_cache);
        settings.named_frame = config->publish_named_frame != 0;
        settings.pipe_server = config->pipe_server != 0;
        for (auto ports = text(config->game_ports); !ports.empty();) {
            auto comma = ports.find(',');
            auto port = std::atoi(ports.substr(0, comma).c_str());
            if (port <= 0 || port > 65535) { fill(error, "game_ports must be a comma list of ports"); return XF_INVALID; }
            settings.game_ports.push_back(static_cast<uint16_t>(port));
            ports = comma == std::string::npos ? std::string() : ports.substr(comma + 1);
        }
        std::string failure;
        auto code = host_start(settings, failure);
        if (code != XF_OK) fill(error, failure);
        return code;
    });
}

XF_API XfCode XF_CALL xf_stop(uint32_t timeout_ms) {
    return guarded([&] { return host_stop(timeout_ms ? timeout_ms : 10000); });
}

XF_API XfCode XF_CALL xf_wait(uint32_t timeout_ms, int32_t *exit_code) {
    return guarded([&] {
        int code = 0;
        auto result = host_wait(timeout_ms, code);
        if (exit_code) *exit_code = code;
        return result;
    });
}

XF_API XfCode XF_CALL xf_reload(XfBuf *error) {
    return guarded([&] {
        std::string failure;
        auto code = host_reload(failure);
        if (code != XF_OK) fill(error, failure);
        return code;
    });
}

XF_API XfCode XF_CALL xf_status(XfStatus *status) {
    return guarded([&] {
        if (!status || status->size < sizeof(XfStatus)) return XF_INVALID;
        host_status(*status);
        return XF_OK;
    });
}

XF_API XfCode XF_CALL xf_live_frame(const uint8_t **base, size_t *len) {
    if (!base || !len) return XF_INVALID;
    *base = host_live_frame(*len);
    return *base ? XF_OK : XF_NOT_READY;
}

XF_API XfCode XF_CALL xf_state(const char *target, XfBuf *result) {
    return guarded([&] {
        auto name = text(target);
        if (!name.empty() && name != "stage") { fill(result, "cached state supports only stage"); return XF_INVALID; }
        std::string json;
        if (!host_state(json)) { fill(result, "state unavailable or stale"); return XF_NOT_READY; }
        fill(result, json);
        return XF_OK;
    });
}

XF_API XfCode XF_CALL xf_inspect(const char *target, XfBuf *result) {
    return guarded([&] {
        auto name = text(target);
        if (name.empty()) return XF_INVALID;
        if (!host_running()) { fill(result, "player is not running"); return XF_NOT_READY; }
        auto out = run_ui([name] {
            std::string parsed, error;
            return flash_call("inspectAS", {name}, parsed, error) ? Outcome{XF_OK, parsed} : Outcome{XF_FLASH_ERROR, error};
        }, ImmediateTimeout);
        fill(result, out.text);
        return out.code;
    });
}

XF_API XfCode XF_CALL xf_invoke(const char *target, const char *method, const char *json_args, uint32_t flags,
                                uint32_t timeout_ms, XfBuf *result, uint64_t *ticket) {
    return guarded([&] { return call(text(target), text(method), text(json_args), flags, timeout_ms, result, ticket); });
}

XF_API XfCode XF_CALL xf_game_command(const char *command_json, uint32_t flags, uint32_t timeout_ms, XfBuf *result, uint64_t *ticket) {
    return guarded([&] {
        auto json = text(command_json);
        if (json.empty()) return XF_INVALID;
        return game_command(json, flags, timeout_ms, result, ticket);
    });
}

XF_API XfCode XF_CALL xf_take_result(uint64_t ticket, XfBuf *result) {
    return guarded([&] {
        Outcome out;
        auto code = take_ticket(ticket, out);
        if (code != XF_PENDING && code != XF_UNKNOWN_TICKET) fill(result, out.text);
        return code;
    });
}

XF_API XfCode XF_CALL xf_move(int32_t x, int32_t y, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] { return direct("[\"move\"," + number(x) + "," + number(y) + "]", flags, result, ticket); });
}

XF_API XfCode XF_CALL xf_stop_hero(uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] { return direct("[\"stop\"]", flags, result, ticket); });
}

XF_API XfCode XF_CALL xf_select(int32_t entity_id, int32_t attack, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] { return direct("[\"select\"," + number(entity_id) + "," + (attack ? "true" : "false") + "]", flags, result, ticket); });
}

XF_API XfCode XF_CALL xf_deselect(uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] { return direct("[\"deselect\"]", flags, result, ticket); });
}

XF_API XfCode XF_CALL xf_collect(int32_t entity_id, int32_t x, int32_t y, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] {
        auto at = x == XF_AT_ENTITY || y == XF_AT_ENTITY ? std::string() : "," + number(x) + "," + number(y);
        return direct("[\"collect\"," + number(entity_id) + at + "]", flags, result, ticket);
    });
}

XF_API XfCode XF_CALL xf_use_item(const char *item_id, int32_t activate, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] {
        auto id = text(item_id);
        if (id.empty()) return XF_INVALID;
        return game_command("{\"command\":\"use_item\",\"item_id\":" + json_string(id) + ",\"activate\":" + (activate ? "true" : "false") + "}",
                            flags, 0, result, ticket);
    });
}

XF_API XfCode XF_CALL xf_refine(int32_t ore_type, uint64_t amount, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] {
        return game_command("{\"command\":\"refine_ore\",\"ore_type\":" + number(ore_type) + ",\"amount\":" + std::to_string(amount) + "}",
                            flags, 0, result, ticket);
    });
}

XF_API XfCode XF_CALL xf_notify(const char *name, const char *json_args, uint32_t flags, XfBuf *result, uint64_t *ticket) {
    return guarded([&] {
        auto notification = text(name);
        if (notification.empty()) return XF_INVALID;
        auto args = text(json_args);
        return direct("[\"notify\"," + json_string(notification) + "," + (args.empty() ? std::string("[]") : args) + "]", flags, result, ticket);
    });
}

XF_API XfCode XF_CALL xf_caps(XfBuf *result) {
    return guarded([&] { return call("xenomit", "caps", "[]", 0, 0, result, nullptr); });
}

XF_API XfCode XF_CALL xf_mouse(int32_t kind, int32_t x, int32_t y) {
    return guarded([&] { return ui([=] { return input_mouse(kind, x, y); }); });
}

XF_API XfCode XF_CALL xf_key(uint32_t virtual_key, int32_t kind) {
    return guarded([&] { return ui([=] { return input_key(virtual_key, kind); }); });
}

XF_API XfCode XF_CALL xf_text(const char *utf8) {
    return guarded([&] {
        auto value = wide(utf8);
        return ui([value] { return input_text(value); });
    });
}

XF_API XfCode XF_CALL xf_paste(const char *utf8) {
    return guarded([&] {
        auto value = wide(utf8);
        return ui([value] { return input_paste(value); });
    });
}

XF_API XfCode XF_CALL xf_set_user_input(int32_t enabled) {
    host_set_user_input(enabled != 0);
    return XF_OK;
}

XF_API XfCode XF_CALL xf_set_visible(int32_t visible) {
    return guarded([&] {
        return ui([=] {
            HWND window = host_window();
            if (!window) return std::string("player window unavailable");
            ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
            return std::string();
        });
    });
}

XF_API XfCode XF_CALL xf_set_position(int32_t x, int32_t y) {
    return guarded([&] { return window_pos(x, y, 0, 0, SWP_NOSIZE); });
}

XF_API XfCode XF_CALL xf_set_size(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return XF_INVALID;
    return guarded([&] { return window_pos(0, 0, width, height, SWP_NOMOVE); });
}

XF_API XfCode XF_CALL xf_set_client_size(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return XF_INVALID;
    return guarded([&] {
        return ui([=] {
            HWND window = host_window();
            if (!window) return std::string("player window unavailable");
            RECT rect{0, 0, width, height};
            AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
            return SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ? std::string() : std::string("SetWindowPos failed");
        });
    });
}

XF_API XfCode XF_CALL xf_set_min_client_size(int32_t width, int32_t height) {
    if (width < 0 || height < 0) return XF_INVALID;
    host_set_min_client_size(width, height);
    return XF_OK;
}

XF_API XfCode XF_CALL xf_set_minimized(int32_t minimized) {
    return guarded([&] {
        return ui([=] {
            HWND window = host_window();
            if (!window) return std::string("player window unavailable");
            ShowWindow(window, minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
            return std::string();
        });
    });
}

XF_API XfCode XF_CALL xf_set_transparency(int32_t alpha) {
    if (alpha < 0 || alpha > 255) return XF_INVALID;
    return guarded([&] {
        return ui([=] {
            HWND window = host_window();
            if (!window) return std::string("player window unavailable");
            auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if (alpha == 255) {
                SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
                return std::string();
            }
            SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
            return SetLayeredWindowAttributes(window, 0, static_cast<BYTE>(alpha), LWA_ALPHA) ? std::string() : std::string("SetLayeredWindowAttributes failed");
        });
    });
}

XF_API XfCode XF_CALL xf_set_max_fps(int32_t fps) {
    if (fps < 0 || fps > 100) return XF_INVALID;
    return guarded([&] { return ui([=] { return host_set_frame_settings(fps, -1); }); });
}

XF_API XfCode XF_CALL xf_set_volume(int32_t percent) {
    if (percent < 0 || percent > 100) return XF_INVALID;
    return guarded([&] { return ui([=] { return host_set_frame_settings(-1, percent); }); });
}

XF_API XfCode XF_CALL xf_set_quality(const char *quality) {
    return guarded([&] {
        auto value = text(quality);
        return ui([value] { return host_set_quality(value); });
    });
}

XF_API XfCode XF_CALL xf_set_backend(const char *backend, XfBuf *error) {
    return guarded([&] {
        auto value = text(backend);
        return ui([value] { return host_set_backend(value); }, ImmediateTimeout, error);
    });
}

XF_API XfCode XF_CALL xf_set_blocking_patterns(const char *const *patterns, size_t count) {
    return guarded([&] {
        if (count && !patterns) return XF_INVALID;
        std::vector<std::string> values;
        for (size_t i = 0; i < count; ++i) values.push_back(text(patterns[i]));
        hooks_set_blocking(std::move(values));
        return XF_OK;
    });
}

XF_API XfCode XF_CALL xf_clear_cache(const char *substring, uint32_t *deleted) {
    return guarded([&] {
        auto count = clear_url_cache(wide(substring));
        if (deleted) *deleted = count;
        return XF_OK;
    });
}

XF_API XfCode XF_CALL xf_trim(void) {
    // InProcessPlayer emptyWorkingSet.
    return SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)) ? XF_OK : XF_INVALID;
}

XF_API XfCode XF_CALL xf_read(uint64_t address, void *out, size_t len) {
    if (!out || !len) return XF_INVALID;
    return guarded_copy(out, static_cast<uintptr_t>(address), len) ? XF_OK : XF_INVALID;
}

XF_API XfCode XF_CALL xf_query_bytes(const uint8_t *pattern, const uint8_t *mask, size_t len, uint64_t *out,
                                     size_t max, size_t *count) {
    return guarded([&] {
        if (!pattern || !len || !out || !max) return XF_INVALID;
        auto found = query_bytes(pattern, mask, len, out, max);
        if (count) *count = found;
        return XF_OK;
    });
}

XF_API XfCode XF_CALL xf_exchange(const char *command, XfBuf *reply) {
    return guarded([&] {
        if (!command) return XF_INVALID;
        fill(reply, dispatch_command(command));
        return XF_OK;
    });
}

} // extern "C"

// Layouts mirrored by the Rust binding (xenomit-game-api src/bridge/flash_ffi.rs).
static_assert(sizeof(XfStatus) == 200 && offsetof(XfStatus, native_root) == 104 && offsetof(XfStatus, cpu_percent) == 184,
              "XfStatus layout is part of the ABI");
static_assert(sizeof(XfConfig) == 160 && offsetof(XfConfig, hidden) == 104 && offsetof(XfConfig, log) == 144,
              "XfConfig layout is part of the ABI");
