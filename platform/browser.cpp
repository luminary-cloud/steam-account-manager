#include "platform/browser.hpp"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>

#include <windows.h>
#include <shellapi.h>

#include "platform/registry.hpp"

namespace sam::platform {

namespace {

enum class BrowserKind { Unknown, Chromium, Firefox };

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Reads a REG_SZ / REG_EXPAND_SZ value (the key's default value when
// `value_name` is null), expanding any environment references.
std::optional<std::wstring> read_reg_sz(HKEY root, const std::wstring& subkey,
                                        const wchar_t* value_name = nullptr) {
    WCHAR buf[2048];
    DWORD cb = sizeof(buf);
    const LSTATUS st = RegGetValueW(root, subkey.c_str(), value_name,
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &cb);
    if (st != ERROR_SUCCESS) return std::nullopt;
    return std::wstring(buf);
}

// The ProgId registered as the default https handler, e.g. "ChromeHTML",
// "MSEdgeHTM", "FirefoxURL", "BraveHTML".
std::optional<std::wstring> default_https_progid() {
    return registry::read_string_hkcu(
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice",
        L"ProgId");
}

// HKCR\<progid>\shell\open\command default value (the launch command line).
std::optional<std::wstring> progid_open_command(const std::wstring& progid) {
    return read_reg_sz(HKEY_CLASSES_ROOT, progid + L"\\shell\\open\\command");
}

// Full path to `exe` from the App Paths registry (per-machine, then per-user).
// This reliably locates a browser when its ProgId command isn't in a standard
// place under HKEY_CLASSES_ROOT (notably Edge, which registers elsewhere).
std::optional<std::wstring> app_path_exe(const std::wstring& exe) {
    const std::wstring sub = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe;
    if (auto p = read_reg_sz(HKEY_LOCAL_MACHINE, sub)) return p;
    return read_reg_sz(HKEY_CURRENT_USER, sub);
}

// Pulls the executable out of a shell open command. Handles the common quoted
// form (`"C:\...\chrome.exe" --foo "%1"`) and a bare first token.
std::wstring exe_from_command(const std::wstring& cmd) {
    if (cmd.empty()) return {};
    if (cmd.front() == L'"') {
        const auto end = cmd.find(L'"', 1);
        return end == std::wstring::npos ? std::wstring{} : cmd.substr(1, end - 1);
    }
    const auto sp = cmd.find(L' ');
    return sp == std::wstring::npos ? cmd : cmd.substr(0, sp);
}

struct DefaultBrowser {
    std::wstring exe;
    BrowserKind kind = BrowserKind::Unknown;
};

DefaultBrowser resolve_default_browser() {
    DefaultBrowser out;
    const auto progid = default_https_progid();
    if (!progid) return out;
    if (auto cmd = progid_open_command(*progid)) out.exe = exe_from_command(*cmd);

    // Identify the family from the ProgId, with the exe filename as a backstop.
    const std::wstring id = to_lower(*progid) + L"|" + to_lower(out.exe);
    auto has = [&](const wchar_t* s) { return id.find(s) != std::wstring::npos; };

    const wchar_t* exe_name = nullptr;  // App Paths key when the exe is unknown
    if (has(L"firefox")) {
        out.kind = BrowserKind::Firefox;
        exe_name = L"firefox.exe";
    } else if (has(L"msedge") || has(L"edge")) {
        out.kind = BrowserKind::Chromium;
        exe_name = L"msedge.exe";
    } else if (has(L"chrome")) {
        out.kind = BrowserKind::Chromium;
        exe_name = L"chrome.exe";
    } else if (has(L"brave")) {
        out.kind = BrowserKind::Chromium;
        exe_name = L"brave.exe";
    } else if (has(L"vivaldi")) {
        out.kind = BrowserKind::Chromium;
        exe_name = L"vivaldi.exe";
    } else if (has(L"opera")) {
        out.kind = BrowserKind::Chromium;
        exe_name = L"opera.exe";
    }

    // Edge and some store-installed browsers don't expose a usable
    // shell\open\command under HKEY_CLASSES_ROOT; fall back to App Paths.
    if (out.exe.empty() && exe_name != nullptr) {
        if (auto p = app_path_exe(exe_name)) out.exe = *p;
    }
    return out;
}

bool shell_open(const std::wstring& file, const std::wstring& params) {
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", file.c_str(),
                                       params.empty() ? nullptr : params.c_str(),
                                       nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

}  // namespace

bool open_isolated_window(const std::wstring& url, const std::wstring& profile_dir) {
    const DefaultBrowser b = resolve_default_browser();
    if (!b.exe.empty() && b.kind != BrowserKind::Unknown) {
        std::wstring params;
        if (b.kind == BrowserKind::Chromium) {
            // A separate --user-data-dir is a fully isolated profile. The
            // first-run/default-browser prompts would otherwise interrupt the
            // sign-in page, so suppress them.
            params = L"--user-data-dir=\"" + profile_dir + L"\""
                     L" --no-first-run --no-default-browser-check"
                     L" \"" + url + L"\"";
        } else {  // Firefox
            params = L"-profile \"" + profile_dir + L"\" \"" + url + L"\"";
        }
        if (shell_open(b.exe, params)) return true;
    }
    // Couldn't identify the browser (or the launch failed): open the URL in the
    // default browser normally so the action still does something.
    shell_open(url, L"");
    return false;
}

}  // namespace sam::platform
