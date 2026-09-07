#pragma once
#include <cstdlib>
// The game asks for internal SRAM first and PSRAM as a fallback. On the host
// there is only one heap, so SPIRAM requests fail and the internal path wins
// -- the same branch the board takes.
#define MALLOC_CAP_INTERNAL 0x01
#define MALLOC_CAP_8BIT     0x02
#define MALLOC_CAP_SPIRAM   0x04
inline void *heap_caps_malloc(size_t n, uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? nullptr : malloc(n);
}
