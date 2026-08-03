#pragma once

#include "core/account_store/account.hpp"
#include "core/launch/steam_launcher.hpp"   // LaunchOptions / LaunchResult / LaunchStatus

namespace sam::launch {

// Signs Steam in with only a JWT refresh token: shuts Steam down, sets AutoLoginUser, writes
// a remembered loginusers.vdf entry and the token into local.vdf's ConnectCache, then
// relaunches. The token must be present, unexpired and "client" audience. Always injects
// `account.refresh_token`, so a full-access account is passed a copy carrying its
// cm_refresh_token there.
//
// `opts` settings are written during the Steam-down window. Steam initializes a first login
// from scratch and wipes those pre-writes, so they are deferred to a first_login_reapply
// worker: it waits for the sign-in, then restarts Steam with the settings applied. That sets
// `first_login_deferred`, telling the caller to leave CS2 autostart to the worker.
LaunchResult launch_account_with_token(const core::Account& account,
                                       const LaunchOptions& opts);

}  // namespace sam::launch
