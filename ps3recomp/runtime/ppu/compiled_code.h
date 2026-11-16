#ifndef PS3_RUNTIME_PPU_COMPILED_CODE_H
#define PS3_RUNTIME_PPU_COMPILED_CODE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
struct ppu_context;
typedef void (*ps3_compiled_function)(struct ppu_context *cpu);
typedef enum ps3_compiled_code_status {
    PS3_COMPILED_CODE_OK = 0,
    PS3_COMPILED_CODE_INVALID_ARGUMENT,
    PS3_COMPILED_CODE_INVALID_TABLE,
    PS3_COMPILED_CODE_ALREADY_ADMITTED,
    PS3_COMPILED_CODE_NOT_ADMITTED,
    PS3_COMPILED_CODE_INVALID_ADDRESS,
    PS3_COMPILED_CODE_NOT_FOUND
} ps3_compiled_code_status;

/* The owner supplies a live immutable lookup. Address is the low 32 bits of
 * CTR, already decoded by the generated caller; it is never an OPD or host
 * pointer. Success returns a compiled target, without invoking it or modifying
 * CPU state. Every failure leaves the caller's target unchanged. No provider
 * guard or mutable lookup cache is required. Cancellation and depth admission
 * remain the execution owner's responsibility before calling the target. */
typedef struct ps3_compiled_code_lookup {
    const void *context;
    ps3_compiled_code_status (*find)(const void *context, uint32_t address,
        ps3_compiled_function *target);
} ps3_compiled_code_lookup;
#ifdef __cplusplus
}
#endif
#endif
