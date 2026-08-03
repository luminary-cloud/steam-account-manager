<div align="center">

# Steam Account Manager

[![build](https://github.com/luminary-cloud/steam-account-manager/actions/workflows/build.yml/badge.svg)](https://github.com/luminary-cloud/steam-account-manager/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/luminary-cloud/steam-account-manager?cacheSeconds=3600)](../../releases)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6)

A native Windows manager for your Steam accounts: encrypted vaults, Steam Guard
codes, mobile confirmations, trade offers, CS2 tools, and one-click login.

<details>
<summary><b>Show screenshot</b></summary>
<br>
<img src="assets/screenshot.png" alt="Steam Account Manager" width="820">
</details>

Keep your accounts in one or more local vaults encrypted with your master password.
No installer, no telemetry, nothing leaves your machine.

</div>

## Features

### Vaults
- Keep separate sets of accounts in independent encrypted vaults (personal, trading, clients).
- Give each vault a name, a color, and a custom image icon.
- A picker on startup lets you choose a vault, or set one to open automatically.
- Switch vaults from the sidebar. Each vault has its own accounts and its own settings; a few options (marked "global" in Settings) apply everywhere.

### Accounts
- Card grid, or a two-pane list view with collapsible, color-coded groups.
- Add manually, from a `.maFile` or `info.dat`, by walking the full mobile login, or
  by pasting an NFA refresh token. Drag and drop files or folders onto the window.
- Per-account password, notes, color tags, and trust labels (green / yellow / red).
- Search, sort, multi-select, and bulk actions.
- Privacy mode hides every login until you click to reveal one.
- NFA (token-only) accounts: import a JWT refresh token, track when it expires, get
  flagged when it goes stale, edit or replace it later, and sign in without a password.

### Authenticator
- Steam Guard codes for any imported authenticator, with a next-code preview.
- Add Steam Guard in-app through the mobile flow, or remove it with your revocation code.
- Optional global hotkey copies the selected account's code without focusing the app.
- Hide-until-click and auto-copy-on-select options, with clipboard auto-clear.

### Confirmations
- List, approve, or deny mobile confirmations (trades, market listings, phone changes,
  gift redemptions), per account or in bulk.
- Optional background poller refreshes on a timer and toasts when new items arrive.
- Auto-approve rules for market listings, phone changes, and trades to a trusted-partner list.
- Every decision is written to a local audit log.

### Trade offers
- View, accept, or decline incoming and outgoing offers, with full item and escrow detail.
- Build and send offers from your inventory to a trade URL, with optional auto-confirm.
- Fetch and copy an account's own trade link, cached and auto-refreshed.
- Optional background poller and a local trade audit log.

### Account review
- Per-account VAC / game / community / trade ban flags, Steam level, and owned games
  (needs a free Steam Web API key, stored encrypted with the vault).
- CS2 Premier rating, Wingman rank, level, Prime status, competitive cooldown, and
  VAC-Live, scraped from the authenticated GCPD page.
- External funds (total spend) per account.
- New bans, cooldown changes, and VAC-Live flips surface as card badges, in-app toasts,
  and optional Windows tray notifications, and are logged. Every indicator is toggleable.

### CS2 (Game Coordinator)
- Connect an account to view and manage its live inventory, moving items in and out of
  storage units.
- Profile medals with icons, weekly drop tracking with one-click claim, and weekly
  mission progress.
- Auto-pull signs in a single account on startup and fetches every account's medals and
  level by Steam ID, on a cache window you set.

### Launch
- One-click sign-in: closes Steam, relaunches, and types the login and Steam Guard code via
  UI Automation, falling back to the clipboard with auto-clear.
- Or sign in without the login window by injecting a client-scoped JWT into Steam's
  ConnectCache. NFA accounts always use this, with no password or code typing.
- Copy a `video.txt` or a whole `730` folder into the account's CS2 config on login.
- Or point it at a `userdata\<id>` folder and every game folder in it (`730`, `570`, `440`, ...)
  is copied on login. Steam's own per-account settings are left alone.
- Set CS2 launch options once and have them written for whichever account you launch.
- Optionally turn off Steam Cloud, new-release news, Remote Play, or friends visibility for
  each account you launch. Existing files are backed up first.
- Optionally block a launched account's subscribed CS2 workshop maps from downloading, so it
  goes straight into the game. Subscriptions are kept.
- Per-account launch method: stop after login, launch CS2, or launch CS2 with an external loader.
- Open any account in your browser, already signed in, from the right-click menu.

### Tracer cleaner
- Wipes what Steam leaves on the PC, as one of three profiles: **Quick Clean** (caches,
  logs, crash dumps), **Account Reset** (adds saved logins, the ConnectCache login tokens
  in `config.vdf` / `local.vdf`, ssfn files, the HKCU autologin values, per-account caches),
  or **Full Wipe** (adds each account's whole `userdata` folder).
- A keep list decides what survives. Tick any vault account, or any Steam login found on this
  PC, and its saved login, `userdata`, registry subtree, avatar and login token are spared.
  `loginusers.vdf` and the ConnectCache are edited entry by entry rather than deleted, so kept
  accounts stay signed in. Everything unticked is signed out.
- Runs on demand, or automatically before every sign-in, when the vault is unlocked, or when
  the app closes. The pre-launch trigger fires while Steam is already down, so it never fights
  the sign-in.
- Preview first: the exact list of files, registry values and VDF entries, with sizes. There is
  no backup and no undo.
- Gated by safe mode: with safe mode on the tab is hidden and no trigger fires, and your profile
  and keep list are kept for when you turn it back off.

### HWID spoofer (educational purposes only)
- Injects a DLL into the Steam process on launch that hooks WMI, SMBIOS, DXGI, D3D9, and
  display APIs to present spoofed hardware identifiers.
- Spoofable components: machine GUID, MAC address, disk serial, PC name, GPU, motherboard,
  RAM, monitor, storage, and sound card, each individually toggleable.
- Per-account hardware profiles: each account gets its own randomly generated profile, or is
  excluded from spoofing entirely.
- "Always spoof" applies a profile to every launch; the per-component mask picks exactly which
  identifiers are replaced.
- Hardware comparison table in Settings shows real vs. spoofed values side by side.

### Background refresh
- Refresh every account on launch, or schedule a logon task that refreshes in the background
  and notifies on new bans or cooldown changes.
- Optional periodic auto-refresh while the app is open, updating only accounts whose cached
  data has expired.
- The logon task can instead just open the app, optionally minimized.
- Optional update check on launch, against the GitHub releases page. No account data is sent.

### Import and export
- Passphrase-protected `.sambundle` to carry accounts between machines, with a merge
  preview before anything is written.
- Plain `login:password` export gated behind a typed confirmation phrase.

### Privacy and security
- Vault encrypted with AES-256-GCM under PBKDF2-HMAC-SHA256 (600,000 iterations). No recovery
  path. Optional DPAPI auto-unlock skips the master-password prompt on launch.
- Safe mode hides and disables everything that can get an account banned: the HWID spoofer, the
  external CS2 loaders and the tracer cleaner all leave the interface, no launch injects or runs
  them, and no cleaner trigger fires. Set per vault, and reversible. Nothing is erased, so
  turning it off restores everything.
- Optional administrator elevation, on by default. Only the Windows-logon task and controlling
  an elevated Steam need it, so most setups can turn it off and never see a UAC prompt again.
- Streamproof mode hides the window from screen-capture software (OBS, Discord, Snipping Tool)
  while it stays fully visible on your monitor.
- Per-account or single proxy (SOCKS5 / HTTP / HTTPS, with credentials) and a test button.
- Portable mode and a relocatable data folder (USB stick, another drive, network share).
- No telemetry. Details in [SECURITY.md](SECURITY.md).

## Download

Grab the latest `steam-account-manager.exe` from the [Releases](../../releases) page.
It is statically linked, so no Visual C++ redistributable is required.

- **Requirements:** Windows 10 or 11, 64-bit. The app asks for administrator on launch,
  which you can turn off in Settings > General if you don't need the Windows-logon task
  (see [Privacy and security](#privacy-and-security)).
- **First run:** the binary is unsigned, so SmartScreen may show "Windows protected
  your PC". Click **More info**, then **Run anyway**.
- On first launch you set a master password (there is no recovery), then add an account.

## Build from source

```powershell
git clone https://github.com/luminary-cloud/steam-account-manager.git
cd steam-account-manager
.\scripts\init_third_party.ps1
start steam-account-manager.sln   # build Release | x64
```

Vendored dependencies are fetched by the script, so there is no vcpkg or submodule
setup. See [CONTRIBUTING.md](CONTRIBUTING.md) for prerequisites, the project layout,
and the vendored-library list.

## Security

Account secrets live in an AES-256-GCM vault keyed from your master password, stored
only on your machine. There is no recovery path and no telemetry. To report a
vulnerability or read the full security model, see [SECURITY.md](SECURITY.md).

## Contributing

Pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Disclaimer

This project is for managing your own accounts and for educational and research
purposes, and is not affiliated with Valve or Steam. The HWID spoofer component is
provided for educational purposes only. You are responsible for how you use this
software, and it must not be used to violate the Steam Subscriber Agreement, any
game's terms, or applicable law. See [DISCLAIMER.md](DISCLAIMER.md).

## License

[MIT](LICENSE).
