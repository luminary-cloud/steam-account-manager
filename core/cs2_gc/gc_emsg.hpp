#pragma once

#include <cstdint>

namespace sam::cs2_gc {

inline constexpr std::uint32_t kCs2AppId = 730;
inline constexpr std::uint32_t kGcProtoMask = 0x80000000u;

// CMsgClientHello.version that the live CS2 client reports.
inline constexpr std::uint32_t kGcHelloVersion = 2000244;

// GC message ids drawn from EGCBaseClientMsg, ESOMsg, EGCItemMsg and ECsgoGCMsg.
enum GcMsg : std::uint32_t {
    ClientWelcome                 = 4004,
    ClientHello                   = 4006,
    ConnectionStatus              = 4009,

    SOCreate                      = 21,
    SOUpdate                      = 22,
    SODestroy                     = 23,
    SOCacheSubscribed             = 24,
    SOUpdateMultiple              = 26,

    UpdateItemSchema              = 1049,
    ItemCustomizationNotification = 1090,
    CasketItemAdd                 = 1092,
    CasketItemExtract             = 1093,
    CasketItemLoadContents        = 1094,

    RequestPlayersProfile         = 9127,
    PlayersProfile                = 9128,
    ClientRedeemFreeReward        = 9219,
    RequestRecurringMissionSchedule = 9225,
    RecurringMissionSchema        = 9226,
};

// EGCItemCustomizationNotification.request subtypes we act on.
enum NotifyRequest : std::uint32_t {
    NotifyCasketTooFull    = 1011,
    NotifyCasketContents   = 1012,
    NotifyCasketAdded      = 1013,
    NotifyCasketRemoved    = 1014,
    NotifyCasketInvFull    = 1015,
    NotifyRedeemFreeReward = 9219,
};

// SO cache type ids (ESOType).
inline constexpr int kSoTypeEconItem = 1;
inline constexpr int kSoTypePersonalStore = 4;

}  // namespace sam::cs2_gc
