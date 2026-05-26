#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace sam::platform::file_dialog {

struct FilterSpec {
    std::wstring description;  // e.g. L"Steam mobile authenticator file (*.maFile)"
    std::wstring pattern;      // e.g. L"*.maFile"; semicolon-separated for multi
};

struct Options {
    HWND parent = nullptr;
    std::wstring title;
    std::wstring default_filename;             // save_file only
    // File-type filters shown in the dialog's combo box. When empty the impl
    // falls back to a single "All files (*.*)" entry.
    std::vector<FilterSpec> filters;
    // Extension automatically appended by save_file when the user doesn't type
    // one. Ignored by open_file.
    std::wstring default_extension;
};

struct Result {
    bool ok = false;
    std::filesystem::path path;
};

// Modal "Open file" dialog. Blocks the calling thread until the user accepts
// or cancels. Initializes COM in apartment-threaded mode for the duration of
// the call.
Result open_file(const Options& opts);

// Modal "Save file" dialog. Default extension is automatically appended if the
// user doesn't type one.
Result save_file(const Options& opts);

// Modal folder picker. Only the title and parent fields of Options are used.
Result pick_folder(const Options& opts);

}  // namespace sam::platform::file_dialog
