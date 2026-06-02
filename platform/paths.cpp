#include "platform/paths.hpp"

#include <array>

#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

namespace sam::platform {

namespace {

std::filesystem::path module_directory() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) return {};
    std::filesystem::path p(buf.data(), buf.data() + len);
    return p.parent_path();
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    return p;
}

}  // namespace

std::filesystem::path exe_dir() {
    return module_directory();
}

std::filesystem::path local_appdata_dir() {
    auto base = known_folder(FOLDERID_LocalAppData);
    if (base.empty()) {
        return module_directory() / "data";
    }
    return base / L"steam-account-manager";
}

std::filesystem::path local_appdata_root() {
    return known_folder(FOLDERID_LocalAppData);
}

std::filesystem::path data_dir() {
    auto exe = module_directory();
    if (!exe.empty() && std::filesystem::exists(exe / L"portable.flag")) {
        return exe / L"data";
    }
    auto d = local_appdata_dir();
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

std::filesystem::path vault_path()    { return data_dir() / L"vault.bin"; }
std::filesystem::path settings_path() { return data_dir() / L"settings.json"; }
std::filesystem::path log_dir()       { return data_dir() / L"logs"; }

}  // namespace sam::platform
