#pragma once

#include <string_view>

namespace sam::sda {

// Known field-encryption keys for the info.dat format. Each released build of
// the export tool bakes its key as a string literal into the binary; this list
// covers the keys we have observed across published builds. try_unwrap_field()
// walks the user-supplied key (if any) first, then iterates these in order.
//
// Adding a new release is a one-line append.
inline constexpr std::string_view kKnownInfoDatKeys[] = {
    "ImnsUDFnXghRF",
    // Open-source placeholder. Rebuilds that forgot to change the constant
    // end up using this verbatim.
    "PRIVATE_KEY",
};

}  // namespace sam::sda
