#pragma once

#include <cstdint>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Maps a Premier CS rating value to a bracket index 0-6 used for the badge.
int premier_bracket_for_rating(int rating);

namespace rank_image {

struct TexEntry {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;
};

// Lifecycle (called from win_main alongside avatar_cache).
void init(void* d3d_device);
void shutdown();

// Returns the cached premier-tier badge for bracket [0..6] or null if missing.
// First call decodes the embedded PNG; subsequent calls hit the cache.
const TexEntry* premier(int bracket);

// Returns the cached wingman rank icon for rank [1..18] or null otherwise.
const TexEntry* wingman(int rank);

}  // namespace rank_image

}  // namespace sam::ui::widgets
