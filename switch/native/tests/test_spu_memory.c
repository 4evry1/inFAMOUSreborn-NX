#include "../adapters/spu_memory.h"
#include "../adapters/guard_pthread.h"
#include "guest_space_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    ElfTestHostSpace *space = elf_test_host_space_create(65536);
    ps3_pthread_guard guard;
    ps3_atomic_domain domain = {0};
    ps3_atomic_owner_record owners[3];
    unsigned char *ls = calloc(1, PS3_SPU_LOCAL_STORE_BYTES);
    assert(space && ls && !ps3_pthread_guard_init(&guard));
    assert(ps3_atomic_domain_init(&domain, elf_test_host_space_port(space), ps3_pthread_guard_port(&guard), owners, 3, NULL, 0) == 0);
    assert(ps3_atomic_commit(&domain, 0x1000, 0x2000) == 0);
    ps3_spu_memory memory = {0}, second = {0};
    unsigned char *peer_ls = calloc(1, PS3_SPU_LOCAL_STORE_BYTES);
    assert(peer_ls);
    assert(ps3_spu_memory_init(&memory, &domain, ls, PS3_SPU_LOCAL_STORE_BYTES - 1) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(!memory.domain);
    assert(ps3_spu_memory_init(&memory, &domain, ls, PS3_SPU_LOCAL_STORE_BYTES) == 0);
    assert(ps3_spu_memory_init(&memory, &domain, ls, PS3_SPU_LOCAL_STORE_BYTES) == PS3_GUEST_MEMORY_BUSY);
    assert(ps3_spu_memory_init(&second, &domain, peer_ls, PS3_SPU_LOCAL_STORE_BYTES) == 0);
    ps3_atomic_owner last_slot;
    ps3_spu_memory refused = {0};
    assert(ps3_atomic_owner_acquire(&domain, &last_slot) == 0);
    assert(ps3_spu_memory_init(&refused, &domain, peer_ls, PS3_SPU_LOCAL_STORE_BYTES) == PS3_GUEST_MEMORY_NO_MEMORY);
    assert(!refused.domain && !refused.owner.domain && !refused.local_store);
    assert(ps3_atomic_owner_release(&domain, &last_slot) == 0);
    for (unsigned i = 0; i < 256; ++i) ls[128 + i] = (unsigned char)(i ^ 0x5a);
    assert(ps3_spu_memory_put(&memory, 0x1fc0, 128, 256) == 0);
    assert(ps3_spu_memory_get(&second, 0x1fc0, 256, 256) == 0);
    assert(!memcmp(ls + 128, peer_ls + 256, 256));
    unsigned char unchanged[256]; memset(unchanged, 0xcc, sizeof unchanged);
    memcpy(peer_ls, unchanged, sizeof unchanged);
    assert(ps3_spu_memory_get(&second, 0x2fc0, 0, 256) == PS3_GUEST_MEMORY_UNMAPPED);
    assert(!memcmp(peer_ls, unchanged, sizeof unchanged));
    assert(ps3_spu_memory_get(&memory, 0x1000, PS3_SPU_LOCAL_STORE_BYTES - 1, 2) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_spu_memory_put(&memory, UINT64_C(0x100000000), 0, 1) == PS3_GUEST_MEMORY_INVALID_RANGE);
    unsigned status = 17; int lost = 17;
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0 && status == 4);
    assert(ps3_spu_memory_getllar(&second, 0x2000, 0, &status) == 0 && status == 4);
    ls[127] ^= 0xff;
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 0);
    assert(ps3_spu_memory_take_reservation_loss(&memory, &lost) == 0 && !lost);
    assert(ps3_spu_memory_take_reservation_loss(&second, &lost) == 0 && lost);
    assert(ps3_spu_memory_putllc(&second, 0x2000, 0, &status) == 0 && status == 1);
    assert(ps3_spu_memory_get(&second, 0x2000, 0, 128) == 0 && !memcmp(ls, peer_ls, 128));
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    assert(ps3_spu_memory_put(&second, 0x2000, 0, 128) == 0);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 1);
    assert(ps3_spu_memory_take_reservation_loss(&memory, &lost) == 0 && lost);
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    assert(ps3_spu_memory_putlluc(&memory, 0x2000, 0, &status) == 0 && status == 2);
    assert(ps3_spu_memory_take_reservation_loss(&memory, &lost) == 0 && lost);
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    assert(ps3_spu_memory_putlluc(&memory, 0x2100, 0, &status) == 0 && status == 2);
    assert(ps3_spu_memory_take_reservation_loss(&memory, &lost) == 0 && !lost);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 0);
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    status = 17;
    assert(ps3_spu_memory_putllc(&memory, 0x2000, PS3_SPU_LOCAL_STORE_BYTES - 1, &status) == PS3_GUEST_MEMORY_INVALID_RANGE && status == 17);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 1);
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, NULL) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 1);
    status = 17;
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 1, &status) == PS3_GUEST_MEMORY_INVALID_RANGE && status == 17);
    assert(ps3_spu_memory_getllar(&memory, 0x2000, 0, &status) == 0);
    status = 17;
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 1, &status) == PS3_GUEST_MEMORY_INVALID_RANGE && status == 17);
    assert(ps3_spu_memory_putllc(&memory, 0x2000, 0, &status) == 0 && status == 1);
    status = 17;
    assert(ps3_spu_memory_getllar(&memory, 0x2001, 0, &status) == PS3_GUEST_MEMORY_INVALID_RANGE && status == 17);
    assert(ps3_spu_memory_putlluc(&memory, 0x3000, 0, &status) == PS3_GUEST_MEMORY_UNMAPPED && status == 17);
    ps3_spu_memory stale = second;
    assert(ps3_spu_memory_release(&second) == 0 && !second.domain);
    assert(ps3_spu_memory_init(&second, &domain, peer_ls, PS3_SPU_LOCAL_STORE_BYTES) == 0);
    assert(second.owner.slot == stale.owner.slot && second.owner.generation != stale.owner.generation);
    assert(ps3_spu_memory_get(&stale, 0x2000, 0, 1) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_spu_memory_put(&stale, 0x2000, 0, 1) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_spu_memory_release(&second) == 0);
    assert(ps3_spu_memory_release(&memory) == 0 && !memory.domain);
    assert(ps3_spu_memory_get(&memory, 0x1000, 0, 1) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_spu_memory_release(&memory) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(!ps3_pthread_guard_destroy(&guard));
    elf_test_host_space_destroy(space); free(peer_ls); free(ls);
    puts("SPU_MEMORY_PASS real_LS_DMA_and_line_atomics=1 executor_linked=0");
    return 0;
}
