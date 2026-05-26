#include "ui/icons.hpp"

#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <cstddef>
#include <vector>

namespace sam::ui::icons {

extern const unsigned char* const kGithubIconPng;
extern const std::size_t kGithubIconPngLen;

namespace {

using Microsoft::WRL::ComPtr;

ID3D11ShaderResourceView* g_github_srv = nullptr;
ID3D11Texture2D* g_github_tex = nullptr;

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

}  // namespace

void load(ID3D11Device* device) {
    if (device == nullptr || kGithubIconPngLen == 0) {
        return;
    }
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool co_owned = SUCCEEDED(co);

    std::vector<unsigned char> rgba;
    UINT w = 0, h = 0;
    bool ok = decode_png(kGithubIconPng, kGithubIconPngLen, rgba, w, h);
    if (co_owned) {
        CoUninitialize();
    }
    if (!ok) {
        return;
    }

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

    if (FAILED(device->CreateTexture2D(&desc, &init, &g_github_tex))) {
        spdlog::warn("icons: CreateTexture2D failed");
        return;
    }
    if (FAILED(device->CreateShaderResourceView(g_github_tex, nullptr, &g_github_srv))) {
        spdlog::warn("icons: CreateShaderResourceView failed");
        g_github_tex->Release();
        g_github_tex = nullptr;
        return;
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
}

ImTextureID github() { return reinterpret_cast<ImTextureID>(g_github_srv); }

}  // namespace sam::ui::icons
