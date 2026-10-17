// Flash ActiveX host: the STA thread, OCX activation, window, movie loading
// and the Flash event sink. This is the former player.cpp core; it now runs
// on its own thread inside whichever process loaded xenomit_flash.dll.
#include "host.h"

#include <windowsx.h>
#include <wininet.h>
#include <urlmon.h>
#include <ole2.h>
#include <olectl.h>
#include <wincrypt.h>
#include <psapi.h>
#include <mmsystem.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "as_bridge.h"
#include "swf_bridge.h"
#include "native_state.h"
#include "game_offsets.h"

#import "Flash.ocx" raw_interfaces_only, raw_native_types, named_guids, no_smart_pointers, rename_namespace("Flash")

using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);
using AtlAxWinInitFn = BOOL (WINAPI *)();
using AtlAxAttachControlFn = HRESULT (WINAPI *)(IUnknown *, HWND, IUnknown **);

namespace xf {
namespace {

Settings g_settings;
XfLogFn g_log = nullptr;
void *g_log_context = nullptr;

HMODULE g_ocx = nullptr;
HMODULE g_atl = nullptr;
IOleObject *g_object = nullptr;
IOleInPlaceObject *g_in_place = nullptr;
Flash::IShockwaveFlash *g_flash = nullptr;
HWND g_window = nullptr;
HWND g_host_window = nullptr;
IUnknown *g_atl_container = nullptr;
constexpr UINT WM_XF_CALL = WM_APP + 78;
constexpr UINT WM_XF_STOP = WM_APP + 79;
std::atomic<bool> g_bridge_injected{false};
std::atomic<bool> g_bridge_ready{false};
std::atomic<bool> g_game_ready{false};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_user_input{true};
std::atomic<int> g_min_client_width{0}, g_min_client_height{0};
std::atomic<int> g_max_fps{0}, g_volume{100};
std::wstring g_bridge_temp;
unsigned g_bridge_generation = 0;
std::string g_page_url;
std::mutex g_cache_mutex;
std::string g_snapshot;
std::atomic<bool> g_native_mode{false};
bool g_native_runtime_supported = false;
// FlashVar gameclientHash of the running client; native offsets are generated per build.
std::string g_game_client_hash;
NativeDiscovery g_native_discovery = NativeDiscovery::Auto;
bool g_hidden_draw_bootstrap = false;
HDC g_bootstrap_dc = nullptr;
HBITMAP g_bootstrap_bitmap = nullptr;
HGDIOBJ g_bootstrap_old_bitmap = nullptr;
std::atomic<uint64_t> g_bootstrap_draws{0}, g_bootstrap_failures{0};
constexpr UINT_PTR BootstrapTimer = 0xDB07;
std::unique_ptr<NativeState> g_native;
std::atomic<const uint8_t *> g_frame_base{nullptr};
std::atomic<size_t> g_frame_size{0};
std::string g_native_navigation;
bool g_native_navigation_dirty = true;
// Accessed only by Flash callbacks on the UI thread.
std::string g_navigation;
ULONGLONG g_navigation_at = 0;
std::string g_catalog = "{\"maps\":[],\"links\":[]}";
std::string g_items = "{}";
ULONGLONG g_items_at = 0;
bool g_items_valid = true;
std::atomic<uint64_t> g_item_updates{0}, g_item_refreshes{0};
ULONGLONG g_snapshot_at = 0;
std::atomic<uint64_t> g_captures{0}, g_capture_total_us{0}, g_capture_max_us{0};
std::atomic<uint64_t> g_frames{0}, g_fps_milli{0}, g_metrics_at{0};
std::atomic<bool> g_native_root_valid{false};
char g_native_root[16] = "none";

HANDLE g_thread = nullptr;
HANDLE g_pipe_thread = nullptr;
std::atomic<int> g_exit_code{0};
std::mutex g_start_mutex;

// CPU usage between status reads (InProcessPlayer getCpuUsage).
std::mutex g_cpu_mutex;
ULONGLONG g_cpu_wall = 0, g_cpu_busy = 0;
double g_cpu_percent = 0;

void finish_hidden_bootstrap() {
    if (g_window) KillTimer(g_window, BootstrapTimer);
    if (g_bootstrap_dc) {
        if (g_bootstrap_old_bitmap) SelectObject(g_bootstrap_dc, g_bootstrap_old_bitmap);
        if (g_bootstrap_bitmap) DeleteObject(g_bootstrap_bitmap);
        DeleteDC(g_bootstrap_dc);
    }
    g_bootstrap_dc = nullptr; g_bootstrap_bitmap = nullptr; g_bootstrap_old_bitmap = nullptr;
}

bool arm_hidden_bootstrap() {
    RECT bounds{};
    GetClientRect(g_window, &bounds);
    HDC screen = GetDC(g_window);
    g_bootstrap_dc = CreateCompatibleDC(screen);
    g_bootstrap_bitmap = CreateCompatibleBitmap(screen, std::max<LONG>(bounds.right, 1), std::max<LONG>(bounds.bottom, 1));
    ReleaseDC(g_window, screen);
    if (!g_bootstrap_dc || !g_bootstrap_bitmap) { finish_hidden_bootstrap(); return false; }
    g_bootstrap_old_bitmap = SelectObject(g_bootstrap_dc, g_bootstrap_bitmap);
    if (!SetTimer(g_window, BootstrapTimer, 16, nullptr)) { finish_hidden_bootstrap(); return false; }
    return true;
}

void log_hresult(const char *operation, HRESULT hr) {
    log(2, "%s failed: HRESULT 0x%08lx", operation, static_cast<unsigned long>(hr));
}

bool pinned_native_runtime(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; bool ok = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        std::vector<BYTE> data(65536); DWORD n = 0; ok = true;
        for (;;) { if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &n, nullptr)) { ok = false; break; } if (!n) break; if (!CryptHashData(hash, data.data(), n, 0)) { ok = false; break; } }
        BYTE digest[32]{}; DWORD size = 32;
        const BYTE expected[] = {0xB0,0x47,0x55,0x12,0xD5,0xAD,0x44,0x9D,0xFA,0x7D,0x21,0xB5,0xA2,0x64,0x5C,0xB1,0x71,0x1D,0x2C,0xE9,0x8B,0x42,0xC9,0xFE,0x00,0x0B,0x5F,0xBC,0x0F,0x82,0x31,0xBD};
        // Same exact signed 34.0.0.267 image before the five compatibility
        // edits. VM layout is unchanged; game class/method guards still apply.
        const BYTE stock[] = {0x9B,0x39,0xAC,0xE0,0x1E,0x55,0x7A,0x4A,0x4D,0x46,0x9B,0xBB,0x4E,0xC7,0xAD,0xF2,0x7D,0x18,0xB9,0xCF,0x3B,0x1C,0xB9,0x6A,0xB2,0x01,0xAC,0xDE,0xFA,0xBA,0xE9,0xF0};
        ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == 32 && (!memcmp(digest, expected, 32) || !memcmp(digest, stock, 32));
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return ok;
}

HRESULT put_bstr(HRESULT (__stdcall Flash::IShockwaveFlash::*setter)(BSTR), const std::wstring &value) {
    BSTR text = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    if (!text) return E_OUTOFMEMORY;
    HRESULT hr = (g_flash->*setter)(text);
    SysFreeString(text);
    return hr;
}

void configure_user_agent(const std::wstring &wide_value) {
    auto value = wide_to_utf8(wide_value);
    if (value.empty()) return;
    HRESULT hr = UrlMkSetSessionOption(URLMON_OPTION_USERAGENT,
        const_cast<char *>(value.c_str()), static_cast<DWORD>(value.size()), 0);
    if (FAILED(hr)) log_hresult("UrlMkSetSessionOption(USERAGENT)", hr);
    log(0, "User agent: %s", value.c_str());
}

void bridge_log(const std::string &text) {
    log(0, "[flash-bridge] %s", text.c_str());
}

std::vector<uint8_t> download_movie(const std::wstring &url, std::string &error) {
    bool is_remote = url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
    if (!is_remote) {
        HANDLE file = CreateFileW(url.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = "could not open movie file"; return {}; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64ll * 1024 * 1024) {
            CloseHandle(file);
            error = "movie file size is invalid";
            return {};
        }
        std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != data.size()) { error = "could not read movie file"; return {}; }
        return data;
    }
    std::wstring proxy;
    DWORD access = proxy_access_type(proxy);
    HINTERNET inet = InternetOpenW(g_settings.user_agent.c_str(), access,
        proxy.empty() ? nullptr : proxy.c_str(), proxy.empty() ? nullptr : L"<local>", 0);
    if (!inet) { error = "InternetOpen failed"; return {}; }
    DWORD timeout = 20000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    DWORD decode = TRUE;
    InternetSetOptionW(inet, INTERNET_OPTION_HTTP_DECODING, &decode, sizeof(decode));
    HINTERNET remote = InternetOpenUrlW(inet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!remote) { InternetCloseHandle(inet); error = "InternetOpenUrl failed"; return {}; }
    DWORD status = 0;
    DWORD length = sizeof(status);
    if (HttpQueryInfoW(remote, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &length, nullptr) && status >= 400) {
        InternetCloseHandle(remote);
        InternetCloseHandle(inet);
        error = "movie download HTTP " + std::to_string(status);
        return {};
    }
    std::vector<uint8_t> data;
    uint8_t buffer[16384];
    DWORD got = 0;
    for (;;) {
        if (!InternetReadFile(remote, buffer, sizeof(buffer), &got)) { error = "movie download was interrupted"; data.clear(); break; }
        if (!got) break;
        if (data.size() + got > 64u * 1024u * 1024u) { error = "movie download is too large"; data.clear(); break; }
        data.insert(data.end(), buffer, buffer + got);
    }
    InternetCloseHandle(remote);
    InternetCloseHandle(inet);
    if (!error.empty()) return {};
    if (data.size() < 8) { error = "movie download was empty"; return {}; }
    return data;
}

bool load_bridged_movie(const std::wstring &movie) {
    std::string error;
    auto bytes = download_movie(movie, error);
    if (bytes.empty()) {
        bridge_log("injected=0 load=url error=" + error);
        return false;
    }
    auto patched = swf_bridge::inject(bytes.data(), bytes.size());
    if (!patched.ok) {
        bridge_log("injected=0 load=url compression=" + patched.compression + " error=" + patched.error);
        return false;
    }
    // CallFunction only binds to a movie that has a URL. put_MovieData loads the
    // bytes, but ExternalInterface stays unavailable, so the patched movie is
    // staged through a temporary file. The downloaded original is never written.
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) {
        bridge_log("injected=0 load=url error=temp path unavailable");
        return false;
    }
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%sdobot_bridge_%lu_%u.swf", directory, GetCurrentProcessId(), ++g_bridge_generation);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD written = 0;
    BOOL wrote = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, patched.swf.data(), static_cast<DWORD>(patched.swf.size()), &written, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!wrote || written != patched.swf.size()) {
        bridge_log("injected=0 load=url error=temp movie write failed");
        DeleteFileW(path);
        return false;
    }
    std::wstring staged = L"file:///" + std::wstring(path);
    for (auto &ch : staged) if (ch == L'\\') ch = L'/';
    HRESULT security = g_flash->DisableLocalSecurity();
    HRESULT hr = put_bstr(&Flash::IShockwaveFlash::put_Movie, staged);
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "injected=%d compression=%s original_bytes=%zu patched_bytes=%zu security=0x%08lx movie=0x%08lx load=%s",
        SUCCEEDED(hr) ? 1 : 0, patched.compression.c_str(), bytes.size(), patched.swf.size(),
        static_cast<unsigned long>(security), static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "temp" : "url");
    bridge_log(line);
    if (FAILED(hr)) { DeleteFileW(path); return false; }
    if (!g_bridge_temp.empty()) DeleteFileW(g_bridge_temp.c_str());
    g_bridge_temp = path;
    g_bridge_injected.store(true);
    return true;
}

// Empty when native capture may run; otherwise the reason it must not.
std::string native_unavailable() {
    namespace go = xenomit::game_offsets;
    if (!g_native_runtime_supported) return "Native offsets require a pinned 34.0.0.267 runtime";
    if (g_game_client_hash.empty()) return "Native offsets require the gameclientHash FlashVar of an authenticated launch";
    if (g_game_client_hash != go::kGameClientHash)
        return "Native profile was generated for game build " + std::string(go::kGameClientHash) + " but " + g_game_client_hash + " is running; regenerate generated/game_offsets.h or select cached";
    return {};
}

// The frame outlives host restarts: in-process readers keep its address
// until the process exits.
void create_native_state() {
    if (g_native) { g_native->invalidate(); return; }
    g_native = std::make_unique<NativeState>(g_native_discovery, g_settings.named_frame);
    size_t size = 0;
    auto base = g_native->frame(size);
    g_frame_size = size;
    g_frame_base = base;
}

double json_number(const std::string &json, const char *name) {
    auto pos = json.find(std::string("\"") + name + "\":");
    return pos == std::string::npos ? 0 : std::strtod(json.c_str() + pos + std::strlen(name) + 3, nullptr);
}

void publish_state(std::string parsed, uint64_t us) {
    if (parsed.find("\"status\":\"ok\"") == std::string::npos || parsed.size() > 1024 * 1024 - 16) return;
    const std::string marker = "\"navigation\":null";
    auto navigation = parsed.find(marker);
    if (navigation != std::string::npos) {
        if (g_navigation.empty() || GetTickCount64() - g_navigation_at > 750) return;
        if (!g_items_valid || !g_items_at || GetTickCount64() - g_items_at > 2500) return;
        auto expanded = g_navigation;
        const std::string catalogMarker = "\"star_system\":null", itemsMarker = "\"items\":null";
        auto catalog = expanded.find(catalogMarker), items = expanded.find(itemsMarker);
        if (catalog == std::string::npos || items == std::string::npos) return;
        expanded.replace(catalog, catalogMarker.size(), "\"star_system\":" + g_catalog);
        items = expanded.find(itemsMarker);
        expanded.replace(items, itemsMarker.size(), "\"items\":" + g_items);
        parsed.replace(navigation, marker.size(), "\"navigation\":" + expanded);
    }
    if (parsed.size() > 1024 * 1024 - 16) return;
    g_captures++; g_capture_total_us += us;
    if (us > g_capture_max_us.load()) g_capture_max_us = us;
    g_frames = static_cast<uint64_t>(json_number(parsed, "frames"));
    g_fps_milli = static_cast<uint64_t>(json_number(parsed, "fps") * 1000);
    g_metrics_at = GetTickCount64();
    g_bridge_ready = true;
    g_game_ready = parsed.find("\"map_id\":null") == std::string::npos && parsed.find("\"playerPosition\":null") == std::string::npos;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_snapshot = std::move(parsed); g_snapshot_at = GetTickCount64();
}

void update_native_root() {
    if (!g_native) return;
    strncpy_s(g_native_root, g_native->root(), _TRUNCATE);
    g_native_root_valid = true;
}

// Flash's outbound ExternalInterface calls arrive on the control's UI thread.
// Capture is pushed by ENTER_FRAME, avoiding WM_TIMER starvation and redundant
// CallFunction/VM entry on every controller read.
class FlashEvents final : public IDispatch {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == Flash::DIID__IShockwaveFlashEvents) {
            *out = static_cast<IDispatch *>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *n) override { if (n) *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *params, VARIANT *, EXCEPINFO *, UINT *) override {
        if (id != 197 || !params || params->cArgs != 1 || params->rgvarg[0].vt != VT_BSTR) return S_OK;
        BSTR raw = params->rgvarg[0].bstrVal;
        auto xml = wide_to_utf8(raw, static_cast<int>(SysStringLen(raw)));
        std::string answer = "<undefined/>";
        // Frame-call completion pushed by the bridge (replaces resultAS polling).
        if (xml.find("name=\"xenomitResult\"") != std::string::npos) {
            auto args = as_bridge::parse_arguments(xml);
            if (args.size() == 2) {
                complete_frame_call(std::strtoull(args[0].c_str(), nullptr, 10), std::move(args[1]));
                answer = "<string>accepted</string>";
            }
            return reply(answer);
        }
        if (g_native_mode && xml.find("name=\"xenomitNativeFrame\"") != std::string::npos) {
            if (g_native_navigation_dirty) {
                g_native_navigation = g_navigation;
                const std::string catalog = "\"star_system\":null", items = "\"items\":null";
                auto p = g_native_navigation.find(catalog);
                if (p != std::string::npos) g_native_navigation.replace(p, catalog.size(), "\"star_system\":" + g_catalog);
                p = g_native_navigation.find(items);
                if (p != std::string::npos) g_native_navigation.replace(p, items.size(), "\"items\":" + g_items);
                g_native_navigation_dirty = false;
            }
            if (g_native && g_items_valid && g_items_at && GetTickCount64() - g_items_at <= 2500 && g_native->capture(g_native_navigation, g_navigation_at)) {
                auto us = g_native->capture_us(); g_captures++; g_capture_total_us += us;
                if (us > g_capture_max_us.load()) g_capture_max_us = us;
                g_bridge_ready = true; g_game_ready = true;
                std::lock_guard<std::mutex> lock(g_cache_mutex); g_snapshot_at = GetTickCount64();
            } else if (g_native) {
                static std::string last;
                if (last != g_native->error()) { last = g_native->error(); log(1, "Native capture: %s", last.c_str()); }
            }
            update_native_root();
        }
        if (xml.find("name=\"xenomitCatalog\"") != std::string::npos || xml.find("name=\"xenomitItems\"") != std::string::npos || xml.find("name=\"xenomitNavigation\"") != std::string::npos) g_native_navigation_dirty = true;
        if (xml.find("name=\"xenomitCatalog\"") != std::string::npos) {
            auto catalog = as_bridge::parse_response(xml);
            if (catalog.size() < 256 * 1024) g_catalog = std::move(catalog);
        }
        if (xml.find("name=\"xenomitItems\"") != std::string::npos) {
            auto items = as_bridge::parse_response(xml);
            if (items.size() < 256 * 1024) { g_items = std::move(items); g_items_at = GetTickCount64(); g_items_valid = true; g_item_updates++; } else g_items_valid = false;
        }
        if (xml.find("name=\"xenomitItemsFresh\"") != std::string::npos && g_items_valid) { g_items_at = GetTickCount64(); g_item_refreshes++; }
        if (xml.find("name=\"xenomitNavigation\"") != std::string::npos) {
            auto nav = as_bridge::parse_response(xml);
            if (nav.size() < 512 * 1024 && nav.find("\"map_id\":") != std::string::npos) {
                g_navigation = std::move(nav); g_navigation_at = GetTickCount64();
            }
        }
        if (xml.find("name=\"xenomitCapture\"") != std::string::npos) {
            auto parsed = as_bridge::parse_response(xml);
            auto cost = static_cast<uint64_t>(json_number(parsed, "captureMs") * 1000);
            publish_state(std::move(parsed), cost);
        }
        if (xml.find("name=\"xenomitMetrics\"") != std::string::npos) {
            auto data = as_bridge::parse_response(xml);
            g_frames = static_cast<uint64_t>(json_number(data, "frames"));
            g_fps_milli = static_cast<uint64_t>(json_number(data, "fps") * 1000);
            g_metrics_at = GetTickCount64();
        }
        // The game reads location through eval/toString. Supply a real page URL
        // and return undefined for unhandled browser-only calls.
        if (xml.find("name=\"xenomitBackend\"") != std::string::npos) answer = g_native_mode ? "<string>native</string>" : "<string>cached</string>";
        if (xml.find("name=\"xenomitAnchor\"") != std::string::npos && g_native) answer = "<string>" + g_native->anchor_nonce() + "</string>";
        if (xml.find("name=\"xenomitSettings\"") != std::string::npos)
            answer = "<string>" + std::to_string(g_max_fps.load()) + "," + std::to_string(g_volume.load()) + "</string>";
        if (xml.find("location.href") != std::string::npos)
            answer = "<string>" + g_page_url + "</string>";
        return reply(answer);
    }
private:
    static HRESULT reply(const std::string &answer) {
        auto wide = utf8_to_wide(answer);
        BSTR value = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
        if (value) { g_flash->SetReturnValue(value); SysFreeString(value); }
        return S_OK;
    }
};
IConnectionPoint *g_events = nullptr;
DWORD g_event_cookie = 0;
HRESULT subscribe_events() {
    IConnectionPointContainer *container = nullptr;
    HRESULT hr = g_object->QueryInterface(IID_IConnectionPointContainer, reinterpret_cast<void **>(&container));
    if (FAILED(hr)) return hr;
    hr = container->FindConnectionPoint(Flash::DIID__IShockwaveFlashEvents, &g_events); container->Release();
    if (FAILED(hr)) return hr;
    auto sink = new FlashEvents(); hr = g_events->Advise(sink, &g_event_cookie); sink->Release(); return hr;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            if (wparam == BootstrapTimer) {
                if (g_game_ready.load()) { finish_hidden_bootstrap(); return 0; }
                if (g_hidden_draw_bootstrap && g_object && g_bootstrap_dc && !IsWindowVisible(window) && !g_game_ready.load()) {
                    RECT bounds{}; GetClientRect(window, &bounds);
                    auto result = OleDraw(g_object, DVASPECT_CONTENT, g_bootstrap_dc, &bounds);
                    ++g_bootstrap_draws; if (FAILED(result)) ++g_bootstrap_failures;
                }
                return 0;
            }
            break;
        case WM_SIZE:
            if (g_host_window) MoveWindow(g_host_window, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
            else if (g_in_place) { RECT rect{}; GetClientRect(window, &rect); g_in_place->SetObjectRects(&rect, &rect); }
            return 0;
        case WM_GETMINMAXINFO: {
            int w = g_min_client_width.load(), h = g_min_client_height.load();
            if (w > 0 && h > 0) {
                RECT rect{0, 0, w, h};
                AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                    static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
                auto info = reinterpret_cast<MINMAXINFO *>(lparam);
                info->ptMinTrackSize.x = rect.right - rect.left;
                info->ptMinTrackSize.y = rect.bottom - rect.top;
                return 0;
            }
            break;
        }
        case WM_XF_STOP:
            g_stopping = true;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_stopping = true;
            queue_detach("player shutting down");
            PostQuitMessage(0);
            return 0;
        case WM_XF_CALL:
            queue_drain();
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HRESULT create_flash(const std::wstring &ocx_path) {
    g_ocx = LoadLibraryExW(ocx_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_ocx) return HRESULT_FROM_WIN32(GetLastError());
    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(g_ocx, "DllGetClassObject"));
    if (!get_class) return HRESULT_FROM_WIN32(GetLastError());
    IClassFactory *factory = nullptr;
    HRESULT hr = get_class(__uuidof(Flash::ShockwaveFlash), IID_IClassFactory, reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->CreateInstance(nullptr, IID_IOleObject, reinterpret_cast<void **>(&g_object));
    factory->Release();
    if (FAILED(hr)) return hr;
    return g_object->QueryInterface(__uuidof(Flash::IShockwaveFlash), reinterpret_cast<void **>(&g_flash));
}

HRESULT attach_with_atl(HWND parent) {
    // InProcess uses an ATL ActiveX host. The system ATL container supplies the
    // complete IOleControlSite/IServiceProvider ambient environment that Flash
    // expects, while the control itself still comes from our registration-free
    // DllGetClassObject activation above.
    g_atl = LoadLibraryW(L"atl.dll");
    if (!g_atl) return HRESULT_FROM_WIN32(GetLastError());
    auto initialize = reinterpret_cast<AtlAxWinInitFn>(GetProcAddress(g_atl, "AtlAxWinInit"));
    auto attach = reinterpret_cast<AtlAxAttachControlFn>(GetProcAddress(g_atl, "AtlAxAttachControl"));
    if (!initialize || !attach) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (!initialize()) return E_FAIL;
    RECT rect{};
    GetClientRect(parent, &rect);
    g_host_window = CreateWindowExW(0, L"AtlAxWin", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rect.right, rect.bottom, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_host_window) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = attach(static_cast<IUnknown *>(g_object), g_host_window, &g_atl_container);
    if (FAILED(hr)) return hr;
    g_object->QueryInterface(IID_IOleInPlaceObject, reinterpret_cast<void **>(&g_in_place));
    return subscribe_events();
}

void cleanup() {
    finish_hidden_bootstrap();
    if (g_events) { if (g_event_cookie) g_events->Unadvise(g_event_cookie); g_events->Release(); g_events = nullptr; }
    if (g_object) {
        g_object->Close(OLECLOSE_NOSAVE);
        g_object->SetClientSite(nullptr);
    }
    if (g_in_place) { g_in_place->Release(); g_in_place = nullptr; }
    if (g_atl_container) { g_atl_container->Release(); g_atl_container = nullptr; }
    if (g_flash) { g_flash->Release(); g_flash = nullptr; }
    if (g_object) { g_object->Release(); g_object = nullptr; }
    // Flash.ocx and atl.dll stay loaded: Flash is not safe to unload in a live
    // process (InProcessPlayer never unloads it either).
    if (!g_bridge_temp.empty()) { DeleteFileW(g_bridge_temp.c_str()); g_bridge_temp.clear(); }
}

struct StartState {
    HANDLE ready = nullptr;
    XfCode code = XF_START_FAILED;
    std::string error;
};

// Mirrors the former wmain startup; codes match the old exe exit codes.
int initialize_ui(StartState &start) {
    auto fail = [&](int code, std::string message) {
        start.error = std::move(message);
        log(2, "%s", start.error.c_str());
        return code;
    };
    configure_user_agent(g_settings.user_agent);
    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) { log_hresult("OleInitialize", hr); return fail(2, "OleInitialize failed"); }
    hooks_install({g_settings.local_proxy_port, g_settings.game_ports});
    if (!g_settings.clear_cache.empty())
        log(0, "Cleared %u URL cache entries", clear_url_cache(g_settings.clear_cache));
    hr = create_flash(g_settings.ocx);
    if (FAILED(hr)) {
        log_hresult("registration-free Flash activation", hr);
        cleanup();
        return fail(3, "registration-free Flash activation failed");
    }
    long version = 0;
    g_flash->FlashVersion(&version);
    log(0, "Flash ActiveX version raw=%ld", version);
    if (!g_settings.base_url.empty() && !g_settings.sid.empty())
        InternetSetCookieW(g_settings.base_url.c_str(), L"dosid", g_settings.sid.c_str());

    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t *class_name = L"Xenomit.FlashClient";
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(instance, class_name, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = window_proc;
        wc.lpszClassName = class_name;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassExW(&wc)) { cleanup(); return fail(4, "RegisterClassExW failed: " + std::to_string(GetLastError())); }
    }
    RECT requested{0, 0, g_settings.width, g_settings.height};
    AdjustWindowRectEx(&requested, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0);
    HWND window = CreateWindowExW(0, class_name, g_settings.title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, requested.right - requested.left, requested.bottom - requested.top, nullptr, nullptr, instance, nullptr);
    if (!window) { cleanup(); return fail(4, "CreateWindowExW failed: " + std::to_string(GetLastError())); }
    g_window = window;
    queue_attach(window, WM_XF_CALL);

    put_bstr(&Flash::IShockwaveFlash::put_AllowScriptAccess, L"always");
    put_bstr(&Flash::IShockwaveFlash::put_AllowNetworking, L"all");
    put_bstr(&Flash::IShockwaveFlash::put_WMode, g_settings.wmode);
    log(0, "Flash window mode: %ls", g_settings.wmode.c_str());
    put_bstr(&Flash::IShockwaveFlash::put_Quality2, g_settings.quality);
    log(0, "Flash quality: %ls (InProcess/DarkBot defaults to low)", g_settings.quality.c_str());
    if (!g_settings.base_url.empty()) {
        put_bstr(&Flash::IShockwaveFlash::put_Base, g_settings.base_url);
        g_page_url = wide_to_utf8(g_settings.base_url) + "indexInternal.es?action=internalMapRevolution";
    }
    if (!g_settings.flash_vars.empty()) put_bstr(&Flash::IShockwaveFlash::put_FlashVars, g_settings.flash_vars);
    g_flash->put_Menu(VARIANT_FALSE);

    hr = attach_with_atl(window);
    if (FAILED(hr)) {
        log_hresult("ATL ActiveX host attachment", hr);
        DestroyWindow(window); cleanup();
        return fail(5, "ATL ActiveX host attachment failed");
    }
    if (!load_bridged_movie(g_settings.movie)) {
        DestroyWindow(window); cleanup();
        return fail(6, "Required bridge injection failed; refusing an uncontrollable game session");
    }
    g_flash->put_Playing(VARIANT_TRUE);
    if (g_hidden_draw_bootstrap) {
        if (!arm_hidden_bootstrap()) { DestroyWindow(window); cleanup(); return fail(7, "Could not prepare the hidden draw bootstrap"); }
        log(0, "Hidden bootstrap: offscreen OleDraw until game readiness");
    }
    log(0, "Reflection bridge: %s", g_bridge_injected.load() ? "injected" : "not injected");
    if (!g_settings.hidden) ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return 0;
}

DWORD WINAPI ui_thread(void *parameter) {
    auto &start = *static_cast<StartState *>(parameter);
    set_ui_thread(GetCurrentThreadId());
    int code = initialize_ui(start);
    if (code) {
        g_exit_code = code;
        g_stopping = true;
        start.code = XF_START_FAILED;
        OleUninitialize();
        SetEvent(start.ready);
        g_running = false;
        return static_cast<DWORD>(code);
    }
    timeBeginPeriod(1);
    g_running = true;
    start.code = XF_OK;
    SetEvent(start.ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // setUserInput(false): real input reaches the thread through the
        // queue; injected input is sent and never passes this loop.
        if (!g_user_input.load() && input_filter(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_stopping = true;
    queue_detach("player shutting down");
    timeEndPeriod(1);
    cleanup();
    g_window = nullptr;
    OleUninitialize();
    g_exit_code = static_cast<int>(message.wParam);
    g_running = false;
    return 0;
}

void fill_defaults(Settings &settings) {
    if (settings.width <= 100 || settings.height <= 100) { settings.width = 1280; settings.height = 800; }
    // InProcessPlayer clamps the FPS cap to 100 instead of rejecting it.
    settings.max_fps = std::min(std::max(settings.max_fps, 0), 100);
    if (settings.user_agent.empty()) settings.user_agent = L"BigpointClient/1.6.9";
    if (settings.title.empty()) settings.title = L"Xenomit Flash Client";
    if (settings.quality.empty()) settings.quality = L"low";
    if (settings.wmode.empty()) settings.wmode = L"window";
    if (settings.backend.empty()) settings.backend = L"cached";
    if (settings.discovery.empty()) settings.discovery = L"auto";
    if (settings.game_ports.empty()) settings.game_ports.push_back(5001);
    if (settings.ocx.empty()) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&fill_defaults), &self);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring module(path);
        settings.ocx = module.substr(0, module.find_last_of(L"\\/")) + L"\\Flash.ocx";
    }
    wchar_t resolved[32768]{};
    if (GetFullPathNameW(settings.ocx.c_str(), 32768, resolved, nullptr)) settings.ocx = resolved;
}

std::string validate(const Settings &settings) {
    if (settings.movie.empty()) return "movie is required";
    if (settings.backend != L"native" && settings.backend != L"cached") return "Unknown capture backend";
    const auto &d = settings.discovery;
    if (d != L"auto" && d != L"anchor" && d != L"pattern" && d != L"parity") return "Native discovery must be auto, anchor, pattern or parity";
    const auto &q = settings.quality;
    if (q != L"low" && q != L"medium" && q != L"high" && q != L"best") return "Invalid quality: use low, medium, high, or best";
    const auto &w = settings.wmode;
    if (w != L"direct" && w != L"window" && w != L"opaque") return "Invalid wmode: use direct, window, or opaque";
    if (settings.local_proxy_port < 0 || settings.local_proxy_port > 65535) return "local_proxy_port must be 0..65535";
    return {};
}

} // namespace

void log(int level, const char *format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_log) { g_log(g_log_context, level, text); return; }
    // The DLL has its own CRT streams: flush so a killed host keeps its log.
    auto stream = level ? stderr : stdout;
    std::fprintf(stream, "%s\n", text);
    std::fflush(stream);
}

void set_log_sink(XfLogFn fn, void *context) {
    g_log = fn;
    g_log_context = context;
}

std::wstring utf8_to_wide(const std::string &value) {
    if (value.empty()) return {};
    int chars = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), chars);
    return wide;
}

std::string wide_to_utf8(const wchar_t *value, int chars) {
    if (!value || chars <= 0) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value, chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, chars, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string json_string(const std::string &value) {
    std::string out = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) { char escape[8]; std::snprintf(escape, sizeof(escape), "\\u%04x", ch); out += escape; }
                else out.push_back(static_cast<char>(ch));
        }
    }
    return out + "\"";
}

bool flash_call(const std::string &name, const std::vector<std::string> &args, std::string &parsed, std::string &error) {
    if (!g_flash) { error = "Flash is not loaded"; return false; }
    std::wstring xml = utf8_to_wide(as_bridge::request_xml(name, args));
    BSTR request = SysAllocStringLen(xml.data(), static_cast<UINT>(xml.size()));
    if (!request) { error = "out of memory"; return false; }
    BSTR response = nullptr;
    HRESULT hr = g_flash->CallFunction(request, &response);
    SysFreeString(request);
    if (FAILED(hr) || !response) {
        if (response) SysFreeString(response);
        char message[80]{};
        std::snprintf(message, sizeof(message), "CallFunction failed: 0x%08lx", static_cast<unsigned long>(hr));
        error = message;
        return false;
    }
    parsed = as_bridge::parse_response(wide_to_utf8(response, static_cast<int>(SysStringLen(response))));
    SysFreeString(response);
    return true;
}

XfCode host_start(const Settings &requested, std::string &error) {
    std::lock_guard<std::mutex> lock(g_start_mutex);
    if (g_thread && WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) {
        // The previous host ended on its own (window closed): allow a restart.
        if (g_pipe_thread) { WaitForSingleObject(g_pipe_thread, 11000); CloseHandle(g_pipe_thread); g_pipe_thread = nullptr; }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_thread) { error = "the Flash host is already running in this process"; return XF_ALREADY_RUNNING; }
    Settings settings = requested;
    fill_defaults(settings);
    error = validate(settings);
    if (!error.empty()) { g_exit_code = 2; return XF_INVALID; }
    g_settings = settings;
    // Fresh session state for a (re)start.
    g_bridge_injected = false; g_bridge_ready = false; g_game_ready = false;
    g_navigation.clear(); g_navigation_at = 0; g_items = "{}"; g_items_at = 0; g_items_valid = true;
    g_catalog = "{\"maps\":[],\"links\":[]}";
    g_native_navigation.clear(); g_native_navigation_dirty = true;
    { std::lock_guard<std::mutex> cache(g_cache_mutex); g_snapshot.clear(); g_snapshot_at = 0; }
    g_captures = 0; g_capture_total_us = 0; g_capture_max_us = 0; g_metrics_at = 0;
    g_exit_code = 0; g_user_input = true; g_game_client_hash.clear();
    g_native_mode = settings.backend == L"native";
    g_native_runtime_supported = pinned_native_runtime(settings.ocx);
    const auto &d = settings.discovery;
    g_native_discovery = d == L"anchor" ? NativeDiscovery::Anchor : d == L"pattern" ? NativeDiscovery::Pattern :
        d == L"parity" ? NativeDiscovery::Parity : NativeDiscovery::Auto;
    auto vars = wide_to_utf8(settings.flash_vars);
    for (size_t at = 0; at <= vars.size();) {
        auto next = vars.find('&', at); if (next == std::string::npos) next = vars.size();
        auto pair = vars.substr(at, next - at);
        if (pair.rfind("gameclientHash=", 0) == 0) g_game_client_hash = pair.substr(15);
        at = next + 1;
    }
    g_hidden_draw_bootstrap = settings.hidden_draw;
    g_max_fps = settings.max_fps;
    if (g_native_mode) {
        error = native_unavailable();
        if (!error.empty()) { g_exit_code = 2; return XF_INVALID; }
        create_native_state();
    }
    // Explicit HighQoS and timer-resolution opt-out prevent hidden-window
    // power heuristics from defeating the 1 ms multimedia timer request.
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    power.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power))) {
        power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power));
    }
    // InProcessPlayer: the Flash window must never be ghosted or flagged as hung.
    DisableProcessWindowsGhosting();

    StartState start;
    start.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!start.ready) { error = "could not create start event"; return XF_START_FAILED; }
    g_stopping = false;
    g_thread = CreateThread(nullptr, 0, ui_thread, &start, 0, nullptr);
    if (!g_thread) { CloseHandle(start.ready); error = "could not start the Flash thread"; return XF_START_FAILED; }
    SetThreadDescription(g_thread, L"xenomit-flash-ui");
    WaitForSingleObject(start.ready, INFINITE);
    CloseHandle(start.ready);
    if (start.code != XF_OK) { error = start.error; return start.code; }
    if (g_settings.pipe_server) g_pipe_thread = pipe_server_start();
    log(0, "ActiveX host: system ATL container");
    return XF_OK;
}

XfCode host_stop(DWORD timeout_ms) {
    std::lock_guard<std::mutex> lock(g_start_mutex);
    if (!g_thread) return XF_NOT_READY;
    if (g_running.load() && g_window) PostMessageW(g_window, WM_XF_STOP, 0, 0);
    if (WaitForSingleObject(g_thread, timeout_ms) != WAIT_OBJECT_0) return XF_TIMEOUT_UNKNOWN;
    if (g_pipe_thread) { WaitForSingleObject(g_pipe_thread, 11000); CloseHandle(g_pipe_thread); g_pipe_thread = nullptr; }
    CloseHandle(g_thread);
    g_thread = nullptr;
    return XF_OK;
}

XfCode host_wait(DWORD timeout_ms, int &exit_code) {
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_start_mutex);
        thread = g_thread;
    }
    if (thread && WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) return XF_PENDING;
    if (g_pipe_thread) { WaitForSingleObject(g_pipe_thread, 11000); }
    exit_code = g_exit_code.load();
    return XF_OK;
}

bool host_running() { return g_running.load() && !g_stopping.load(); }
bool host_stopping() { return g_stopping.load(); }

XfCode host_reload(std::string &error) {
    if (!host_running()) { error = "player is not running"; return XF_NOT_READY; }
    // A reload replaces the movie: pending frame calls can no longer complete.
    auto out = run_ui([] {
        g_bridge_ready = false; g_game_ready = false; g_bridge_injected = false;
        g_navigation.clear(); g_navigation_at = 0; g_items_at = 0; g_items_valid = true;
        g_native_navigation.clear(); g_native_navigation_dirty = true;
        g_catalog = "{\"maps\":[],\"links\":[]}";
        if (g_native) g_native->invalidate();
        { std::lock_guard<std::mutex> lock(g_cache_mutex); g_snapshot.clear(); g_snapshot_at = 0; }
        g_captures = 0; g_capture_total_us = 0; g_capture_max_us = 0;
        if (!load_bridged_movie(g_settings.movie)) return Outcome{XF_FLASH_ERROR, "movie reload failed"};
        g_flash->put_Playing(VARIANT_TRUE);
        if (g_hidden_draw_bootstrap && !IsWindowVisible(g_window)) { finish_hidden_bootstrap(); arm_hidden_bootstrap(); }
        return Outcome{XF_OK, "reloaded"};
    }, 60000);
    if (out.code != XF_OK) error = out.text;
    return out.code;
}

HWND host_window() { return g_window; }

HWND flash_window() {
    HWND control = nullptr;
    if (g_in_place) g_in_place->GetWindow(&control);
    if (control) return control;
    return g_host_window ? g_host_window : g_window;
}

std::string host_set_backend(const std::string &backend) {
    const bool native = backend == "native";
    if (!native && backend != "cached") return "Unknown capture backend";
    auto unavailable = native ? native_unavailable() : std::string();
    if (!unavailable.empty()) return unavailable;
    if (native && !g_native) create_native_state();
    std::string parsed, error;
    if (!flash_call("configureBackendAS", {backend}, parsed, error)) return error;
    if (parsed != backend) return "Flash did not confirm the requested backend";
    g_native_mode = native;
    if (g_native) g_native->invalidate();
    g_native_navigation.clear(); g_native_navigation_dirty = true;
    g_navigation.clear(); g_navigation_at = 0;
    g_game_ready = false;
    g_captures = 0; g_capture_total_us = 0; g_capture_max_us = 0;
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    g_snapshot.clear(); g_snapshot_at = 0;
    return {};
}

std::string host_set_quality(const std::string &quality) {
    if (quality != "low" && quality != "medium" && quality != "high" && quality != "best") return "Invalid quality: use low, medium, high, or best";
    if (!g_flash) return "Flash is not loaded";
    return SUCCEEDED(put_bstr(&Flash::IShockwaveFlash::put_Quality2, utf8_to_wide(quality))) ? std::string() : "could not set Flash quality";
}

std::string host_set_frame_settings(int fps, int volume) {
    if (fps >= 0) g_max_fps = fps;
    if (volume >= 0) g_volume = volume;
    std::string parsed, error;
    // The bridge also asks for these on its first frame, so a missing bridge
    // keeps the value for later instead of failing.
    if (!flash_call("settingsAS", {std::to_string(g_max_fps.load()), std::to_string(g_volume.load())}, parsed, error)) return {};
    return parsed == "configured" ? std::string() : parsed;
}

void host_set_user_input(bool enabled) { g_user_input = enabled; }
void host_set_min_client_size(int width, int height) { g_min_client_width = width; g_min_client_height = height; }

const uint8_t *host_live_frame(size_t &len) {
    len = g_frame_size.load();
    return g_frame_base.load();
}

bool host_state(std::string &json) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_snapshot.empty() || GetTickCount64() - g_snapshot_at > 1000) return false;
    json = g_snapshot;
    return true;
}

void host_status(XfStatus &status) {
    auto size = status.size;
    status = XfStatus{};
    status.size = size;
    ULONGLONG now = GetTickCount64(), age = 999999;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_snapshot_at) age = now - g_snapshot_at;
    }
    status.player = host_running() ? 1 : 0;
    status.bridge = g_bridge_injected.load() ? 1 : 0;
    status.ready = g_game_ready.load() && age <= 1000 ? 1 : 0;
    status.native = g_native_mode.load() ? 1 : 0;
    status.frames = g_frames.load();
    status.fps_milli = g_metrics_at.load() && now - g_metrics_at.load() <= 1500 ? g_fps_milli.load() : 0;
    status.captures = g_captures.load();
    status.capture_us_avg = status.captures ? g_capture_total_us.load() / status.captures : 0;
    status.capture_us_max = g_capture_max_us.load();
    status.state_age_ms = age;
    status.item_updates = g_item_updates.load();
    status.item_refreshes = g_item_refreshes.load();
    status.bootstrap_draws = g_bootstrap_draws.load();
    status.bootstrap_failures = g_bootstrap_failures.load();
    // Root and parity counters are written on the STA; a torn read only skews a diagnostic.
    if (g_native) {
        status.parity_checks = g_native->parity_checks();
        status.parity_mismatches = g_native->parity_mismatches();
        strncpy_s(status.native_root, g_native_root_valid.load() ? g_native_root : "none", _TRUNCATE);
    }
    hooks_status(status);
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) status.working_set_bytes = memory.WorkingSetSize;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto busy = (ULARGE_INTEGER{{kernel.dwLowDateTime, kernel.dwHighDateTime}}.QuadPart +
                     ULARGE_INTEGER{{user.dwLowDateTime, user.dwHighDateTime}}.QuadPart) / 10000;
        std::lock_guard<std::mutex> lock(g_cpu_mutex);
        if (g_cpu_wall && now - g_cpu_wall >= 250) {
            SYSTEM_INFO info{}; GetSystemInfo(&info);
            g_cpu_percent = 100.0 * static_cast<double>(busy - g_cpu_busy) /
                (static_cast<double>(now - g_cpu_wall) * std::max<DWORD>(info.dwNumberOfProcessors, 1));
        }
        if (!g_cpu_wall || now - g_cpu_wall >= 250) { g_cpu_wall = now; g_cpu_busy = busy; }
        status.cpu_percent = g_cpu_percent;
    }
}

std::string host_status_line() {
    XfStatus s{};
    s.size = sizeof(s);
    host_status(s);
    auto age = [](uint64_t value) { return value == UINT64_MAX ? std::string("-1") : std::to_string(value); };
    std::string line = "OK player=1 bridge=" + std::to_string(s.bridge) + " ready=" + std::to_string(s.ready) +
        " native=" + std::to_string(s.native);
    if (g_native) line += " native_root=" + std::string(s.native_root) + " parity_checks=" + std::to_string(s.parity_checks) +
        " parity_mismatches=" + std::to_string(s.parity_mismatches);
    char cpu[32];
    std::snprintf(cpu, sizeof(cpu), "%.1f", s.cpu_percent);
    line += " bootstrap_draws=" + std::to_string(s.bootstrap_draws) + " bootstrap_failures=" + std::to_string(s.bootstrap_failures) +
        " frames=" + std::to_string(s.frames) + " fps_milli=" + std::to_string(s.fps_milli) +
        " captures=" + std::to_string(s.captures) + " capture_us_avg=" + std::to_string(s.capture_us_avg) +
        " capture_us_max=" + std::to_string(s.capture_us_max) + " item_updates=" + std::to_string(s.item_updates) +
        " item_refreshes=" + std::to_string(s.item_refreshes) + " state_age_ms=" + std::to_string(s.state_age_ms) +
        " net_reads=" + std::to_string(s.net_reads) + " net_read_age_ms=" + age(s.net_read_age_ms) +
        " game_socket=" + std::to_string(s.game_socket) + " game_recv=" + std::to_string(s.game_recv) +
        " game_recv_bytes=" + std::to_string(s.game_recv_bytes) + " game_recv_age_ms=" + age(s.game_recv_age_ms) +
        " vfs_hits=" + std::to_string(s.vfs_hits) + " blocked_requests=" + std::to_string(s.blocked_requests) +
        " cpu_pct=" + cpu + " mem_ws_bytes=" + std::to_string(s.working_set_bytes) + "\n";
    return line;
}

} // namespace xf
