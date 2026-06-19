#pragma once

#include <cstdint>

namespace sam::steam_cm {

// High bit of the EMsg dword marks a protobuf-backed message.
inline constexpr std::uint32_t kProtoMask = 0x80000000u;

// The subset of Steam EMsg values the CM client uses (from SteamKit's EMsg enum).
enum EMsg : std::uint32_t {
    Multi                        = 1,
    ClientHeartBeat              = 703,
    ClientLogOff                 = 706,
    ClientGamesPlayed            = 742,
    ClientLogOnResponse          = 751,
    ClientLoggedOff              = 757,
    ClientCMList                 = 783,
    ClientToGC                   = 5452,
    ClientFromGC                 = 5453,
    ClientGamesPlayedWithDataBlob = 5410,
    ClientLogon                  = 5514,
    ClientPlayingSessionState    = 9600,
    ClientKickPlayingSession     = 9601,
};

}  // namespace sam::steam_cm
