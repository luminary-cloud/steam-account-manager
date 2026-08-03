#include "core/cleaner/crash_dumps.hpp"

#include <array>
#include <cwctype>
#include <system_error>

#include "platform/paths.hpp"

namespace sam::cleaner::crashdumps {
namespace {

constexpr std::array<std::wstring_view, 11> kPrefixes = {{
    L"steam",
    L"cs2",
    L"csgo",
    L"gmod",
    L"hl2",
    L"l4d",
    L"dota",
    L"tf",
    L"srcds",
    L"vrserver",
    L"vrclient",
}};

std::wstring lower(std::wstring_view in) {
    std::wstring out{in};
    for (auto& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

}  // namespace

bool is_steam_related_filename(std::wstring_view filename) {
    const auto lc = lower(filename);
    for (const auto p : kPrefixes) {
        if (lc.starts_with(p)) return true;
    }
    return false;
}

std::vector<DumpFile> find_local_appdata_dumps() {
    namespace fs = std::filesystem;
    std::vector<DumpFile> out;
    const auto root = platform::local_appdata_root() / "CrashDumps";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;

    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::end(it); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension();
        if (ext != L".dmp" && ext != L".mdmp") continue;
        if (!is_steam_related_filename(it->path().filename().wstring())) continue;

        DumpFile d;
        d.path = it->path();
        std::error_code size_ec;
        d.size_bytes = it->file_size(size_ec);
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace sam::cleaner::crashdumps
