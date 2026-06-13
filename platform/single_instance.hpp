#pragma once

#include <string>

namespace sam::platform {

class SingleInstance {
public:
    explicit SingleInstance(const std::wstring& mutex_name);
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
