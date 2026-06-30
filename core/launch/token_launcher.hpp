#pragma once

#include <cstdint>
#include <string_view>

#include "core/account_store/account.hpp"
#include "core/launch/steam_launcher.hpp"   // LaunchResult / LaunchStatus

namespace sam::launch {

// Signs the Steam client into an NFA account using only its JWT refresh token:
// shuts down Steam, sets AutoLoginUser, writes a remembered loginusers.vdf entry
// and the token into local.vdf's ConnectCache, then relaunches steam.exe. Validates
// the token (present, unexpired, "client" audience) and account fields first.
//
// `cs2_launch_options`, when non-empty, is written to appid 730's LaunchOptions
// while Steam is shut down (before the relaunch), so Steam can't overwrite it.
//
// `disable_cloud_on_login` / `disable_news_on_login`, when set, force Steam Cloud and
// the new-release news notification off for the account in the same Steam-down window.
LaunchResult launch_account_with_token(const core::Account& account,
                                       std::string_view cs2_launch_options = {},
                                       bool disable_cloud_on_login = false,
                                       bool disable_news_on_login = false,
                                       std::uint32_t hwid_component_mask = 0x3FFu);

}  // namespace sam::launch
