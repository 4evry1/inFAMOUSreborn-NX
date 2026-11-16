#include "spu_memory.h"

enum { SPU_ATOMIC_CONDITIONAL_SUCCESS = 0, SPU_ATOMIC_CONDITIONAL_FAILURE = 1,
       SPU_ATOMIC_UNCONDITIONAL_COMPLETE = 2, SPU_ATOMIC_RESERVE_COMPLETE = 4 };
static int valid_binding(const ps3_spu_memory *memory) {
    return memory && memory->domain && memory->local_store && memory->owner.domain == memory->domain;
}
static int valid_ls(size_t lsa, size_t bytes) {
    return lsa <= PS3_SPU_LOCAL_STORE_BYTES && bytes <= PS3_SPU_LOCAL_STORE_BYTES - lsa;
}
static int valid_atomic_ls(size_t lsa) {
    return lsa % PS3_RESERVATION_GRANULE_BYTES == 0 && valid_ls(lsa, PS3_RESERVATION_GRANULE_BYTES);
}
ps3_guest_memory_status ps3_spu_memory_init(ps3_spu_memory *memory, ps3_atomic_domain *domain,
                                           void *local_store, size_t bytes) {
    if (!memory || !domain || !local_store || bytes != PS3_SPU_LOCAL_STORE_BYTES)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (memory->domain || memory->owner.domain || memory->local_store) return PS3_GUEST_MEMORY_BUSY;
    ps3_atomic_owner owner;
    ps3_guest_memory_status status = ps3_atomic_owner_acquire(domain, &owner);
    if (status == PS3_GUEST_MEMORY_OK) *memory = (ps3_spu_memory){domain, owner, local_store};
    return status;
}
ps3_guest_memory_status ps3_spu_memory_release(ps3_spu_memory *memory) {
    if (!valid_binding(memory)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_guest_memory_status status = ps3_atomic_owner_release(memory->domain, &memory->owner);
    if (status == PS3_GUEST_MEMORY_OK) *memory = (ps3_spu_memory){0};
    return status;
}
ps3_guest_memory_status ps3_spu_memory_get(ps3_spu_memory *memory, uint64_t ea, size_t lsa, size_t bytes) {
    if (!valid_binding(memory) || !valid_ls(lsa, bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    return ps3_atomic_dma_read(memory->domain, memory->owner, ea, memory->local_store + lsa, bytes);   /* Decision 135 */
}
ps3_guest_memory_status ps3_spu_memory_put(ps3_spu_memory *memory, uint64_t ea, size_t lsa, size_t bytes) {
    if (!valid_binding(memory) || !valid_ls(lsa, bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    return ps3_atomic_dma_write(memory->domain, memory->owner, ea, memory->local_store + lsa, bytes);   /* Decision 135 */
}
ps3_guest_memory_status ps3_spu_memory_getllar(ps3_spu_memory *memory, uint64_t ea, size_t lsa, unsigned *atomic_status) {
    if (!valid_binding(memory) || !atomic_status || !valid_atomic_ls(lsa))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_guest_memory_status status = ps3_atomic_load_reserve128(memory->domain, memory->owner, ea, memory->local_store + lsa);
    if (status == PS3_GUEST_MEMORY_OK) *atomic_status = SPU_ATOMIC_RESERVE_COMPLETE;
    return status;
}
ps3_guest_memory_status ps3_spu_memory_putllc(ps3_spu_memory *memory, uint64_t ea, size_t lsa, unsigned *atomic_status) {
    if (!valid_binding(memory)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!atomic_status || !valid_atomic_ls(lsa)) {
        ps3_guest_memory_status status = ps3_atomic_owner_clear_reservation(memory->domain, memory->owner);
        return status == PS3_GUEST_MEMORY_OK ? PS3_GUEST_MEMORY_INVALID_RANGE : status;
    }
    int stored;
    ps3_guest_memory_status status = ps3_atomic_store_conditional128(memory->domain, memory->owner, ea,
                                                                    memory->local_store + lsa, &stored);
    if (status == PS3_GUEST_MEMORY_OK)
        *atomic_status = stored ? SPU_ATOMIC_CONDITIONAL_SUCCESS : SPU_ATOMIC_CONDITIONAL_FAILURE;
    return status;
}
ps3_guest_memory_status ps3_spu_memory_putlluc(ps3_spu_memory *memory, uint64_t ea, size_t lsa, unsigned *atomic_status) {
    if (!valid_binding(memory) || !atomic_status || !valid_atomic_ls(lsa))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_guest_memory_status status = ps3_atomic_store_unconditional128(memory->domain, memory->owner, ea, memory->local_store + lsa);
    if (status == PS3_GUEST_MEMORY_OK) *atomic_status = SPU_ATOMIC_UNCONDITIONAL_COMPLETE;
    return status;
}
ps3_guest_memory_status ps3_spu_memory_take_reservation_loss(ps3_spu_memory *memory, int *lost) {
    if (!valid_binding(memory)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    return ps3_atomic_owner_take_reservation_loss(memory->domain, memory->owner, lost);
}
