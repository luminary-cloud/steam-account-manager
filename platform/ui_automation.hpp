#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations keep <windows.h> and <UIAutomationClient.h> out of TUs.
struct HWND__;
typedef HWND__* HWND;
struct IUIAutomation;
struct IUIAutomationElement;

namespace sam::platform::uia {

// COM apartment + IUIAutomation lifetime. Construct ONE per worker thread and
// use all its Elements on that same thread (apartment-threaded model).
// Construction may fail (CoInitialize race, missing UIAutomationCore.dll,
// elevation mismatch); check ok() before use.
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

    // nullopt if the HWND is invalid or UIA refused (e.g. window already destroyed).
    std::optional<Element> from_hwnd(HWND hwnd);

private:
    bool com_owned_ = false;
    IUIAutomation* root_ = nullptr;
};

// Owning wrapper around IUIAutomationElement. Copy = AddRef; move = transfer.
// Methods are no-ops on an invalid Element.
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

    void focus() noexcept;

    // Tree walks use the Control view (filters out raw-view noise).
    std::optional<Element> first_descendant_by_control_type(int control_type_id) const;
    std::vector<Element>   all_children() const;
    std::vector<Element>   children_by_control_type(int control_type_id) const;
    std::vector<Element>   descendants_by_control_type(int control_type_id) const;
    std::optional<Element> first_child_by_control_type(int control_type_id) const;

    // False if the pattern is unsupported or the call errored.
    bool set_value(std::wstring_view text);   // ValuePattern::SetValue
    bool invoke();                             // InvokePattern::Invoke
    // LegacyIAccessible::DoDefaultAction. Chromium exposes this on nodes whose
    // InvokePattern call fails, so it makes a useful second attempt.
    bool do_default_action();

    // TogglePattern (checkboxes). toggle_state() is nullopt when the pattern is
    // unsupported or the state is indeterminate; toggle() flips it.
    std::optional<bool> toggle_state() const;
    bool toggle();

    bool wait_until_enabled(std::chrono::milliseconds timeout);

private:
    void release_();
    void retain_();

    IUIAutomation*        automation_ = nullptr;  // non-owning; tied to Session
    IUIAutomationElement* raw_        = nullptr;  // owning (AddRef/Release)
};

}  // namespace sam::platform::uia
