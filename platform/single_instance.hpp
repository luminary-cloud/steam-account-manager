#pragma once

#include <string>

namespace sam::platform {

class SingleInstance {
public:
    // wait_for_release: if another instance holds the mutex, wait briefly for it to
    // exit and then take ownership. Used by a "switch vault" relaunch, which races
    // the outgoing instance's still-held mutex.
    explicit SingleInstance(const std::wstring& mutex_name,
                            bool wait_for_release = false);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool is_primary() const { return primary_; }

    // Restores and foregrounds an existing instance's window. True if found.
    static bool raise_existing(const std::wstring& window_class);

private:
    void* handle_ = nullptr;  // HANDLE
    bool primary_ = false;
};

}  // namespace sam::platform
