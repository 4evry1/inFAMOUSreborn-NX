#define _POSIX_C_SOURCE 200809L
/* M2: address-less charges share the allocator quota (real allocator, real backing). */
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { WINDOW = 0x00400000, WINDOW_END = 0x01400000, QUOTA = 0x00800000 };
typedef struct {
    ElfTestHostSpace *space;
    ps3_pthread_guard guard;
    ps3_user_memory memory;
    ps3_user_memory_slot slots[4];
    ps3_user_memory_charge_record charges[3];
} Fixture;
static void setup(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(WINDOW_END);
    CHECK(f->space && ps3_pthread_guard_init(&f->guard) == 0);
    ps3_user_memory_config c = {WINDOW, WINDOW_END, QUOTA, elf_test_host_space_port(f->space), ps3_pthread_guard_port(&f->guard)};
    CHECK(ps3_user_memory_init(&f->memory, &c, f->slots, 4) == PS3_USER_MEMORY_OK);
}
static void teardown(Fixture *f) {
    ps3_pthread_guard_destroy(&f->guard);
    elf_test_host_space_destroy(f->space);
}
static ps3_user_memory_info info(Fixture *f) {
    ps3_user_memory_info i;
    CHECK(ps3_user_memory_query(&f->memory, &i) == PS3_USER_MEMORY_OK);
    return i;
}
int main(void) {
    Fixture f; setup(&f);
    ps3_user_memory_charge_token t1, t2, t3, t4;
    /* Charges require an attached table. */
    CHECK(ps3_user_memory_charge(&f.memory, 0x1000, &t1) == PS3_USER_MEMORY_CAPACITY_EXCEEDED);
    CHECK(ps3_user_memory_attach_charges(&f.memory, f.charges, 3) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f.memory, f.charges, 3) == PS3_USER_MEMORY_INVALID_ARGUMENT);
    CHECK(ps3_user_memory_charge(&f.memory, 0, &t1) == PS3_USER_MEMORY_INVALID_ARGUMENT);
    CHECK(ps3_user_memory_charge(&f.memory, 1, NULL) == PS3_USER_MEMORY_INVALID_ARGUMENT);
    /* 5 x 256 KiB group + 0x8000 helper + 0x4000 handler = 0x14C000 */
    CHECK(ps3_user_memory_charge(&f.memory, 0x140000, &t1) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_charge(&f.memory, 0x8000, &t2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_charge(&f.memory, 0x4000, &t3) == PS3_USER_MEMORY_OK);
    ps3_user_memory_info i = info(&f);
    CHECK(i.used_bytes == 0x14C000 && i.available_bytes == QUOTA - 0x14C000 && i.allocation_count == 0);
    /* Capacity of charge table. */
    CHECK(ps3_user_memory_charge(&f.memory, 0x1000, &t4) == PS3_USER_MEMORY_CAPACITY_EXCEEDED);
    /* Shared quota: allocations see charges. */
    uint32_t address = 0;
    CHECK(ps3_user_memory_allocate(&f.memory, QUOTA - 0x100000, PS3_USER_MEMORY_ALIGN_1M, &address).status == PS3_USER_MEMORY_QUOTA_EXCEEDED);
    CHECK(ps3_user_memory_allocate(&f.memory, 0x100000, PS3_USER_MEMORY_ALIGN_1M, &address).status == PS3_USER_MEMORY_OK);
    i = info(&f);
    CHECK(i.used_bytes == 0x24C000 && i.available_bytes == QUOTA - 0x24C000 && i.allocation_count == 1);
    /* Over-quota charge leaves state unchanged (and the token untouched). */
    ps3_user_memory_charge_token before = t4, released = t3;
    CHECK(ps3_user_memory_uncharge(&f.memory, &t3) == PS3_USER_MEMORY_OK);
    CHECK(t3.id == 0);
    t4 = before;
    CHECK(ps3_user_memory_charge(&f.memory, QUOTA, &t4) == PS3_USER_MEMORY_QUOTA_EXCEEDED);
    CHECK(memcmp(&t4, &before, sizeof t4) == 0);
    CHECK(info(&f).used_bytes == 0x248000);
    /* Stale token release and double release are rejected without effect. */
    ps3_user_memory_charge_token stale = released;
    CHECK(ps3_user_memory_uncharge(&f.memory, &stale) == PS3_USER_MEMORY_NOT_OWNED);
    CHECK(ps3_user_memory_charge(&f.memory, 0x4000, &t3) == PS3_USER_MEMORY_OK); /* reuses the slot, new id */
    CHECK(ps3_user_memory_uncharge(&f.memory, &stale) == PS3_USER_MEMORY_NOT_OWNED);
    CHECK(info(&f).used_bytes == 0x24C000);
    ps3_user_memory_charge_token forged = t3; forged.slot = 99;
    CHECK(ps3_user_memory_uncharge(&f.memory, &forged) == PS3_USER_MEMORY_NOT_OWNED);
    /* Another allocator's token never releases here, even with identical slot/id. */
    {
        Fixture g; setup(&g);
        CHECK(ps3_user_memory_attach_charges(&g.memory, g.charges, 3) == PS3_USER_MEMORY_OK);
        ps3_user_memory_charge_token foreign;
        CHECK(ps3_user_memory_charge(&g.memory, 0x4000, &foreign) == PS3_USER_MEMORY_OK);
        ps3_user_memory_charge_token substituted = foreign; substituted.slot = t1.slot; substituted.id = t1.id;
        ps3_user_memory_charge_token copy = substituted;
        CHECK(ps3_user_memory_uncharge(&f.memory, &substituted) == PS3_USER_MEMORY_NOT_OWNED);
        CHECK(memcmp(&copy, &substituted, sizeof copy) == 0 && info(&f).used_bytes == 0x24C000);
        CHECK(ps3_user_memory_uncharge(&f.memory, &foreign) == PS3_USER_MEMORY_NOT_OWNED);
        /* Cleanup refuses while charges are outstanding and frees nothing. */
        CHECK(ps3_user_memory_cleanup(&g.memory).status == PS3_USER_MEMORY_CHARGES_OUTSTANDING);
        CHECK(ps3_user_memory_uncharge(&g.memory, &foreign) == PS3_USER_MEMORY_OK);
        CHECK(ps3_user_memory_cleanup(&g.memory).status == PS3_USER_MEMORY_OK);
        teardown(&g);
    }
    CHECK(ps3_user_memory_cleanup(&f.memory).status == PS3_USER_MEMORY_CHARGES_OUTSTANDING);
    CHECK(info(&f).used_bytes == 0x24C000 && info(&f).allocation_count == 1);
    /* Overflowing request: refused, nothing changes. */
    ps3_user_memory_charge_token untouched = {NULL, 7, 7};
    CHECK(ps3_user_memory_charge(&f.memory, UINT64_MAX, &untouched) == PS3_USER_MEMORY_QUOTA_EXCEEDED);
    CHECK(untouched.slot == 7 && untouched.id == 7 && info(&f).used_bytes == 0x24C000);
    /* Balanced release returns to start. */
    CHECK(ps3_user_memory_uncharge(&f.memory, &t3) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_uncharge(&f.memory, &t2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_uncharge(&f.memory, &t1) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_free(&f.memory, address).status == PS3_USER_MEMORY_OK);
    i = info(&f);
    CHECK(i.used_bytes == 0 && i.available_bytes == QUOTA && i.allocation_count == 0);
    /* A charge of the exact remaining quota succeeds; one more byte fails. */
    CHECK(ps3_user_memory_charge(&f.memory, QUOTA, &t1) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_charge(&f.memory, 1, &t2) == PS3_USER_MEMORY_QUOTA_EXCEEDED);
    CHECK(ps3_user_memory_uncharge(&f.memory, &t1) == PS3_USER_MEMORY_OK);
    teardown(&f);
    puts("test_user_memory_charge: OK");
    return 0;
}
