#include "platform/fs.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace sam::platform {

namespace {

std::wstring tmp_path_for(const std::filesystem::path& path) {
    auto tmp = path;
    tmp += L".tmp";
    return tmp.wstring();
}

}  // namespace

void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data) {
    const std::wstring tmp_w = tmp_path_for(path);

    HANDLE h = CreateFileW(tmp_w.c_str(),
                            GENERIC_WRITE,
                            0,
                            nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("atomic_write_file: CreateFileW failed");
    }

    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - written_total, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + written_total, chunk, &written, nullptr)) {
            const DWORD err = GetLastError();
            CloseHandle(h);
            DeleteFileW(tmp_w.c_str());
            throw std::runtime_error("atomic_write_file: WriteFile failed (" +
                                     std::to_string(err) + ")");
        }
        written_total += written;
    }

    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        DeleteFileW(tmp_w.c_str());
        throw std::runtime_error("atomic_write_file: FlushFileBuffers failed");
    }
    CloseHandle(h);

    if (!MoveFileExW(tmp_w.c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        DeleteFileW(tmp_w.c_str());
        throw std::runtime_error("atomic_write_file: MoveFileExW failed (" +
                                 std::to_string(err) + ")");
    }

    restrict_to_current_user(path);
}

bool restrict_to_current_user(const std::filesystem::path& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return false;
    }

    std::vector<std::uint8_t> buf(needed);
    if (!GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    PSID user_sid = tu->User.Sid;

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(user_sid);

    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS || !acl) {
        return false;
    }

    const DWORD rc = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.wstring().c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);

    LocalFree(acl);
    return rc == ERROR_SUCCESS;
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("read_binary_file: open failed");
    }
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size < 0) {
        throw std::runtime_error("read_binary_file: tellg failed");
    }
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(buf.data()), size);
    }
    return buf;
}

}  // namespace sam::platform
