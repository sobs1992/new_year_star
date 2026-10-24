// Replaces the global operator new / delete of the firmware. The star's own code is C, so
// this only moves the C++ objects of cspot and its glue into PSRAM (see cspot_alloc.h).
#include <cstdlib>
#include <new>

#include "cspot_alloc.h"

void* operator new(std::size_t size) {
  void* p = cspot_malloc(size ? size : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size) {
  return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return cspot_malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return cspot_malloc(size ? size : 1);
}

void operator delete(void* p) noexcept {
  free(p);
}

void operator delete[](void* p) noexcept {
  free(p);
}

void operator delete(void* p, std::size_t) noexcept {
  free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
  free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
  free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
  free(p);
}
