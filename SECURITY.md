# Security Policy

## Reporting a vulnerability

Please report security issues privately. **Do not open a public issue for a
vulnerability.**

Email **cloud@luminary.pw** with:

- a description of the issue and its impact,
- steps to reproduce (or a proof of concept),
- the app version (Settings shows it, or the release tag) and your Windows version.

You can expect an acknowledgement within a few days. Please give a reasonable
window to ship a fix before any public disclosure.

## Supported versions

Only the latest release on the [Releases](../../releases) page receives fixes.

## How your data is protected

- **Encrypted vault.** Every account secret (Steam password, refresh tokens,
  Steam Guard `shared_secret` / `identity_secret`, session data) lives in
  `vault.bin`, encrypted with **AES-256-GCM**. The key is derived from your master
  password with **PBKDF2-HMAC-SHA256, 600,000 iterations**.
- **No recovery path.** The master password is never stored (unless you opt into
  the DPAPI cache below). If you lose it, the vault cannot be decrypted. There is
  no backdoor.
- **Local only.** The vault, settings, and logs stay on your machine, under
  `%LOCALAPPDATA%\steam-account-manager` (or next to the executable in portable
  mode). Nothing is uploaded.
- **No telemetry.** The only network calls are the ones you trigger (refreshing
  account data, logging in, confirmations, trades) plus an optional GitHub release
  check on launch, which you can disable in Settings.

## Trade-offs to understand

- **DPAPI auto-unlock.** Enabling "remember master password" caches it with Windows
  DPAPI so the vault opens without a prompt. This trades protection for convenience:
  anyone signed in as the same Windows user on that machine can then open the vault
  without knowing the master password. Leave it off if that is not acceptable.
- **Administrator elevation.** The app runs elevated because the Steam registry
  login flow and per-account `userdata` writes require it.
- **No code signing.** Release binaries are unsigned, so Windows SmartScreen warns
  on first run. Verify the SHA-256 published with each release if you want to
  confirm the download.

This tool ships no cheats, scripts, or game modifications. The optional "launch CS2
with an external loader" method only runs a loader executable that you supply and
point it at; nothing of the sort is bundled or downloaded.
