#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace sam::platform::file_dialog {

struct FilterSpec {
    std::wstring description;
    std::wstring pattern;      // semicolon-separated for multi
};

struct Options {
    HWND parent = nullptr;
    std::wstring title;
    std::wstring default_filename;             // save_file only
    // Empty falls back to a single "All files (*.*)" entry.
    std::vector<FilterSpec> filters;
    // Appended by save_file when the user doesn't type one. Ignored by open_file.
    std::wstring default_extension;
    // open_file only; results come back in Result::paths.
    bool allow_multiselect = false;
};

struct Result {
    bool ok = false;
    // First selected path; always set when ok.
    std::filesystem::path path;
    std::vector<std::filesystem::path> paths;
};

// Modal; blocks the calling thread. Initializes COM apartment-threaded for the call.
Result open_file(const Options& opts);

Result save_file(const Options& opts);

// Only the title and parent fields of Options are used.
Result pick_folder(const Options& opts);

}  // namespace sam::platform::file_dialog
