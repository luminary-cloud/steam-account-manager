#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "app/app_paths.hpp"
#include "app/app_state.hpp"
#include "app/conf_poller.hpp"
#include "app/job_pump.hpp"
#include "app/resource.h"
#include "core/account_store/store.hpp"
#include "core/cs2_gc/cs2_gc_client.hpp"
#include "core/http/client.hpp"
#include "core/launch/cs2_autostart.hpp"
#include "core/log.hpp"
#include "core/time_aligner.hpp"
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"
#include "platform/dpapi.hpp"
#include "platform/dpi.hpp"
#include "platform/fs.hpp"
#include "platform/global_hotkey.hpp"
#include "platform/paths.hpp"
#include "platform/single_instance.hpp"
#include "platform/tray_icon.hpp"
#include "platform/window_affinity.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/rank_image.hpp"
#include "ui/widgets/title_bar.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Not in the public SDK. An elevated window must allow this through the UIPI
// filter for normal-integrity Explorer to deliver a drag-drop payload.
#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"sam-main";
constexpr wchar_t kWindowTitle[] = L"Steam Account Manager";
constexpr int kInitialWidth = 1240;
constexpr int kInitialHeight = 820;

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swap_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT                    g_resize_w = 0;
UINT                    g_resize_h = 0;
// Set when Present reports occlusion/minimized; the loop throttles instead of
// spinning at full speed while hidden in the tray.
bool                    g_occluded = false;

// Set after AppState is constructed (WM_DROPFILES routing), cleared before it
// is destroyed.
sam::app::AppState* g_state = nullptr;

// wnd_proc must not forward to the ImGui Win32 handler before this is true; the
// headless --startup run never inits ImGui at all.
bool g_imgui_ready = false;

// Posted to the primary instance when a --startup launch arrives while open, so
// it refreshes in place instead of opening a second window.
constexpr UINT WM_APP_STARTUP_REFRESH = WM_APP + 1;

void create_render_target() {
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void release_render_target() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#endif

    D3D_FEATURE_LEVEL fl_out{};
    const D3D_FEATURE_LEVEL fl_in[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                              flags, fl_in, _countof(fl_in),
                                              D3D11_SDK_VERSION, &sd, &g_swap_chain,
                                              &g_device, &fl_out, &g_context))) {
        return false;
    }
    create_render_target();
    return true;
}

void destroy_device() {
    release_render_target();
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context)    { g_context->Release();    g_context = nullptr; }
    if (g_device)     { g_device->Release();     g_device = nullptr; }
}

// `ext_lower` includes the leading dot and must be lowercase.
bool has_extension(const std::wstring& path, std::wstring_view ext_lower) {
    if (path.size() < ext_lower.size()) return false;
    for (std::size_t i = 0; i < ext_lower.size(); ++i) {
        const wchar_t a = static_cast<wchar_t>(std::towlower(
            static_cast<wint_t>(path[path.size() - ext_lower.size() + i])));
        if (a != ext_lower[i]) return false;
    }
    return true;
}

enum class DropKind { Skip, MaFile, InfoDat };

DropKind classify_path(const std::wstring& path) {
    if (has_extension(path, L".mafile")) return DropKind::MaFile;
    if (has_extension(path, L".dat"))    return DropKind::InfoDat;
    return DropKind::Skip;
}

// Non-recursive: yields each *.maFile / *.dat child.
void scan_directory(const std::wstring& dir_w,
                    std::vector<std::string>& out_mafiles,
                    std::vector<std::string>& out_info_dat) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_w, ec);
    if (ec) return;
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring p = entry.path().wstring();
        switch (classify_path(p)) {
            case DropKind::MaFile:  out_mafiles.push_back(sam::ui::to_utf8(p)); break;
            case DropKind::InfoDat: out_info_dat.push_back(sam::ui::to_utf8(p)); break;
            case DropKind::Skip: break;
        }
    }
}

void handle_dropped_files(HDROP hdrop) {
    if (!g_state) { DragFinish(hdrop); return; }

    std::vector<std::string> mafiles;
    std::vector<std::string> info_dat;

    const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(hdrop, i, path.data(), len + 1);

        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            scan_directory(path, mafiles, info_dat);
            continue;
        }

        switch (classify_path(path)) {
            case DropKind::MaFile:
                mafiles.push_back(sam::ui::to_utf8(path));
                break;
            case DropKind::InfoDat:
                info_dat.push_back(sam::ui::to_utf8(path));
                break;
            case DropKind::Skip: break;
        }
    }
    DragFinish(hdrop);

    if (mafiles.empty() && info_dat.empty()) return;

    {
        std::lock_guard lk(g_state->drop_mutex);
        g_state->pending_mafile_drops.insert(g_state->pending_mafile_drops.end(),
                                              mafiles.begin(), mafiles.end());
        g_state->pending_info_dat_drops.insert(g_state->pending_info_dat_drops.end(),
                                                info_dat.begin(), info_dat.end());
        // Focus the dominant queue's tab; mafile wins on tie.
        if (!mafiles.empty() && mafiles.size() >= info_dat.size()) {
            g_state->pending_mafile_focus = true;
        } else if (!info_dat.empty()) {
            g_state->pending_info_dat_focus = true;
        }
    }
    g_state->selected_account_id.clear();
    g_state->current_screen = sam::app::Screen::AddAccount;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imgui_ready && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
            return 0;

        case WM_NCCALCSIZE:
            // Collapse the standard frame so the client covers the whole window;
            // when maximized, inset by the DPI-aware frame so content isn't clipped.
            if (wp == TRUE) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    const UINT dpi = GetDpiForWindow(hwnd);
                    const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                                   GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                   GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left   += fx;
                    params->rgrc[0].right  -= fx;
                    params->rgrc[0].top    += fy;
                    params->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;

        case WM_NCHITTEST: {
            // Resize borders + caption are non-client so Windows drives
            // drag/snap/maximize; the title-bar button block stays client for ImGui.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr;
            GetWindowRect(hwnd, &wr);
            const bool zoomed = IsZoomed(hwnd);
            const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0F;
            const int border = zoomed ? 0
                : static_cast<int>(sam::ui::widgets::kResizeBorder * scale);

            const bool left   = pt.x <  wr.left  + border;
            const bool right  = pt.x >= wr.right - border;
            const bool top    = pt.y <  wr.top   + border;
            const bool bottom = pt.y >= wr.bottom - border;

            if (!zoomed) {
                if (top && left)     return HTTOPLEFT;
                if (top && right)    return HTTOPRIGHT;
                if (bottom && left)  return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left)   return HTLEFT;
                if (right)  return HTRIGHT;
                if (top)    return HTTOP;
                if (bottom) return HTBOTTOM;
            }

            const int title_h = static_cast<int>(sam::ui::widgets::kTitleBarHeight * scale);
            if (pt.y < wr.top + title_h) {
                const int strip = static_cast<int>(sam::ui::widgets::kCaptionBtnWidth *
                                                   sam::ui::widgets::kCaptionBtnCount * scale);
                if (pt.x >= wr.right - strip) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_GETMINMAXINFO: {
            // A frameless window would otherwise shrink to nothing.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0F;
            mmi->ptMinTrackSize.x = static_cast<LONG>(640 * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(480 * scale);
            return 0;
        }

        case WM_DROPFILES:
            handle_dropped_files(reinterpret_cast<HDROP>(wp));
            return 0;

        case WM_SYSCOMMAND:
            // Block Alt opening the system menu.
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;

        case WM_HOTKEY:
            if (g_state && wp == sam::platform::global_hotkey::kCopyCodeId) {
                const std::string sel = g_state->selected_account_id;
                g_state->post_ui_callback([sel] {
                    if (!g_state) return;
                    auto* a = g_state->find_account(sel);
                    if (!a || !a->sda.has_value()) {
                        sam::ui::widgets::ToastItem t;
                        t.id = "hotkey-nosel";
                        t.message = "No authenticator selected.";
                        t.is_warning = true;
                        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        t.expires_at_unix = now + 4;
                        g_state->toasts.push(std::move(t));
                        return;
                    }
                    if (!sam::time_aligner::synced()) return;
                    const auto now = sam::time_aligner::aligned_now();
                    const auto code = sam::sda::generate_code(a->sda->shared_secret, now);
                    sam::platform::clipboard::set_text_with_auto_clear(
                        code,
                        std::chrono::seconds(g_state->settings.clipboard_clear_seconds));
                });
            }
            return 0;

        case sam::platform::tray_icon::kCallbackMessage:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT ||
                LOWORD(lp) == NIN_BALLOONUSERCLICK) {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }
            return 0;

        case WM_APP_STARTUP_REFRESH:
            if (g_state) g_state->refresh_account_data();
            return 0;

        case WM_DESTROY:
            sam::platform::global_hotkey::unregister_hotkey(
                hwnd, sam::platform::global_hotkey::kCopyCodeId);
            sam::platform::tray_icon::remove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// True once the Web API phase finished and (with GCPD on) the staggered
// per-account pass too.
bool startup_refresh_complete(const sam::app::AppState& state) {
    if (!state.refresh_web_phase_done.load(std::memory_order_relaxed)) return false;
    if (!state.settings.gcpd_enabled) return true;
    const int total = state.refresh_all_total.load(std::memory_order_relaxed);
    if (total == 0) return true;
    return state.refresh_all_done.load(std::memory_order_relaxed) >= total;
}

// Headless logon run (scheduled task, --startup): refresh with the window hidden,
// raise a balloon for any new ban/cooldown, then exit. No D3D or ImGui. Tears
// down `hwnd` itself.
int run_startup_refresh(HINSTANCE inst, HWND hwnd) {
    sam::time_aligner::start();
    sam::app::job_pump::start_workers(4);

    sam::app::AppState state;
    state.load_settings();
    state.notifications.set_path(sam::app::notifications_path());
    state.notifications.load();
    state.main_hwnd = hwnd;
    g_state = &state;
    sam::platform::tray_icon::set_owner(hwnd, IDI_APP_ICON);

    bool refreshing = false;
    if (state.settings.remember_master_password &&
        sam::core::store::vault_exists(sam::app::vault_path())) {
        const auto cache_path = sam::app::master_pw_cache_path();
        std::error_code cache_ec;
        if (std::filesystem::exists(cache_path, cache_ec)) {
            try {
                auto wrapped = sam::platform::read_binary_file(cache_path);
                auto plain = sam::platform::dpapi::unprotect(wrapped);
                auto pw = sam::crypto::make_secure(
                    std::string(plain.begin(), plain.end()));
                if (!plain.empty())
                    sam::crypto::zero_buffer(plain.data(), plain.size());
                state.vault = sam::core::store::load_vault(sam::app::vault_path(), pw);
                state.master_password = pw;
                state.unlocked = true;
                SAM_LOG_INFO("startup: auto-unlock ok, refreshing in background");
                state.refresh_account_data();
                refreshing = true;
            } catch (const std::exception& ex) {
                SAM_LOG_WARN("startup: auto-unlock failed ({}); exiting", ex.what());
            }
        }
    }
    if (!refreshing) {
        SAM_LOG_INFO("startup: no auto-unlock (locked or no cache); exiting");
    }

    if (refreshing) {
        const auto begin = std::chrono::steady_clock::now();
        const int accounts = static_cast<int>(state.vault.accounts.size());
        const auto cap = std::min<std::chrono::seconds>(
            std::chrono::seconds(30) +
                std::chrono::seconds(3) * (accounts > 0 ? accounts : 1),
            std::chrono::minutes(10));

        bool quit = false;
        MSG msg{};
        while (!quit) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) quit = true;
            }
            if (quit) break;
            sam::app::job_pump::drain(state);
            if (startup_refresh_complete(state)) break;
            if (std::chrono::steady_clock::now() - begin > cap) {
                SAM_LOG_WARN("startup: refresh timed out; exiting");
                break;
            }
            Sleep(50);
        }

        // Give a fired balloon a moment on screen before the icon is removed.
        if (state.balloon_shown.load(std::memory_order_relaxed)) {
            Sleep(8000);
        }
    }

    sam::platform::tray_icon::remove();
    state.flush_pending_save();
    state.scrub_vault_secrets();
    g_state = nullptr;
    sam::app::job_pump::stop_workers();
    sam::time_aligner::stop();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, inst);
    SAM_LOG_INFO("startup: done");
    sam::log::shutdown();
    return 0;
}

}  // namespace

_Use_decl_annotations_
int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd_line, int) {
    sam::platform::dpi::enable_per_monitor_v2();

    const bool startup_mode = cmd_line && wcsstr(cmd_line, L"--startup") != nullptr;
    // Only the OpenApp logon task passes --minimized, so manual launches still open
    // normally.
    const bool start_minimized = cmd_line && wcsstr(cmd_line, L"--minimized") != nullptr;

    sam::platform::SingleInstance one(L"luminary-sam-mutex-v1");
    if (!one.is_primary()) {
        // A --startup launch while already open just asks the running instance to
        // refresh; no second window.
        if (startup_mode) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
                PostMessageW(existing, WM_APP_STARTUP_REFRESH, 0, 0);
            }
        } else {
            sam::platform::SingleInstance::raise_existing(kWindowClass);
        }
        return 0;
    }

    sam::app::ensure_data_dirs();
    // Drop a relocated-away old data folder before opening the log in the new one.
    sam::platform::cleanup_relocated_old_dir();
    sam::log::init(sam::app::log_dir());
    SAM_LOG_INFO("starting up");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                kInitialWidth, kInitialHeight,
                                nullptr, nullptr, inst, nullptr);

    if (startup_mode) {
        return run_startup_refresh(inst, hwnd);
    }

    DragAcceptFiles(hwnd, TRUE);

    // App runs elevated, so UIPI blocks drop messages from normal-integrity
    // Explorer; opt the drag-drop messages through so maFile/info.dat drops work.
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, nullptr);

    if (!create_device(hwnd)) {
        SAM_LOG_ERROR("D3D11 create failed");
        DestroyWindow(hwnd);
        UnregisterClassW(kWindowClass, inst);
        return 1;
    }

    // Frame collapsed in WM_NCCALCSIZE; a thin DWM extension keeps the system drop
    // shadow and rounded corners.
    const MARGINS frame_margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &frame_margins);

    // Force a non-client recompute so the collapsed frame takes effect before show.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Cloak then show: DWM plays the open animation off-screen during heavy init;
    // uncloak after the first frame so there's no flash.
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (start_minimized) {
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    sam::ui::theme::apply_dark();
    sam::ui::fonts::load(1.0F);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_imgui_ready = true;

    sam::ui::icons::load(g_device);
    sam::ui::widgets::avatar_cache::init(g_device);
    sam::ui::widgets::rank_image::init(g_device);

    sam::time_aligner::start();
    sam::app::job_pump::start_workers(4);

    sam::app::AppState state;
    state.load_settings();
    // Keep the logon task in lockstep with the current mode: re-register when on
    // (also repairs a moved exe path), remove an orphaned task when off.
    state.sync_logon_task();
    if (state.settings.streamproof) {
        sam::platform::set_capture_excluded(hwnd, true);
    }
    state.start_update_check();
    state.notifications.set_path(sam::app::notifications_path());
    state.notifications.load();
    state.notifications.prune_older_than(state.settings.notifications.retention_days,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    state.conf_audit.set_path(sam::app::conf_audit_path());
    state.conf_audit.load();
    state.conf_audit.prune_older_than(state.settings.confirmations.audit_retention_days,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    state.trade_audit.set_path(sam::app::trade_audit_path());
    state.trade_audit.load();
    state.trade_audit.prune_older_than(90,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (state.settings.sda.global_hotkey_mods == 0 &&
        state.settings.sda.global_hotkey_vk   == 0) {
        state.settings.sda.global_hotkey_mods = MOD_CONTROL | MOD_SHIFT;
        state.settings.sda.global_hotkey_vk   = 'G';
    }
    state.main_hwnd = hwnd;
    g_state = &state;
    sam::app::conf_poller::start(state);

    // Auto-unlock with the cached DPAPI-wrapped password if opted in. On failure
    // fall through to the Unlock screen and wipe the cache so it stops failing.
    if (state.settings.remember_master_password &&
        sam::core::store::vault_exists(sam::app::vault_path())) {
        const auto cache_path = sam::app::master_pw_cache_path();
        std::error_code cache_ec;
        if (std::filesystem::exists(cache_path, cache_ec)) {
            try {
                auto wrapped = sam::platform::read_binary_file(cache_path);
                auto plain = sam::platform::dpapi::unprotect(wrapped);
                auto pw = sam::crypto::make_secure(
                    std::string(plain.begin(), plain.end()));
                if (!plain.empty())
                    sam::crypto::zero_buffer(plain.data(), plain.size());
                state.vault = sam::core::store::load_vault(
                    sam::app::vault_path(), pw);
                state.master_password = pw;
                state.unlocked = true;
                state.last_interaction = std::chrono::steady_clock::now();
                state.current_screen = sam::app::Screen::Accounts;
                if (state.settings.refresh_on_launch) state.refresh_account_data();
                // One-time fill of external funds for accounts missing a figure.
                if (state.settings.info.show_external_funds) {
                    state.refresh_all_spend(/*only_missing=*/true);
                }
                SAM_LOG_INFO("auto-unlock: vault opened via DPAPI cache");
            } catch (const std::exception& ex) {
                SAM_LOG_WARN("auto-unlock failed ({}); removing stale cache", ex.what());
                std::error_code rm_ec;
                std::filesystem::remove(cache_path, rm_ec);
            }
        }
    }

    MSG msg{};
    bool quit = false;
    bool window_shown = false;
    while (!quit) {
        if (state.needs_hotkey_reregister) {
            sam::platform::global_hotkey::unregister_hotkey(
                hwnd, sam::platform::global_hotkey::kCopyCodeId);
            if (state.settings.sda.global_hotkey_enabled) {
                sam::platform::global_hotkey::register_hotkey(
                    hwnd, sam::platform::global_hotkey::kCopyCodeId,
                    state.settings.sda.global_hotkey_mods,
                    state.settings.sda.global_hotkey_vk);
            }
            state.needs_hotkey_reregister = false;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;

        // Minimized/occluded: Present stops blocking on vsync, so cap the loop
        // instead of burning a core. Wakes on any message, still drains jobs ~10x/sec.
        if (g_occluded || IsIconic(hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }

        if (g_resize_w != 0 && g_resize_h != 0) {
            release_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        {
            const bool had_input =
                io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                ImGui::IsAnyMouseDown() || io.InputQueueCharacters.Size > 0;
            if (had_input) {
                state.last_interaction = std::chrono::steady_clock::now();
            }
        }

        if (state.unlocked &&
            state.settings.auto_lock_minutes > 0 &&
            !state.settings.remember_master_password) {
            const auto idle = std::chrono::steady_clock::now() - state.last_interaction;
            if (idle >= std::chrono::minutes(state.settings.auto_lock_minutes)) {
                SAM_LOG_INFO("auto-lock: idle for {} min, locking vault",
                             state.settings.auto_lock_minutes);
                state.lock_vault();
            }
        }

        sam::app::job_pump::drain(state);

        // Advance the CS2 GC auto-pull sweep, and kick it once on startup if opted in
        // (after unlock so vault credentials are available; it self-skips fresh caches).
        state.tick_gc_autopull();
        if (state.unlocked && state.settings.cs2_gc.auto_pull_on_startup &&
            !state.gc_startup_pull_done) {
            state.gc_startup_pull_done = true;
            state.start_gc_autopull();
        }
        {
            static unsigned long long cs2_toast_seq = 0;
            auto msgs = sam::launch::cs2_autostart::take_status_messages();
            if (!msgs.empty()) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                for (auto& m : msgs) {
                    sam::ui::widgets::ToastItem t;
                    t.id = "cs2-autostart-" + std::to_string(cs2_toast_seq++);
                    t.message = std::move(m.text);
                    t.is_warning = m.warning;
                    t.expires_at_unix = now + state.settings.notifications.toast_duration_seconds;
                    state.toasts.push(std::move(t));
                }
            }
        }
        sam::ui::draw(state);

        ImGui::Render();
        const float bg[4] = {0.035F, 0.035F, 0.035F, 1.0F};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT pr = g_swap_chain->Present(1, 0);
        g_occluded = (pr == DXGI_STATUS_OCCLUDED);

        // Uncloak after the first frame so the window appears already painted.
        if (!window_shown) {
            BOOL uncloak = FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
            window_shown = true;
        }
    }

    // Abort in-flight HTTP so the threads joined below return immediately instead
    // of blocking on a WinHTTP timeout.
    sam::http::cancel_all();

    // Join any retired CS2 GC clients while AppState is still alive, so their worker
    // callbacks never touch a destroyed state.
    sam::cs2_gc::reap_all();

    // Flush the debounced vault save before tearing down the worker so the last
    // edit isn't lost.
    state.flush_pending_save();
    state.scrub_vault_secrets();
    g_state = nullptr;

    SAM_LOG_INFO("shutting down");
    sam::app::conf_poller::stop();
    sam::app::job_pump::stop_workers();
    sam::time_aligner::stop();

    sam::ui::widgets::rank_image::shutdown();
    sam::ui::widgets::avatar_cache::shutdown();
    sam::ui::icons::shutdown();
    g_imgui_ready = false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, inst);

    sam::log::shutdown();
    return 0;
}
