#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sam::platform::process {

std::vector<std::uint32_t> find_by_image_name(std::wstring_view name);

bool terminate(std::uint32_t pid);

std::optional<std::uint32_t> launch(const std::filesystem::path& exe,
                                     const std::wstring& args,
                                     const std::optional<std::filesystem::path>& working_dir = {});

}  // namespace sam::platform::process
