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

// Background-thread vault writer. Snapshotting + encryption + atomic write all
// happen off the UI thread; rapid mutations are coalesced into a single save
// via latest-wins debouncing.
class VaultSaver {
public:
    VaultSaver() = default;
    ~VaultSaver();

    VaultSaver(const VaultSaver&) = delete;
    VaultSaver& operator=(const VaultSaver&) = delete;

    // Called once before any schedule(). Idempotent.
    void start(std::filesystem::path path);

    // Snapshot vault + password and schedule an async save. The save fires
    // ~debounce_ms after the most recent schedule() call, so rapid bursts
    // collapse into one write.
    void schedule(const core::Vault& vault,
                  const crypto::SecureString& password);

    // Block until any pending or in-flight save has completed. Safe to call
    // from the UI thread.
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
