#include "core/cleaner/plan.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <set>
#include <vector>

#include "core/cleaner/vdf_tree.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::cleaner {
namespace {

namespace fs = std::filesystem;

std::uint32_t crc32_ieee(const char* data, std::size_t len) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::wstring connect_cache_key(std::wstring_view account_name) {
    std::string lower;
    lower.reserve(account_name.size());
    for (const wchar_t ch : account_name) lower.push_back(static_cast<char>(towlower(ch)));
    const auto crc = crc32_ieee(lower.data(), lower.size());
    wchar_t buf[20];
    std::swprintf(buf, 20, L"%x1", crc);
    return buf;
}

bool target_is_loginusers(std::string_view id) { return id == "steam.loginusers"; }
bool target_is_autologin(std::string_view id) { return id == "steam.reg.autologin"; }
bool target_is_remote_clients(std::string_view id) { return id == "steam.remoteclients"; }
bool target_is_config_vdf(std::string_view id) { return id == "steam.config_vdf"; }
bool target_is_local_vdf(std::string_view id) { return id == "steam.local_vdf"; }

void compute_size(Operation& op) {
    if (op.kind == OpKind::RemoveFile || op.kind == OpKind::RemoveTree) {
        op.size_bytes = platform::size_recursive(fs::path{op.target});
    }
}

bool preserve_filters_op(const Operation& op, const PreserveList* preserve) {
    if (preserve == nullptr) return false;
    return !op.account_steamid64.empty() && preserve->preserves(op.account_steamid64);
}

void rewrite_loginusers(std::vector<PlanStep>& steps, const std::string& target_id,
                        const ResolveContext& ctx, const PreserveList* preserve) {

    if (preserve == nullptr || preserve->empty()) return;

    const auto vdf_path = ctx.install.config_dir / "loginusers.vdf";
    std::erase_if(steps, [&](const PlanStep& s) {
        return s.target_id == target_id && s.op.kind == OpKind::RemoveFile;
    });

    for (const auto& acc : ctx.accounts) {
        if (preserve->preserves(acc.steamid64)) continue;
        Operation op;
        op.kind = OpKind::VdfRemoveChild;
        op.target = vdf_path.wstring();
        op.value_name = acc.steamid64;
        op.account_steamid64 = acc.steamid64;
        steps.push_back(PlanStep{target_id, std::move(op)});
    }
}

void strip_autologin_coupled_deletes(std::vector<PlanStep>& steps, const std::string& target_id) {
    std::erase_if(steps, [&](const PlanStep& s) {
        if (s.target_id != target_id || s.op.kind != OpKind::RemoveRegistryValue) return false;
        return s.op.value_name == L"AutoLoginUser" || s.op.value_name == L"RememberPassword" ||
               s.op.value_name == L"LastGameNameUsed";
    });
}

void push_vdf_set(std::vector<PlanStep>& steps, const std::string& target_id,
                  const fs::path& vdf_path, std::wstring_view subkey_path, std::wstring_view value,
                  std::wstring_view account_steamid64) {
    Operation op;
    op.kind = OpKind::VdfSetValue;
    op.target = vdf_path.wstring();
    op.value_name = std::wstring{subkey_path};
    op.payload = std::wstring{value};
    op.account_steamid64 = std::wstring{account_steamid64};
    steps.push_back(PlanStep{target_id, std::move(op)});
}

void rewrite_autologin(std::vector<PlanStep>& steps, const std::string& target_id,
                       const ResolveContext& ctx, const PreserveList* preserve) {

    if (preserve == nullptr || preserve->empty()) return;

    const auto current = platform::registry::read_string_hkcu(L"Software\\Valve\\Steam",
                                                              L"AutoLoginUser");
    if (!current || current->empty()) return;

    const auto sid = resolve_auto_login(ctx.install, *current);
    if (!sid.empty() && preserve->preserves(sid)) {

        strip_autologin_coupled_deletes(steps, target_id);
        return;
    }

    const auto redirect = pick_autologin_redirect(ctx.accounts, *preserve);
    strip_autologin_coupled_deletes(steps, target_id);

    if (!redirect) {
        SAM_LOG_WARN("cleaner: no preserved account has an AccountName in loginusers.vdf; "
                     "AutoLoginUser left as-is");
        return;
    }

    Operation reg_op;
    reg_op.kind = OpKind::WriteRegistryString;
    reg_op.target = L"HKCU\\Software\\Valve\\Steam";
    reg_op.value_name = L"AutoLoginUser";
    reg_op.payload = redirect->account_name;
    reg_op.account_steamid64 = redirect->steamid64;
    steps.push_back(PlanStep{target_id, std::move(reg_op)});

    const auto vdf_path = ctx.install.config_dir / "loginusers.vdf";
    const auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const auto ts_str = std::to_wstring(now_secs);

    push_vdf_set(steps, target_id, vdf_path, redirect->steamid64 + L"\\mostrecent", L"1",
                 redirect->steamid64);
    push_vdf_set(steps, target_id, vdf_path, redirect->steamid64 + L"\\MostRecent", L"1",
                 redirect->steamid64);
    push_vdf_set(steps, target_id, vdf_path, redirect->steamid64 + L"\\Timestamp", ts_str,
                 redirect->steamid64);

    for (const auto& acc : ctx.accounts) {
        if (acc.steamid64 == redirect->steamid64) continue;
        if (!preserve->preserves(acc.steamid64)) continue;
        push_vdf_set(steps, target_id, vdf_path, acc.steamid64 + L"\\mostrecent", L"0",
                     acc.steamid64);
        push_vdf_set(steps, target_id, vdf_path, acc.steamid64 + L"\\MostRecent", L"0",
                     acc.steamid64);
    }
}

void rewrite_remote_clients(std::vector<PlanStep>& steps, const std::string& target_id,
                            const PreserveList* preserve) {

    if (preserve == nullptr || preserve->empty()) return;
    std::erase_if(steps, [&](const PlanStep& s) { return s.target_id == target_id; });
}

std::set<std::wstring> preserved_connect_cache_keys(const ResolveContext& ctx,
                                                    const PreserveList& preserve) {
    std::set<std::wstring> keys;
    for (const auto& acc : ctx.accounts) {
        if (acc.account_name.empty()) continue;
        if (preserve.preserves(acc.steamid64)) keys.insert(connect_cache_key(acc.account_name));
    }
    return keys;
}

void rewrite_config_vdf(std::vector<PlanStep>& steps, const std::string& target_id,
                        const ResolveContext& ctx, const PreserveList* preserve) {
    if (preserve == nullptr || preserve->empty()) return;
    const auto vdf_path = ctx.install.config_dir / "config.vdf";
    auto doc = vdf::load(vdf_path);
    if (!doc || !doc->root || !doc->root->is_object()) return;

    auto* software = doc->root->find(L"Software");
    auto* valve = software != nullptr ? software->find(L"Valve") : nullptr;
    auto* steam_node = valve != nullptr ? valve->find(L"Steam") : nullptr;
    if (steam_node == nullptr || !steam_node->is_object()) return;

    std::erase_if(steps, [&](const PlanStep& s) {
        return s.target_id == target_id && s.op.kind == OpKind::RemoveFile;
    });

    if (auto* accounts_node = steam_node->find(L"Accounts");
        accounts_node != nullptr && accounts_node->is_object()) {
        for (const auto& entry : accounts_node->children()) {
            if (!entry.second || !entry.second->is_object()) continue;
            auto* sid_node = entry.second->find(L"SteamID");
            if (sid_node == nullptr || !sid_node->is_value()) continue;
            const std::wstring& sid64 = sid_node->value();
            if (preserve->preserves(sid64)) continue;
            Operation op;
            op.kind = OpKind::VdfRemoveChild;
            op.target = vdf_path.wstring();
            op.value_name = L"Software\\Valve\\Steam\\Accounts\\";
            op.value_name += entry.first;
            op.account_steamid64 = sid64;
            steps.push_back(PlanStep{target_id, std::move(op)});
        }
    }

    const auto preserved_keys = preserved_connect_cache_keys(ctx, *preserve);
    if (auto* cache_node = steam_node->find(L"ConnectCache");
        cache_node != nullptr && cache_node->is_object()) {
        for (const auto& entry : cache_node->children()) {
            const std::wstring& hex_key = entry.first;
            if (hex_key.empty() || preserved_keys.count(hex_key) != 0) continue;
            Operation op;
            op.kind = OpKind::VdfRemoveChild;
            op.target = vdf_path.wstring();
            op.value_name = L"Software\\Valve\\Steam\\ConnectCache\\";
            op.value_name += hex_key;
            steps.push_back(PlanStep{target_id, std::move(op)});
        }
    }
}

void rewrite_local_vdf(std::vector<PlanStep>& steps, const std::string& target_id,
                       const ResolveContext& ctx, const PreserveList* preserve) {
    if (preserve == nullptr || preserve->empty()) return;
    const auto vdf_path = ctx.install.local_vdf_path;
    auto doc = vdf::load(vdf_path);
    if (!doc || !doc->root || !doc->root->is_object()) return;

    auto* software = doc->root->find(L"Software");
    auto* valve = software != nullptr ? software->find(L"Valve") : nullptr;
    auto* steam_node = valve != nullptr ? valve->find(L"Steam") : nullptr;
    if (steam_node == nullptr || !steam_node->is_object()) return;

    auto* cache_node = steam_node->find(L"ConnectCache");
    if (cache_node == nullptr || !cache_node->is_object()) return;

    std::erase_if(steps, [&](const PlanStep& s) {
        return s.target_id == target_id && s.op.kind == OpKind::RemoveFile;
    });

    const auto preserved_keys = preserved_connect_cache_keys(ctx, *preserve);
    for (const auto& entry : cache_node->children()) {
        const std::wstring& hex_key = entry.first;
        if (hex_key.empty() || preserved_keys.count(hex_key) != 0) continue;
        Operation op;
        op.kind = OpKind::VdfRemoveChild;
        op.target = vdf_path.wstring();
        op.value_name = L"Software\\Valve\\Steam\\ConnectCache\\";
        op.value_name += hex_key;
        steps.push_back(PlanStep{target_id, std::move(op)});
    }
}

}  // namespace

std::optional<AutoLoginRedirect> pick_autologin_redirect(std::span<const AccountInfo> accounts,
                                                          const PreserveList& preserve) {
    const AccountInfo* fallback = nullptr;
    for (const auto& acc : accounts) {
        if (acc.account_name.empty()) continue;
        if (!preserve.preserves(acc.steamid64)) continue;
        if (acc.most_recent) return AutoLoginRedirect{acc.account_name, acc.steamid64};
        if (fallback == nullptr) fallback = &acc;
    }
    if (fallback != nullptr) return AutoLoginRedirect{fallback->account_name, fallback->steamid64};
    return std::nullopt;
}

Plan build_plan(std::span<const Target* const> targets, const ResolveContext& ctx,
                const PlanOptions& opts) {
    Plan plan;

    for (const Target* t : targets) {
        if (t == nullptr || !t->resolve) continue;

        std::vector<PlanStep> local;
        for (auto& op : t->resolve(ctx)) {
            if (preserve_filters_op(op, opts.preserve)) continue;
            local.push_back(PlanStep{t->id, std::move(op)});
        }

        if (target_is_loginusers(t->id)) rewrite_loginusers(local, t->id, ctx, opts.preserve);
        if (target_is_autologin(t->id)) rewrite_autologin(local, t->id, ctx, opts.preserve);
        if (target_is_remote_clients(t->id)) rewrite_remote_clients(local, t->id, opts.preserve);
        if (target_is_config_vdf(t->id)) rewrite_config_vdf(local, t->id, ctx, opts.preserve);
        if (target_is_local_vdf(t->id)) rewrite_local_vdf(local, t->id, ctx, opts.preserve);

        for (auto& step : local) {
            if (opts.measure) {
                compute_size(step.op);
                plan.total_bytes += step.op.size_bytes;
                if (step.op.kind == OpKind::RemoveFile) {
                    plan.total_file_count += 1;
                } else if (step.op.kind == OpKind::RemoveTree) {
                    plan.total_file_count += platform::file_count_recursive(fs::path{step.op.target});
                }
            }
            plan.steps.push_back(std::move(step));
        }
    }

    return plan;
}

Plan build_plan_by_ids(std::span<const std::string> target_ids, const ResolveContext& ctx,
                       const PlanOptions& opts) {
    std::vector<const Target*> resolved;
    resolved.reserve(target_ids.size());
    for (const auto& id : target_ids) {
        if (const auto* t = find_target(id)) resolved.push_back(t);
    }
    return build_plan(std::span<const Target* const>{resolved.data(), resolved.size()}, ctx, opts);
}

}  // namespace sam::cleaner
