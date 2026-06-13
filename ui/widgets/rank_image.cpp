#include "ui/widgets/rank_image.hpp"

#include <array>
#include <cstdint>
#include <mutex>

#include <windows.h>
#include <d3d11.h>
#include <stb_image.h>

#include "app/resource.h"

namespace sam::ui::widgets {

int premier_bracket_for_rating(int rating) {
    if (rating < 0)         return 0;
    if (rating < 5000)      return 0;
    if (rating < 10000)     return 1;
    if (rating < 15000)     return 2;
    if (rating < 20000)     return 3;
    if (rating < 25000)     return 4;
    if (rating < 30000)     return 5;
    return 6;
}

namespace rank_image {

namespace {

std::mutex g_mtx;
ID3D11Device* g_device = nullptr;

std::array<TexEntry, IDR_PREMIER_COUNT> g_premier{};
std::array<bool,     IDR_PREMIER_COUNT> g_premier_tried{};
std::array<TexEntry, IDR_WINGMAN_COUNT> g_wingman{};
std::array<bool,     IDR_WINGMAN_COUNT> g_wingman_tried{};

bool load_resource_bytes(WORD resource_id, const unsigned char*& out_data, std::size_t& out_size) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC h = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(module, h);
    if (!g) return false;
    out_data = static_cast<const unsigned char*>(LockResource(g));
    out_size = SizeofResource(module, h);
    return out_data && out_size > 0;
}

bool decode_into(TexEntry& entry, const unsigned char* bytes, std::size_t size) {
    if (!g_device || !bytes || size == 0) return false;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes, static_cast<int>(size),
                                                  &w, &h, &comp, STBI_rgb_alpha);
    if (!pixels) return false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = pixels;
    init_data.SysMemPitch = static_cast<UINT>(w * 4);

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, &init_data, &tex)) && tex) {
        g_device->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    stbi_image_free(pixels);

    if (!srv) return false;
    entry.srv = srv;
    entry.w = w;
    entry.h = h;
    return true;
}

const TexEntry* get_or_load(TexEntry& slot, bool& tried, WORD resource_id) {
    std::lock_guard lk(g_mtx);
    if (slot.srv) return &slot;
    if (tried)    return nullptr;
    tried = true;

    const unsigned char* bytes = nullptr;
    std::size_t size = 0;
    if (!load_resource_bytes(resource_id, bytes, size)) return nullptr;
    if (!decode_into(slot, bytes, size)) return nullptr;
    return &slot;
}

}  // namespace

void init(void* d3d_device) {
    g_device = static_cast<ID3D11Device*>(d3d_device);
}

void shutdown() {
    std::lock_guard lk(g_mtx);
    for (auto& e : g_premier) {
        if (e.srv) { e.srv->Release(); e.srv = nullptr; }
    }
    for (auto& e : g_wingman) {
        if (e.srv) { e.srv->Release(); e.srv = nullptr; }
    }
    g_premier_tried.fill(false);
    g_wingman_tried.fill(false);
    g_device = nullptr;
}

const TexEntry* premier(int bracket) {
    if (bracket < 0 || bracket >= IDR_PREMIER_COUNT) return nullptr;
    return get_or_load(g_premier[bracket], g_premier_tried[bracket],
                       static_cast<WORD>(IDR_PREMIER_BASE + bracket));
}

const TexEntry* wingman(int rank) {
    if (rank < 0 || rank >= IDR_WINGMAN_COUNT) return nullptr;
    return get_or_load(g_wingman[rank], g_wingman_tried[rank],
                       static_cast<WORD>(IDR_WINGMAN_BASE + rank));
}

}  // namespace rank_image

}  // namespace sam::ui::widgets
