#pragma once

#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace sam::ui::widgets {

// Returns nullptr until the avatar is fetched; the first call for a URL kicks
// off a background download. Caller draws a placeholder meanwhile.
ID3D11ShaderResourceView* avatar_for(std::string_view url);

namespace avatar_cache {

void init(void* d3d_device);
void shutdown();

}  // namespace avatar_cache

}  // namespace sam::ui::widgets
