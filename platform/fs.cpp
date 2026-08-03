#include "platform/fs.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "core/log.hpp"

#pragma comment(lib, "advapi32.lib")

namespace sam::platform {

namespace {

std::wstring tmp_path_for(const std::filesystem::path& path) {
    auto tmp = path;
    tmp += L".tmp";
    return tmp.wstring();
}

bool delete_via_share_handle(const std::filesystem::path& p) {
    HANDLE h = CreateFileW(p.c_str(), DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
}

}  // namespace

void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data,
                       bool restrict_acl) {
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

    if (restrict_acl) restrict_to_current_user(path);
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

bool set_file_read_only(const std::filesystem::path& path, bool read_only) {
    const std::wstring p = path.wstring();
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    const DWORD updated = read_only ? (attrs | FILE_ATTRIBUTE_READONLY)
                                    : (attrs & ~FILE_ATTRIBUTE_READONLY);
    if (updated == attrs) return true;
    return SetFileAttributesW(p.c_str(), updated) != 0;
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

std::uintmax_t size_recursive(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    if (fs::is_regular_file(p, ec)) {
        const auto sz = fs::file_size(p, ec);
        return ec ? 0 : sz;
    }
    std::uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(
             p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code inner;
        if (it->is_regular_file(inner)) {
            const auto sz = it->file_size(inner);
            if (!inner) total += sz;
        }
    }
    return total;
}

std::uintmax_t file_count_recursive(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    if (fs::is_regular_file(p, ec)) return 1;
    std::uintmax_t count = 0;
    for (auto it = fs::recursive_directory_iterator(
             p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code inner;
        if (it->is_regular_file(inner)) ++count;
    }
    return count;
}

bool delete_file_forced(const std::filesystem::path& p) {
    std::error_code probe;
    if (!std::filesystem::exists(p, probe)) return true;

    const std::wstring w = p.wstring();
    constexpr DWORD kBlocking =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    const DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & kBlocking) != 0) {
        SetFileAttributesW(w.c_str(), attrs & ~kBlocking);
    }
    if (DeleteFileW(w.c_str())) return true;

    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
    if ((err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ||
         err == ERROR_ACCESS_DENIED) &&
        delete_via_share_handle(p)) {
        return true;
    }
    SAM_LOG_WARN("fs: DeleteFileW({}) failed: {}", p.string(), err);
    return false;
}

bool delete_directory_recursive(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    if (!fs::is_directory(p, ec)) return delete_file_forced(p);

    bool all_ok = true;
    for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code inner;

        if (it->is_directory(inner) && !it->is_symlink(inner)) {
            if (!delete_directory_recursive(it->path())) all_ok = false;
        } else if (!delete_file_forced(it->path())) {
            all_ok = false;
        }
    }
    if (!RemoveDirectoryW(p.wstring().c_str())) {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            SAM_LOG_WARN("fs: RemoveDirectoryW({}) failed: {}", p.string(), err);
            all_ok = false;
        }
    }
    return all_ok;
}

}  // namespace sam::platform
