#pragma once

#include <cstdint>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Maps a Premier rating to a badge bracket index 0-6.
int premier_bracket_for_rating(int rating);

namespace rank_image {

struct TexEntry {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;
};

void init(void* d3d_device);
void shutdown();

// bracket [0..6] / rank [1..18], or null if out of range. First call decodes
// the embedded PNG; later calls hit the cache.
const TexEntry* premier(int bracket);
const TexEntry* wingman(int rank);

}  // namespace rank_image

}  // namespace sam::ui::widgets
