// InProcessPlayer §9 input injection. Messages are sent synchronously to the Flash
// control window on the STA, so they never pass the GetMessage loop where real
// input can be filtered (setUserInput).
#include "host.h"

namespace xf {
namespace {

LPARAM point(int x, int y) {
    return static_cast<LPARAM>((static_cast<uint32_t>(y & 0xffff) << 16) | static_cast<uint32_t>(x & 0xffff));
}

LPARAM key_lparam(UINT vk, bool up) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM value = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
    switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU:
            value |= LPARAM(1) << 24; // extended key
            break;
    }
    if (up) value |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
    return value;
}

HWND target() {
    HWND window = flash_window();
    return window && IsWindow(window) ? window : nullptr;
}

void send_key(HWND window, UINT vk, bool up) {
    // Flash reads modifiers from the thread key state, not the message stream.
    BYTE state[256]{};
    GetKeyboardState(state);
    state[vk & 0xff] = up ? 0 : 0x80;
    SetKeyboardState(state);
    SendMessageW(window, up ? WM_KEYUP : WM_KEYDOWN, vk, key_lparam(vk, up));
    if (!up) {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        WCHAR chars[4]{};
        int n = ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), keys, chars, 4, 0);
        for (int i = 0; i < n; ++i) SendMessageW(window, WM_CHAR, chars[i], key_lparam(vk, false));
    }
}

} // namespace

std::string input_mouse(int kind, int x, int y) {
    HWND window = target();
    if (!window) return "Flash window unavailable";
    switch (kind) {
        case XF_MOUSE_MOVE: SendMessageW(window, WM_MOUSEMOVE, 0, point(x, y)); break;
        case XF_MOUSE_DOWN: SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(x, y)); break;
        case XF_MOUSE_UP: SendMessageW(window, WM_LBUTTONUP, 0, point(x, y)); break;
        case XF_MOUSE_CLICK:
            SendMessageW(window, WM_MOUSEMOVE, 0, point(x, y));
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(x, y));
            SendMessageW(window, WM_LBUTTONUP, 0, point(x, y));
            break;
        default: return "unknown mouse action";
    }
    return {};
}

std::string input_key(UINT vk, int kind) {
    HWND window = target();
    if (!window) return "Flash window unavailable";
    if (!vk || vk > 0xfe) return "invalid virtual key";
    switch (kind) {
        case XF_KEY_DOWN: send_key(window, vk, false); break;
        case XF_KEY_UP: send_key(window, vk, true); break;
        case XF_KEY_CLICK: send_key(window, vk, false); send_key(window, vk, true); break;
        default: return "unknown key action";
    }
    return {};
}

std::string input_text(const std::wstring &text) {
    HWND window = target();
    if (!window) return "Flash window unavailable";
    // InProcessPlayer sendText: one WM_CHAR per UTF-16 code unit.
    for (wchar_t unit : text) SendMessageW(window, WM_CHAR, unit, 1);
    return {};
}

std::string input_paste(const std::wstring &text) {
    HWND window = target();
    if (!window) return "Flash window unavailable";
    if (!OpenClipboard(host_window())) return "clipboard unavailable";
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { CloseClipboard(); return "out of memory"; }
    memcpy(GlobalLock(memory), text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) { GlobalFree(memory); CloseClipboard(); return "could not set clipboard"; }
    CloseClipboard();
    BYTE saved[256]{};
    GetKeyboardState(saved);
    BYTE state[256];
    memcpy(state, saved, sizeof(state));
    state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    SetKeyboardState(state);
    SendMessageW(window, WM_KEYDOWN, VK_CONTROL, key_lparam(VK_CONTROL, false));
    SendMessageW(window, WM_KEYDOWN, 'V', key_lparam('V', false));
    SendMessageW(window, WM_CHAR, 0x16, key_lparam('V', false));
    SendMessageW(window, WM_KEYUP, 'V', key_lparam('V', true));
    SendMessageW(window, WM_KEYUP, VK_CONTROL, key_lparam(VK_CONTROL, true));
    SetKeyboardState(saved);
    return {};
}

bool input_filter(const MSG &message) {
    const UINT m = message.message;
    // Client-area input only: the frame (move, resize, close) stays usable.
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST);
}

} // namespace xf
