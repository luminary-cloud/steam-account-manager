#include "platform/clipboard.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <windows.h>

namespace sam::platform::clipboard {

namespace {

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool set_clipboard_text_internal(std::string_view utf8) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    const std::wstring wide = utf8_to_wide(utf8);
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
    std::memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

}  // namespace

bool set_text(std::string_view text) {
    return set_clipboard_text_internal(text);
}

void set_text_with_auto_clear(std::string_view text, std::chrono::seconds delay) {
    if (!set_clipboard_text_internal(text)) return;

    const DWORD seq = GetClipboardSequenceNumber();

    std::thread([seq, delay] {
        std::this_thread::sleep_for(delay);
        if (GetClipboardSequenceNumber() != seq) return;
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        CloseClipboard();
    }).detach();
}

}  // namespace sam::platform::clipboard
