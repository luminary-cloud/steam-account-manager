#pragma once

#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Returns nullptr until the image is fetched; the first call for a URL kicks off a
// background download into a D3D11 texture, cached by URL. Caller draws a placeholder
// meanwhile. Used for avatars and CS2 item icons alike.
ID3D11ShaderResourceView* texture_for(std::string_view url);

// Same, but for a local image file (absolute path). The first call reads and
// decodes it on a background worker into a D3D11 texture, cached by path. Used for
// custom per-vault icons. Returns nullptr until ready or if decoding fails.
ID3D11ShaderResourceView* texture_for_file(std::string_view abs_path);

// Avatar alias for texture_for, kept for existing call sites.
inline ID3D11ShaderResourceView* avatar_for(std::string_view url) { return texture_for(url); }

namespace avatar_cache {

void init(void* d3d_device);
void shutdown();

}  // namespace avatar_cache

}  // namespace sam::ui::widgets
