#pragma once

#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Returns nullptr until the image is fetched; the first call for a URL kicks off a
// background download into a D3D11 texture, cached by URL. Caller draws a placeholder
// meanwhile. Used for avatars and CS2 item icons alike.
ID3D11ShaderResourceView* texture_for(std::string_view url);

// Avatar alias for texture_for, kept for existing call sites.
inline ID3D11ShaderResourceView* avatar_for(std::string_view url) { return texture_for(url); }

namespace avatar_cache {

void init(void* d3d_device);
void shutdown();

}  // namespace avatar_cache

}  // namespace sam::ui::widgets
