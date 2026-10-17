#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <urlmon.h>
#include <ole2.h>
#include <olectl.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <deque>
#include <chrono>
#include <thread>
#include <mmsystem.h>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "as_bridge.h"

#include "swf_bridge.h"
#include "native_state.h"
#include "game_offsets.h"

#import "Flash.ocx" raw_interfaces_only, raw_native_types, named_guids, no_smart_pointers, rename_namespace("Flash")

using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);
using AtlAxWinInitFn = BOOL (WINAPI *)();
using AtlAxAttachControlFn = HRESULT (WINAPI *)(IUnknown *, HWND, IUnknown **);

namespace {

HMODULE g_ocx = nullptr;
HMODULE g_atl = nullptr;
IOleObject *g_object = nullptr;
IOleInPlaceObject *g_in_place = nullptr;
Flash::IShockwaveFlash *g_flash = nullptr;
HWND g_window = nullptr;
HWND g_host_window = nullptr;
IUnknown *g_atl_container = nullptr;
constexpr UINT WM_DOBOT_CALL = WM_APP + 78;
std::atomic<bool> g_bridge_injected{false};
std::atomic<bool> g_bridge_ready{false};
std::atomic<bool> g_game_ready{false};
std::atomic<bool> g_stopping{false};
std::wstring g_user_agent = L"BigpointClient/1.6.9";
std::wstring g_bridge_temp;
std::string g_page_url;
std::mutex g_cache_mutex;
std::string g_snapshot;
std::atomic<bool> g_native_mode{false};
bool g_native_runtime_supported=false;
// FlashVar gameclientHash of the running client; native offsets are generated per build.
std::string g_game_client_hash;
NativeDiscovery g_native_discovery=NativeDiscovery::Auto;
bool g_hidden_draw_bootstrap=false;
HDC g_bootstrap_dc=nullptr;
HBITMAP g_bootstrap_bitmap=nullptr;
HGDIOBJ g_bootstrap_old_bitmap=nullptr;
std::atomic<uint64_t> g_bootstrap_draws{0},g_bootstrap_failures{0};
constexpr UINT_PTR BootstrapTimer=0xDB07;
std::unique_ptr<NativeState> g_native;
std::string g_native_navigation;
bool g_native_navigation_dirty=true;
// Accessed only by Flash callbacks on the UI thread.
std::string g_navigation;
ULONGLONG g_navigation_at=0;
std::string g_catalog="{\"maps\":[],\"links\":[]}";
std::string g_items="{}";
ULONGLONG g_items_at=0;
bool g_items_valid=true;
std::atomic<uint64_t> g_item_updates{0},g_item_refreshes{0};
ULONGLONG g_snapshot_at = 0;
std::atomic<uint64_t> g_captures{0}, g_capture_total_us{0}, g_capture_max_us{0};
std::atomic<uint64_t> g_frames{0}, g_fps_milli{0}, g_frame_id{0}, g_metrics_at{0};
struct BridgeCall {
    std::string name;
    std::vector<std::string> args;
    std::string parsed, error;
    bool ok = false;
    std::atomic<bool> cancelled{false};
    ULONGLONG deadline = 0;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~BridgeCall() { if (done) CloseHandle(done); }
};
std::mutex g_calls_mutex;
std::deque<std::shared_ptr<BridgeCall>> g_calls;

void finish_hidden_bootstrap() {
    if(g_window)KillTimer(g_window,BootstrapTimer);
    if(g_bootstrap_dc) {
        if(g_bootstrap_old_bitmap)SelectObject(g_bootstrap_dc,g_bootstrap_old_bitmap);
        if(g_bootstrap_bitmap)DeleteObject(g_bootstrap_bitmap);
        DeleteDC(g_bootstrap_dc);
    }
    g_bootstrap_dc=nullptr;g_bootstrap_bitmap=nullptr;g_bootstrap_old_bitmap=nullptr;
}

void print_hresult(const char *operation, HRESULT hr) {
    std::fprintf(stderr, "%s failed: HRESULT 0x%08lx\n", operation, static_cast<unsigned long>(hr));
}

bool pinned_native_runtime(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;bool ok=false;
    if(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) {
        std::vector<BYTE> data(65536);DWORD n=0;ok=true;
        for(;;) {if(!ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)){ok=false;break;}if(!n)break;if(!CryptHashData(hash,data.data(),n,0)){ok=false;break;}}
        BYTE digest[32]{};DWORD size=32;
        const BYTE expected[]={0xB0,0x47,0x55,0x12,0xD5,0xAD,0x44,0x9D,0xFA,0x7D,0x21,0xB5,0xA2,0x64,0x5C,0xB1,0x71,0x1D,0x2C,0xE9,0x8B,0x42,0xC9,0xFE,0x00,0x0B,0x5F,0xBC,0x0F,0x82,0x31,0xBD};
        // Same exact signed 34.0.0.267 image before the five compatibility
        // edits. VM layout is unchanged; game class/method guards still apply.
        const BYTE stock[]={0x9B,0x39,0xAC,0xE0,0x1E,0x55,0x7A,0x4A,0x4D,0x46,0x9B,0xBB,0x4E,0xC7,0xAD,0xF2,0x7D,0x18,0xB9,0xCF,0x3B,0x1C,0xB9,0x6A,0xB2,0x01,0xAC,0xDE,0xFA,0xBA,0xE9,0xF0};
        ok=ok&&CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)&&size==32&&(!memcmp(digest,expected,32)||!memcmp(digest,stock,32));
    }
    if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);CloseHandle(file);return ok;
}

HRESULT put_bstr(HRESULT (__stdcall Flash::IShockwaveFlash::*setter)(BSTR), const std::wstring &value) {
    BSTR text = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    if (!text) return E_OUTOFMEMORY;
    HRESULT hr = (g_flash->*setter)(text);
    SysFreeString(text);
    return hr;
}

void configure_user_agent(const std::wstring &wide_value) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string value(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide_value.c_str(), -1, value.data(), bytes, nullptr, nullptr);
    value.resize(static_cast<size_t>(bytes - 1));

    HRESULT hr = UrlMkSetSessionOption(URLMON_OPTION_USERAGENT,
        const_cast<char *>(value.c_str()), static_cast<DWORD>(value.size()), 0);
    if (FAILED(hr)) print_hresult("UrlMkSetSessionOption(USERAGENT)", hr);
    std::printf("User agent: %s\n", value.c_str());
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

void bridge_log(const std::string &text) {
    std::fprintf(stderr,"[flash-bridge] %s",text.c_str());
}

std::vector<uint8_t> download_movie(const std::wstring &url, std::string &error) {
    bool is_remote = url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
    if (!is_remote) {
        HANDLE file = CreateFileW(url.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "could not open movie file";
            return {};
        }
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
        if (!ok || read != data.size()) {
            error = "could not read movie file";
            return {};
        }
        return data;
    }

    HINTERNET inet = InternetOpenW(g_user_agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) {
        error = "InternetOpen failed";
        return {};
    }
    DWORD timeout=20000;
    InternetSetOptionW(inet,INTERNET_OPTION_CONNECT_TIMEOUT,&timeout,sizeof(timeout));
    InternetSetOptionW(inet,INTERNET_OPTION_RECEIVE_TIMEOUT,&timeout,sizeof(timeout));
    DWORD decode = TRUE;
    InternetSetOptionW(inet, INTERNET_OPTION_HTTP_DECODING, &decode, sizeof(decode));
    HINTERNET remote = InternetOpenUrlW(inet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!remote) {
        InternetCloseHandle(inet);
        error = "InternetOpenUrl failed";
        return {};
    }
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
        if (!InternetReadFile(remote,buffer,sizeof(buffer),&got)) {error="movie download was interrupted";data.clear();break;}
        if (!got) break;
        if (data.size() + got > 64u * 1024u * 1024u) {
            error = "movie download is too large";
            data.clear();
            break;
        }
        data.insert(data.end(), buffer, buffer + got);
    }
    InternetCloseHandle(remote);
    InternetCloseHandle(inet);
    if (!error.empty()) return {};
    if (data.size() < 8) {
        error = "movie download was empty";
        return {};
    }
    return data;
}

bool flash_call(const std::string &name, const std::vector<std::string> &args,
                std::string &parsed, std::string &error) {
    if (!g_flash) {
        error = "Flash is not loaded";
        return false;
    }
    std::wstring xml = utf8_to_wide(as_bridge::request_xml(name, args));
    BSTR request = SysAllocStringLen(xml.data(), static_cast<UINT>(xml.size()));
    if (!request) {
        error = "out of memory";
        return false;
    }
    BSTR response = nullptr;
    HRESULT hr = g_flash->CallFunction(request, &response);
    SysFreeString(request);
    if (FAILED(hr) || !response) {
        if (response) SysFreeString(response);
        char message[80]{};
        std::snprintf(message, sizeof(message), "CallFunction failed: 0x%08lx",
                      static_cast<unsigned long>(hr));
        error = message;
        return false;
    }
    parsed = as_bridge::parse_response(wide_to_utf8(response, static_cast<int>(SysStringLen(response))));
    SysFreeString(response);
    return true;
}

bool load_bridged_movie(const std::wstring &movie) {
    std::string error;
    auto bytes = download_movie(movie, error);
    if (bytes.empty()) {
        bridge_log("injected=0\r\nload=url\r\nerror=" + error + "\r\n");
        return false;
    }
    auto patched = swf_bridge::inject(bytes.data(), bytes.size());
    if (!patched.ok) {
        bridge_log("injected=0\r\nload=url\r\ncompression=" + patched.compression +
                   "\r\nerror=" + patched.error + "\r\n");
        return false;
    }
    // CallFunction only binds to a movie that has a URL. put_MovieData loads the
    // bytes, but ExternalInterface stays unavailable, so the patched movie is
    // staged through a temporary file. The downloaded original is never written.
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) {
        bridge_log("injected=0\r\nload=url\r\nerror=temp path unavailable\r\n");
        return false;
    }
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%sdobot_bridge_%lu.swf", directory, GetCurrentProcessId());
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD written = 0;
    BOOL wrote = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, patched.swf.data(), static_cast<DWORD>(patched.swf.size()), &written, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!wrote || written != patched.swf.size()) {
        bridge_log("injected=0\r\nload=url\r\nerror=temp movie write failed\r\n");
        DeleteFileW(path);
        return false;
    }
    std::wstring staged = L"file:///" + std::wstring(path);
    for (auto &ch : staged) if (ch == L'\\') ch = L'/';
    HRESULT security = g_flash->DisableLocalSecurity();
    HRESULT hr = put_bstr(&Flash::IShockwaveFlash::put_Movie, staged);
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "injected=%d\r\ncompression=%s\r\noriginal_bytes=%zu\r\npatched_bytes=%zu\r\n"
        "security=0x%08lx\r\nmovie=0x%08lx\r\nload=%s\r\n",
        SUCCEEDED(hr) ? 1 : 0, patched.compression.c_str(), bytes.size(), patched.swf.size(),
        static_cast<unsigned long>(security), static_cast<unsigned long>(hr),
        SUCCEEDED(hr) ? "temp" : "url");
    bridge_log(line);
    if (FAILED(hr)) {
        DeleteFileW(path);
        return false;
    }
    g_bridge_temp = path;
    g_bridge_injected.store(true);
    return true;
}


// Empty when native capture may run; otherwise the reason it must not.
std::string native_unavailable() {
    namespace go=xenomit::game_offsets;
    if(!g_native_runtime_supported) return "Native offsets require a pinned 34.0.0.267 runtime";
    if(g_game_client_hash.empty()) return "Native offsets require the gameclientHash FlashVar of an authenticated launch";
    if(g_game_client_hash!=go::kGameClientHash)
        return "Native profile was generated for game build "+std::string(go::kGameClientHash)+" but "+g_game_client_hash+" is running; regenerate generated/game_offsets.h or select cached";
    return {};
}

std::string call_on_ui(const std::string &name, const std::vector<std::string> &args) {
    if (!g_window || g_stopping.load()) return "ERR player shutting down\n";
    auto call = std::make_shared<BridgeCall>();
    call->name=name; call->args=args; call->deadline=GetTickCount64()+5000;
    if (!call->done) return "ERR could not allocate call event\n";
    {
        std::lock_guard<std::mutex> guard(g_calls_mutex);
        if (g_calls.size() >= 32) return "ERR player call queue full\n";
        g_calls.push_back(call);
    }
    if (!PostMessageW(g_window, WM_DOBOT_CALL, 0, 0)) {
        call->cancelled=true; return "ERR player window unavailable\n";
    }
    if (WaitForSingleObject(call->done,5000) != WAIT_OBJECT_0) {
        call->cancelled=true;
        return "ERR Flash call timed out; execution may have started; do not retry automatically\n";
    }
    return call->ok ? "OK "+call->parsed+"\n" : "ERR "+call->error+"\n";
}

std::string frame_call(const std::vector<std::string> &args) {
    auto id=std::to_string(++g_frame_id);
    std::vector<std::string> request{id};
    request.insert(request.end(),args.begin(),args.end());
    auto reply=call_on_ui("queueAS",request);
    if (reply != "OK queued\n") return reply;
    auto deadline=GetTickCount64()+4500;
    while (!g_stopping.load() && GetTickCount64() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        reply=call_on_ui("resultAS",{id});
        if (reply != "OK pending\n") return reply;
    }
    return "ERR frame call timed out; execution unknown; do not retry automatically\n";
}

double json_number(const std::string &json,const char *name) {
    auto pos=json.find(std::string("\"")+name+"\":");
    return pos == std::string::npos ? 0 : std::strtod(json.c_str()+pos+std::strlen(name)+3,nullptr);
}

void publish_state(std::string parsed,uint64_t us) {
    if (parsed.find("\"status\":\"ok\"")==std::string::npos || parsed.size()>1024*1024-16) return;
    const std::string marker="\"navigation\":null";
    auto navigation=parsed.find(marker);
    if (navigation!=std::string::npos) {
        if (g_navigation.empty() || GetTickCount64()-g_navigation_at>750) return;
        if (!g_items_valid || !g_items_at || GetTickCount64()-g_items_at>2500) return;
        auto expanded=g_navigation;
        const std::string catalogMarker="\"star_system\":null",itemsMarker="\"items\":null";
        auto catalog=expanded.find(catalogMarker),items=expanded.find(itemsMarker);
        if (catalog==std::string::npos || items==std::string::npos) return;
        expanded.replace(catalog,catalogMarker.size(),"\"star_system\":"+g_catalog);
        items=expanded.find(itemsMarker);
        expanded.replace(items,itemsMarker.size(),"\"items\":"+g_items);
        parsed.replace(navigation,marker.size(),"\"navigation\":"+expanded);
    }
    if (parsed.size()>1024*1024-16) return;
    g_captures++; g_capture_total_us+=us;
    if (us>g_capture_max_us.load()) g_capture_max_us=us;
    g_frames=static_cast<uint64_t>(json_number(parsed,"frames"));
    g_fps_milli=static_cast<uint64_t>(json_number(parsed,"fps")*1000);
    g_metrics_at=GetTickCount64();
    g_bridge_ready=true;
    g_game_ready=parsed.find("\"map_id\":null")==std::string::npos && parsed.find("\"playerPosition\":null")==std::string::npos;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_snapshot=std::move(parsed);g_snapshot_at=GetTickCount64();
}
// Flash's outbound ExternalInterface calls arrive on the control's UI thread.
// Capture is pushed by ENTER_FRAME, avoiding WM_TIMER starvation and redundant
// CallFunction/VM entry on every controller read.
class FlashEvents final : public IDispatch {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if (!out) return E_POINTER;
        *out=nullptr;
        if (iid==IID_IUnknown || iid==IID_IDispatch || iid==Flash::DIID__IShockwaveFlashEvents) {
            *out=static_cast<IDispatch *>(this);AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {auto n=--refs_;if (!n) delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *n) override {if(n)*n=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo **) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR *,UINT,LCID,DISPID *) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id,REFIID,LCID,WORD,DISPPARAMS *params,VARIANT *,EXCEPINFO *,UINT *) override {
        if (id!=197 || !params || params->cArgs!=1 || params->rgvarg[0].vt!=VT_BSTR) return S_OK;
        BSTR raw=params->rgvarg[0].bstrVal;
        auto xml=wide_to_utf8(raw,static_cast<int>(SysStringLen(raw)));
        if (g_native_mode && xml.find("name=\"xenomitNativeFrame\"")!=std::string::npos) {
            if(g_native_navigation_dirty) {
                g_native_navigation=g_navigation;
                const std::string catalog="\"star_system\":null",items="\"items\":null";
                auto p=g_native_navigation.find(catalog);
                if(p!=std::string::npos)g_native_navigation.replace(p,catalog.size(),"\"star_system\":"+g_catalog);
                p=g_native_navigation.find(items);
                if(p!=std::string::npos)g_native_navigation.replace(p,items.size(),"\"items\":"+g_items);
                g_native_navigation_dirty=false;
            }
            if(g_native && g_items_valid && g_items_at && GetTickCount64()-g_items_at<=2500 && g_native->capture(g_native_navigation,g_navigation_at)) {
                auto us=g_native->capture_us();g_captures++;g_capture_total_us+=us;
                if(us>g_capture_max_us.load())g_capture_max_us=us;
                g_bridge_ready=true;g_game_ready=true;
                std::lock_guard<std::mutex> lock(g_cache_mutex);g_snapshot_at=GetTickCount64();
            }
            else if(g_native) {
                static std::string last;
                if(last!=g_native->error()) {last=g_native->error();std::fprintf(stderr,"Native capture: %s\n",last.c_str());}
            }
        }
        if(xml.find("name=\"xenomitCatalog\"")!=std::string::npos || xml.find("name=\"xenomitItems\"")!=std::string::npos || xml.find("name=\"xenomitNavigation\"")!=std::string::npos)g_native_navigation_dirty=true;
        if (xml.find("name=\"xenomitCatalog\"")!=std::string::npos) {
            auto catalog=as_bridge::parse_response(xml);
            if (catalog.size()<256*1024) g_catalog=std::move(catalog);
        }
        if (xml.find("name=\"xenomitItems\"")!=std::string::npos) {
            auto items=as_bridge::parse_response(xml);
            if (items.size()<256*1024) {g_items=std::move(items);g_items_at=GetTickCount64();g_items_valid=true;g_item_updates++;} else g_items_valid=false;
        }
        if (xml.find("name=\"xenomitItemsFresh\"")!=std::string::npos && g_items_valid) {g_items_at=GetTickCount64();g_item_refreshes++;}
        if (xml.find("name=\"xenomitNavigation\"")!=std::string::npos) {
            auto nav=as_bridge::parse_response(xml);
            if (nav.size()<512*1024 && nav.find("\"map_id\":")!=std::string::npos) {
                g_navigation=std::move(nav);g_navigation_at=GetTickCount64();
            }
        }
        if (xml.find("name=\"xenomitCapture\"")!=std::string::npos) {
            auto parsed=as_bridge::parse_response(xml);
            auto cost=static_cast<uint64_t>(json_number(parsed,"captureMs")*1000);
            publish_state(std::move(parsed),cost);
        }
        if (xml.find("name=\"xenomitMetrics\"")!=std::string::npos) {
            auto data=as_bridge::parse_response(xml);
            g_frames=static_cast<uint64_t>(json_number(data,"frames"));
            g_fps_milli=static_cast<uint64_t>(json_number(data,"fps")*1000);
            g_metrics_at=GetTickCount64();
        }
        // The game reads location through eval/toString. Supply a real page URL
        // and return undefined for unhandled browser-only calls.
        std::string answer="<undefined/>";
        if(xml.find("name=\"xenomitBackend\"")!=std::string::npos)answer=g_native_mode?"<string>native</string>":"<string>cached</string>";
        if(xml.find("name=\"xenomitAnchor\"")!=std::string::npos && g_native)answer="<string>"+g_native->anchor_nonce()+"</string>";
        if (xml.find("location.href")!=std::string::npos)
            answer="<string>"+g_page_url+"</string>";
        auto wide=utf8_to_wide(answer);
        BSTR value=SysAllocStringLen(wide.data(),static_cast<UINT>(wide.size()));
        if (value) {g_flash->SetReturnValue(value);SysFreeString(value);}
        return S_OK;
    }
};
IConnectionPoint *g_events=nullptr;
DWORD g_event_cookie=0;
HRESULT subscribe_events() {
    IConnectionPointContainer *container=nullptr;
    HRESULT hr=g_object->QueryInterface(IID_IConnectionPointContainer,reinterpret_cast<void **>(&container));
    if (FAILED(hr))return hr;
    hr=container->FindConnectionPoint(Flash::DIID__IShockwaveFlashEvents,&g_events);container->Release();
    if (FAILED(hr))return hr;
    auto sink=new FlashEvents();hr=g_events->Advise(sink,&g_event_cookie);sink->Release();return hr;
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            if(wparam==BootstrapTimer) {
                if(g_game_ready.load()) {finish_hidden_bootstrap();return 0;}
                if(g_hidden_draw_bootstrap && g_object && g_bootstrap_dc && !IsWindowVisible(window) && !g_game_ready.load()) {
                    RECT bounds{};GetClientRect(window,&bounds);
                    auto result=OleDraw(g_object,DVASPECT_CONTENT,g_bootstrap_dc,&bounds);
                    ++g_bootstrap_draws;if(FAILED(result))++g_bootstrap_failures;
                }
                return 0;
            }
            break;
        case WM_SIZE:
            if (g_host_window) MoveWindow(g_host_window,0,0,LOWORD(lparam),HIWORD(lparam),TRUE);
            else if (g_in_place) { RECT rect{}; GetClientRect(window,&rect); g_in_place->SetObjectRects(&rect,&rect); }
            return 0;
        case WM_DESTROY: {
            g_stopping=true;
            std::lock_guard<std::mutex> lock(g_calls_mutex);
            for (auto &call:g_calls) { call->error="player shutting down"; SetEvent(call->done); }
            g_calls.clear(); PostQuitMessage(0); return 0;
        }
        case WM_DOBOT_CALL: {
            std::shared_ptr<BridgeCall> call;
            { std::lock_guard<std::mutex> guard(g_calls_mutex);
              if (!g_calls.empty()) { call=g_calls.front(); g_calls.pop_front(); } }
            if (call) {
                if (call->cancelled || GetTickCount64() > call->deadline) call->error="call expired before execution";
                else if (call->name=="hostVisibility") {
                    ShowWindow(window,call->args[0]=="1" ? SW_SHOWNA : SW_HIDE);
                    call->ok=true; call->parsed="visible="+call->args[0];
                } else if (call->name=="hostBackend") {
                    const bool native=call->args[0]=="native";
                    auto unavailable=native ? native_unavailable() : std::string();
                    if(!unavailable.empty()) call->error=unavailable;
                    else {
                        if(native && !g_native) g_native=std::make_unique<NativeState>(g_native_discovery);
                        call->ok=flash_call("configureBackendAS",call->args,call->parsed,call->error);
                        if(call->ok && call->parsed==call->args[0]) {
                            g_native_mode=native;
                            if(g_native) g_native->invalidate();
                            g_native_navigation.clear();g_native_navigation_dirty=true;
                            g_navigation.clear();g_navigation_at=0;
                            g_game_ready=false;
                            g_captures=0;g_capture_total_us=0;g_capture_max_us=0;
                            std::lock_guard<std::mutex> guard(g_cache_mutex);
                            g_snapshot.clear();g_snapshot_at=0;
                        } else {call->ok=false;if(call->error.empty())call->error="Flash did not confirm the requested backend";}
                    }
                } else if (call->name=="hostQuality") {
                    auto quality=utf8_to_wide(call->args[0]);
                    call->ok=SUCCEEDED(put_bstr(&Flash::IShockwaveFlash::put_Quality2,quality));
                    call->parsed="quality="+call->args[0];call->error="could not set Flash quality";
                } else call->ok=flash_call(call->name,call->args,call->parsed,call->error);
                SetEvent(call->done);
            }
            return 0;
        }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

// Overlapped pipe I/O has a bounded wait. A client that connects and disappears
// cannot occupy the only pipe forever. Never wait for FlushFileBuffers.
bool finish_io(HANDLE pipe,OVERLAPPED &overlap,BOOL started,DWORD &bytes,DWORD timeout) {
    if (started) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    auto wait=WaitForSingleObject(overlap.hEvent,timeout);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe,&overlap);
        GetOverlappedResult(pipe,&overlap,&bytes,TRUE);
        return false;
    }
    return GetOverlappedResult(pipe,&overlap,&bytes,FALSE) != FALSE;
}
// Root and parity counters are written on the STA; a torn read only skews a diagnostic.
std::string native_status() {
    if(!g_native) return "";
    return " native_root="+std::string(g_native->root())+" parity_checks="+std::to_string(g_native->parity_checks())+
        " parity_mismatches="+std::to_string(g_native->parity_mismatches());
}

std::string process_command(const std::string &command) {
    if (command == "STATUS") {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto age=g_snapshot_at ? GetTickCount64()-g_snapshot_at : 999999;
        bool ready=g_game_ready.load() && age<=1000;
        return "OK player=1 bridge="+std::to_string(g_bridge_injected.load())+" ready="+std::to_string(ready)+
            " native="+std::to_string(g_native_mode.load())+native_status()+
            " bootstrap_draws="+std::to_string(g_bootstrap_draws.load())+" bootstrap_failures="+std::to_string(g_bootstrap_failures.load())+
            " frames="+std::to_string(g_frames.load())+" fps_milli="+std::to_string(g_metrics_at.load() && GetTickCount64()-g_metrics_at.load()<=1500 ? g_fps_milli.load() : 0)+
            " captures="+std::to_string(g_captures.load())+" capture_us_avg="+
            std::to_string(g_captures ? g_capture_total_us.load()/g_captures.load() : 0)+
            " capture_us_max="+std::to_string(g_capture_max_us.load())+" item_updates="+std::to_string(g_item_updates.load())+" item_refreshes="+std::to_string(g_item_refreshes.load())+" state_age_ms="+std::to_string(age)+"\n";
    }
    if (command=="QUALITY low" || command=="QUALITY medium" || command=="QUALITY high" || command=="QUALITY best")
        return call_on_ui("hostQuality",{command.substr(8)});
    if (command=="BACKEND native" || command=="BACKEND cached") return call_on_ui("hostBackend",{command.substr(8)});
    if (command=="CAPTURE 0" || command=="CAPTURE 1") return call_on_ui("configureAS",{command.substr(8)});
    if (command=="WINDOW 0" || command=="WINDOW 1") return call_on_ui("hostVisibility",{command.substr(7)});
    if (command == "BRIDGE") return call_on_ui("pingBridge",{});
    // Explicit diagnostic reference; native polling never issues this command.
    if (command == "REFERENCE") return call_on_ui("referenceAS",{});
    if (command == "STATE" || command == "STATE\tstage") {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_snapshot.empty() || GetTickCount64()-g_snapshot_at > 1000) return "ERR state unavailable or stale\n";
        return "OK "+g_snapshot+"\n";
    }
    if (command.rfind("STATE\t",0)==0) return "ERR cached state supports only stage\n";
    if (command.rfind("INSPECT\t",0)==0) return call_on_ui("inspectAS",{command.substr(8)});
    bool frame=command.rfind("FINVOKE\t",0)==0;
    if (frame || command.rfind("INVOKE\t",0)==0) {
        auto body=command.substr(frame ? 8 : 7);
        auto first=body.find('\t'),second=first==std::string::npos ? std::string::npos : body.find('\t',first+1);
        if (first==std::string::npos || second==std::string::npos) return "ERR expected target, method, JSON arguments\n";
        std::vector<std::string> args{body.substr(0,first),body.substr(first+1,second-first-1),body.substr(second+1)};
        return frame ? frame_call(args) : call_on_ui("invokeAS",args);
    }
    return "ERR unknown command\n";
}
DWORD WINAPI control_thread(void *) {
    wchar_t name[96]{};
    swprintf_s(name,L"\\\\.\\pipe\\xenomit_flash_%lu",GetCurrentProcessId());
    // Keep the owned endpoint alive across connections. Recreating a FIRST_PIPE
    // instance races the reader's closing handle after its receipt byte.
    HANDLE pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
        1,1<<20,65536,0,nullptr);
    if (pipe==INVALID_HANDLE_VALUE) {std::fprintf(stderr,"Control pipe creation failed: %lu\n",GetLastError());return 1;}
    OVERLAPPED overlap{};overlap.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!overlap.hEvent) {CloseHandle(pipe);return 1;}
    while (!g_stopping.load()) {
        ResetEvent(overlap.hEvent);
        DWORD bytes=0;
        BOOL connected=ConnectNamedPipe(pipe,&overlap);
        DWORD err=connected ? ERROR_SUCCESS : GetLastError();
        bool ok=connected || err==ERROR_PIPE_CONNECTED;
        if (!ok && err==ERROR_IO_PENDING) {
            while (!g_stopping.load() && WaitForSingleObject(overlap.hEvent,100)==WAIT_TIMEOUT) {}
            if (g_stopping.load()) {CancelIoEx(pipe,&overlap);GetOverlappedResult(pipe,&overlap,&bytes,TRUE);}
            else ok=GetOverlappedResult(pipe,&overlap,&bytes,FALSE)!=FALSE;
        }
        if (ok && !g_stopping.load()) {
            ResetEvent(overlap.hEvent);
            std::vector<char> input(65536);
            if (finish_io(pipe,overlap,ReadFile(pipe,input.data(),65535,&bytes,&overlap),bytes,5000) && bytes) {
                std::string command(input.data(),bytes);
                while (!command.empty() && (command.back()=='\r' || command.back()=='\n')) command.pop_back();
                auto reply=command.find('\0')==std::string::npos ? process_command(command) : "ERR embedded NUL\n";
                ResetEvent(overlap.hEvent);
                if (finish_io(pipe,overlap,WriteFile(pipe,reply.data(),static_cast<DWORD>(reply.size()),&bytes,&overlap),bytes,5000)) {
                    ResetEvent(overlap.hEvent);
                    char receipt=0;
                    finish_io(pipe,overlap,ReadFile(pipe,&receipt,1,&bytes,&overlap),bytes,2000);
                }
            }
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(overlap.hEvent);CloseHandle(pipe);
    return 0;
}

HRESULT create_flash(const std::wstring &ocx_path) {
    g_ocx = LoadLibraryExW(ocx_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_ocx) return HRESULT_FROM_WIN32(GetLastError());

    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(g_ocx, "DllGetClassObject"));
    if (!get_class) return HRESULT_FROM_WIN32(GetLastError());
    IClassFactory *factory = nullptr;
    HRESULT hr = get_class(__uuidof(Flash::ShockwaveFlash), IID_IClassFactory,
                           reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->CreateInstance(nullptr, IID_IOleObject, reinterpret_cast<void **>(&g_object));
    factory->Release();
    if (FAILED(hr)) return hr;
    hr = g_object->QueryInterface(__uuidof(Flash::IShockwaveFlash),
                                  reinterpret_cast<void **>(&g_flash));
    return hr;
}

HRESULT attach_with_atl(HWND parent) {
    // Kekka uses an ATL ActiveX host. The system ATL container supplies the
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
    if (g_events) {if(g_event_cookie)g_events->Unadvise(g_event_cookie);g_events->Release();g_events=nullptr;}
    if (g_object) {
        g_object->Close(OLECLOSE_NOSAVE);
        g_object->SetClientSite(nullptr);
    }
    if (g_in_place) { g_in_place->Release(); g_in_place = nullptr; }
    if (g_atl_container) { g_atl_container->Release(); g_atl_container = nullptr; }
    if (g_flash) { g_flash->Release(); g_flash = nullptr; }
    if (g_object) { g_object->Release(); g_object = nullptr; }

    if (g_ocx) { FreeLibrary(g_ocx); g_ocx = nullptr; }
    if (g_atl) { FreeLibrary(g_atl); g_atl = nullptr; }
    if (!g_bridge_temp.empty()) {
        DeleteFileW(g_bridge_temp.c_str());
        g_bridge_temp.clear();
    }
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
    HINSTANCE instance = GetModuleHandleW(nullptr);
    int show = SW_SHOWDEFAULT;
    setvbuf(stdout,nullptr,_IONBF,0);
    auto options = parse_args();
    g_user_agent = options.count(L"--user-agent") ? options[L"--user-agent"] : L"BigpointClient/1.6.9";
    configure_user_agent(g_user_agent);
    if (options.count(L"--launch-stdin")) {
        for (const auto *key:{L"--movie",L"--url",L"--sid",L"--vars",L"--width",L"--height"}) {
            std::string line;
            for (int ch; (ch=std::getchar())!=EOF && ch!='\n';) {
                if (line.size() >= 65535) { std::fprintf(stderr,"bootstrap record too large\n"); return 2; }
                if (ch!='\r') line.push_back(static_cast<char>(ch));
            }
            if (line.empty()) { std::fprintf(stderr,"missing bootstrap record\n"); return 2; }
            options[key]=utf8_to_wide(line);
        }
    }
    if (!options.count(L"--movie")) { std::fprintf(stderr, "--movie is required\n" ); return 2; }
    if (!options.count(L"--flash")) {
        wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH);
        std::wstring executable(path);
        options[L"--flash"]=executable.substr(0,executable.find_last_of(L"\\/"))+L"\\Flash.ocx";
    }
    wchar_t resolved_ocx[32768]{};
    if (!GetFullPathNameW(options[L"--flash"].c_str(),32768,resolved_ocx,nullptr)) return 2;
    options[L"--flash"]=resolved_ocx;
    if(options.count(L"--backend") && options[L"--backend"]!=L"native" && options[L"--backend"]!=L"cached") {std::fprintf(stderr,"Unknown capture backend\n");return 2;}
    g_native_mode=options[L"--backend"]==L"native";
    g_native_runtime_supported=pinned_native_runtime(options[L"--flash"]);
    if(options.count(L"--native-discovery")) {
        const auto &d=options[L"--native-discovery"];
        if(d==L"auto")g_native_discovery=NativeDiscovery::Auto;
        else if(d==L"anchor")g_native_discovery=NativeDiscovery::Anchor;
        else if(d==L"pattern")g_native_discovery=NativeDiscovery::Pattern;
        else if(d==L"parity")g_native_discovery=NativeDiscovery::Parity;
        else {std::fprintf(stderr,"Native discovery must be auto, anchor, pattern or parity\n");return 2;}
    }
    if(options.count(L"--vars")) {
        auto vars=wide_to_utf8(options[L"--vars"].c_str(),static_cast<int>(options[L"--vars"].size()));
        for(size_t at=0;at<=vars.size();) {
            auto next=vars.find('&',at);if(next==std::string::npos)next=vars.size();
            auto pair=vars.substr(at,next-at);
            if(pair.rfind("gameclientHash=",0)==0)g_game_client_hash=pair.substr(15);
            at=next+1;
        }
    }
    if(options.count(L"--hidden-bootstrap")) {
        if(options[L"--hidden-bootstrap"]!=L"draw") {std::fprintf(stderr,"Hidden bootstrap must be draw\n");return 2;}
        g_hidden_draw_bootstrap=true;
    }
    if(g_native_mode) {
        auto unavailable=native_unavailable();
        if(!unavailable.empty()) {std::fprintf(stderr,"%s\n",unavailable.c_str());return 2;}
        g_native=std::make_unique<NativeState>(g_native_discovery);
    }
    // Explicit HighQoS and timer-resolution opt-out prevent hidden-window
    // power heuristics from defeating the 1 ms multimedia timer request.
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    power.StateMask=0;
    if (!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power))) {
        // Older Windows may not support the timer-resolution policy.
        power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power));
    }
    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) { print_hresult("OleInitialize", hr); return 2; }
    hr = create_flash(options[L"--flash"]);
    if (FAILED(hr)) {
        print_hresult("registration-free Flash activation", hr);
        cleanup();
        OleUninitialize();
        return 3;
    }

    long version = 0;
    g_flash->FlashVersion(&version);
    std::printf("Flash ActiveX version raw=%ld\n", version);

    if (options.count(L"--url") && options.count(L"--sid"))
        InternetSetCookieW(options[L"--url"].c_str(), L"dosid", options[L"--sid"].c_str());

    const wchar_t *class_name = L"Xenomit.FlashClient";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "RegisterClassExW failed: %lu\n", GetLastError());
        cleanup(); OleUninitialize(); return 4;
    }
    int width = options.count(L"--width") ? _wtoi(options[L"--width"].c_str()) : 1280;
    int height = options.count(L"--height") ? _wtoi(options[L"--height"].c_str()) : 800;
    std::wstring title = options.count(L"--name") ? options[L"--name"] : L"Xenomit Flash Client";
    RECT requested{0,0,width,height};
    AdjustWindowRectEx(&requested,WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,FALSE,0);
    HWND window = CreateWindowExW(0, class_name, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, requested.right-requested.left, requested.bottom-requested.top, nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::fprintf(stderr, "CreateWindowExW failed: %lu\n", GetLastError());
        cleanup(); OleUninitialize(); return 4;
    }
    g_window = window;

    put_bstr(&Flash::IShockwaveFlash::put_AllowScriptAccess, L"always");
    put_bstr(&Flash::IShockwaveFlash::put_AllowNetworking, L"all");
    auto wmode=options.count(L"--wmode") ? options[L"--wmode"] : L"window";
    if (wmode!=L"direct" && wmode!=L"window" && wmode!=L"opaque") {
        std::fprintf(stderr,"Invalid --wmode: use direct, window, or opaque\n");cleanup();OleUninitialize();return 2;
    }
    put_bstr(&Flash::IShockwaveFlash::put_WMode,wmode);
    std::printf("Flash window mode: %ls\n",wmode.c_str());
    auto quality=options.count(L"--quality") ? options[L"--quality"] : L"low";
    if (quality!=L"low" && quality!=L"medium" && quality!=L"high" && quality!=L"best") {
        std::fprintf(stderr,"Invalid --quality: use low, medium, high, or best\n");cleanup();OleUninitialize();return 2;
    }
    put_bstr(&Flash::IShockwaveFlash::put_Quality2,quality);
    std::printf("Flash quality: %ls (Kekka/DarkBot defaults to low)\n",quality.c_str());
    if (options.count(L"--url")) {
        put_bstr(&Flash::IShockwaveFlash::put_Base,options[L"--url"]);
        g_page_url=wide_to_utf8(options[L"--url"].c_str(),static_cast<int>(options[L"--url"].size()))+"indexInternal.es?action=internalMapRevolution";
    }
    if (options.count(L"--vars")) put_bstr(&Flash::IShockwaveFlash::put_FlashVars, options[L"--vars"]);
    g_flash->put_Menu(VARIANT_FALSE);

    hr = attach_with_atl(window);
    if (FAILED(hr)) {
        print_hresult("ATL ActiveX host attachment", hr);
        DestroyWindow(window);
        cleanup(); OleUninitialize(); return 5;
    }
    if (!load_bridged_movie(options[L"--movie"])) {
        std::fprintf(stderr,"Required bridge injection failed; refusing an uncontrollable game session\n");
        DestroyWindow(window); cleanup(); OleUninitialize(); return 6;
    }
    g_flash->put_Playing(VARIANT_TRUE);
    if(g_hidden_draw_bootstrap) {
        HDC screen=GetDC(window);
        g_bootstrap_dc=CreateCompatibleDC(screen);
        g_bootstrap_bitmap=CreateCompatibleBitmap(screen,width,height);
        ReleaseDC(window,screen);
        if(!g_bootstrap_dc || !g_bootstrap_bitmap) {std::fprintf(stderr,"Could not allocate hidden draw surface\n");DestroyWindow(window);cleanup();OleUninitialize();return 7;}
        g_bootstrap_old_bitmap=SelectObject(g_bootstrap_dc,g_bootstrap_bitmap);
        if(!SetTimer(window,BootstrapTimer,16,nullptr)) {std::fprintf(stderr,"Could not schedule hidden draw bootstrap\n");DestroyWindow(window);cleanup();OleUninitialize();return 7;}
        std::printf("Hidden bootstrap: offscreen OleDraw until game readiness\n");
    }
    std::printf("Reflection bridge: %s\n", g_bridge_injected.load() ? "injected" : "not injected");
    if (!options.count(L"--hidden")) ShowWindow(window, show);
    UpdateWindow(window);
    HANDLE controller = CreateThread(nullptr, 0, control_thread, nullptr, 0, nullptr);

    std::printf("Control pipe: \\\\.\\pipe\\xenomit_flash_%lu\n", GetCurrentProcessId());
    std::printf("ActiveX host: system ATL container\n");
    timeBeginPeriod(1);


    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_stopping=true;
    if (controller) { WaitForSingleObject(controller,11000); CloseHandle(controller); }
    timeEndPeriod(1);
    cleanup();
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
