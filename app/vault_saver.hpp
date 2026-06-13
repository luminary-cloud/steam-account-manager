#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::app {

// Background-thread vault writer: snapshot + encryption + atomic write happen off
// the UI thread; rapid mutations are coalesced via latest-wins debouncing.
class VaultSaver {
public:
    VaultSaver() = default;
    ~VaultSaver();

    VaultSaver(const VaultSaver&) = delete;
    VaultSaver& operator=(const VaultSaver&) = delete;

    // Call once before any schedule(). Idempotent.
    void start(std::filesystem::path path);

    // Snapshots vault + password and schedules a save that fires ~debounce after
    // the most recent schedule(), so bursts collapse into one write.
    void schedule(const core::Vault& vault,
                  const crypto::SecureString& password);

    // Blocks until any pending or in-flight save completes. UI-thread safe.
    void flush();

private:
    struct Pending {
        core::Vault vault;
        crypto::SecureString password;
        std::chrono::steady_clock::time_point deadline;
    };

    void run();

    std::filesystem::path path_;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::optional<Pending> pending_;
    bool busy_ = false;
    bool stop_ = false;
    bool started_ = false;
};

}  // namespace sam::app
