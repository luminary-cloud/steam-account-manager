#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare just enough of the Win32 + UIA surface to keep
// <windows.h> and <UIAutomationClient.h> out of every translation unit
// that wants to drive UI elements.
struct HWND__;
typedef HWND__* HWND;
struct IUIAutomation;
struct IUIAutomationElement;

namespace sam::platform::uia {

// COM apartment + IUIAutomation lifetime. Construct ONE per worker thread,
// keep it for as long as you intend to use any Element it hands you. All
// Elements must be used on the same thread that created the Session
// (apartment-threaded model).
//
// Construction may fail (CoInitialize race, missing UIAutomationCore.dll,
// elevation mismatch). Always check `ok()` before using.
class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    bool ok() const noexcept { return root_ != nullptr; }

    class Element;

    // Wraps the UIA root element for the given window. Returns nullopt if
    // the HWND is invalid or UIA refused (e.g. the window was destroyed
    // between the EnumWindows check and now).
    std::optional<Element> from_hwnd(HWND hwnd);

private:
    bool com_owned_ = false;
    IUIAutomation* root_ = nullptr;
};

// Owning wrapper around IUIAutomationElement. Copy = AddRef; move = transfer.
// Methods are no-ops on a default-constructed (invalid) Element.
class Session::Element {
public:
    Element() noexcept = default;
    Element(IUIAutomation* automation, IUIAutomationElement* raw) noexcept;
    ~Element();

    Element(const Element&);
    Element& operator=(const Element&);
    Element(Element&&) noexcept;
    Element& operator=(Element&&) noexcept;

    bool valid() const noexcept { return raw_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // UIA_*ControlTypeId; 0 if the call fails.
    int control_type() const;
    std::wstring name() const;
    std::wstring class_name() const;
    bool is_enabled() const;

    // SetFocus. Silently no-ops if the element is invalid.
    void focus() noexcept;

    // Tree walks use the Control view (the same view FlaUI uses by default,
    // which filters out the noise the raw view exposes).
    std::optional<Element> first_descendant_by_control_type(int control_type_id) const;
    std::vector<Element>   all_children() const;
    std::vector<Element>   children_by_control_type(int control_type_id) const;
    std::optional<Element> first_child_by_control_type(int control_type_id) const;

    // Pattern actions. Return false if the element doesn't support the
    // pattern or the call errored.
    bool set_value(std::wstring_view text);   // ValuePattern::SetValue
    bool invoke();                             // InvokePattern::Invoke

    // Polls is_enabled() every ~10ms until true or timeout. Returns final state.
    bool wait_until_enabled(std::chrono::milliseconds timeout);

private:
    void release_();
    void retain_();

    IUIAutomation*        automation_ = nullptr;  // non-owning; tied to Session
    IUIAutomationElement* raw_        = nullptr;  // owning (AddRef/Release)
};

}  // namespace sam::platform::uia
