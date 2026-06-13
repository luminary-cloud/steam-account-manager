#include "platform/dpapi.hpp"

#include <cstring>

#include <windows.h>
#include <dpapi.h>

#pragma comment(lib, "crypt32.lib")

namespace sam::platform::dpapi {

namespace {

struct LocalFreeGuard {
    void* p = nullptr;
    ~LocalFreeGuard() { if (p) ::LocalFree(p); }
};

}  // namespace

std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext) {
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plaintext.size());
    in.pbData = const_cast<BYTE*>(plaintext.data());

    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"steam-account-manager-master-password",
                            nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        throw DpapiError("CryptProtectData failed");
    }
    LocalFreeGuard guard{out.pbData};

    return std::vector<std::uint8_t>(out.pbData, out.pbData + out.cbData);
}

std::vector<std::uint8_t> unprotect(std::span<const std::uint8_t> wrapped) {
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(wrapped.size());
    in.pbData = const_cast<BYTE*>(wrapped.data());

    DATA_BLOB out{};
    LPWSTR description = nullptr;
    if (!::CryptUnprotectData(&in, &description,
                              nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        throw DpapiError("CryptUnprotectData failed");
    }
    LocalFreeGuard guard{out.pbData};
    LocalFreeGuard desc_guard{description};

    return std::vector<std::uint8_t>(out.pbData, out.pbData + out.cbData);
}

std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext,
                                  std::span<const std::uint8_t> entropy) {
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plaintext.size());
    in.pbData = const_cast<BYTE*>(plaintext.data());

    DATA_BLOB ent{};
    DATA_BLOB* pent = nullptr;
    if (!entropy.empty()) {
        ent.cbData = static_cast<DWORD>(entropy.size());
        ent.pbData = const_cast<BYTE*>(entropy.data());
        pent = &ent;
    }

    DATA_BLOB out{};
    // No description: CryptUnprotectData ignores it on read, so Steam can decrypt.
    if (!::CryptProtectData(&in, nullptr, pent, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        throw DpapiError("CryptProtectData (entropy) failed");
    }
    LocalFreeGuard guard{out.pbData};
    return std::vector<std::uint8_t>(out.pbData, out.pbData + out.cbData);
}

}  // namespace sam::platform::dpapi
