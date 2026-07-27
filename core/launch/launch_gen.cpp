#include "core/launch/launch_gen.hpp"

#include <atomic>

namespace sam::launch::launch_gen {

namespace {
std::atomic<std::uint64_t> g_current{0};
}  // namespace

std::uint64_t begin() {
    return g_current.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool is_current(std::uint64_t gen) {
    return g_current.load(std::memory_order_acquire) == gen;
}

}  // namespace sam::launch::launch_gen
