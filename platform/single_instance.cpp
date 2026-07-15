#include "platform/single_instance.hpp"

#include <windows.h>

namespace sam::platform {

SingleInstance::SingleInstance(const std::wstring& mutex_name, bool wait_for_release) {
    handle_ = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    primary_ = handle_ && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!primary_ && handle_ && wait_for_release) {
        // The outgoing instance releases the mutex in its destructor as it exits;
        // wait for that, then we own it. WAIT_ABANDONED covers a hard exit.
        const DWORD r = WaitForSingleObject(static_cast<HANDLE>(handle_), 10000);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) primary_ = true;
    }
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
