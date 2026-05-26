#include "platform/single_instance.hpp"

#include <windows.h>

namespace sam::platform {

SingleInstance::SingleInstance(const std::wstring& mutex_name) {
    handle_ = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    primary_ = handle_ && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (handle_) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

bool SingleInstance::raise_existing(const std::wstring& window_class) {
    HWND hwnd = FindWindowW(window_class.c_str(), nullptr);
    if (!hwnd) return false;

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    return true;
}

}  // namespace sam::platform
