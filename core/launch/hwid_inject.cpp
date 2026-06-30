#include "core/launch/hwid_inject.hpp"

#include <thread>

#include "core/hwid/hwid_ipc.hpp"
#include "core/hwid/injector.hpp"
#include "core/log.hpp"

namespace sam::launch {

namespace {
core::hwid::HwidSharedMemory g_shared_mem;
}

InjectResult maybe_inject_hwid(const core::Account& account, std::uint32_t steam_pid,
                                std::uint32_t component_mask) {
    if (!account.hwid.has_value())
        return {InjectOutcome::Skipped, {}};

    if (!g_shared_mem.publish(*account.hwid, component_mask)) {
        std::string err = "failed to create shared memory";
        SAM_LOG_WARN("hwid: {} for pid={}", err, steam_pid);
        return {InjectOutcome::Failed, std::move(err)};
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto result = core::hwid::inject_hwid_dll(steam_pid, g_shared_mem.shared_name());
    if (result.ok) {
        SAM_LOG_INFO("hwid: injected spoofer into steam.exe pid={} as {}",
                     steam_pid, account.login);
        return {InjectOutcome::Success, {}};
    }

    SAM_LOG_WARN("hwid: injection failed for pid={}: {}", steam_pid, result.error);
    return {InjectOutcome::Failed, result.error};
}

}  // namespace sam::launch
