#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sam::app {

std::filesystem::path gamesense_dir();

// First *.exe in gamesense_dir(), or nullopt if no loader configured yet.
std::optional<std::filesystem::path> gamesense_loader_path();

// Copies `src` into gamesense_dir(), removing existing *.exe first so the
// "first .exe" lookup stays unambiguous. Fills `err` on failure.
bool install_gamesense_loader(const std::filesystem::path& src, std::string* err);

}  // namespace sam::app
