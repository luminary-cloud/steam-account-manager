#include "platform/file_dialog.hpp"

#include <array>
#include <memory>

#include <objbase.h>
#include <shobjidl.h>

#pragma comment(lib, "ole32.lib")

namespace sam::platform::file_dialog {

namespace {

// COINIT_DISABLE_OLE1DDE matches what the Windows file-dialog samples use and
// avoids the legacy DDE broadcast that file dialogs would otherwise fire.
struct ComScope {
    HRESULT hr = E_FAIL;
    ComScope() {
        hr = CoInitializeEx(nullptr,
                            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

const std::array<COMDLG_FILTERSPEC, 1> kFallbackFilters{{
    {L"All files (*.*)", L"*.*"},
}};

template <typename T>
struct Releaser {
    void operator()(T* p) const { if (p) p->Release(); }
};

template <typename T>
using ComPtr = std::unique_ptr<T, Releaser<T>>;

bool item_to_path(IShellItem* item, std::filesystem::path& out) {
    PWSTR psz = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) || !psz) return false;
    out = std::filesystem::path(psz);
    CoTaskMemFree(psz);
    return true;
}

Result run_dialog(IFileDialog* dlg, const Options& opts) {
    Result r;

    // Filter strings are backed by the caller's Options, so pass pointers directly.
    std::vector<COMDLG_FILTERSPEC> com_filters;
    if (!opts.filters.empty()) {
        com_filters.reserve(opts.filters.size());
        for (const auto& f : opts.filters) {
            com_filters.push_back({f.description.c_str(), f.pattern.c_str()});
        }
        dlg->SetFileTypes(static_cast<UINT>(com_filters.size()), com_filters.data());
    } else {
        dlg->SetFileTypes(static_cast<UINT>(kFallbackFilters.size()),
                          kFallbackFilters.data());
    }
    dlg->SetFileTypeIndex(1);
    if (!opts.default_extension.empty()) {
        dlg->SetDefaultExtension(opts.default_extension.c_str());
    }
    if (!opts.title.empty()) {
        dlg->SetTitle(opts.title.c_str());
    }
    if (!opts.default_filename.empty()) {
        dlg->SetFileName(opts.default_filename.c_str());
    }

    HRESULT hr = dlg->Show(opts.parent);
    if (FAILED(hr)) {
        return r;  // cancelled or failed
    }

    if (opts.allow_multiselect) {
        IFileOpenDialog* open_raw = nullptr;
        if (FAILED(dlg->QueryInterface(IID_PPV_ARGS(&open_raw))) || !open_raw) {
            return r;
        }
        ComPtr<IFileOpenDialog> open(open_raw);

        IShellItemArray* arr_raw = nullptr;
        if (FAILED(open->GetResults(&arr_raw)) || !arr_raw) {
            return r;
        }
        ComPtr<IShellItemArray> arr(arr_raw);

        DWORD count = 0;
        if (FAILED(arr->GetCount(&count)) || count == 0) {
            return r;
        }
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item_raw = nullptr;
            if (FAILED(arr->GetItemAt(i, &item_raw)) || !item_raw) continue;
            ComPtr<IShellItem> item(item_raw);
            std::filesystem::path p;
            if (item_to_path(item.get(), p)) r.paths.push_back(std::move(p));
        }
    } else {
        IShellItem* item_raw = nullptr;
        if (FAILED(dlg->GetResult(&item_raw)) || !item_raw) {
            return r;
        }
        ComPtr<IShellItem> item(item_raw);
        std::filesystem::path p;
        if (item_to_path(item.get(), p)) r.paths.push_back(std::move(p));
    }

    if (r.paths.empty()) return r;
    r.path = r.paths.front();
    r.ok = true;
    return r;
}

}  // namespace

Result open_file(const Options& opts) {
    ComScope com;
    if (FAILED(com.hr)) return {};

    IFileOpenDialog* dlg_raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg_raw))) || !dlg_raw) {
        return {};
    }
    ComPtr<IFileOpenDialog> dlg(dlg_raw);

    DWORD flags = 0;
    dlg->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST |
             FOS_NOCHANGEDIR;
    if (opts.allow_multiselect) flags |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(flags);

    return run_dialog(dlg.get(), opts);
}

Result save_file(const Options& opts) {
    ComScope com;
    if (FAILED(com.hr)) return {};

    IFileSaveDialog* dlg_raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg_raw))) || !dlg_raw) {
        return {};
    }
    ComPtr<IFileSaveDialog> dlg(dlg_raw);

    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT |
                            FOS_NOCHANGEDIR);

    return run_dialog(dlg.get(), opts);
}

Result pick_folder(const Options& opts) {
    ComScope com;
    if (FAILED(com.hr)) return {};

    IFileOpenDialog* dlg_raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg_raw))) || !dlg_raw) {
        return {};
    }
    ComPtr<IFileOpenDialog> dlg(dlg_raw);

    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);

    if (!opts.title.empty()) {
        dlg->SetTitle(opts.title.c_str());
    }

    Result r;
    if (FAILED(dlg->Show(opts.parent))) return r;

    IShellItem* item_raw = nullptr;
    if (FAILED(dlg->GetResult(&item_raw)) || !item_raw) return r;
    ComPtr<IShellItem> item(item_raw);

    PWSTR psz = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) || !psz) return r;
    r.path = std::filesystem::path(psz);
    CoTaskMemFree(psz);
    r.ok = true;
    return r;
}

}  // namespace sam::platform::file_dialog
