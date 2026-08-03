#include "app/vault_saver.hpp"

#include <chrono>
#include <optional>

#include "core/account_store/store.hpp"
#include "core/log.hpp"

namespace sam::app {

namespace {

constexpr std::chrono::milliseconds kSaveDebounce{250};
}  // namespace

VaultSaver::~VaultSaver() {
    {
        std::lock_guard lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VaultSaver::start(std::filesystem::path path) {
    std::lock_guard lk(mtx_);
    if (started_) return;
    path_ = std::move(path);
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void VaultSaver::schedule(const core::Vault& vault,
                           const crypto::SecureString& password) {
    {
        std::lock_guard lk(mtx_);
        pending_ = Pending{vault, password,
                            std::chrono::steady_clock::now() + kSaveDebounce};
    }
    cv_.notify_all();
}

void VaultSaver::flush() {
    std::unique_lock lk(mtx_);
    if (!started_) return;
    if (pending_.has_value()) {

        pending_->deadline = std::chrono::steady_clock::now();
        cv_.notify_all();
    }
    cv_.wait(lk, [&] { return !pending_.has_value() && !busy_; });
}

void VaultSaver::run() {
    while (true) {
        std::optional<Pending> work;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] { return stop_ || pending_.has_value(); });

            while (!stop_ && pending_.has_value() &&
                   pending_->deadline > std::chrono::steady_clock::now()) {
                cv_.wait_until(lk, pending_->deadline);
            }

            if (pending_.has_value()) {
                work = std::move(pending_);
                pending_.reset();
                busy_ = true;
            } else if (stop_) {
                break;
            }
        }

        if (work) {
            try {
                core::store::save_vault(path_, work->vault, work->password);
            } catch (const std::exception& ex) {
                SAM_LOG_ERROR("vault save (async) failed: {}", ex.what());
            }
            std::lock_guard lk(mtx_);
            busy_ = false;
            cv_.notify_all();
        }
    }

    std::optional<Pending> final_work;
    {
        std::lock_guard lk(mtx_);
        if (pending_.has_value()) {
            final_work = std::move(pending_);
            pending_.reset();
        }
    }
    if (final_work) {
        try {
            core::store::save_vault(path_, final_work->vault, final_work->password);
        } catch (const std::exception& ex) {
            SAM_LOG_ERROR("vault save (shutdown drain) failed: {}", ex.what());
        }
    }
}

}  // namespace sam::app
