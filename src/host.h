#pragma once
// Internal interfaces shared by the host modules. The public C ABI is in
// include/xenomit_flash.h; nothing here crosses the DLL boundary.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenomit_flash.h"

namespace xf {

// Logging goes to the XfConfig callback, or stderr when none is set.
void log(int level, const char *format, ...);

// ---- Task engine (task_queue.cpp) -------------------------------------------
// InProcessPlayer's ring queue + sync engine: owned tasks go to the STA, callers
// either wait with a bound (sync) or receive a ticket (async).
struct Outcome {
    XfCode code = XF_OK;
    std::string text;
};

class Completion {
public:
    void complete(Outcome outcome);
    // False on timeout. Marks the completion cancelled so queued work is skipped.
    bool wait(DWORD timeout_ms, Outcome &out);
    bool done();
    bool cancelled() const { return cancelled_.load(); }
    void cancel() { cancelled_ = true; }
    ULONGLONG created = GetTickCount64();
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    bool done_ = false;
    std::atomic<bool> cancelled_{false};
    Outcome outcome_;
};
using CompletionPtr = std::shared_ptr<Completion>;
// Runs on the STA. It completes the completion now or later (frame calls).
using UiJob = std::function<void(const CompletionPtr &)>;

void queue_attach(HWND window, UINT message);
void queue_detach(const char *reason);
bool on_ui_thread();
void set_ui_thread(DWORD id);
// Called by the window procedure for the queue message.
void queue_drain();
// Begins work on the STA. Returns a completion that is already finished when
// the request could not be queued.
CompletionPtr begin_ui(UiJob job, DWORD deadline_ms, CompletionPtr done = nullptr);
// Convenience: run a function on the STA and complete with its outcome.
CompletionPtr begin_ui_call(std::function<Outcome()> fn, DWORD deadline_ms);
// Bridge FIFO call completed by the bridge's xenomitResult push.
CompletionPtr begin_frame_call(const std::string &target, const std::string &method,
                               const std::string &json_args, DWORD deadline_ms);
void complete_frame_call(uint64_t id, std::string value);
// Sync wait or async ticket, according to XF_ASYNC.
XfCode finish(const CompletionPtr &done, uint32_t flags, DWORD timeout_ms, Outcome &out, uint64_t *ticket);
XfCode take_ticket(uint64_t ticket, Outcome &out);
Outcome run_ui(std::function<Outcome()> fn, DWORD timeout_ms);

// ---- Host (host.cpp) ----------------------------------------------------------
struct Settings {
    std::wstring movie, base_url, sid, flash_vars, backend, discovery, quality, wmode, ocx, user_agent, title;
    int width = 1280, height = 800, max_fps = 0, local_proxy_port = 0;
    bool hidden = false, hidden_draw = false, named_frame = false, pipe_server = false;
    std::wstring clear_cache;
    std::vector<uint16_t> game_ports;
};
XfCode host_start(const Settings &settings, std::string &error);
XfCode host_stop(DWORD timeout_ms);
XfCode host_reload(std::string &error);
XfCode host_wait(DWORD timeout_ms, int &exit_code);
void set_log_sink(XfLogFn fn, void *context);
bool host_running();
bool host_stopping();
// STA only.
bool flash_call(const std::string &name, const std::vector<std::string> &args, std::string &parsed, std::string &error);
HWND host_window();
HWND flash_window();
void host_status(XfStatus &status);
std::string host_status_line();
bool host_state(std::string &json);
const uint8_t *host_live_frame(size_t &len);
// STA only; returns an error message or empty.
std::string host_set_backend(const std::string &backend);
std::string host_set_quality(const std::string &quality);
// Negative values keep the current setting.
std::string host_set_frame_settings(int fps, int volume);
void host_set_user_input(bool enabled);
void host_set_min_client_size(int width, int height);

// ---- Input and window (input.cpp) ------------------------------------------------
// STA only.
std::string input_mouse(int kind, int x, int y);
std::string input_key(UINT vk, int kind);
std::string input_text(const std::wstring &text);
std::string input_paste(const std::wstring &text);
bool input_filter(const MSG &message); // true drops a real input message

// ---- Hooks (hooks.cpp) -------------------------------------------------------------
struct HookOptions {
    int local_proxy_port = 0;
    std::vector<uint16_t> game_ports;
};
void hooks_install(const HookOptions &options);
void hooks_set_blocking(std::vector<std::string> patterns);
bool hooks_blocked(const wchar_t *text);
void hooks_status(XfStatus &status);
uint32_t clear_url_cache(const std::wstring &substring);
// Applies the configured proxy to a WinINet session opened by the host.
DWORD proxy_access_type(std::wstring &proxy);

// ---- Pipe server (pipe_server.cpp) -------------------------------------------------
HANDLE pipe_server_start();
std::string dispatch_command(const std::string &command);

// ---- Utilities ---------------------------------------------------------------------
std::wstring utf8_to_wide(const std::string &value);
std::string wide_to_utf8(const wchar_t *value, int chars);
inline std::string wide_to_utf8(const std::wstring &value) {
    return wide_to_utf8(value.c_str(), static_cast<int>(value.size()));
}
std::string json_string(const std::string &value);
// SEH-guarded copy from an arbitrary address (native_state.cpp).
bool guarded_copy(void *to, uintptr_t from, size_t size);
size_t query_bytes(const uint8_t *pattern, const uint8_t *mask, size_t len, uint64_t *out, size_t max);

} // namespace xf
