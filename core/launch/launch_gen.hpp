#pragma once

#include <cstdint>

namespace sam::launch::launch_gen {

// One counter shared by every launch, whatever the sign-in method. Detached workers
// spawned by a launch (the login driver, the first-login reapply) capture the generation
// their launch got and check is_current() on each poll, so a newer launch of any account
// supersedes them instead of two workers fighting over Steam.
//
// Bumped once per launch: login_driver::run_async for the password path,
// launch_account_with_token for the token path.
std::uint64_t begin();

// False once a newer launch has begun.
bool is_current(std::uint64_t gen);

}  // namespace sam::launch::launch_gen
