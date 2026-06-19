# Regenerates *.pb.cc / *.pb.h from third_party/steam_protos/*.proto.
#
# Called from the MSBuild pre-build target so the generated files always
# match the committed .proto definitions. Safe to run by hand too.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$protoc = Join-Path $root "third_party\protobuf\bin\protoc.exe"
$proto_dir = Join-Path $root "third_party\steam_protos"
$wk_dir = Join-Path $root "third_party\protobuf\proto-include"
$gen_dir = Join-Path $root "core\steam_auth\gen"

if (-not (Test-Path $protoc)) {
    Write-Error "protoc.exe not found at $protoc. See third_party/protobuf/README.md."
}

New-Item -ItemType Directory -Force -Path $gen_dir | Out-Null

# The auth-dependency closure: protos required by IAuthenticationService.
$proto_files = @(
    "steammessages_base.proto"
    "enums.proto"
    "steammessages_unified_base.steamclient.proto"
    "steammessages_auth.steamclient.proto"
    "gc_options.proto"
    "steammessages_clientserver_login.proto"
    "steammessages_clientserver_extra.proto"
    "engine_gcmessages.proto"
    "gcsdk_gcmessages.proto"
    "base_gcmessages.proto"
    "econ_gcmessages.proto"
    "cstrike15_gcmessages.proto"
)

$args = @(
    "--proto_path=$proto_dir",
    "--proto_path=$wk_dir",
    "--cpp_out=$gen_dir"
) + $proto_files
& $protoc @args
if ($LASTEXITCODE -ne 0) {
    Write-Error "protoc failed with exit code $LASTEXITCODE"
}

$generated = Get-ChildItem $gen_dir -Filter "*.pb.cc"
Write-Output ("generated {0} .pb.cc files" -f $generated.Count)
