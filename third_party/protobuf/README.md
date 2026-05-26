# Vendored protobuf v3.21.12

Self-contained subset of Google's [Protocol Buffers](https://github.com/protocolbuffers/protobuf) used to compile the Steam wire-format definitions in `third_party/steam_protos/`.

## Layout

- `bin/protoc.exe` — Windows x64 build of the protobuf compiler (v3.21.12).
  Runs the pre-build target in `steam-account-manager.vcxproj` to regenerate
  `.pb.cc` / `.pb.h` from `.proto` whenever a proto changes.
- `include/google/protobuf/` — public C++ headers for the lite runtime.
- `lib/libprotobuf-lite.lib` — static library (x64, /MT, Release).

## Why v3.21.12?

Last protobuf release before Abseil became a hard dependency. The whole vendor
ships in roughly 5 MB. The newer (v22+) C++ runtimes pull in Abseil — another
~50 MB of code — for marginal gains over wire-format encode/decode, which is
the only thing we use here.

## Why lite only?

`libprotobuf-lite` is the runtime needed by code generated with the `optimize_for = LITE_RUNTIME` annotation in the Steam protos, and what generated code falls back to when descriptors / reflection aren't needed. We never need descriptor parsing or text-format I/O, so the full `libprotobuf` is overkill.

## Rebuilding from source

If you ever need to rebuild this vendor:

```powershell
# 1. Get the source.
$src_url = "https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.zip"
Invoke-WebRequest -Uri $src_url -OutFile $env:TEMP\pb.zip
Expand-Archive $env:TEMP\pb.zip $env:TEMP\pb-src

# 2. Configure + build libprotobuf-lite.
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S $env:TEMP\pb-src\protobuf-3.21.12 -B $env:TEMP\pb-build `
  -G "Visual Studio 17 2022" -A x64 `
  -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_PROTOC_BINARIES=OFF `
  -Dprotobuf_BUILD_LIBPROTOC=OFF -Dprotobuf_WITH_ZLIB=OFF `
  -Dprotobuf_INSTALL=OFF -DBUILD_SHARED_LIBS=OFF `
  -Dprotobuf_MSVC_STATIC_RUNTIME=ON
& $cmake --build $env:TEMP\pb-build --config Release --target libprotobuf-lite

# 3. Copy outputs back here.
Copy-Item $env:TEMP\pb-build\Release\libprotobuf-lite.lib third_party\protobuf\lib\
# Headers come from $env:TEMP\pb-src\protobuf-3.21.12\src\google\protobuf\.
```

The `protoc.exe` here came from the official `protoc-35.0-win64.zip` release. Newer protoc builds are wire-compatible with older runtimes, and a single binary is much smaller than the v3.21.12 protoc, so we use the modern one.

## License

Protocol Buffers ships under the BSD-3-Clause license; see https://github.com/protocolbuffers/protobuf/blob/main/LICENSE.
