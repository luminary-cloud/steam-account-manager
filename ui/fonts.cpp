#include "ui/fonts.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>

namespace sam::ui::fonts {

namespace {

ImFont* g_body = nullptr;
ImFont* g_title = nullptr;
ImFont* g_badge = nullptr;

std::filesystem::path windows_font_path(const char* filename) {
    const char* win_dir = std::getenv("WINDIR");
    std::filesystem::path root = win_dir ? std::filesystem::path(win_dir) : std::filesystem::path("C:\\Windows");
    return root / "Fonts" / filename;
}

std::vector<std::uint8_t> read_font_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = static_cast<std::streamsize>(in.tellg());
    if (size <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

ImFont* add_font_from_disk(ImFontAtlas* atlas, const std::filesystem::path& path,
                           float pixel_size, const ImWchar* glyph_ranges) {
    auto bytes = read_font_file(path);
    if (bytes.empty()) return nullptr;

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.OversampleH = 5;
    cfg.OversampleV = 5;
    cfg.PixelSnapH = false;
    cfg.RasterizerMultiply = 1.20F;

    void* data = IM_ALLOC(bytes.size());
    std::memcpy(data, bytes.data(), bytes.size());
    return atlas->AddFontFromMemoryTTF(data, static_cast<int>(bytes.size()),
                                       pixel_size, &cfg, glyph_ranges);
}

const ImWchar* extended_glyph_ranges() {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
        0x2000, 0x206F,  // General Punctuation
        0x2070, 0x209F,  // Superscripts and Subscripts
        0x20A0, 0x20CF,  // Currency Symbols
        0x2100, 0x214F,  // Letterlike Symbols
        0x2190, 0x21FF,  // Arrows
        0x2200, 0x22FF,  // Mathematical Operators
        0x2300, 0x23FF,  // Misc Technical
        0x25A0, 0x25FF,  // Geometric Shapes
        0x2600, 0x26FF,  // Misc Symbols
        0x2700, 0x27BF,  // Dingbats
        0xE000, 0xF8FF,  // Private Use Area (reserved for future icon fonts)
        0,
    };
    return &ranges[0];
}

}  // namespace

void load(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    const float body_size = 16.0F * scale;
    const float title_size = 17.0F * scale;
    const float badge_size = 20.0F * scale;

    const ImWchar* ranges = extended_glyph_ranges();
    static const ImWchar badge_ranges[] = {
        0x0020, 0x0020,   // space
        0x002C, 0x002E,   // , - .
        0x0030, 0x0039,   // 0-9
        0,
    };

    g_body = add_font_from_disk(io.Fonts, windows_font_path("segoeui.ttf"), body_size, ranges);
    g_title = add_font_from_disk(io.Fonts, windows_font_path("segoeuib.ttf"), title_size, ranges);
    g_badge = add_font_from_disk(io.Fonts, windows_font_path("segoeuib.ttf"), badge_size, badge_ranges);

    if (!g_body) {
        g_body = io.Fonts->AddFontDefault();
    }
    if (!g_title) {
        g_title = g_body;
    }
    if (!g_badge) {
        g_badge = g_title;
    }
}

ImFont* body()  { return g_body; }
ImFont* title() { return g_title; }
ImFont* badge() { return g_badge; }

}  // namespace sam::ui::fonts
