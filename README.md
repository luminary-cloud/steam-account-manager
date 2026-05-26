# steam-account-manager

Native Windows account manager for Steam. Encrypted vault, Steam Guard codes, mobile confirmations, one-click login, no installer, no telemetry.

## Download

Grab the latest `steam-account-manager.exe` from the [Releases](../../releases) page. The binary is statically linked, so it runs on any 64-bit Windows 10 or 11 install with no Visual C++ Redistributable required.

On first launch Windows SmartScreen may show "Windows protected your PC" because the binary is unsigned. Click **More info** then **Run anyway**. Code-signing certificates are not free; this is normal for small open-source Windows tools.

## What it does

Five things, picked from the left-nav.

**Accounts.** Every account in the vault, in one of two layouts (toggle in Settings → Appearance): a responsive card grid, or a list view with user-created groups in a left rail and the selected account's details and actions in a right panel. Groups have a name plus a colour, accounts get assigned by dragging them onto a group header, and an "Ungrouped" bucket catches the rest. Add an account manually, by `.maFile` import (the Steam Guard mobile authenticator backup format), by `info.dat` import (a legacy XML-in-Rijndael-256 export), or by walking the full mobile login flow (RSA-encrypted password, BeginAuthSession, polling). Per-account password, freeform notes, tags with custom colours, trust labels (green / yellow / red), and trade-hold timers. Search, filter, multi-select. Privacy mode hides every login as `<hidden>` until you click to reveal one. Drag-and-drop a `.maFile`, an `info.dat`, or a directory of either onto the window and the add-account screen pops open with the queue pre-filled.

**Authenticator.** Generate Steam Guard codes for any imported authenticator. Add Steam Guard from inside the app via the mobile auth flow (phone status check, AddAuthenticator, SMS finalize). Remove Steam Guard using the saved revocation code; scheme 1 reverts to email-based Steam Guard, scheme 2 strips Steam Guard entirely (15-day market hold either way). Optional configurable global hotkey copies the current selected account's code to the clipboard without focusing the app, with auto-clear; a preview of the next code is shown in the panel so you don't lose a half-typed code as the window rolls over.

**Confirmations.** List, approve, or deny pending mobile confirmations: trade offers, market listings, phone-number changes, gift redemptions, the lot. Per-account, with bulk approve / deny across selected accounts. Optional background poller refreshes every account on a configurable interval (default off; 10 minutes when on) and pops a toast when new items land. Auto-approve rules can clear market listings, phone-number changes, and trades to a per-SteamID64 trusted-partner list without prompting; everything else still waits for you. Every approve / deny is written to a local audit log with configurable retention so you can see what was decided when.

**Account review.** VAC / game / community / trade ban indicators. Steam level, persona, avatar, profile country, owned games count. CS2 Premier rating, Wingman rank, account level, Prime status, current cooldown, VAC-Live indicator (the CS2 fields come from the authenticated `/gcpd/730` page on steamcommunity.com). The ban / level / owned-games fields use the public Steam Web API and need a key pasted into Settings (free, get one at `steamcommunity.com/dev/apikey`); the key is stored encrypted with the vault. Every indicator is individually toggleable in Settings. New bans, cooldown changes, and VAC-Live flips are detected between refreshes and surfaced as a badge on the account card and a toast in the corner; the events are stored in a local notification log with configurable retention so you can scroll back through what changed and when. Per-event-type toggles, coalescing threshold, and toast duration all live in Settings.

**Launch.** One-click launch into any saved account through the standard registry flow (`HKCU\Software\Valve\Steam\AutoLoginUser` + `RememberPassword`, restart `steam.exe`, flip the matching `loginusers.vdf` entry so Steam actually picks it). Best-effort auto-typing of the Steam Guard code into the freshly-opened Steam login popup via Windows UI Automation; falls back to placing the code on the clipboard with auto-clear after a configurable timeout.

**Export / import.** Encrypted bundle round-trip: a passphrase-protected `.sambundle` you can carry between machines (passphrase distinct from your master password). Import shows a merge preview first - which logins would be added, which would be skipped because the same account is already in the vault - and only writes after you confirm. Plain combo export (`login:password` text) is gated behind a typed confirmation phrase, in case you really do need to dump credentials in cleartext.

## What it doesn't do

- No telemetry. The only network calls this app makes are the ones you trigger: account refresh, login, confirmations, GCPD scrape.
- No password recovery, no escrow, no support email. Lose the master password, lose the vault. There is no backdoor.
- No multi-platform. Windows x64 only.
- No code signing. SmartScreen will warn on first run.
- No bundled cheats, scripts, or game modifications. This is an account manager; that is the whole scope.

## First run

1. Launch `steam-account-manager.exe`. The window opens on the Unlock screen.
2. Create a master password. A strength meter is shown; pick something memorable, because there is no recovery path. The vault gets sealed with AES-256-GCM under a key derived from this password via PBKDF2-HMAC-SHA256 (600 000 iterations).
3. Add an account: manual, `.maFile`, `info.dat`, or full mobile login. For the `.maFile` route, drop the file onto the window or pick it from the file dialog. For the full login route, you type the username and password into the wizard and the app handles RSA encryption, Steam Guard prompts, and session minting.
4. Optional: in Settings, enable *Skip master-password prompt on launch (DPAPI)* if you want the vault to open automatically next time. The master password is wrapped under the current Windows user via the Data Protection API; anyone signed in as you on this machine can open the vault without typing the password.
5. Optional: tweak auto-lock minutes, clipboard auto-clear seconds, the per-indicator toggles, and whether the GCPD scraper runs (it requires a valid `steamLoginSecure` cookie minted by the Full Login wizard).

## Security model

The vault file (`vault.bin`) holds every account's login, password, refresh token, Steam Guard secrets, and cached profile info. It is encrypted with AES-256-GCM under a key derived from your master password via PBKDF2-HMAC-SHA256 with 600 000 iterations. Every write is atomic (rename-over-tempfile) and goes through a background-thread saver that coalesces rapid mutations into a single write so a stream of trust-label clicks or tag toggles never blocks the UI.

The vault defends against:

- Casual snooping by anyone sharing your machine.
- Theft of the vault file off your disk.

The vault does **not** defend against:

- Malware running as the same Windows user. If a hostile process can read your memory or your typed master password, no encrypted file format helps you.
- Memory dumps captured while the vault is unlocked.
- Keyloggers.

The DPAPI auto-unlock cache is opt-in and reduces the protection: anyone signed in as you on this machine can open the vault without typing the master password. Disable the option in Settings to delete the cached blob.

Losing your master password means losing the vault. There is no recovery code, no escrow, no support email.

## Build from source

The project is self-contained. Vendored dependencies under `third_party/` are fetched by a script, so there is no vcpkg, Conan, or git submodule setup.

Requirements:

- Visual Studio 2022 (Community is fine) with the **Desktop development with C++** workload and the Windows 10 SDK.

Steps:

```powershell
git clone https://github.com/luminary-cloud/steam-account-manager.git
cd steam-account-manager
.\scripts\init_third_party.ps1
start steam-account-manager.sln
```

In Visual Studio pick `Release | x64` and hit F7. The binary lands at `build\Release\steam-account-manager.exe`. The C runtime is statically linked, so the binary runs on a clean Windows 11 install with no redistributables.

A pre-build target invokes `scripts\gen_protos.ps1` to regenerate the `.pb.cc` files under `core\steam_auth\gen\` from the `.proto` files in `third_party\steam_protos\`. The script needs `protoc` on PATH, which is vendored at `third_party\protobuf\bin\protoc.exe`.

Note: Debug builds are currently broken because the vendored libprotobuf is Release-MT only. Build Release.

## Project layout

```
app/             WinMain, app state, background vault writer, job pump, drag-and-drop handler
core/
  sda/           TOTP, confirmations, maFile import, info.dat import, add and remove Steam Guard
  steam_api/     Steam Web API (player summaries, bans, level, owned games, vanity resolution)
  steam_login/   mobile auth flow (RSA, BeginAuthSession, poll, finalizelogin, settoken)
  steam_gcpd/    GCPD page scraper and parser for CS2 ranks / cooldowns
  steam_local/   loginusers.vdf parsing and rewriting
  steam_auth/    generated protobuf for IAuthenticationService
  account_store/ vault types, atomic-write file format, filter and sort helpers
  crypto/        AES-GCM, AES-CBC, HMAC, PBKDF2, base64, Rijndael-256, RSA, SecureString
  http/          WinHTTP wrapper, URL helpers
  launch/        steam.exe relaunch, UI-Automation login driver, code clipboard with auto-clear
platform/        Win32 wrappers: paths, registry, process, clipboard, DPAPI, DPI, file dialog, UIA
ui/
  screens/       one file per screen (unlock, accounts, add_account, sda, confirmations, settings)
  widgets/       reusable composites (account_card, rank_image, ban_pills, tag_chip, rail_nav, ...)
third_party/     fetched by scripts/init_third_party.ps1
scripts/         proto codegen, third-party fetch
assets/          app icon and CS2 rank images, compiled into the .exe via app.rc
```

The `core/ <-> ui/` split means everything in `core/` is headless and unit-testable without ImGui or a Win32 message pump.

## Portable mode

Drop a file named `portable.flag` next to the .exe. The vault, settings, logs, and DPAPI cache all live in the same folder as the binary instead of `%LOCALAPPDATA%\steam-account-manager`. Useful from a USB stick or sandbox.

## Vendored libraries

| Library | License | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | MIT | UI |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | AES-GCM, AES-CBC, PBKDF2, HMAC, RSA |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON, CBOR |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging |
| [stb](https://github.com/nothings/stb) | MIT / public | PNG decode for avatars |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 | UTF-8 / UTF-16 conversion |
| [doctest](https://github.com/doctest/doctest) | MIT | Unit tests (vendored, not run by default) |
| [protobuf](https://github.com/protocolbuffers/protobuf) | BSD-3 | Codegen for IAuthenticationService |

## License

[MIT](LICENSE).

## Contributing

Pull requests welcome. Code style:

- `clang-format` config is at the repo root; the build is `/W4 /WX`, so warnings break the build.
- Naming: snake_case for functions and variables, PascalCase for structs and enums, `k` prefix for constants, `g_` prefix for globals. Member variables do not get an `m_` prefix.
- Use `.hpp` / `.cpp`. Use `std::span` instead of `(pointer, length)` pairs in interfaces. Use `std::filesystem::path` for paths, never `std::string`.
- Comments: default to writing none. Add one when the WHY is non-obvious. No em dashes, no decorative banners (`// =====`), no `IMPORTANT:` prefixes, no PR / ticket references, no emojis. When in doubt, delete the comment.
- New vendored dependencies are a meaningful change. Open an issue first with what the library does, why an existing dependency can't cover it, and under what license it ships.
