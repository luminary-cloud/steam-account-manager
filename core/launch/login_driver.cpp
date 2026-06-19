#include "core/launch/login_driver.hpp"

#include <atomic>
#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <UIAutomationClient.h>

#include "core/crypto/secure_string.hpp"
#include "core/launch/code_clipboard.hpp"
#include "core/log.hpp"
#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"
#include "platform/ui_automation.hpp"

namespace sam::launch::login_driver {

namespace {

using namespace std::chrono_literals;
using Elt = sam::platform::uia::Session::Element;

constexpr auto kWindowPollInterval = 100ms;
constexpr auto kClassifyInterval   = 100ms;
constexpr auto kDigitInterval      = 50ms;
constexpr auto kOverallTimeout     = 90s;

// Each run_async() bumps this and captures its own value; workers compare against
// the latest on every poll so a newer run supersedes an in-flight one.
std::atomic<std::uint64_t> g_current_gen{0};

bool ieq(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void zero_wstring(std::wstring& s) noexcept {
    if (!s.empty()) {
        sam::crypto::zero_buffer(s.data(), s.size() * sizeof(wchar_t));
    }
}

std::vector<std::uint32_t> find_webhelper_children(std::uint32_t parent_pid) {
    std::vector<std::uint32_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (e.th32ParentProcessID == parent_pid &&
                ieq(e.szExeFile, L"steamwebhelper.exe")) {
                out.push_back(e.th32ProcessID);
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

struct EnumCtx {
    const std::vector<std::uint32_t>* pids;
    std::vector<HWND>* out;
};

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (auto p : *ctx->pids) {
        if (p == pid) { ctx->out->push_back(hwnd); break; }
    }
    return TRUE;
}

std::vector<HWND> enumerate_windows_for_pids(const std::vector<std::uint32_t>& pids) {
    std::vector<HWND> out;
    if (pids.empty()) return out;
    EnumCtx ctx{&pids, &out};
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::wstring get_window_title(HWND h) {
    const int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring out(static_cast<std::size_t>(len) + 1, L'\0');
    const int n = GetWindowTextW(h, out.data(), len + 1);
    out.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
    return out;
}

// The login UI is hosted by a steamwebhelper.exe child of steam.exe; its title
// contains "Steam" with length > 5 (excludes the plain "Steam" main-client title).
// Also accepts the Chinese localised title.
HWND find_login_window(std::uint32_t main_pid) {
    const auto helpers = find_webhelper_children(main_pid);
    if (helpers.empty()) return nullptr;
    const auto windows = enumerate_windows_for_pids(helpers);
    for (HWND h : windows) {
        const std::wstring title = get_window_title(h);
        if ((title.find(L"Steam") != std::wstring::npos && title.size() > 5)
            || title == L"蒸汽平台登录") {
            return h;
        }
    }
    return nullptr;
}

void send_unicode_char(wchar_t ch) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = static_cast<WORD>(ch);
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

enum class WindowState { None, Loading, Login, Selection, Code, Error, Invalid };

const char* to_str(WindowState s) {
    switch (s) {
        case WindowState::None:      return "None";
        case WindowState::Loading:   return "Loading";
        case WindowState::Login:     return "Login";
        case WindowState::Selection: return "Selection";
        case WindowState::Code:      return "Code";
        case WindowState::Error:     return "Error";
        case WindowState::Invalid:   return "Invalid";
    }
    return "?";
}

WindowState classify(const Elt& doc) {
    auto children = doc.all_children();
    if (children.empty()) return WindowState::Invalid;
    if (children.size() == 2) return WindowState::Loading;

    int n_edit = 0, n_button = 0, n_group = 0, n_image = 0, n_text = 0;
    for (const auto& c : children) {
        switch (c.control_type()) {
            case UIA_EditControlTypeId:   ++n_edit;   break;
            case UIA_ButtonControlTypeId: ++n_button; break;
            case UIA_GroupControlTypeId:  ++n_group;  break;
            case UIA_ImageControlTypeId:  ++n_image;  break;
            case UIA_TextControlTypeId:   ++n_text;   break;
            default: break;
        }
    }

    SAM_LOG_DEBUG("login_driver: tree children={} edit={} button={} group={} image={} text={}",
                  children.size(), n_edit, n_button, n_group, n_image, n_text);

    // Empirical child-control-type counts for each login-window state.
    if (n_edit == 0 && n_button == 5 && n_group == 0 && n_image == 3 && n_text == 5) {
        return WindowState::Code;
    }
    if (n_edit == 2 && n_button == 1) return WindowState::Login;
    if (n_edit == 0 && n_image >= 2 && n_button > 0 && n_text == 0) return WindowState::Selection;
    if (n_edit == 0 && n_button == 1 && n_text == 2 && n_image == 1) return WindowState::Error;
    if (n_edit == 0 && n_button >= 2 && n_image >= 1 && n_text > 0) return WindowState::Error;
    return WindowState::Invalid;
}

bool drive_login_state(const Elt& doc, const Credentials& creds) {
    auto children = doc.all_children();
    std::vector<Elt> edits, buttons, groups;
    for (auto& c : children) {
        switch (c.control_type()) {
            case UIA_EditControlTypeId:   edits.push_back(c);   break;
            case UIA_ButtonControlTypeId: buttons.push_back(c); break;
            case UIA_GroupControlTypeId:  groups.push_back(c);  break;
            default: break;
        }
    }
    if (edits.size() < 2 || buttons.empty()) return false;

    Elt& sign_in = buttons[0];
    sign_in.wait_until_enabled(2000ms);
    if (!sign_in.is_enabled()) return false;

    {
        std::wstring user = utf8_to_wide(sam::crypto::view(creds.login));
        edits[0].wait_until_enabled(2000ms);
        edits[0].set_value(user);
        zero_wstring(user);
    }
    {
        std::wstring pass = utf8_to_wide(sam::crypto::view(creds.password));
        edits[1].wait_until_enabled(2000ms);
        edits[1].set_value(pass);
        zero_wstring(pass);
    }

    if (!groups.empty()) {
        // Presence of an Image child = checkbox is currently ticked.
        const bool checked =
            groups[0].first_child_by_control_type(UIA_ImageControlTypeId).has_value();
        if (creds.remember_password != checked) {
            groups[0].focus();
            groups[0].wait_until_enabled(500ms);
            groups[0].invoke();
        }
    }

    sign_in.focus();
    sign_in.wait_until_enabled(500ms);
    return sign_in.invoke();
}

bool drive_code_state(const Elt& doc, std::string_view shared_secret) {
    if (shared_secret.empty()) {
        SAM_LOG_INFO("login_driver: 2FA prompt but no stored secret");
        return false;
    }
    auto buttons = doc.children_by_control_type(UIA_ButtonControlTypeId);
    if (buttons.size() < 5) return false;

    const std::string code = sam::sda::generate_code(
        shared_secret, sam::time_aligner::aligned_now());
    if (code.size() < 5) {
        SAM_LOG_WARN("login_driver: TOTP generation failed");
        return false;
    }

    constexpr int  kMaxAttemptsPerDigit = 3;
    constexpr auto kSlotSettleTimeout   = 500ms;
    constexpr auto kSlotPollInterval    = 20ms;

    for (std::size_t i = 0; i < 5; ++i) {
        bool digit_ok = false;
        for (int attempt = 0; attempt < kMaxAttemptsPerDigit && !digit_ok; ++attempt) {
            buttons[i].focus();
            buttons[i].invoke();
            std::this_thread::sleep_for(20ms);
            send_unicode_char(static_cast<wchar_t>(code[i]));

            // A filled slot button grows a child element (the rendered digit);
            // poll until it appears before moving to the next digit.
            using clk = std::chrono::steady_clock;
            const auto deadline = clk::now() + kSlotSettleTimeout;
            while (clk::now() < deadline) {
                if (!buttons[i].all_children().empty()) { digit_ok = true; break; }
                std::this_thread::sleep_for(kSlotPollInterval);
            }
            if (!digit_ok) {
                SAM_LOG_DEBUG("login_driver: 2FA digit {} attempt {} dropped; retrying",
                              i, attempt + 1);
            }
        }
        if (!digit_ok) {
            SAM_LOG_WARN("login_driver: 2FA digit {} failed after {} attempts",
                         i, kMaxAttemptsPerDigit);
            return false;
        }
    }
    return true;
}

bool drive_selection_state(const Elt& doc) {
    // Account picker: invoke "Add Account" (the last Group).
    auto groups = doc.children_by_control_type(UIA_GroupControlTypeId);
    if (groups.empty()) return false;
    auto& g = groups.back();
    g.focus();
    return g.invoke();
}

void worker_body(std::uint64_t gen, std::uint32_t pid, Credentials creds) {
    sam::platform::uia::Session uia;
    if (!uia.ok()) {
        SAM_LOG_ERROR("login_driver: UIA session init failed");
        sam::launch::code_clipboard::copy_code_to_clipboard(creds.shared_secret);
        return;
    }

    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + kOverallTimeout;

    auto superseded = [gen]() {
        return g_current_gen.load(std::memory_order_acquire) != gen;
    };

    HWND login_hwnd = nullptr;
    while (clk::now() < deadline) {
        if (superseded()) {
            SAM_LOG_INFO("login_driver: superseded by newer run; exiting");
            return;
        }
        if (auto h = find_login_window(pid)) { login_hwnd = h; break; }
        std::this_thread::sleep_for(kWindowPollInterval);
    }
    if (!login_hwnd) {
        SAM_LOG_WARN("login_driver: login window did not appear within {}s",
                     std::chrono::duration_cast<std::chrono::seconds>(kOverallTimeout).count());
        sam::launch::code_clipboard::copy_code_to_clipboard(creds.shared_secret);
        return;
    }
    SAM_LOG_INFO("login_driver: login window acquired hwnd=0x{:x}",
                 reinterpret_cast<std::uintptr_t>(login_hwnd));

    bool entered_creds = false;
    bool entered_code  = false;
    while (clk::now() < deadline) {
        if (superseded()) {
            SAM_LOG_INFO("login_driver: superseded by newer run; exiting");
            return;
        }
        // Gate the registry success check on entered_creds: prevents a stale
        // ActiveUser (e.g. after hard-killing the previous session) from ending
        // the run before we've typed anything.
        if (entered_creds) {
            const auto au = sam::platform::registry::read_active_user();
            if (au && *au != 0 &&
                (creds.expected_account_id == 0 || *au == creds.expected_account_id)) {
                SAM_LOG_INFO("login_driver: login confirmed (ActiveUser={})", *au);
                if (!superseded() && creds.on_login_confirmed) {
                    try {
                        creds.on_login_confirmed([gen] {
                            return g_current_gen.load(std::memory_order_acquire) == gen;
                        });
                    } catch (...) {
                        SAM_LOG_ERROR("login_driver: on_login_confirmed threw");
                    }
                }
                return;
            }
        }

        if (!IsWindow(login_hwnd)) {
            login_hwnd = find_login_window(pid);
            if (!login_hwnd) { std::this_thread::sleep_for(kClassifyInterval); continue; }
        }

        auto win = uia.from_hwnd(login_hwnd);
        if (!win) { std::this_thread::sleep_for(kClassifyInterval); continue; }
        win->focus();

        auto doc = win->first_descendant_by_control_type(UIA_DocumentControlTypeId);
        if (!doc) { std::this_thread::sleep_for(kClassifyInterval); continue; }

        const WindowState s = classify(*doc);
        SAM_LOG_DEBUG("login_driver: state={}", to_str(s));

        switch (s) {
            case WindowState::Login:
                if (!entered_creds) {
                    if (drive_login_state(*doc, creds)) {
                        entered_creds = true;
                        SAM_LOG_INFO("login_driver: credentials submitted");
                    }
                }
                break;
            case WindowState::Code:
                if (!entered_code) {
                    if (drive_code_state(*doc, creds.shared_secret)) {
                        entered_code = true;
                        SAM_LOG_INFO("login_driver: 2FA code submitted");
                    } else if (creds.shared_secret.empty()) {
                        return;   // user has to type the code themselves
                    } else {
                        // Per-digit retries already exhausted in drive_code_state.
                        // Don't re-run: slots are partially filled and we'd type on
                        // top of an unknown state. Bow out and let the user finish.
                        entered_code = true;
                        SAM_LOG_WARN("login_driver: 2FA entry failed after "
                                     "retries; finish in Steam manually");
                    }
                }
                break;
            case WindowState::Selection:
                drive_selection_state(*doc);
                break;
            case WindowState::Error:
                SAM_LOG_WARN("login_driver: Steam reported an error; bailing");
                return;
            case WindowState::Loading:
            case WindowState::None:
            case WindowState::Invalid:
                break;
        }

        std::this_thread::sleep_for(kClassifyInterval);
    }

    SAM_LOG_WARN("login_driver: overall timeout reached");
}

}  // namespace

bool run_async(std::uint32_t steam_pid, Credentials creds) {
    const std::uint64_t gen =
        g_current_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::thread([gen, pid = steam_pid, c = std::move(creds)]() mutable {
        try {
            worker_body(gen, pid, std::move(c));
        } catch (...) {
            SAM_LOG_ERROR("login_driver: worker threw");
        }
    }).detach();
    return true;
}

}  // namespace sam::launch::login_driver
