#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

// Detours the function at `target` to `detour`. The first `stolen` bytes (5..16) must equal `expected` and be
// whole instructions without relative addressing; they move to a trampoline that then jumps back into the
// function. `*original` is set to the trampoline (call it as the original) before the patch goes live, since the
// detour can run at once. Returns false, leaving the function untouched, if the bytes differ or patching fails.
bool Install(uintptr_t target, const uint8_t* expected, size_t stolen, void* detour, void** original);

}  // namespace hook
