#pragma once

#include <string>

namespace sam::platform {

class SingleInstance {
public:
    // Returns true if we are the first instance.
    explicit SingleInstance(const std::wstring& mutex_name);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool is_primary() const { return primary_; }

    // Tries to raise an existing instance's main window: restores it if
    // minimized and brings it to the foreground. Returns true if a window
    // matching `window_class` was found.
    static bool raise_existing(const std::wstring& window_class);

private:
    void* handle_ = nullptr;  // HANDLE
    bool primary_ = false;
};

}  // namespace sam::platform
