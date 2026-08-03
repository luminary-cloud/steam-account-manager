#include "core/cleaner/execute.hpp"

#include <windows.h>

#include <filesystem>
#include <utility>

#include "core/cleaner/vdf_tree.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::cleaner {
namespace {

namespace fs = std::filesystem;

struct ParsedRegPath {
    HKEY root = nullptr;
    std::wstring subkey;
};

bool parse_reg_path(std::wstring_view full, ParsedRegPath& out) {
    const auto pos = full.find(L'\\');
    if (pos == std::wstring_view::npos) return false;
    const auto hive = full.substr(0, pos);
    const auto rest = full.substr(pos + 1);
    if (hive == L"HKCU" || hive == L"HKEY_CURRENT_USER") {
        out.root = HKEY_CURRENT_USER;
    } else if (hive == L"HKLM" || hive == L"HKEY_LOCAL_MACHINE") {
        out.root = HKEY_LOCAL_MACHINE;
    } else if (hive == L"HKCR" || hive == L"HKEY_CLASSES_ROOT") {
        out.root = HKEY_CLASSES_ROOT;
    } else if (hive == L"HKU" || hive == L"HKEY_USERS") {
        out.root = HKEY_USERS;
    } else {
        return false;
    }
    out.subkey = std::wstring{rest};
    return true;
}

vdf::Node* walk_to_parent(vdf::Node& root, const std::wstring& path, std::wstring& leaf) {
    auto* node = &root;
    std::wstring_view remaining{path};
    while (true) {
        const auto sep = remaining.find(L'\\');
        if (sep == std::wstring_view::npos) {
            leaf.assign(remaining);
            return node;
        }
        auto* next = node->find(remaining.substr(0, sep));
        if (next == nullptr || !next->is_object()) return nullptr;
        node = next;
        remaining.remove_prefix(sep + 1);
    }
}

bool execute_one(const PlanStep& step, std::wstring& err_out) {
    const auto& op = step.op;
    switch (op.kind) {
        case OpKind::RemoveFile: {
            const fs::path p{op.target};
            if (!platform::delete_file_forced(p)) {
                err_out = L"DeleteFile failed: " + p.wstring();
                return false;
            }
            return true;
        }
        case OpKind::RemoveTree: {
            const fs::path p{op.target};
            if (!platform::delete_directory_recursive(p)) {
                err_out = L"DeleteDirectory failed: " + p.wstring();
                return false;
            }
            return true;
        }
        case OpKind::RemoveRegistryValue: {
            ParsedRegPath rp;
            if (!parse_reg_path(op.target, rp)) {
                err_out = L"Invalid registry path: " + op.target;
                return false;
            }
            if (!platform::registry::delete_value(rp.root, rp.subkey, op.value_name)) {
                err_out = L"DeleteRegistryValue failed: " + op.target + L" :: " + op.value_name;
                return false;
            }
            return true;
        }
        case OpKind::RemoveRegistryKey: {
            ParsedRegPath rp;
            if (!parse_reg_path(op.target, rp)) {
                err_out = L"Invalid registry path: " + op.target;
                return false;
            }
            if (!platform::registry::delete_key_recursive(rp.root, rp.subkey)) {
                err_out = L"DeleteRegistryKey failed: " + op.target;
                return false;
            }
            return true;
        }
        case OpKind::WriteRegistryString: {
            ParsedRegPath rp;
            if (!parse_reg_path(op.target, rp)) {
                err_out = L"Invalid registry path: " + op.target;
                return false;
            }
            if (!platform::registry::write_string(rp.root, rp.subkey, op.value_name, op.payload)) {
                err_out = L"WriteRegistryString failed: " + op.target + L" :: " + op.value_name;
                return false;
            }
            return true;
        }
        case OpKind::VdfRemoveChild: {
            const fs::path p{op.target};
            auto doc = vdf::load(p);
            if (!doc) {
                err_out = L"VDF parse failed: " + p.wstring();
                return false;
            }
            if (!doc->root || !doc->root->is_object()) {
                err_out = L"VDF root is not an object: " + p.wstring();
                return false;
            }
            std::wstring leaf;
            auto* parent = walk_to_parent(*doc->root, op.value_name, leaf);
            if (parent == nullptr) return true;
            parent->remove(leaf);
            if (!vdf::save(*doc, p)) {
                err_out = L"VDF save failed: " + p.wstring();
                return false;
            }
            return true;
        }
        case OpKind::VdfSetValue: {
            const fs::path p{op.target};
            auto doc = vdf::load(p);
            if (!doc) {
                err_out = L"VDF parse failed: " + p.wstring();
                return false;
            }
            if (!doc->root || !doc->root->is_object()) {
                err_out = L"VDF root is not an object: " + p.wstring();
                return false;
            }
            std::wstring leaf;
            auto* parent = walk_to_parent(*doc->root, op.value_name, leaf);
            if (parent == nullptr) {

                SAM_LOG_WARN("cleaner: VdfSetValue path segment missing in {}", p.string());
                return true;
            }
            parent->set(leaf, std::make_unique<vdf::Node>(op.payload));
            if (!vdf::save(*doc, p)) {
                err_out = L"VDF save failed: " + p.wstring();
                return false;
            }
            return true;
        }
    }
    err_out = L"Unknown OpKind";
    return false;
}

}  // namespace

CleanResult execute(const Plan& plan, const CleanOptions& opts) {
    CleanResult result;
    const int total = static_cast<int>(plan.steps.size());
    int done = 0;

    for (const auto& step : plan.steps) {
        std::wstring err;
        if (execute_one(step, err)) {
            ++result.succeeded;
            result.bytes_freed += step.op.size_bytes;
        } else {
            ++result.failed;
            result.failure_messages.push_back(err);
            SAM_LOG_WARN("cleaner: FAIL [{}] {}", step.target_id,
                         fs::path{step.op.target}.string());
        }
        ++done;
        if (opts.on_progress) opts.on_progress(done, total);
    }
    return result;
}

}  // namespace sam::cleaner
