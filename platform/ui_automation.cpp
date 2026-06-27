#include "platform/ui_automation.hpp"

#include <thread>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <wrl/client.h>

#include "core/log.hpp"

namespace sam::platform::uia {

namespace {

using Microsoft::WRL::ComPtr;

// RAII for BSTR returned from UIA.
struct BstrHolder {
    BSTR b = nullptr;
    ~BstrHolder() { if (b) SysFreeString(b); }
    std::wstring to_wstring() const {
        if (!b) return {};
        return std::wstring(b, SysStringLen(b));
    }
};

std::wstring read_bstr_property(IUIAutomationElement* elem,
                                 HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*)) {
    if (!elem) return {};
    BstrHolder h;
    if (FAILED((elem->*getter)(&h.b))) return {};
    return h.to_wstring();
}

}  // namespace

Session::Session() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        com_owned_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // COM already initialised on this thread with a different apartment;
        // UIA calls still work, we just don't own teardown.
        com_owned_ = false;
    } else {
        SAM_LOG_ERROR("uia: CoInitializeEx failed: 0x{:08x}",
                      static_cast<unsigned>(hr));
        return;
    }

    ComPtr<IUIAutomation> a;
    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&a));
    if (FAILED(hr) || !a) {
        SAM_LOG_ERROR("uia: CoCreateInstance(CUIAutomation) failed: 0x{:08x}",
                      static_cast<unsigned>(hr));
        if (com_owned_) {
            CoUninitialize();
            com_owned_ = false;
        }
        return;
    }
    root_ = a.Detach();
}

Session::~Session() {
    if (root_) {
        root_->Release();
        root_ = nullptr;
    }
    if (com_owned_) {
        CoUninitialize();
    }
}

std::optional<Session::Element> Session::from_hwnd(HWND hwnd) {
    if (!root_ || !hwnd) return std::nullopt;
    IUIAutomationElement* e = nullptr;
    if (FAILED(root_->ElementFromHandle(hwnd, &e)) || !e) {
        return std::nullopt;
    }
    return Element(root_, e);   // Element takes ownership of the ref
}

Session::Element::Element(IUIAutomation* automation, IUIAutomationElement* raw) noexcept
    : automation_(automation), raw_(raw) {}

Session::Element::~Element() { release_(); }

Session::Element::Element(const Element& o)
    : automation_(o.automation_), raw_(o.raw_) { retain_(); }

Session::Element& Session::Element::operator=(const Element& o) {
    if (this == &o) return *this;
    release_();
    automation_ = o.automation_;
    raw_ = o.raw_;
    retain_();
    return *this;
}

Session::Element::Element(Element&& o) noexcept
    : automation_(o.automation_), raw_(o.raw_) {
    o.automation_ = nullptr;
    o.raw_ = nullptr;
}

Session::Element& Session::Element::operator=(Element&& o) noexcept {
    if (this == &o) return *this;
    release_();
    automation_ = o.automation_;
    raw_ = o.raw_;
    o.automation_ = nullptr;
    o.raw_ = nullptr;
    return *this;
}

void Session::Element::release_() {
    if (raw_) {
        raw_->Release();
        raw_ = nullptr;
    }
}

void Session::Element::retain_() {
    if (raw_) raw_->AddRef();
}

int Session::Element::control_type() const {
    if (!raw_) return 0;
    CONTROLTYPEID t = 0;
    if (FAILED(raw_->get_CurrentControlType(&t))) return 0;
    return static_cast<int>(t);
}

std::wstring Session::Element::name() const {
    return read_bstr_property(raw_, &IUIAutomationElement::get_CurrentName);
}

std::wstring Session::Element::class_name() const {
    return read_bstr_property(raw_, &IUIAutomationElement::get_CurrentClassName);
}

bool Session::Element::is_enabled() const {
    if (!raw_) return false;
    BOOL enabled = FALSE;
    if (FAILED(raw_->get_CurrentIsEnabled(&enabled))) return false;
    return enabled != FALSE;
}

void Session::Element::focus() noexcept {
    if (raw_) raw_->SetFocus();
}

std::optional<Session::Element>
Session::Element::first_descendant_by_control_type(int ct) const {
    if (!raw_ || !automation_) return std::nullopt;

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = ct;

    ComPtr<IUIAutomationCondition> cond;
    HRESULT hr = automation_->CreatePropertyCondition(
        UIA_ControlTypePropertyId, v, &cond);
    VariantClear(&v);
    if (FAILED(hr) || !cond) return std::nullopt;

    IUIAutomationElement* found = nullptr;
    hr = raw_->FindFirst(TreeScope_Descendants, cond.Get(), &found);
    if (FAILED(hr) || !found) return std::nullopt;
    return Element(automation_, found);
}

std::vector<Session::Element> Session::Element::all_children() const {
    std::vector<Element> out;
    if (!raw_ || !automation_) return out;

    ComPtr<IUIAutomationCondition> truecond;
    if (FAILED(automation_->CreateTrueCondition(&truecond)) || !truecond) {
        return out;
    }

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(raw_->FindAll(TreeScope_Children, truecond.Get(), &arr)) || !arr) {
        return out;
    }

    int n = 0;
    if (FAILED(arr->get_Length(&n))) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* e = nullptr;
        if (SUCCEEDED(arr->GetElement(i, &e)) && e) {
            out.emplace_back(automation_, e);
        }
    }
    return out;
}

std::vector<Session::Element>
Session::Element::children_by_control_type(int ct) const {
    std::vector<Element> out;
    if (!raw_ || !automation_) return out;

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = ct;

    ComPtr<IUIAutomationCondition> cond;
    HRESULT hr = automation_->CreatePropertyCondition(
        UIA_ControlTypePropertyId, v, &cond);
    VariantClear(&v);
    if (FAILED(hr) || !cond) return out;

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(raw_->FindAll(TreeScope_Children, cond.Get(), &arr)) || !arr) {
        return out;
    }
    int n = 0;
    if (FAILED(arr->get_Length(&n))) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* e = nullptr;
        if (SUCCEEDED(arr->GetElement(i, &e)) && e) {
            out.emplace_back(automation_, e);
        }
    }
    return out;
}

std::optional<Session::Element>
Session::Element::first_child_by_control_type(int ct) const {
    if (!raw_ || !automation_) return std::nullopt;

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = ct;

    ComPtr<IUIAutomationCondition> cond;
    HRESULT hr = automation_->CreatePropertyCondition(
        UIA_ControlTypePropertyId, v, &cond);
    VariantClear(&v);
    if (FAILED(hr) || !cond) return std::nullopt;

    IUIAutomationElement* found = nullptr;
    hr = raw_->FindFirst(TreeScope_Children, cond.Get(), &found);
    if (FAILED(hr) || !found) return std::nullopt;
    return Element(automation_, found);
}

bool Session::Element::set_value(std::wstring_view text) {
    if (!raw_) return false;

    ComPtr<IUIAutomationValuePattern> vp;
    HRESULT hr = raw_->GetCurrentPatternAs(
        UIA_ValuePatternId, IID_PPV_ARGS(&vp));
    if (FAILED(hr) || !vp) {
        SAM_LOG_DEBUG("uia: element does not support ValuePattern");
        return false;
    }

    // SysAllocStringLen takes a length (no terminator required in src).
    BSTR b = SysAllocStringLen(text.data(),
                                static_cast<UINT>(text.size()));
    if (!b) return false;
    hr = vp->SetValue(b);
    SysFreeString(b);
    return SUCCEEDED(hr);
}

bool Session::Element::invoke() {
    if (!raw_) return false;
    ComPtr<IUIAutomationInvokePattern> ip;
    HRESULT hr = raw_->GetCurrentPatternAs(
        UIA_InvokePatternId, IID_PPV_ARGS(&ip));
    if (FAILED(hr) || !ip) return false;
    return SUCCEEDED(ip->Invoke());
}

std::optional<bool> Session::Element::toggle_state() const {
    if (!raw_) return std::nullopt;
    ComPtr<IUIAutomationTogglePattern> tp;
    HRESULT hr = raw_->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&tp));
    if (FAILED(hr) || !tp) return std::nullopt;
    ToggleState st = ToggleState_Indeterminate;
    if (FAILED(tp->get_CurrentToggleState(&st))) return std::nullopt;
    if (st == ToggleState_On) return true;
    if (st == ToggleState_Off) return false;
    return std::nullopt;
}

bool Session::Element::toggle() {
    if (!raw_) return false;
    ComPtr<IUIAutomationTogglePattern> tp;
    HRESULT hr = raw_->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&tp));
    if (FAILED(hr) || !tp) return false;
    return SUCCEEDED(tp->Toggle());
}

bool Session::Element::wait_until_enabled(std::chrono::milliseconds timeout) {
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + timeout;
    while (clk::now() < deadline) {
        if (is_enabled()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return is_enabled();
}

}  // namespace sam::platform::uia
