#pragma once

#include <cstdint>

namespace scanner {

uintptr_t find_pattern(const char* module_name, const char* pattern);

uintptr_t resolve_call(uintptr_t address);

void* vtable_swap(void* vtable_base, int index, void* new_func);

}  // namespace scanner
