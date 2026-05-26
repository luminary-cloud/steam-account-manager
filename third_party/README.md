# Vendored dependencies

This directory holds the third-party libraries the project depends on.
They are fetched by `scripts/init_third_party.ps1`.

Nothing in this directory should be committed to the repository. The init
script reproduces the layout from pinned upstream tags.

## Layout

| Subdir            | Source                                       | Version  | Why                              |
|-------------------|----------------------------------------------|----------|----------------------------------|
| `imgui/`          | github.com/ocornut/imgui (docking branch)    | latest   | UI                               |
| `mbedtls/`        | github.com/Mbed-TLS/mbedtls                  | v3.6.2   | AES-GCM, PBKDF2, HMAC, RSA       |
| `spdlog/`         | github.com/gabime/spdlog                     | v1.14.1  | Logging                          |
| `nlohmann_json/`  | github.com/nlohmann/json                     | v3.11.3  | JSON + CBOR                      |
| `stb/`            | github.com/nothings/stb                      | master   | Image decode for avatars / icons |
| `utfcpp/`         | github.com/nemtrif/utfcpp                    | v4.0.5   | UTF-8 / UTF-16 helpers           |
| `curl/`           | curl.se Windows static binary                | 8.10.1   | HTTPS, cookie jar                |
| `doctest/`        | github.com/doctest/doctest                   | v2.4.11  | Unit tests (vendored, no runner) |
| `protobuf/`       | protocolbuffers/protobuf                     | v3.21.12 | IAuthenticationService codegen   |

The Steam protobuf subset (`steam_protos/`) is a hand-curated snapshot from
SteamDatabase/Protobufs. See `steam_protos/README.md` for the four `.proto`
files we actually compile.

## Updating a dependency

1. Change the tag in `scripts/init_third_party.ps1`.
2. `Remove-Item -Recurse -Force third_party/<name>`.
3. `./scripts/init_third_party.ps1`.
4. Build the solution. Fix any compile errors.

## License notes

Every dependency above is permissively licensed (MIT, BSD, Apache-2.0, or
zlib-style). Their license texts are reproduced in the About screen and
`README.md`.
