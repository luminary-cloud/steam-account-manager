#include "app/vault_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/app_state.hpp"
#include "core/account_store/store.hpp"
#include "core/log.hpp"
#include "platform/paths.hpp"

namespace sam::app {

namespace {

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void move_if_exists(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return;
    std::filesystem::rename(src, dst, ec);
    if (!ec) return;
    ec.clear();
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) std::filesystem::remove(src, ec);
}

void migrate_legacy_vault(const std::string& id) {
    const auto dir = vault_dir_for(id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto root = platform::data_dir();
    move_if_exists(root / L"vault.bin", dir / L"vault.bin");
    move_if_exists(root / L"master_pw.bin", dir / L"master_pw.bin");
    move_if_exists(root / L"notifications.json", dir / L"notifications.json");
    move_if_exists(root / L"conf_audit.json", dir / L"conf_audit.json");
    move_if_exists(root / L"trade_audit.json", dir / L"trade_audit.json");
    SAM_LOG_INFO("vaults: migrated legacy vault into {}", dir.string());
}

}  // namespace

VaultInfo* VaultRegistry::find(const std::string& id) {
    for (auto& v : vaults)
        if (v.id == id) return &v;
    return nullptr;
}

const VaultInfo* VaultRegistry::find(const std::string& id) const {
    for (const auto& v : vaults)
        if (v.id == id) return &v;
    return nullptr;
}

std::string new_vault_id() {

    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08llx",
                  static_cast<unsigned long long>(now_unix()),
                  static_cast<unsigned long long>(n));
    return buf;
}

std::filesystem::path vaults_json_path() {
    return platform::vaults_root() / L"registry.json";
}

std::filesystem::path vault_dir_for(const std::string& id) {
    return platform::vaults_root() / std::filesystem::path(id);
}

VaultRegistry load_registry() {
    VaultRegistry reg;
    std::ifstream in(vaults_json_path());
    if (!in) return reg;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return reg;
    }
    reg.version = j.value("version", 1);
    reg.auto_open_id = j.value("auto_open_id", std::string{});
    if (j.contains("vaults") && j["vaults"].is_array()) {
        for (const auto& e : j["vaults"]) {
            VaultInfo v;
            v.id = e.value("id", std::string{});
            if (v.id.empty()) continue;
            v.name = e.value("name", std::string{"Vault"});
            v.icon_file = e.value("icon_file", std::string{});
            v.color_rgba = e.value("color_rgba", 0x4F8EF7FFu);
            v.order = e.value("order", 0);
            v.last_opened_unix = e.value("last_opened_unix", std::int64_t{0});
            reg.vaults.push_back(std::move(v));
        }
    }
    return reg;
}

void save_registry(const VaultRegistry& reg) {
    nlohmann::json j;
    j["version"] = reg.version;
    j["auto_open_id"] = reg.auto_open_id;
    auto& arr = j["vaults"] = nlohmann::json::array();
    for (const auto& v : reg.vaults) {
        arr.push_back({
            {"id", v.id},
            {"name", v.name},
            {"icon_file", v.icon_file},
            {"color_rgba", v.color_rgba},
            {"order", v.order},
            {"last_opened_unix", v.last_opened_unix},
        });
    }
    const auto path = vaults_json_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (out) out << j.dump(4);
}

void add_vault_entry(VaultRegistry& reg, std::string id, std::string name,
                     std::uint32_t color) {
    VaultInfo v;
    v.id = std::move(id);
    v.name = std::move(name);
    v.color_rgba = color;
    int max_order = -1;
    for (const auto& e : reg.vaults) max_order = std::max(max_order, e.order);
    v.order = max_order + 1;
    reg.vaults.push_back(std::move(v));
    save_registry(reg);
}

void rename_vault(VaultRegistry& reg, const std::string& id, std::string name) {
    if (auto* v = reg.find(id)) {
        v->name = std::move(name);
        save_registry(reg);
    }
}

bool set_vault_icon(VaultRegistry& reg, const std::string& id,
                    const std::filesystem::path& src, std::string* err) {
    auto* v = reg.find(id);
    if (!v) {
        if (err) *err = "Unknown vault.";
        return false;
    }
    const auto dir = vault_dir_for(id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto dst = dir / L"icon.png";
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (err) *err = "Couldn't copy the image: " + ec.message();
        return false;
    }
    v->icon_file = "icon.png";
    save_registry(reg);
    return true;
}

void clear_vault_icon(VaultRegistry& reg, const std::string& id) {
    auto* v = reg.find(id);
    if (!v || v->icon_file.empty()) return;
    std::error_code ec;
    std::filesystem::remove(vault_dir_for(id) / std::filesystem::path(v->icon_file), ec);
    v->icon_file.clear();
    save_registry(reg);
}

bool remove_vault(VaultRegistry& reg, const std::string& id, std::string* err) {
    if (reg.vaults.size() <= 1) {
        if (err) *err = "You must keep at least one vault.";
        return false;
    }
    if (id == platform::active_vault_id()) {
        if (err) *err = "Switch to another vault before deleting this one.";
        return false;
    }
    auto it = std::find_if(reg.vaults.begin(), reg.vaults.end(),
                           [&](const VaultInfo& v) { return v.id == id; });
    if (it == reg.vaults.end()) {
        if (err) *err = "Unknown vault.";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(vault_dir_for(id), ec);
    reg.vaults.erase(it);
    if (reg.auto_open_id == id) reg.auto_open_id.clear();
    save_registry(reg);
    return true;
}

void set_auto_open(VaultRegistry& reg, const std::string& id) {
    reg.auto_open_id = id;
    save_registry(reg);
}

VaultResolution init_vaults(AppState& state, bool gui) {

    std::error_code ec;
    if (!std::filesystem::exists(vaults_json_path(), ec)) {
        VaultRegistry reg;
        if (std::filesystem::exists(platform::data_dir() / L"vault.bin", ec)) {
            const auto id = new_vault_id();
            migrate_legacy_vault(id);
            VaultInfo v;
            v.id = id;
            v.name = "My Vault";
            v.order = 0;
            reg.vaults.push_back(v);
        }
        save_registry(reg);
    }

    state.vault_registry = load_registry();
    auto& reg = state.vault_registry;

    if (gui) {
        const auto pending = platform::read_pending_vault();
        platform::clear_pending_vault();
        if (!pending.empty() && reg.find(pending)) {
            platform::set_active_vault_id(pending);
            return VaultResolution::OpenDirect;
        }
    }

    if (reg.vaults.empty()) {
        const auto id = new_vault_id();
        platform::set_active_vault_id(id);
        std::filesystem::create_directories(vault_dir_for(id), ec);
        return VaultResolution::CreateFirst;
    }

    if (reg.vaults.size() == 1) {
        platform::set_active_vault_id(reg.vaults.front().id);
        return VaultResolution::OpenDirect;
    }

    if (!reg.auto_open_id.empty() && reg.find(reg.auto_open_id)) {
        const bool has_cache = std::filesystem::exists(
            vault_dir_for(reg.auto_open_id) / L"master_pw.bin", ec);
        if (has_cache && state.settings.remember_master_password) {
            platform::set_active_vault_id(reg.auto_open_id);
            return VaultResolution::AutoUnlock;
        }
    }
    return VaultResolution::ShowPicker;
}

}  // namespace sam::app
