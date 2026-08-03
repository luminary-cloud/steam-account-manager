#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace sam::cleaner::crashdumps {

struct DumpFile {
    std::filesystem::path path;
    std::uintmax_t size_bytes = 0;
};

// Minidumps under %LOCALAPPDATA%\CrashDumps that look Steam-related. Windows writes them as
// "<exe>.<pid>.dmp", so prefix matching on the filename is reliable.
std::vector<DumpFile> find_local_appdata_dumps();

// True if `filename` starts with a Steam/Source-game prefix (steam, cs2, csgo, gmod, hl2, l4d,
// dota, tf, srcds, vrserver, vrclient).
bool is_steam_related_filename(std::wstring_view filename);

}  // namespace sam::cleaner::crashdumps
