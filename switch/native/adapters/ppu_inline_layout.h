#ifndef NATIVE_PPU_INLINE_LAYOUT_H
#define NATIVE_PPU_INLINE_LAYOUT_H
/* ppu-speed-20261113 item 2: the one number the inline loads of the generated game code and the memory owner must agree on.
 * The generated code receives a ppu_context*; every such context is the first member of a PpuMemoryExecution, whose last member
 * is the per-context access counter (inline_tick, uint32_t) at this byte offset. ppu_memory_context.h static_asserts it. */
#define PPU_INLINE_TICK_OFFSET 1208
#include <stdint.h>
typedef struct ppu_inline_window {
    const unsigned char *bitmap;   /* commit bitmap of the flat guest window, one bit per 4 KiB page */
    uintptr_t base;                /* the window's base address */
} ppu_inline_window;
#endif
