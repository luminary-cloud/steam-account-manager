#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sam::app {

// Directory where the user's gamesense loader .exe is copied: data_dir()/"gamesense".
std::filesystem::path gamesense_dir();

// Path to the installed loader (the first *.exe in gamesense_dir()), or nullopt
// if no loader has been configured yet.
std::optional<std::filesystem::path> gamesense_loader_path();

// Copies `src` into gamesense_dir(), replacing any previously installed loader
// (existing *.exe in the folder are removed first so the lookup stays
// unambiguous after a loader update). Returns false and fills `err` on failure.
bool install_gamesense_loader(const std::filesystem::path& src, std::string* err);

}  // namespace sam::app
