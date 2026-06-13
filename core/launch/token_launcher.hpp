#pragma once

#include "core/account_store/account.hpp"
#include "core/launch/steam_launcher.hpp"   // LaunchResult / LaunchStatus

namespace sam::launch {

// Signs the Steam client into an NFA account using only its JWT refresh token:
// shuts down Steam, sets AutoLoginUser, writes a remembered loginusers.vdf entry
// and the token into local.vdf's ConnectCache, then relaunches steam.exe. Validates
// the token (present, unexpired, "client" audience) and account fields first.
LaunchResult launch_account_with_token(const core::Account& account);

}  // namespace sam::launch
