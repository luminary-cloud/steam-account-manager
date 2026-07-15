#pragma once

#define SAM_VERSION_MAJOR 1
#define SAM_VERSION_MINOR 8
#define SAM_VERSION_PATCH 7

#ifdef __cplusplus

#include <string_view>

namespace sam {

inline constexpr int kVersionMajor = SAM_VERSION_MAJOR;
inline constexpr int kVersionMinor = SAM_VERSION_MINOR;
inline constexpr int kVersionPatch = SAM_VERSION_PATCH;

inline constexpr std::string_view kVersion = "1.8.7";
inline constexpr std::string_view kAppName = "Steam Account Manager";
inline constexpr std::string_view kRepoUrl = "https://github.com/luminary-cloud/steam-account-manager";

}  // namespace sam

#endif
