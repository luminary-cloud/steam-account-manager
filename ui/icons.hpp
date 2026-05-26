#pragma once

#include <imgui.h>

struct ID3D11Device;

namespace sam::ui::icons {

void load(ID3D11Device* device);
void shutdown();

ImTextureID github();

}  // namespace sam::ui::icons
