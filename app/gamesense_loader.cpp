#include "app/gamesense_loader.hpp"

#include <system_error>

#include "platform/paths.hpp"

namespace sam::app {

namespace fs = std::filesystem;

std::filesystem::path gamesense_dir() {
    return platform::data_dir() / "gamesense";
}

std::optional<std::filesystem::path> gamesense_loader_path() {
    std::error_code ec;
    const auto dir = gamesense_dir();
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == L".exe") {
            return entry.path();
        }
    }
    return std::nullopt;
}

bool install_gamesense_loader(const std::filesystem::path& src, std::string* err) {
    auto set_err = [err](const std::string& m) { if (err != nullptr) *err = m; };

    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        set_err("Selected loader does not exist.");
        return false;
    }

    const auto dir = gamesense_dir();
    fs::create_directories(dir, ec);
    if (ec) {
        set_err("Could not create the gamesense folder: " + ec.message());
        return false;
    }

    const auto dest = dir / src.filename();

    // Remove old loaders so the "first .exe" lookup stays unambiguous, but never
    // the file the user just picked (it may be the already-installed loader).
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != L".exe") continue;
        std::error_code same;
        if (fs::equivalent(entry.path(), src, same) && !same) continue;
        std::error_code rm;
        fs::remove(entry.path(), rm);
    }

    std::error_code same;
    if (fs::equivalent(src, dest, same) && !same) {
        return true;
    }

    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        set_err("Could not copy the loader: " + ec.message());
        return false;
    }
    return true;
}

}  // namespace sam::app
