# steam-account-manager

Native Windows account manager for Steam. Encrypted vault, Steam Guard codes, mobile confirmations, one-click login. No installer, no telemetry.

## Download

Grab the latest `steam-account-manager.exe` from the [Releases](../../releases) page. The binary is statically linked, so it runs on any 64-bit Windows 10 or 11 with no Visual C++ Redistributable.

On first launch Windows SmartScreen may show "Windows protected your PC" because the binary is unsigned. Click **More info** then **Run anyway**. Code-signing certificates aren't free; this is normal for small open-source Windows tools.

## What it does

**Accounts.** Your vault, as a card grid or a list view with coloured groups (toggle in Settings).

- Add manually, by `.maFile`, by `info.dat`, or by walking the full mobile login flow.
- Drag a `.maFile`, `info.dat`, or a folder of either onto the window to queue them.
- Per-account password, notes, tags, trust labels, trade-hold timers. Search, filter, multi-select.
- Privacy mode hides every login until you click to reveal one.
- Right-click an account to copy its login, password, 2FA code, SteamID64, profile URL, or CS2 friend code; apply the CS2 video config; or change its Steam display name (honouring Steam's 5-minute persona cooldown).

**Authenticator.** Steam Guard codes for any imported authenticator.

- Add Steam Guard from inside the app via the mobile flow (phone check, AddAuthenticator, SMS finalize).
- Remove it with the saved revocation code (scheme 1 reverts to email Guard, scheme 2 strips it; 15-day market hold either way).
- Next-code preview so a window roll-over doesn't cost you a half-typed code.
- Optional global hotkey copies the selected account's code without focusing the app, with auto-clear.

**Confirmations.** List, approve, or deny pending mobile confirmations: trades, market listings, phone-number changes, gift redemptions.

- Per-account, plus bulk approve / deny across a selection.
- Optional background poller refreshes on an interval and toasts when new items land.
- Auto-approve rules can clear market listings, phone changes, and trades to a trusted-partner list; everything else still waits for you.
- Every decision goes to a local audit log with configurable retention.

**Account review.** Per-account status pulled on refresh: VAC / game / community / trade ban indicators, Steam level, owned games, profile info, plus CS2 Premier rating, Wingman rank, CS2 level, Prime status, cooldown, and VAC-Live (scraped from the authenticated `/gcpd/730` page).

- The public-API fields (bans, level, owned games) need a free Steam Web API key pasted into Settings; it's stored encrypted with the vault.
- Every indicator is individually toggleable.
- New bans, cooldown changes, and VAC-Live flips between refreshes surface as a card badge, an in-app toast, and an optional Windows tray notification, and are kept in a local log.

**Launch.** One-click launch into any saved account through the standard registry login flow.

- Best-effort auto-typing of the Steam Guard code into the Steam login popup via UI Automation; falls back to the clipboard with auto-clear.
- Optional per-launch CS2 video config: pick a `video.txt` in Settings and each launch copies it into that account's CS2 cfg folder, backing up any existing file first.

**Export / import.** A passphrase-protected `.sambundle` you can carry between machines (passphrase separate from your master password). Import shows a merge preview before writing. Plain `login:password` export is gated behind a typed confirmation phrase.

## Update check

On launch the app fetches `https://api.github.com/repos/luminary-cloud/steam-account-manager/releases/latest`. If the tag is newer than the version baked into the .exe, a modal offers to open the releases page or skip that version (skipping persists, so the same release stops nagging). The check runs once per launch on a background thread and stays quiet if the network is down. Turn it off in Settings.

## Start with Windows

Enable *Start with Windows* in Settings to register a Task Scheduler task that runs the app at logon with `--startup`: headless, it unlocks the vault (DPAPI cache required), refreshes every account, fires a tray notification for any new bans or cooldowns, then exits. It's a Scheduled Task rather than the `Run` key because the app requires administrator elevation, which the `Run` key skips.

## What it doesn't do

- No telemetry. The only network calls are the ones you trigger, plus the launch update check.
- No password recovery. Lose the master password, lose the vault. There is no backdoor.
- Windows x64 only.
- No code signing. SmartScreen warns on first run.
- No bundled cheats, scripts, or game modifications.

## First run

The app requests UAC elevation on launch; the registry login flow and per-account `userdata` writes need it.

1. Launch `steam-account-manager.exe`. The window opens on the Unlock screen.
2. Create a master password. There is no recovery path, so pick something you'll remember.
3. Add an account: manual, `.maFile`, `info.dat`, or full mobile login.
4. Optional: enable *Skip master-password prompt on launch (DPAPI)* to auto-open the vault next time. Anyone signed in as you on this machine can then open it without the password.
5. Optional: set auto-lock minutes, clipboard auto-clear, the per-indicator toggles, and your Steam Web API key.

## Security

The vault (`vault.bin`) holds every account's secrets, encrypted with AES-256-GCM under a key derived from your master password (PBKDF2-HMAC-SHA256, 600 000 iterations). There is no recovery path: lose the master password, lose the vault. The optional DPAPI auto-unlock trades some of that protection for convenience.

## Build from source

Self-contained: vendored deps under `third_party/` are fetched by a script, so there's no vcpkg, Conan, or submodule setup. Requires Visual Studio 2022 with the **Desktop development with C++** workload and the Windows 10 SDK.

```powershell
git clone https://github.com/luminary-cloud/steam-account-manager.git
cd steam-account-manager
.\scripts\init_third_party.ps1
start steam-account-manager.sln
```

Pick `Release | x64` and hit F7; the binary lands at `build\Release\steam-account-manager.exe`. A pre-build step runs `scripts\gen_protos.ps1` to regenerate the protobuf sources using the vendored `protoc`.

Debug builds are currently broken because the vendored libprotobuf is Release-MT only. Build Release.

## Project layout

```
app/         WinMain, app state, background vault writer, job pump, drag-and-drop
core/
  sda/           TOTP, confirmations, maFile / info.dat import, add and remove Steam Guard
  steam_api/     Steam Web API (summaries, bans, level, owned games, vanity resolution)
  steam_login/   mobile auth flow (RSA, BeginAuthSession, poll, finalizelogin)
  profile/       Steam display-name change
  steam_gcpd/    GCPD scraper for CS2 ranks / cooldowns
  steam_local/   loginusers.vdf parsing and rewriting
  steam_auth/    generated protobuf for IAuthenticationService
  account_store/ vault types, atomic-write format, filter / sort helpers
  crypto/        AES-GCM, AES-CBC, HMAC, PBKDF2, Rijndael-256, RSA, SecureString
  cs2/           CS2 friend-code conversion
  cs2_config/    CS2 video.txt deploy into the per-account cfg folder
  http/          WinHTTP wrapper
  launch/        steam.exe relaunch, UI-Automation login driver, clipboard auto-clear
  update_check   GitHub release check
platform/    Win32 wrappers: paths, registry, process, clipboard, DPAPI, tray icon, startup task, UIA
ui/
  screens/   one file per screen (unlock, accounts, add_account, sda, confirmations, settings)
  widgets/   reusable composites (account_card, account_context_menu, rank_image, ...)
third_party/ fetched by scripts/init_third_party.ps1
assets/      app icon and CS2 rank images, compiled into the .exe via app.rc
```

The `core/ <-> ui/` split keeps everything in `core/` headless and unit-testable.

## Portable mode

Drop a file named `portable.flag` next to the .exe. The vault, settings, logs, and DPAPI cache live alongside the binary instead of `%LOCALAPPDATA%\steam-account-manager`. Useful from a USB stick or sandbox.

## Vendored libraries

| Library | License | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | MIT | UI |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | AES-GCM, AES-CBC, PBKDF2, HMAC, RSA |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON, CBOR |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging |
| [stb](https://github.com/nothings/stb) | MIT / public | PNG decode for avatars |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 | UTF-8 / UTF-16 conversion |
| [doctest](https://github.com/doctest/doctest) | MIT | Unit tests |
| [protobuf](https://github.com/protocolbuffers/protobuf) | BSD-3 | Codegen for IAuthenticationService |

## License

[MIT](LICENSE).

## Contributing

Pull requests welcome. A `clang-format` config and a `/W4 /WX` build are at the repo root, so formatting is enforced and warnings break the build.
