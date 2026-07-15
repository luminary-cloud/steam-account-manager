#include "ui/widgets/avatar_cache.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <stb_image.h>

#include "app/job_pump.hpp"
#include "core/http/client.hpp"

namespace sam::ui::widgets {

namespace {

enum class State {
    Idle,
    Pending,
    Ready,
    Failed,
};

struct Entry {
    State state = State::Idle;
    ID3D11ShaderResourceView* srv = nullptr;
};

std::mutex g_mtx;
std::unordered_map<std::string, Entry> g_cache;
ID3D11Device* g_device = nullptr;

ID3D11ShaderResourceView* create_srv(const std::vector<std::uint8_t>& bytes) {
    if (!g_device || bytes.empty()) return nullptr;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(),
                                                  static_cast<int>(bytes.size()),
                                                  &w, &h, &comp, STBI_rgb_alpha);
    if (!pixels) return nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels;
    init.SysMemPitch = static_cast<UINT>(w * 4);

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, &init, &tex)) && tex) {
        g_device->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    stbi_image_free(pixels);
    return srv;
}

}  // namespace

namespace avatar_cache {

void init(void* d3d_device) {
    g_device = static_cast<ID3D11Device*>(d3d_device);
}

void shutdown() {
    std::lock_guard lk(g_mtx);
    for (auto& [url, e] : g_cache) {
        if (e.srv) e.srv->Release();
    }
    g_cache.clear();
    g_device = nullptr;
}

}  // namespace avatar_cache

ID3D11ShaderResourceView* texture_for(std::string_view url) {
    if (url.empty()) return nullptr;
    const std::string key(url);

    {
        std::lock_guard lk(g_mtx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            return it->second.state == State::Ready ? it->second.srv : nullptr;
        }
        g_cache[key].state = State::Pending;
    }

    app::job_pump::submit([key] {
        http::Request req;
        req.method = http::Method::Get;
        req.url = key;
        req.timeout_seconds = 12;
        req.user_agent = "sam/0.1 (+avatar-cache)";
        auto resp = http::request(req);

        std::vector<std::uint8_t> body(resp.body.begin(), resp.body.end());

        std::lock_guard lk(g_mtx);
        auto& entry = g_cache[key];
        if (resp.status == 200 && !body.empty()) {
            entry.srv = create_srv(body);
            entry.state = entry.srv ? State::Ready : State::Failed;
        } else {
            entry.state = State::Failed;
        }
    });

    return nullptr;
}

ID3D11ShaderResourceView* texture_for_file(std::string_view abs_path) {
    if (abs_path.empty()) return nullptr;
    // Prefix keeps local paths from colliding with URL keys and hitting the HTTP path.
    const std::string key = "file://" + std::string(abs_path);
    const std::string path(abs_path);

    {
        std::lock_guard lk(g_mtx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            return it->second.state == State::Ready ? it->second.srv : nullptr;
        }
        g_cache[key].state = State::Pending;
    }

    app::job_pump::submit([key, path] {
        std::vector<std::uint8_t> body;
        {
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            if (in) {
                body.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
            }
        }

        std::lock_guard lk(g_mtx);
        auto& entry = g_cache[key];
        if (!body.empty()) {
            entry.srv = create_srv(body);
            entry.state = entry.srv ? State::Ready : State::Failed;
        } else {
            entry.state = State::Failed;
        }
    });

    return nullptr;
}

}  // namespace sam::ui::widgets
