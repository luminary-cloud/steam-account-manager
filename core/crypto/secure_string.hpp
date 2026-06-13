#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace sam::crypto {

// Zeroes the memory it owns when freed.
template <class T>
struct SecureAllocator {
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::true_type;

    SecureAllocator() noexcept = default;

    template <class U>
    SecureAllocator(const SecureAllocator<U>&) noexcept {}

    T* allocate(size_type n) {
        if (n == 0) return nullptr;
        void* p = ::operator new(n * sizeof(T));
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type n) noexcept {
        if (!p) return;
        // Volatile write so the compiler can't optimize the zeroing away
        // (memset_s isn't on MSVC); this is what SecureZeroMemory does.
        volatile unsigned char* v = reinterpret_cast<volatile unsigned char*>(p);
        const std::size_t bytes = n * sizeof(T);
        for (std::size_t i = 0; i < bytes; ++i) {
            v[i] = 0;
        }
        ::operator delete(p);
    }

    template <class U>
    bool operator==(const SecureAllocator<U>&) const noexcept {
        return true;
    }

    template <class U>
    bool operator!=(const SecureAllocator<U>&) const noexcept {
        return false;
    }
};

using SecureString = std::basic_string<char, std::char_traits<char>, SecureAllocator<char>>;
using SecureBytes = std::basic_string<std::uint8_t, std::char_traits<std::uint8_t>,
                                       SecureAllocator<std::uint8_t>>;

inline SecureString make_secure(std::string_view s) {
    return SecureString(s.begin(), s.end());
}

inline std::string_view view(const SecureString& s) {
    return std::string_view(s.data(), s.size());
}

inline void zero_buffer(void* p, std::size_t n) noexcept {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) v[i] = 0;
}

}  // namespace sam::crypto
