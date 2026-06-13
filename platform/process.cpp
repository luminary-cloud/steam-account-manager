#include "platform/process.hpp"

#include <cwctype>

#include <windows.h>
#include <tlhelp32.h>

namespace sam::platform::process {

namespace {

bool ieq(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

}  // namespace

std::vector<std::uint32_t> find_by_image_name(std::wstring_view name) {
    std::vector<std::uint32_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (ieq(e.szExeFile, name)) {
                out.push_back(e.th32ProcessID);
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

bool terminate(std::uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const BOOL ok = TerminateProcess(h, 0);
    if (ok) WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return ok != 0;
}

std::optional<std::uint32_t> launch(const std::filesystem::path& exe,
                                     const std::wstring& args,
                                     const std::optional<std::filesystem::path>& working_dir) {
    if (exe.empty() || !std::filesystem::exists(exe)) return std::nullopt;

    std::wstring cmdline = L"\"" + exe.wstring() + L"\"";
    if (!args.empty()) {
        cmdline += L" ";
        cmdline += args;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring wd = working_dir ? working_dir->wstring() : exe.parent_path().wstring();

    if (!CreateProcessW(nullptr,
                        cmdline.data(),
                        nullptr, nullptr,
                        FALSE,
                        CREATE_UNICODE_ENVIRONMENT,
                        nullptr,
                        wd.empty() ? nullptr : wd.c_str(),
                        &si, &pi)) {
        return std::nullopt;
    }
    const std::uint32_t pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

}  // namespace sam::platform::process
