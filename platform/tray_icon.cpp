#include "platform/tray_icon.hpp"

#include <windows.h>
#include <shellapi.h>

namespace sam::platform::tray_icon {

namespace {

HWND g_owner = nullptr;
unsigned g_icon_res = 0;
bool g_added = false;
NOTIFYICONDATAW g_nid{};

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

bool ensure_added() {
    if (g_added) return true;
    if (!g_owner) return false;

    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_owner;
    g_nid.uID = kIconId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kCallbackMessage;
    g_nid.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr),
                              MAKEINTRESOURCEW(g_icon_res));
    ::lstrcpynW(g_nid.szTip, L"Steam Account Manager", ARRAYSIZE(g_nid.szTip));

    if (!::Shell_NotifyIconW(NIM_ADD, &g_nid)) return false;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    g_added = true;
    return true;
}

}  // namespace

void set_owner(HWND owner, unsigned icon_resource_id) {
    g_owner = owner;
    g_icon_res = icon_resource_id;
}

void remove() {
    if (!g_added) return;
    ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = false;
}

bool show_balloon(const std::string& title, const std::string& message, bool warning) {
    if (!ensure_added()) return false;

    const std::wstring wtitle = utf8_to_wide(title);
    const std::wstring wmsg = utf8_to_wide(message);

    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    ::lstrcpynW(nid.szInfoTitle, wtitle.c_str(), ARRAYSIZE(nid.szInfoTitle));
    ::lstrcpynW(nid.szInfo, wmsg.c_str(), ARRAYSIZE(nid.szInfo));
    return ::Shell_NotifyIconW(NIM_MODIFY, &nid) != 0;
}

bool owner_is_foreground() {
    return g_owner != nullptr && ::GetForegroundWindow() == g_owner;
}

}  // namespace sam::platform::tray_icon
