// firmware: PSRAM allocator hooks (adapted from earlier internal
// firmware; renamed into the mwf namespace).
//
// Portable seam that puts a WORKING SET (e.g. replay history buffers) in the 8 MB
// PSRAM instead of the ~300 KB internal SRAM. Portable code can't call
// heap_caps_malloc, so allocations route through a pair of global function-pointer
// HOOKS that default to malloc/free (host tests + no-PSRAM boards) and are overridden
// by the device at boot with PSRAM-backed versions. Install the device hooks BEFORE
// the first working-set allocation so the whole set lands in PSRAM.
#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

namespace mwf {

using AllocFn = void* (*)(std::size_t);
using FreeFn  = void  (*)(void*);

// Install PSRAM-backed hooks (device) or leave the malloc/free default (host). Call
// once at boot. Passing null for either restores the default.
void  setWorkingAllocators(AllocFn a, FreeFn f);
void* workingAlloc(std::size_t n);
void  workingFree(void* p);

// Stateless STL allocator that routes through the working-set hooks. Host-safe
// (default hook = malloc), so containers using it run unchanged in tests and get
// PSRAM residency for free on device.
template <class T>
struct WorkingAllocator {
  using value_type = T;
  WorkingAllocator() noexcept = default;
  template <class U>
  WorkingAllocator(const WorkingAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    void* p = workingAlloc(n * sizeof(T));
    if (!p) {
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)
      throw std::bad_alloc();  // host: standard container contract
#else
      std::abort();            // device (-fno-exceptions): OOM is fatal, like operator new
#endif
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { workingFree(p); }

  template <class U> bool operator==(const WorkingAllocator<U>&) const noexcept { return true; }
  template <class U> bool operator!=(const WorkingAllocator<U>&) const noexcept { return false; }
};

}  // namespace mwf
