[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $DllPath,
    [Parameter(Mandatory)][string] $OutHeader
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $DllPath)) {
    Write-Error "DLL not found: $DllPath"
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($DllPath)
$size  = $bytes.Length

Write-Host "[embed_dll] $DllPath -> $OutHeader ($size bytes)"

$sb = [System.Text.StringBuilder]::new(($size * 6) + 512)

[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.AppendLine("#include <cstdint>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace sam::core::hwid {")
[void]$sb.AppendLine("inline constexpr std::uint8_t kHwidDll[] = {")

$line = "    "
for ($i = 0; $i -lt $size; $i++) {
    $line += "0x{0:X2}," -f $bytes[$i]
    if (($i + 1) % 20 -eq 0) {
        [void]$sb.AppendLine($line)
        $line = "    "
    }
}
if ($line.Trim().Length -gt 0) {
    [void]$sb.AppendLine($line)
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("inline constexpr std::size_t kHwidDllSize = $size;")
[void]$sb.AppendLine("}  // namespace sam::core::hwid")

$dir = Split-Path -Parent $OutHeader
if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

[System.IO.File]::WriteAllText($OutHeader, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
Write-Host "[embed_dll] done"
