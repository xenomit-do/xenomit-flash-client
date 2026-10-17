// InProcessPlayer §7.1/§11 equivalents, installed with Microsoft Detours before
// Flash.ocx loads. Only operating-system APIs are hooked; Flash's VM and JIT
// are never patched.
//  - Virtual Flash\mms.cfg and settings.sol (CreateFileW/ReadFile/GetFileSize*,
//    CloseHandle), so Flash sees our policy without admin rights.
//  - Network observation: InternetReadFile count/age, and the game socket
//    (connect on a configured port, recv/WSARecv counts, bytes and age,
//    closesocket to report a dropped connection).
//  - Request blocking patterns and the optional local proxy.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "host.h"

#include <wininet.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "detours.h"
#include "flash_config.inc"

namespace xf {
namespace {

struct VirtualFile {
    const std::string *data;
    uint64_t position = 0;
};

std::mutex g_vfs_mutex;
std::map<HANDLE, VirtualFile> g_vfs;
std::atomic<size_t> g_vfs_open{0};
std::atomic<uint64_t> g_vfs_hits{0};
std::string g_mms_cfg;
std::string g_settings_sol;

std::atomic<uint64_t> g_net_reads{0}, g_net_read_at{0};
std::atomic<uint64_t> g_game_recv{0}, g_game_recv_bytes{0}, g_game_recv_at{0};
std::atomic<SOCKET> g_game_socket{INVALID_SOCKET};
std::atomic<uint64_t> g_blocked{0};
std::vector<uint16_t> g_game_ports;
int g_proxy_port = 0;

std::mutex g_block_mutex;
std::vector<std::wstring> g_block_patterns;
std::atomic<bool> g_block_any{false};

decltype(&::CreateFileW) Real_CreateFileW = ::CreateFileW;
decltype(&::ReadFile) Real_ReadFile = ::ReadFile;
decltype(&::GetFileSizeEx) Real_GetFileSizeEx = ::GetFileSizeEx;
decltype(&::GetFileSize) Real_GetFileSize = ::GetFileSize;
decltype(&::CloseHandle) Real_CloseHandle = ::CloseHandle;
decltype(&::InternetReadFile) Real_InternetReadFile = ::InternetReadFile;
decltype(&::InternetConnectW) Real_InternetConnectW = ::InternetConnectW;
decltype(&::HttpOpenRequestW) Real_HttpOpenRequestW = ::HttpOpenRequestW;
decltype(&::InternetOpenW) Real_InternetOpenW = ::InternetOpenW;
using ConnectFn = int(WSAAPI *)(SOCKET, const sockaddr *, int);
using RecvFn = int(WSAAPI *)(SOCKET, char *, int, int);
using WsaRecvFn = int(WSAAPI *)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using CloseSocketFn = int(WSAAPI *)(SOCKET);
ConnectFn Real_connect = nullptr;
RecvFn Real_recv = nullptr;
WsaRecvFn Real_WSARecv = nullptr;
CloseSocketFn Real_closesocket = nullptr;

bool ends_with_insensitive(const wchar_t *path, const wchar_t *suffix) {
    if (!path) return false;
    size_t n = wcslen(path), m = wcslen(suffix);
    if (n < m) return false;
    return _wcsicmp(path + n - m, suffix) == 0;
}

const std::string *virtual_file(const wchar_t *path) {
    // Same registered suffixes as InProcessPlayer (§3.5): the system mms.cfg and
    // the per-user Flash Player settings LSO.
    if (ends_with_insensitive(path, L"\\Flash\\mms.cfg")) return &g_mms_cfg;
    if (ends_with_insensitive(path, L"Player\\macromedia.com\\support\\flashplayer\\sys\\settings.sol")) return &g_settings_sol;
    return nullptr;
}

VirtualFile *find_virtual(HANDLE handle) {
    auto it = g_vfs.find(handle);
    return it == g_vfs.end() ? nullptr : &it->second;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    auto data = (access & writes) || (disposition != OPEN_EXISTING && disposition != OPEN_ALWAYS) ? nullptr : virtual_file(name);
    if (!data) return Real_CreateFileW(name, access, share, security, disposition, flags, templ);
    // A real, closable handle: a reused value is removed again in CloseHandle.
    HANDLE token = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!token) return Real_CreateFileW(name, access, share, security, disposition, flags, templ);
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    g_vfs[token] = {data, 0};
    g_vfs_open = g_vfs.size();
    ++g_vfs_hits;
    SetLastError(ERROR_SUCCESS);
    return token;
}

BOOL WINAPI Hook_ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
    if (g_vfs_open.load()) {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        if (auto entry = find_virtual(file)) {
            uint64_t position = overlapped ? (uint64_t(overlapped->OffsetHigh) << 32 | overlapped->Offset) : entry->position;
            auto total = entry->data->size();
            DWORD amount = position >= total ? 0 : static_cast<DWORD>(std::min<uint64_t>(size, total - position));
            if (amount) memcpy(buffer, entry->data->data() + position, amount);
            if (!overlapped) entry->position = position + amount;
            if (read) *read = amount;
            if (overlapped) {
                overlapped->Internal = 0;
                overlapped->InternalHigh = amount;
                if (overlapped->hEvent) SetEvent(overlapped->hEvent);
            }
            return TRUE;
        }
    }
    return Real_ReadFile(file, buffer, size, read, overlapped);
}

BOOL WINAPI Hook_GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
    if (g_vfs_open.load()) {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        if (auto entry = find_virtual(file)) {
            if (!size) return FALSE;
            size->QuadPart = static_cast<LONGLONG>(entry->data->size());
            return TRUE;
        }
    }
    return Real_GetFileSizeEx(file, size);
}

DWORD WINAPI Hook_GetFileSize(HANDLE file, LPDWORD high) {
    if (g_vfs_open.load()) {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        if (auto entry = find_virtual(file)) {
            if (high) *high = 0;
            return static_cast<DWORD>(entry->data->size());
        }
    }
    return Real_GetFileSize(file, high);
}

BOOL WINAPI Hook_CloseHandle(HANDLE handle) {
    if (g_vfs_open.load()) {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        if (g_vfs.erase(handle)) g_vfs_open = g_vfs.size();
    }
    return Real_CloseHandle(handle);
}

BOOL WINAPI Hook_InternetReadFile(HINTERNET file, LPVOID buffer, DWORD size, LPDWORD read) {
    ++g_net_reads;
    g_net_read_at = GetTickCount64();
    return Real_InternetReadFile(file, buffer, size, read);
}

bool reject_blocked(const wchar_t *text) {
    if (!hooks_blocked(text)) return false;
    ++g_blocked;
    SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
    return true;
}

HINTERNET WINAPI Hook_InternetConnectW(HINTERNET session, LPCWSTR server, INTERNET_PORT port, LPCWSTR user,
                                       LPCWSTR password, DWORD service, DWORD flags, DWORD_PTR context) {
    if (reject_blocked(server)) return nullptr;
    return Real_InternetConnectW(session, server, port, user, password, service, flags, context);
}

HINTERNET WINAPI Hook_HttpOpenRequestW(HINTERNET connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                       LPCWSTR referrer, LPCWSTR *accept, DWORD flags, DWORD_PTR context) {
    if (reject_blocked(object)) return nullptr;
    return Real_HttpOpenRequestW(connect, verb, object, version, referrer, accept, flags, context);
}

HINTERNET WINAPI Hook_InternetOpenW(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags) {
    std::wstring forced;
    if (proxy_access_type(forced) == INTERNET_OPEN_TYPE_PROXY)
        return Real_InternetOpenW(agent, INTERNET_OPEN_TYPE_PROXY, forced.c_str(), L"<local>", flags);
    return Real_InternetOpenW(agent, access, proxy, bypass, flags);
}

uint16_t socket_port(const sockaddr *address, int length) {
    if (!address) return 0;
    if (address->sa_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in)))
        return ntohs(reinterpret_cast<const sockaddr_in *>(address)->sin_port);
    if (address->sa_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6)))
        return ntohs(reinterpret_cast<const sockaddr_in6 *>(address)->sin6_port);
    return 0;
}

int WSAAPI Hook_connect(SOCKET s, const sockaddr *address, int length) {
    auto port = socket_port(address, length);
    if (port && std::find(g_game_ports.begin(), g_game_ports.end(), port) != g_game_ports.end()) {
        g_game_socket = s;
        g_game_recv_at = GetTickCount64();
    }
    return Real_connect(s, address, length);
}

void count_game_bytes(SOCKET s, uint64_t bytes) {
    if (!bytes || s != g_game_socket.load()) return;
    ++g_game_recv;
    g_game_recv_bytes += bytes;
    g_game_recv_at = GetTickCount64();
}

int WSAAPI Hook_recv(SOCKET s, char *buffer, int length, int flags) {
    int result = Real_recv(s, buffer, length, flags);
    if (result > 0) count_game_bytes(s, static_cast<uint64_t>(result));
    return result;
}

int WSAAPI Hook_WSARecv(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags,
                        LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
    int result = Real_WSARecv(s, buffers, count, received, flags, overlapped, routine);
    // Overlapped completions finish later and are not counted here.
    if (result == 0 && received) count_game_bytes(s, *received);
    return result;
}

int WSAAPI Hook_closesocket(SOCKET s) {
    SOCKET expected = s;
    g_game_socket.compare_exchange_strong(expected, INVALID_SOCKET);
    return Real_closesocket(s);
}

template <class T>
void attach(T &real, T hook, const char *name) {
    if (!real) { log(1, "Hook target missing: %s", name); return; }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG error = DetourAttach(reinterpret_cast<PVOID *>(&real), reinterpret_cast<PVOID>(hook));
    LONG commit = DetourTransactionCommit();
    if (error != NO_ERROR || commit != NO_ERROR) log(2, "Failed to attach %s! (%ld/%ld)", name, error, commit);
}

uint64_t age(uint64_t at) {
    return at ? GetTickCount64() - at : UINT64_MAX;
}

} // namespace

void hooks_install(const HookOptions &options) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_game_ports = options.game_ports;
    g_proxy_port = options.local_proxy_port;
    g_mms_cfg = flash_config::mms_cfg();
    g_settings_sol = flash_config::settings_sol();
    attach(Real_CreateFileW, &Hook_CreateFileW, "CreateFileW");
    attach(Real_ReadFile, &Hook_ReadFile, "ReadFile");
    attach(Real_GetFileSizeEx, &Hook_GetFileSizeEx, "GetFileSizeEx");
    attach(Real_GetFileSize, &Hook_GetFileSize, "GetFileSize");
    attach(Real_CloseHandle, &Hook_CloseHandle, "CloseHandle");
    attach(Real_InternetReadFile, &Hook_InternetReadFile, "InternetReadFile");
    attach(Real_InternetConnectW, &Hook_InternetConnectW, "InternetConnectW");
    attach(Real_HttpOpenRequestW, &Hook_HttpOpenRequestW, "HttpOpenRequestW");
    attach(Real_InternetOpenW, &Hook_InternetOpenW, "InternetOpenW");
    if (HMODULE ws2 = LoadLibraryW(L"ws2_32.dll")) {
        Real_connect = reinterpret_cast<ConnectFn>(GetProcAddress(ws2, "connect"));
        Real_recv = reinterpret_cast<RecvFn>(GetProcAddress(ws2, "recv"));
        Real_WSARecv = reinterpret_cast<WsaRecvFn>(GetProcAddress(ws2, "WSARecv"));
        Real_closesocket = reinterpret_cast<CloseSocketFn>(GetProcAddress(ws2, "closesocket"));
        attach(Real_connect, &Hook_connect, "connect");
        attach(Real_recv, &Hook_recv, "recv");
        attach(Real_WSARecv, &Hook_WSARecv, "WSARecv");
        attach(Real_closesocket, &Hook_closesocket, "closesocket");
    }
}

void hooks_set_blocking(std::vector<std::string> patterns) {
    std::vector<std::wstring> wide;
    for (auto &pattern : patterns) {
        auto text = utf8_to_wide(pattern);
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (!text.empty()) wide.push_back(std::move(text));
    }
    std::lock_guard<std::mutex> lock(g_block_mutex);
    g_block_patterns = std::move(wide);
    g_block_any = !g_block_patterns.empty();
}

bool hooks_blocked(const wchar_t *text) {
    if (!text || !g_block_any.load()) return false;
    std::wstring lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::lock_guard<std::mutex> lock(g_block_mutex);
    for (auto &pattern : g_block_patterns)
        if (lower.find(pattern) != std::wstring::npos) return true;
    return false;
}

void hooks_status(XfStatus &status) {
    status.net_reads = g_net_reads.load();
    status.net_read_age_ms = age(g_net_read_at.load());
    status.game_socket = g_game_socket.load() != INVALID_SOCKET ? 1 : 0;
    status.game_recv = g_game_recv.load();
    status.game_recv_bytes = g_game_recv_bytes.load();
    status.game_recv_age_ms = age(g_game_recv_at.load());
    status.vfs_hits = g_vfs_hits.load();
    status.blocked_requests = g_blocked.load();
}

DWORD proxy_access_type(std::wstring &proxy) {
    if (g_proxy_port <= 0) { proxy.clear(); return INTERNET_OPEN_TYPE_PRECONFIG; }
    proxy = L"127.0.0.1:" + std::to_wstring(g_proxy_port);
    return INTERNET_OPEN_TYPE_PROXY;
}

uint32_t clear_url_cache(const std::wstring &substring) {
    std::wstring needle(substring);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::vector<uint8_t> buffer(4096);
    uint32_t deleted = 0;
    auto entry = [&] { return reinterpret_cast<INTERNET_CACHE_ENTRY_INFOW *>(buffer.data()); };
    DWORD size = static_cast<DWORD>(buffer.size());
    HANDLE find = FindFirstUrlCacheEntryW(nullptr, entry(), &size);
    if (!find && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(size);
        find = FindFirstUrlCacheEntryW(nullptr, entry(), &size);
    }
    if (!find) return 0;
    for (;;) {
        if (entry()->lpszSourceUrlName) {
            std::wstring url(entry()->lpszSourceUrlName);
            std::transform(url.begin(), url.end(), url.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if ((needle.empty() || url.find(needle) != std::wstring::npos) && DeleteUrlCacheEntryW(entry()->lpszSourceUrlName)) ++deleted;
        }
        size = static_cast<DWORD>(buffer.size());
        if (FindNextUrlCacheEntryW(find, entry(), &size)) continue;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        buffer.resize(size);
        size = static_cast<DWORD>(buffer.size());
        if (!FindNextUrlCacheEntryW(find, entry(), &size)) break;
    }
    FindCloseUrlCache(find);
    return deleted;
}

} // namespace xf
