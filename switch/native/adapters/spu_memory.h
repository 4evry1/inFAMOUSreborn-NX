#ifndef PS3_NATIVE_SPU_MEMORY_H
#define PS3_NATIVE_SPU_MEMORY_H
#include "atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_SPU_LOCAL_STORE_BYTES = 256 * 1024 };
typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner;
    unsigned char *local_store;
} ps3_spu_memory;

/* Composition supplies a fresh zeroed binding and disjoint caller-owned 256 KiB
 * LS. Never copy/reinitialize a live binding. One binding is one serialized
 * execution agent. Retain binding/LS/domain/guard/backing until all users join;
 * release only after joining. No thread or MFC engine is constructed here.
 * Calls contain no cancellation points; poll only after they return. Provider
 * failures preserve LS/status outputs and never mean successful MFC completion.
 * This adapter does not implement channel queues, tags, events or guest errors. */
ps3_guest_memory_status ps3_spu_memory_init(ps3_spu_memory *, ps3_atomic_domain *, void *local_store, size_t bytes);
ps3_guest_memory_status ps3_spu_memory_release(ps3_spu_memory *);
ps3_guest_memory_status ps3_spu_memory_get(ps3_spu_memory *, uint64_t ea, size_t lsa, size_t bytes);
ps3_guest_memory_status ps3_spu_memory_put(ps3_spu_memory *, uint64_t ea, size_t lsa, size_t bytes);
/* Addresses are already128-byte aligned by the instruction/ABI adapter. These
 * are literal MFC atomic-stat values verified in the existing lifted runtime:
 * GETLLAR 4, PUTLLC success 0/failure 1, PUTLLUC 2. They are not job result codes.
 * Invalid PUTLLC LS/status arguments consume the valid owner's reservation.
 * The unconditional operation uses the coherent line-write contract; exact
 * instruction-specific self-event handling and queued PUTQLLUC are not provided. */
ps3_guest_memory_status ps3_spu_memory_getllar(ps3_spu_memory *, uint64_t ea, size_t lsa, unsigned *atomic_status);
ps3_guest_memory_status ps3_spu_memory_putllc(ps3_spu_memory *, uint64_t ea, size_t lsa, unsigned *atomic_status);
ps3_guest_memory_status ps3_spu_memory_putlluc(ps3_spu_memory *, uint64_t ea, size_t lsa, unsigned *atomic_status);
/* Generic loss indication only; the actual SPU event-bit adapter is not present. */
ps3_guest_memory_status ps3_spu_memory_take_reservation_loss(ps3_spu_memory *, int *lost);
#ifdef __cplusplus
}
#endif
#endif
