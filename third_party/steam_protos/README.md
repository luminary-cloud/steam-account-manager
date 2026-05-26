# Steam protobufs

These `.proto` files come from
[SteamDatabase / GameTracking-CS2](https://github.com/SteamDatabase/GameTracking-CS2/tree/master/Protobufs)
under the same MIT-style license the upstream project ships under.

## What we compile

`scripts/gen_protos.ps1` only compiles the four files needed by
`IAuthenticationService`:

- `steammessages_base.proto`
- `enums.proto`
- `steammessages_unified_base.steamclient.proto`
- `steammessages_auth.steamclient.proto`

The `gen_protos.ps1` list is the source of truth. If a new message type is
needed, drop the corresponding `.proto` into this directory and add it to
that list.
