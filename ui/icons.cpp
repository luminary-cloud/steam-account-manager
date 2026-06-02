#include "ui/icons.hpp"

#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <cstddef>
#include <vector>

#include "app/resource.h"

namespace sam::ui::icons {

extern const unsigned char* const kGithubIconPng;
extern const std::size_t kGithubIconPngLen;

namespace {

using Microsoft::WRL::ComPtr;

ID3D11ShaderResourceView* g_github_srv = nullptr;
ID3D11Texture2D* g_github_tex = nullptr;
ID3D11ShaderResourceView* g_app_srv = nullptr;
ID3D11Texture2D* g_app_tex = nullptr;

bool decode_png(const unsigned char* data, std::size_t len, std::vector<unsigned char>& rgba_out,
                UINT& w_out, UINT& h_out) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        spdlog::warn("icons: WIC factory creation failed: 0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(len));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                          &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) return false;
    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    rgba_out.resize(static_cast<std::size_t>(w) * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba_out.size()), rgba_out.data());
    if (FAILED(hr)) return false;
    w_out = w;
    h_out = h;
    return true;
}

// Rasterizes the app icon to straight-alpha RGBA. Reads hbmColor directly (not
// DrawIconEx) so alpha stays un-premultiplied, as ImGui's blending expects.
bool load_app_icon_rgba(std::vector<unsigned char>& rgba_out, UINT& w_out, UINT& h_out) {
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (icon == nullptr) return false;

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        DestroyIcon(icon);
        return false;
    }

    BITMAP bm{};
    GetObject(info.hbmColor, sizeof(bm), &bm);
    const UINT w = static_cast<UINT>(bm.bmWidth);
    const UINT h = static_cast<UINT>(bm.bmHeight);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    rgba_out.resize(static_cast<std::size_t>(w) * h * 4);
    HDC dc = GetDC(nullptr);
    const int scanned = GetDIBits(dc, info.hbmColor, 0, h, rgba_out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    DestroyIcon(icon);
    if (scanned == 0) return false;

    // GetDIBits hands back BGRA; swap to RGBA for DXGI_FORMAT_R8G8B8A8_UNORM.
    bool any_alpha = false;
    for (std::size_t i = 0; i + 3 < rgba_out.size(); i += 4) {
        const unsigned char b = rgba_out[i];
        rgba_out[i] = rgba_out[i + 2];
        rgba_out[i + 2] = b;
        if (rgba_out[i + 3] != 0) any_alpha = true;
    }
    // A legacy icon with no alpha channel reads back fully transparent; show it
    // opaque rather than invisible.
    if (!any_alpha) {
        for (std::size_t i = 0; i + 3 < rgba_out.size(); i += 4) rgba_out[i + 3] = 255;
    }

    w_out = w;
    h_out = h;
    return true;
}

ID3D11ShaderResourceView* create_texture(ID3D11Device* device,
                                         const std::vector<unsigned char>& rgba, UINT w, UINT h,
                                         ID3D11Texture2D** out_tex) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba.data();
    init.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &init, &tex))) {
        spdlog::warn("icons: CreateTexture2D failed");
        return nullptr;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
        spdlog::warn("icons: CreateShaderResourceView failed");
        tex->Release();
        return nullptr;
    }
    *out_tex = tex;
    return srv;
}

}  // namespace

void load(ID3D11Device* device) {
    if (device == nullptr) {
        return;
    }

    if (kGithubIconPngLen != 0) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool co_owned = SUCCEEDED(co);
        std::vector<unsigned char> rgba;
        UINT w = 0, h = 0;
        const bool ok = decode_png(kGithubIconPng, kGithubIconPngLen, rgba, w, h);
        if (co_owned) {
            CoUninitialize();
        }
        if (ok) {
            g_github_srv = create_texture(device, rgba, w, h, &g_github_tex);
        }
    }

    {
        std::vector<unsigned char> rgba;
        UINT w = 0, h = 0;
        if (load_app_icon_rgba(rgba, w, h)) {
            g_app_srv = create_texture(device, rgba, w, h, &g_app_tex);
        }
    }
}

void shutdown() {
    if (g_github_srv) {
        g_github_srv->Release();
        g_github_srv = nullptr;
    }
    if (g_github_tex) {
        g_github_tex->Release();
        g_github_tex = nullptr;
    }
    if (g_app_srv) {
        g_app_srv->Release();
        g_app_srv = nullptr;
    }
    if (g_app_tex) {
        g_app_tex->Release();
        g_app_tex = nullptr;
    }
}

ImTextureID github() { return reinterpret_cast<ImTextureID>(g_github_srv); }
ImTextureID app_icon() { return reinterpret_cast<ImTextureID>(g_app_srv); }

}  // namespace sam::ui::icons
