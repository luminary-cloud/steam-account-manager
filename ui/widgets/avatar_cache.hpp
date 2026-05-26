#pragma once

#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Returns a D3D11 SRV for the avatar at `url`. Returns nullptr if the avatar
// hasn't been fetched yet; caller should draw a placeholder. The first call
// for a URL kicks off a background download; subsequent calls hit the cache.
ID3D11ShaderResourceView* avatar_for(std::string_view url);

namespace avatar_cache {

// Should be called once at startup so the cache knows which D3D device to use.
void init(void* d3d_device);

// Drop everything (called on shutdown).
void shutdown();

}  // namespace avatar_cache

}  // namespace sam::ui::widgets
