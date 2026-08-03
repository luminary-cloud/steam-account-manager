# Contributing

Pull requests are welcome. This is a native Windows C++ app, so the toolchain is
Visual Studio rather than a cross-platform build.

## Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload
- The Windows 10 SDK (installed with that workload)

## Build

The vendored dependencies under `third_party/` are fetched by a script, so there is
no vcpkg, Conan, or submodule setup.

```powershell
git clone https://github.com/luminary-cloud/steam-account-manager.git
cd steam-account-manager
.\scripts\init_third_party.ps1
start steam-account-manager.sln
```

Pick **`Release | x64`** and build (F7). The binary lands at
`build\Release\steam-account-manager.exe`. A pre-build step runs
`scripts\gen_protos.ps1` to regenerate the protobuf sources with the vendored
`protoc`.

> Debug builds are currently broken: the vendored libprotobuf is Release-MT only.
> Build Release.

## Style and quality

- Formatting is governed by `.clang-format` and `.editorconfig` at the repo root.
  Run clang-format before committing.
- The project builds at `/W4` with warnings treated as errors, so a new warning
  breaks the build. CI (`.github/workflows/build.yml`) builds the same Release x64
  config on every push and pull request.
- Comments live in headers. A `.cpp` file carries no comments at all, apart from the
  `}  // namespace ...` markers that close a namespace. Document a function where it
  is declared, and keep that documentation factual rather than decorative.

## Project files

`steam-account-manager.vcxproj` and `steam-account-manager.vcxproj.filters` list
every source file explicitly (no globbing). When you add, remove, or rename a file
you must update **both**: the `.vcxproj` (`ClCompile` for `.cpp`, `ClInclude` for
headers) and the `.vcxproj.filters` (same entry plus a `<Filter>` so it lands in the
right Solution Explorer folder).

## Layout

```
app/            WinMain, app state, settings, background vault writer, job pump,
                conf poller, drag-and-drop, gamesense loader
core/
  account_store/  vault types, atomic-write format, filter / sort, groups / tags
  crypto/         AES-GCM, AES-CBC, HMAC, PBKDF2, Rijndael-256, RNG, SecureString
  sda/            TOTP, confirmations, maFile / info.dat import, add / remove Guard
  steam_login/    mobile auth flow (RSA, BeginAuthSession, poll, finalizelogin),
                  web session, browser login
  steam_api/      Steam Web API (summaries, bans, level, owned games)
  steam_gcpd/     GCPD scraper for CS2 Premier / Wingman / cooldown / VAC-Live
  steam_local/    loginusers.vdf parsing, connect-cache token injection
  steam_spend/    external funds (TotalSpend) scrape
  steam_auth/     generated protobuf for IAuthenticationService
  trade/          trade offers, inventory, actions, trade-URL parsing, audit log
  launch/         steam.exe relaunch, UI-Automation login driver, token launcher,
                  CS2 autostart, clipboard auto-clear
  cs2/            CS2 friend-code conversion
  cs2_config/     CS2 video.txt / 730 folder deploy into the per-account cfg folder
  profile/        Steam display-name change
  notifications/  ban / cooldown change store
  http/           WinHTTP client and proxy (in-process SOCKS5 bridge)
platform/       Win32 wrappers: paths, registry, process, clipboard, DPAPI, tray
                icon, startup task, UI Automation, global hotkey, file dialog
ui/
  screens/      one feature per screen (unlock, accounts, add_account, sda,
                confirmations, trade_offers, settings); large screens split into
                per-tab / per-section translation units
  widgets/      reusable composites (account_card, account_context_menu, title_bar,
                rank_image, ...)
third_party/    fetched by scripts\init_third_party.ps1
assets/         app icon and CS2 rank images, compiled into the .exe via app.rc
```

The `core/` and `ui/` split keeps everything in `core/` headless and testable.

## Vendored libraries

| Library | License | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | MIT | UI |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | AES-GCM, AES-CBC, PBKDF2, HMAC, RSA |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON, CBOR |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging |
| [stb](https://github.com/nothings/stb) | MIT / public domain | PNG decode for avatars |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 | UTF-8 / UTF-16 conversion |
| [doctest](https://github.com/doctest/doctest) | MIT | Unit tests |
| [protobuf](https://github.com/protocolbuffers/protobuf) | BSD-3 | Codegen for IAuthenticationService |
