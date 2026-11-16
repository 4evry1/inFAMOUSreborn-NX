#define _POSIX_C_SOURCE 200809L
#include "atomic_domain.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

enum { FAIL_NONE, FAIL_READ, FAIL_WRITE, FAIL_ZERO, FAIL_COMMIT, FAIL_DECOMMIT };
typedef struct {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[8];
    ps3_atomic_pin_record pins[4];
    int failure;
    unsigned read_only;
    unsigned backing_calls;
    atomic_int block_read, read_observed, allow_read;
} Fixture;
static unsigned tests;
static ps3_guest_memory_status read_backing(void *context, uint32_t a, void *p, size_t n) {
    Fixture *f = context;
    ++f->backing_calls;
    if (f->failure == FAIL_READ) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    ps3_guest_memory_status status = f->backing.read(f->backing.context, a, p, n);
    if (atomic_load(&f->block_read)) {
        atomic_store(&f->read_observed, 1);
        while (!atomic_load(&f->allow_read)) sched_yield();
    }
    return status;
}
static ps3_guest_memory_status write_backing(void *context, uint32_t a, const void *p, size_t n) {
    Fixture *f = context;
    ++f->backing_calls;
    if (f->failure == FAIL_WRITE || f->read_only) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.write(f->backing.context, a, p, n);
}
static ps3_guest_memory_status zero_backing(void *context, uint32_t a, uint64_t n) {
    Fixture *f = context;
    ++f->backing_calls;
    if (f->failure == FAIL_ZERO) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.zero(f->backing.context, a, n);
}
static ps3_guest_memory_status commit_backing(void *context, uint32_t a, uint64_t n) {
    Fixture *f = context;
    ++f->backing_calls;
    if (f->failure == FAIL_COMMIT) return PS3_GUEST_MEMORY_NO_MEMORY;
    return f->backing.commit(f->backing.context, a, n);
}
static ps3_guest_memory_status decommit_backing(void *context, uint32_t a, uint64_t n) {
    Fixture *f = context;
    ++f->backing_calls;
    if (f->failure == FAIL_DECOMMIT) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.decommit(f->backing.context, a, n);
}
static void init(Fixture *f) {
    memset(f, 0, sizeof *f);
    atomic_init(&f->block_read, 0);
    atomic_init(&f->read_observed, 0);
    atomic_init(&f->allow_read, 0);
    f->space = elf_test_host_space_create(65536);
    assert(f->space && !ps3_pthread_guard_init(&f->guard));
    f->backing = elf_test_host_space_port(f->space);
    ps3_guest_memory memory = {f, commit_backing, decommit_backing, read_backing, write_backing, zero_backing};
    assert(ps3_atomic_domain_init(&f->domain, memory, ps3_pthread_guard_port(&f->guard),
                                  f->owners, 8, f->pins, 4) == PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_commit(&f->domain, 0x1000, 0x3000) == PS3_GUEST_MEMORY_OK);
}
static void finish(Fixture *f) {
    for (unsigned i = 0; i < 8; ++i) assert(!f->owners[i].active);
    for (unsigned i = 0; i < 4; ++i) assert(!f->pins[i].active);
    assert(!ps3_pthread_guard_destroy(&f->guard));
    elf_test_host_space_destroy(f->space);
}
static ps3_atomic_owner owner(Fixture *f) {
    ps3_atomic_owner result;
    assert(ps3_atomic_owner_acquire(&f->domain, &result) == PS3_GUEST_MEMORY_OK);
    return result;
}
static void put(Fixture *f, uint32_t address, uint32_t value) {
    unsigned char b[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
                          (unsigned char)(value >> 8), (unsigned char)value};
    assert(ps3_atomic_write(&f->domain, address, b, 4) == PS3_GUEST_MEMORY_OK);
}
static void reservation_cases(void) {
    Fixture f;
    init(&f);
    ps3_atomic_owner o = owner(&f);
    uint32_t value;
    int stored;
    put(&f, 0x1000, 0x12345678);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0 && value == 0x12345678);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 7, &stored) == 0 && stored);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 8, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1000, 7);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 8, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1000, 9); put(&f, 0x1000, 7);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 8, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x107c, 1);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 8, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1080, 1);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 8, &stored) == 0 && stored); ++tests;
    uint64_t wide;
    assert(ps3_atomic_load_reserve64(&f.domain, o, 0x1100, &wide) == 0);
    assert(ps3_atomic_store_conditional64(&f.domain, o, 0x1100, UINT64_C(0x123456789abcdef0), &stored) == 0 && stored);
    assert(ps3_atomic_load_reserve64(&f.domain, o, 0x1100, &wide) == 0 && wide == UINT64_C(0x123456789abcdef0));
    assert(ps3_atomic_zero(&f.domain, 0x1106, 1) == 0);
    assert(ps3_atomic_store_conditional64(&f.domain, o, 0x1100, wide, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    assert(ps3_atomic_decommit(&f.domain, 0x1000, 4096) == 0);
    assert(ps3_atomic_commit(&f.domain, 0x1000, 4096) == 0);
    put(&f, 0x1000, value);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 10, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    finish(&f);
}
static void reservation_loss(void) {
    Fixture f; init(&f);
    ps3_atomic_owner o = owner(&f);
    uint32_t value;
    int lost = 17, stored;
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1000, value);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && lost);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1000, value + 1); put(&f, 0x1000, value);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1100, &value) == 0);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && lost);
    assert(ps3_atomic_owner_clear_reservation(&f.domain, o) == 0);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    lost = 17;
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, NULL) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_owner_take_reservation_loss(NULL, o, &lost) == PS3_GUEST_MEMORY_INVALID_RANGE && lost == 17);
    const ps3_atomic_owner stale = o;
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    o = owner(&f);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, stale, &lost) == PS3_GUEST_MEMORY_INVALID_RANGE && lost == 17);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    put(&f, 0x1000, value);
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0); o = owner(&f);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    finish(&f); ++tests;
}
static void line_reservations(void) {
    Fixture f, other; init(&f); init(&other);
    ps3_atomic_owner o = owner(&f), peer = owner(&f), foreign = owner(&other);
    unsigned char source[128], copy[128], got[128], unchanged[128];
    for (unsigned i = 0; i < sizeof source; ++i) source[i] = (unsigned char)(i ^ 0xa5);
    memset(unchanged, 0xcc, sizeof unchanged);
    assert(ps3_atomic_write(&f.domain, 0x1000, source, sizeof source) == 0);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(!memcmp(copy, source, sizeof copy));
    assert(ps3_atomic_load_reserve128(&f.domain, peer, 0x1000, got) == 0);
    copy[127] ^= 0xff;
    int stored = 17, lost = 17;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && stored);
    assert(ps3_atomic_read(&f.domain, 0x1000, got, sizeof got) == 0 && !memcmp(got, copy, sizeof got));
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, peer, &lost) == 0 && lost);
    assert(ps3_atomic_store_conditional128(&f.domain, peer, 0x1000, source, &stored) == 0 && !stored);
    ++tests;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_write(&f.domain, 0x1000, copy, sizeof copy) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && lost);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    unsigned char byte = copy[31] ^ 1;
    assert(ps3_atomic_write(&f.domain, 0x101f, &byte, 1) == 0);
    assert(ps3_atomic_write(&f.domain, 0x101f, copy + 31, 1) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && lost);
    ++tests;
    uint32_t narrow; uint64_t wide;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_load_reserve32(&f.domain, peer, 0x107c, &narrow) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, peer, 0x107c, narrow, &stored) == 0 && stored);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_load_reserve64(&f.domain, peer, 0x1000, &wide) == 0);
    assert(ps3_atomic_store_conditional64(&f.domain, peer, 0x1000, wide, &stored) == 0 && stored);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && !stored);
    ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &narrow) == 0);
    const unsigned calls_before_cross_width = f.backing_calls;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(f.backing_calls == calls_before_cross_width);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, narrow, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, narrow, &stored) == 0 && !stored);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, NULL) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && !stored);
    const unsigned calls_without_reservation = f.backing_calls;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x4000, copy, &stored) == 0 && !stored);
    assert(f.backing_calls == calls_without_reservation);
    ++tests;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    f.failure = FAIL_READ; memcpy(got, unchanged, sizeof got);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1100, got) == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    assert(!memcmp(got, unchanged, sizeof got)); f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    f.failure = FAIL_WRITE; stored = 17;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == PS3_GUEST_MEMORY_PLATFORM_FAILURE && stored == 17);
    f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    ++tests;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1080, source, &stored) == 0 && !stored);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1001, source, &stored) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, NULL, &stored) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    memcpy(got, unchanged, sizeof got);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1001, got) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_load_reserve128(&f.domain, o, UINT64_C(0x100000000), got) == PS3_GUEST_MEMORY_INVALID_RANGE);
    const unsigned calls_before_top = f.backing_calls;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0xffffff80u, got) != PS3_GUEST_MEMORY_OK);
    assert(f.backing_calls == calls_before_top + 1);
    assert(!memcmp(got, unchanged, sizeof got));
    assert(ps3_atomic_load_reserve128(&f.domain, foreign, 0x1000, got) == PS3_GUEST_MEMORY_INVALID_RANGE);
    ++tests;
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, peer, &lost) == 0);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_load_reserve128(&f.domain, peer, 0x1000, got) == 0);
    assert(ps3_atomic_store_unconditional128(&f.domain, o, 0x1000, source) == 0);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && lost);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, peer, &lost) == 0 && lost);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    assert(ps3_atomic_store_unconditional128(&f.domain, o, 0x1100, source) == 0);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == 0 && !lost);
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && stored);
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    f.failure = FAIL_WRITE;
    assert(ps3_atomic_store_unconditional128(&f.domain, o, 0x1000, source) == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, copy, &stored) == 0 && stored);
    ++tests;
    assert(ps3_atomic_load_reserve128(&f.domain, o, 0x1000, copy) == 0);
    f.read_only = 1; stored = 17;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == PS3_GUEST_MEMORY_PLATFORM_FAILURE && stored == 17);
    assert(ps3_atomic_read(&f.domain, 0x1000, got, sizeof got) == 0 && !memcmp(got, copy, sizeof got));
    assert(ps3_atomic_store_unconditional128(&f.domain, o, 0x1000, source) == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    f.read_only = 0;
    assert(ps3_atomic_store_conditional128(&f.domain, o, 0x1000, source, &stored) == 0 && !stored);
    ++tests;
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    assert(ps3_atomic_owner_release(&f.domain, &peer) == 0);
    assert(ps3_atomic_owner_release(&other.domain, &foreign) == 0);
    finish(&f); finish(&other);
}
static void owned_transfers(void) {
    Fixture f, other; init(&f); init(&other);
    ps3_atomic_owner o = owner(&f), stale = o;
    unsigned char bytes[16] = {1}, output[16];
    assert(ps3_atomic_owner_write(&f.domain, o, 0x1000, bytes, sizeof bytes) == 0);
    assert(ps3_atomic_owner_read(&f.domain, o, 0x1000, output, sizeof output) == 0);
    assert(!memcmp(output, bytes, sizeof bytes));
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0); o = owner(&f);
    assert(o.slot == stale.slot && o.generation != stale.generation);
    uint32_t value; int stored;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    memset(output, 0xcc, sizeof output);
    unsigned calls = f.backing_calls;
    assert(ps3_atomic_owner_read(&f.domain, stale, 0x1000, output, sizeof output) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(output[0] == 0xcc && output[15] == 0xcc);
    assert(ps3_atomic_owner_write(&f.domain, stale, 0x1000, bytes, sizeof bytes) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(f.backing_calls == calls);
    calls = other.backing_calls;
    assert(ps3_atomic_owner_read(&other.domain, o, 0x1000, output, sizeof output) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_owner_write(&other.domain, o, 0x1000, bytes, sizeof bytes) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(other.backing_calls == calls);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && stored);
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    finish(&f); finish(&other); ++tests;
}
static void explicit_reservation_clear(void) {
    Fixture f, other;
    init(&f);
    init(&other);
    ps3_atomic_owner cleared = owner(&f), peer = owner(&f), foreign = owner(&other);
    uint32_t value;
    int stored;
    put(&f, 0x1000, 7);
    put(&other, 0x1000, 11);
    assert(ps3_atomic_load_reserve32(&f.domain, cleared, 0x1000, &value) == 0);
    assert(ps3_atomic_load_reserve32(&f.domain, peer, 0x1000, &value) == 0);
    const unsigned calls = f.backing_calls;
    assert(ps3_atomic_owner_clear_reservation(&f.domain, cleared) == PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_owner_clear_reservation(&f.domain, cleared) == PS3_GUEST_MEMORY_OK);
    assert(f.backing_calls == calls);
    assert(ps3_atomic_store_conditional32(&f.domain, cleared, 0x1000, 8, &stored) == 0 && !stored);
    assert(f.backing_calls == calls);
    assert(ps3_atomic_store_conditional32(&f.domain, peer, 0x1000, 9, &stored) == 0 && stored);
    ++tests;

    assert(ps3_atomic_load_reserve32(&f.domain, cleared, 0x1000, &value) == 0 && value == 9);
    assert(ps3_atomic_store_conditional32(&f.domain, cleared, 0x1000, 10, &stored) == 0 && stored);
    assert(ps3_atomic_load_reserve32(&other.domain, foreign, 0x1000, &value) == 0 && value == 11);
    assert(ps3_atomic_owner_clear_reservation(NULL, cleared) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_owner_clear_reservation(&other.domain, cleared) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional32(&other.domain, foreign, 0x1000, 12, &stored) == 0 && stored);
    ++tests;

    const ps3_atomic_owner stale = cleared;
    assert(ps3_atomic_owner_release(&f.domain, &cleared) == 0);
    cleared = owner(&f);
    assert(cleared.slot == stale.slot && cleared.generation != stale.generation);
    assert(ps3_atomic_load_reserve32(&f.domain, cleared, 0x1000, &value) == 0);
    assert(ps3_atomic_owner_clear_reservation(&f.domain, stale) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional32(&f.domain, cleared, 0x1000, 13, &stored) == 0 && stored);
    ++tests;

    assert(ps3_atomic_owner_release(&f.domain, &cleared) == 0);
    assert(ps3_atomic_owner_release(&f.domain, &peer) == 0);
    assert(ps3_atomic_owner_release(&other.domain, &foreign) == 0);
    finish(&f);
    finish(&other);
}
static void failures_and_lifetime(void) {
    Fixture f, other;
    init(&f); init(&other);
    ps3_atomic_owner o = owner(&f), stale = o;
    uint32_t value = 0xfeedface;
    int stored = 17;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1001, &value) == PS3_GUEST_MEMORY_INVALID_RANGE && value == 0xfeedface);
    assert(ps3_atomic_load_reserve32(&f.domain, o, UINT64_C(0x100000000), &value) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_load_reserve32(&other.domain, o, 0x1000, &value) == PS3_GUEST_MEMORY_INVALID_RANGE); ++tests;
    put(&f, 0x1000, 4);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    f.failure = FAIL_WRITE;
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 5, &stored) == PS3_GUEST_MEMORY_PLATFORM_FAILURE && stored == 17);
    f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 5, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1001, 5, &stored) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 5, &stored) == 0 && !stored); ++tests;
    for (int failure = FAIL_WRITE; failure <= FAIL_DECOMMIT; ++failure) {
        assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
        f.failure = failure;
        ps3_guest_memory_status status;
        if (failure == FAIL_WRITE) { unsigned char b = 1; status = ps3_atomic_write(&f.domain, 0x1000, &b, 1); }
        else if (failure == FAIL_ZERO) status = ps3_atomic_zero(&f.domain, 0x1000, 4);
        else if (failure == FAIL_COMMIT) status = ps3_atomic_commit(&f.domain, 0x1000, 4096);
        else status = ps3_atomic_decommit(&f.domain, 0x1000, 4096);
        assert(status != 0);
        f.failure = FAIL_NONE;
        assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && stored);
    } ++tests;
    ps3_atomic_pin pin, old_pin;
    assert(ps3_atomic_pin_acquire(&f.domain, 0x1004, 8, &pin) == 0); old_pin = pin;
    assert(ps3_atomic_decommit(&f.domain, 0x1000, 4096) == PS3_GUEST_MEMORY_BUSY);
    assert(ps3_atomic_pin_release(&f.domain, &pin) == 0);
    assert(ps3_atomic_pin_acquire(&f.domain, 0x1004, 8, &pin) == 0);
    assert(ps3_atomic_pin_release(&f.domain, &old_pin) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_pin_release(&other.domain, &pin) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_pin_release(&f.domain, &pin) == 0); ++tests;
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0); o = owner(&f);
    assert(ps3_atomic_load_reserve32(&f.domain, stale, 0x1000, &value) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0); ++tests;
    ps3_atomic_owner handles[8];
    for (unsigned i = 0; i < 8; ++i) handles[i] = owner(&f);
    assert(ps3_atomic_owner_acquire(&f.domain, &o) == PS3_GUEST_MEMORY_NO_MEMORY);
    for (unsigned i = 0; i < 8; ++i) assert(ps3_atomic_owner_release(&f.domain, &handles[i]) == 0);
    ++tests;
    finish(&f); finish(&other);
}
typedef struct { Fixture *fixture; ps3_atomic_owner owner; unsigned count; atomic_int started, done; } Worker;
static void *increment(void *context) {
    Worker *w = context;
    for (unsigned i = 0; i < w->count; ++i) {
        int stored;
        do {
            uint32_t old;
            assert(ps3_atomic_load_reserve32(&w->fixture->domain, w->owner, 0x1000, &old) == 0);
            assert(ps3_atomic_store_conditional32(&w->fixture->domain, w->owner, 0x1000, old + 1, &stored) == 0);
        } while (!stored);
    }
    return NULL;
}
static void *load_once(void *context) {
    Worker *w = context; uint32_t value;
    assert(ps3_atomic_load_reserve32(&w->fixture->domain, w->owner, 0x1000, &value) == 0);
    return NULL;
}
static void *write_once(void *context) {
    Worker *w = context; atomic_store(&w->started, 1);
    put(w->fixture, 0x1000, 0);
    atomic_store(&w->done, 1); return NULL;
}
static void *clear_once(void *context) {
    Worker *worker = context;
    atomic_store(&worker->started, 1);
    assert(ps3_atomic_owner_clear_reservation(&worker->fixture->domain, worker->owner) == PS3_GUEST_MEMORY_OK);
    atomic_store(&worker->done, 1);
    return NULL;
}
typedef struct { Fixture *fixture; ps3_atomic_owner owner; atomic_int start; int lost; } LossReader;
static void *take_loss_once(void *context) {
    LossReader *reader = context;
    while (!atomic_load(&reader->start)) sched_yield();
    assert(ps3_atomic_owner_take_reservation_loss(&reader->fixture->domain, reader->owner, &reader->lost) == 0);
    return NULL;
}
static void reservation_loss_race(void) {
    Fixture f; init(&f);
    ps3_atomic_owner o = owner(&f);
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t value;
        int final_loss;
        assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
        LossReader reader = {.fixture = &f, .owner = o};
        Worker writer = {.fixture = &f};
        pthread_t take_thread, write_thread;
        assert(!pthread_create(&take_thread, NULL, take_loss_once, &reader));
        assert(!pthread_create(&write_thread, NULL, write_once, &writer));
        atomic_store(&reader.start, 1);
        assert(!pthread_join(take_thread, NULL) && !pthread_join(write_thread, NULL));
        assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &final_loss) == 0);
        assert(reader.lost + final_loss == 1);
        assert(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &final_loss) == 0 && !final_loss);
    }
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    finish(&f); ++tests;
}
static void concurrency(void) {
    Fixture f; init(&f);
    Worker workers[4] = {0}; pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i].fixture = &f; workers[i].owner = owner(&f); workers[i].count = 1000;
        assert(!pthread_create(&threads[i], NULL, increment, &workers[i]));
    }
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    uint32_t value;
    assert(ps3_atomic_load_reserve32(&f.domain, workers[0].owner, 0x1000, &value) == 0 && value == 4000);
    for (unsigned i = 0; i < 4; ++i) assert(ps3_atomic_owner_release(&f.domain, &workers[i].owner) == 0);
    ++tests;
    Worker loader = {.fixture = &f, .owner = owner(&f)}, writer = {.fixture = &f};
    put(&f, 0x1000, 0); atomic_store(&f.block_read, 1);
    assert(!pthread_create(&threads[0], NULL, load_once, &loader));
    while (!atomic_load(&f.read_observed)) sched_yield();
    assert(pthread_mutex_trylock(&f.guard.mutex) == EBUSY);
    assert(!pthread_create(&threads[1], NULL, write_once, &writer));
    while (!atomic_load(&writer.started)) sched_yield();
    assert(!atomic_load(&writer.done)); atomic_store(&f.allow_read, 1);
    assert(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    int stored;
    assert(ps3_atomic_store_conditional32(&f.domain, loader.owner, 0x1000, 1, &stored) == 0 && !stored);
    assert(ps3_atomic_owner_release(&f.domain, &loader.owner) == 0); ++tests;
    Worker blocked_loader = {.fixture = &f, .owner = owner(&f)};
    Worker clearer = {.fixture = &f, .owner = blocked_loader.owner};
    atomic_store(&f.read_observed, 0);
    atomic_store(&f.allow_read, 0);
    assert(!pthread_create(&threads[0], NULL, load_once, &blocked_loader));
    while (!atomic_load(&f.read_observed)) sched_yield();
    assert(!pthread_create(&threads[1], NULL, clear_once, &clearer));
    while (!atomic_load(&clearer.started)) sched_yield();
    assert(!atomic_load(&clearer.done));
    atomic_store(&f.allow_read, 1);
    assert(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    assert(ps3_atomic_store_conditional32(&f.domain, blocked_loader.owner, 0x1000, 2, &stored) == 0 && !stored);
    assert(ps3_atomic_owner_release(&f.domain, &blocked_loader.owner) == 0);
    ++tests;
    finish(&f);
}
static void control_blocks_and_bounds(void) {
    Fixture f; init(&f);
    ps3_atomic_owner o = owner(&f);
    ps3_atomic_compare_result result = {UINT64_MAX, 17};
    assert(ps3_atomic_compare_exchange32(&f.domain, 0x1000, 0, 0x12345678, &result) == 0);
    assert(result.exchanged && result.observed == 0);
    assert(ps3_atomic_compare_exchange32(&f.domain, 0x1000, 0, 1, &result) == 0);
    assert(!result.exchanged && result.observed == 0x12345678); ++tests;
    assert(ps3_atomic_compare_exchange64(&f.domain, 0x1100, 0, UINT64_C(0xfedcba9876543210), &result) == 0 && result.exchanged);
    uint64_t wide;
    assert(ps3_atomic_load_reserve64(&f.domain, o, 0x1100, &wide) == 0 && wide == UINT64_C(0xfedcba9876543210));
    assert(ps3_atomic_compare_exchange64(&f.domain, 0x1100, wide, wide, &result) == 0 && result.exchanged);
    int stored;
    assert(ps3_atomic_store_conditional64(&f.domain, o, 0x1100, wide, &stored) == 0 && !stored); ++tests;
    uint32_t value;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    result = (ps3_atomic_compare_result){UINT64_MAX, 17};
    f.failure = FAIL_WRITE;
    assert(ps3_atomic_compare_exchange32(&f.domain, 0x1000, value, value, &result) == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    assert(result.observed == UINT64_MAX && result.exchanged == 17);
    f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && stored); ++tests;
    unsigned char maximum[2] = {255, 255};
    assert(ps3_atomic_write(&f.domain, 0x1200, maximum, 2) == 0);
    uint16_t sum;
    assert(ps3_atomic_add16(&f.domain, 0x1200, 1, &sum) == 0 && sum == 0);
    assert(ps3_atomic_add16(&f.domain, 0x1200, -1, &sum) == 0 && sum == 65535);
    assert(ps3_atomic_add16(&f.domain, 0x1200, INT32_MIN, &sum) == 0 && sum == 65535); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1200, &value) == 0);
    assert(ps3_atomic_add16(&f.domain, 0x1200, 0, &sum) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1200, value, &stored) == 0 && !stored); ++tests;
    sum = 123;
    f.failure = FAIL_READ;
    assert(ps3_atomic_add16(&f.domain, 0x1200, 1, &sum) == PS3_GUEST_MEMORY_PLATFORM_FAILURE && sum == 123);
    f.failure = FAIL_WRITE;
    assert(ps3_atomic_add16(&f.domain, 0x1200, 1, &sum) == PS3_GUEST_MEMORY_PLATFORM_FAILURE && sum == 123);
    f.failure = FAIL_NONE; ++tests;
    ps3_atomic_pin pins[4], unchanged = {&f.domain, 99, 99};
    for (unsigned i = 0; i < 4; ++i) assert(ps3_atomic_pin_acquire(&f.domain, 0x1000 + i, 1, &pins[i]) == 0);
    assert(ps3_atomic_pin_acquire(&f.domain, 0x2000, 1, &unchanged) == PS3_GUEST_MEMORY_NO_MEMORY && unchanged.slot == 99);
    for (unsigned i = 0; i < 4; ++i) assert(ps3_atomic_pin_release(&f.domain, &pins[i]) == 0);
    assert(ps3_atomic_pin_acquire(&f.domain, UINT32_MAX, 2, &unchanged) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_pin_acquire(&f.domain, 0x1000, 0, &unchanged) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_pin_acquire(&f.domain, 0x3fff, 2, &unchanged) == PS3_GUEST_MEMORY_UNMAPPED && unchanged.slot == 99); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    assert(ps3_atomic_pin_acquire(&f.domain, 0x1000, 24, &pins[0]) == 0);
    assert(ps3_atomic_decommit(&f.domain, 0x1000, 4096) == PS3_GUEST_MEMORY_BUSY);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && stored);
    assert(ps3_atomic_pin_release(&f.domain, &pins[0]) == 0); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, NULL) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && !stored); ++tests;
    assert(ps3_atomic_compare_exchange32(&f.domain, 0x1001, 0, 0, &result) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_compare_exchange64(&f.domain, UINT64_MAX, 0, 0, &result) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_commit(&f.domain, 0x1001, 4096) == PS3_GUEST_MEMORY_INVALID_RANGE);
    assert(ps3_atomic_decommit(&f.domain, 0x1000, 0) == PS3_GUEST_MEMORY_INVALID_RANGE); ++tests;
    ps3_guest_memory port = ps3_atomic_domain_memory(&f.domain);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    assert(port.write(port.context, 0x1000, maximum, 2) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && !stored); ++tests;
    ps3_atomic_owner second = owner(&f), outside = owner(&f);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1080, &value) == 0);
    assert(ps3_atomic_load_reserve32(&f.domain, second, 0x1180, &value) == 0);
    assert(ps3_atomic_load_reserve32(&f.domain, outside, 0x1200, &value) == 0);
    unsigned char bulk[256] = {0};
    assert(port.write(port.context, 0x10f8, bulk, sizeof bulk) == 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1080, 0, &stored) == 0 && !stored);
    assert(ps3_atomic_store_conditional32(&f.domain, second, 0x1180, 0, &stored) == 0 && !stored);
    assert(ps3_atomic_store_conditional32(&f.domain, outside, 0x1200, 0, &stored) == 0 && stored);
    assert(ps3_atomic_owner_release(&f.domain, &second) == 0);
    assert(ps3_atomic_owner_release(&f.domain, &outside) == 0); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x3ff0, &value) == 0);
    assert(port.write(port.context, 0x3ff0, bulk, 32) == PS3_GUEST_MEMORY_UNMAPPED);
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x3ff0, value, &stored) == 0 && stored); ++tests;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == 0);
    uint32_t unchanged_value = 0xfeedface;
    f.failure = FAIL_READ;
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1100, &unchanged_value) == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    assert(unchanged_value == 0xfeedface);
    f.failure = FAIL_NONE;
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, value, &stored) == 0 && stored); ++tests;
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0);
    finish(&f);
}
typedef struct {
    Fixture *fixture;
    ps3_atomic_owner owner;
    ps3_atomic_pin pin;
    atomic_int ready, start;
} CancelWorker;
static void cancel_cleanup(void *context) {
    CancelWorker *w = context;
    if (w->pin.domain) assert(ps3_atomic_pin_release(&w->fixture->domain, &w->pin) == 0);
    if (w->owner.domain) assert(ps3_atomic_owner_release(&w->fixture->domain, &w->owner) == 0);
}
static void *cancellable_owner(void *context) {
    CancelWorker *w = context;
    pthread_cleanup_push(cancel_cleanup, w);
    assert(ps3_atomic_owner_acquire(&w->fixture->domain, &w->owner) == 0);
    assert(ps3_atomic_pin_acquire(&w->fixture->domain, 0x1000, 24, &w->pin) == 0);
    atomic_store(&w->ready, 1);
    while (!atomic_load(&w->start)) sched_yield();
    uint32_t value;
    assert(ps3_atomic_load_reserve32(&w->fixture->domain, w->owner, 0x1000, &value) == 0);
    pthread_testcancel();
    pthread_cleanup_pop(1);
    return NULL;
}
static void cancellation_boundary(void) {
    Fixture f; init(&f);
    CancelWorker w = {.fixture = &f}; pthread_t thread;
    assert(!pthread_create(&thread, NULL, cancellable_owner, &w));
    while (!atomic_load(&w.ready)) sched_yield();
    atomic_store(&f.block_read, 1); atomic_store(&w.start, 1);
    while (!atomic_load(&f.read_observed)) sched_yield();
    assert(!pthread_cancel(thread));
    atomic_store(&f.allow_read, 1);
    void *result;
    assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
    assert(!w.owner.domain && !w.pin.domain);
    assert(ps3_atomic_decommit(&f.domain, 0x1000, 4096) == 0);
    finish(&f); ++tests;
}
/* Decision 112 (rv50s): a PPU reservation that sees writes to the whole line (models vm_read32 and ppu_stwcx32 of native/adapters/lle_sre.cpp). */
static uint32_t lwarx_model(Fixture *f, uint64_t ea) {
    const uint32_t seq = ps3_atomic_reservation_snapshot(ea);
    unsigned char b[4];
    assert(ps3_atomic_read(&f->domain, ea, b, 4) == PS3_GUEST_MEMORY_OK);
    if (seq != PS3_ATOMIC_NO_SEQ) ps3_atomic_reservation_note(ea, seq);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static int stwcx_model(Fixture *f, ps3_atomic_owner cpu, uint64_t ea, uint32_t expected, uint32_t value) {
    uint32_t seen; int stored = 0;
    assert(ps3_atomic_load_reserve32(&f->domain, cpu, ea, &seen) == PS3_GUEST_MEMORY_OK);
    if (seen != expected) { (void)ps3_atomic_reservation_intact(ea); return 0; }
    if (!ps3_atomic_reservation_intact(ea)) { assert(ps3_atomic_owner_clear_reservation(&f->domain, cpu) == PS3_GUEST_MEMORY_OK); return 0; }
    assert(ps3_atomic_store_conditional32(&f->domain, cpu, ea, value, &stored) == PS3_GUEST_MEMORY_OK);
    return stored;
}
static uint32_t get32(Fixture *f, uint32_t address) {
    unsigned char b[4];
    assert(ps3_atomic_read(&f->domain, address, b, 4) == PS3_GUEST_MEMORY_OK);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
/* what an SPU does to the line: getllar, change one word, putllc */
static void spu_changes_word(Fixture *f, ps3_atomic_owner spu, uint64_t line, unsigned offset, uint32_t value) {
    unsigned char image[128]; int stored = 0;
    assert(ps3_atomic_load_reserve128(&f->domain, spu, line, image) == PS3_GUEST_MEMORY_OK);
    image[offset] = (unsigned char)(value >> 24); image[offset + 1] = (unsigned char)(value >> 16); image[offset + 2] = (unsigned char)(value >> 8); image[offset + 3] = (unsigned char)value;
    assert(ps3_atomic_store_conditional128(&f->domain, spu, line, image, &stored) == PS3_GUEST_MEMORY_OK && stored);
}
typedef struct { Fixture *f; ps3_atomic_owner spu; uint64_t line; unsigned offset; uint32_t address, value; } LineWriter;
static void *spu_thread(void *context) { LineWriter *w = context; spu_changes_word(w->f, w->spu, w->line, w->offset, w->value); return NULL; }
/* the same, from another host thread (a note of the calling thread is its own: only another thread's write cancels it) */
static void spu_thread_changes_word(Fixture *f, ps3_atomic_owner spu, uint64_t line, unsigned offset, uint32_t value) {
    LineWriter w = {f, spu, line, offset, 0, value}; pthread_t t;
    assert(!pthread_create(&t, NULL, spu_thread, &w)); assert(!pthread_join(t, NULL));
}
static void *line_writer(void *context) {
    LineWriter *w = context;
    put(w->f, w->address, w->value);
    return NULL;
}
static void granule_reservations(void) {
    Fixture f; init(&f);
    ps3_atomic_owner spu = owner(&f), cpu = owner(&f);
    const uint64_t word = 0x2010;
    assert(g_ps3_atomic_granule_reservations == 1);
    /* a line no SPU ever reserved: no sequence, no note, the conditional store is the old value check */
    put(&f, word, 5);
    assert(ps3_atomic_reservation_snapshot(word) == PS3_ATOMIC_NO_SEQ);
    uint32_t v = lwarx_model(&f, word);
    assert(v == 5 && stwcx_model(&f, cpu, word, v, 6) == 1 && get32(&f, word) == 6);
    /* an SPU reserves the line: now it is sticky */
    spu_changes_word(&f, spu, 0x2000, 0x40, 0x11111111u);
    assert(ps3_atomic_reservation_snapshot(word) != PS3_ATOMIC_NO_SEQ);
    const uint64_t noted0 = g_ps3_atomic_reservation_noted, lost0 = g_ps3_atomic_reservation_lost;
    /* nothing in between: succeeds */
    v = lwarx_model(&f, word);
    assert(v == 6 && stwcx_model(&f, cpu, word, v, 7) == 1 && get32(&f, word) == 7);
    assert(g_ps3_atomic_reservation_noted == noted0 + 1 && g_ps3_atomic_reservation_lost == lost0);
    ++tests;
    /* the SPU's putllc changes ANOTHER word of the line between the lwarx and the stwcx: the hardware's stwcx fails (the value check alone let it through) */
    v = lwarx_model(&f, word);
    spu_thread_changes_word(&f, spu, 0x2000, 0x40, 0x22222222u);
    assert(get32(&f, word) == 7 && get32(&f, 0x2040) == 0x22222222u);
    assert(stwcx_model(&f, cpu, word, v, 8) == 0 && get32(&f, word) == 7);
    assert(g_ps3_atomic_reservation_lost == lost0 + 1);
    /* the loop retries: lwarx again, now it holds */
    v = lwarx_model(&f, word);
    assert(stwcx_model(&f, cpu, word, v, 8) == 1 && get32(&f, word) == 8);
    ++tests;
    /* the old behaviour (switch off): the same interleaving succeeds */
    g_ps3_atomic_granule_reservations = 0;
    v = lwarx_model(&f, word);
    spu_thread_changes_word(&f, spu, 0x2000, 0x40, 0x33333333u);
    assert(stwcx_model(&f, cpu, word, v, 9) == 1 && get32(&f, word) == 9);
    g_ps3_atomic_granule_reservations = 1;
    ++tests;
    /* a store by the thread itself to the same line does not cancel its own reservation; a store by another thread does */
    v = lwarx_model(&f, word);
    put(&f, 0x2050, 0x44444444u);
    assert(stwcx_model(&f, cpu, word, v, 10) == 1 && get32(&f, word) == 10);
    v = lwarx_model(&f, word);
    { LineWriter w = {&f, spu, 0, 0, 0x2060, 0x55555555u}; pthread_t t; assert(!pthread_create(&t, NULL, line_writer, &w)); assert(!pthread_join(t, NULL)); }
    assert(stwcx_model(&f, cpu, word, v, 11) == 0 && get32(&f, word) == 10);
    ++tests;
    /* a write of more than 64 lines moves every sticky line on */
    spu_changes_word(&f, spu, 0x3800, 0x00, 0x66666666u);
    v = lwarx_model(&f, 0x3810);
    assert(ps3_atomic_zero(&f.domain, 0x1000, 0x2700) == PS3_GUEST_MEMORY_OK);   /* (the thread's own write: a range of more than 64 lines moves the global sequence, which cancels even its own note) */
    assert(stwcx_model(&f, cpu, 0x3810, v, 12) == 0);
    v = lwarx_model(&f, 0x3810);
    assert(stwcx_model(&f, cpu, 0x3810, v, 12) == 1 && get32(&f, 0x3810) == 12);
    ++tests;
    /* a stale note (a plain read of the word long ago, no lwarx) must not fail a later, unrelated stwcx on a line that was never noted */
    assert(ps3_atomic_reservation_intact(0x3FF0) == 1);
    assert(ps3_atomic_owner_release(&f.domain, &spu) == PS3_GUEST_MEMORY_OK && ps3_atomic_owner_release(&f.domain, &cpu) == PS3_GUEST_MEMORY_OK);
    finish(&f); ++tests;
}
/* ---- Root (2026-11-13, console run rv56s): the layout of the domain and the tests of the lock-free store paths. */
#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif
uint64_t ps3_atomic_domain_debug_enter_count(unsigned line);
/* every cache line (64 bytes) the bytes [p, p + n) touch, as a set of line numbers in a small bitmap keyed by (line - base_line) */
static void mark_lines(const volatile void *p, size_t n, uintptr_t base_line, unsigned char *bits, size_t bit_count) {
    const uintptr_t first = (uintptr_t)p / 64u, last = ((uintptr_t)p + n - 1u) / 64u;
    for (uintptr_t line = first; line <= last; ++line) { assert(line >= base_line && line - base_line < bit_count); bits[line - base_line] = 1; }
}
static unsigned overlap(const unsigned char *a, const unsigned char *b, size_t n) { unsigned count = 0; for (size_t i = 0; i < n; ++i) count += a[i] && b[i]; return count; }
static void domain_layout(void) {
    Fixture f; init(&f);
    const ps3_atomic_domain *d = &f.domain;
    enum { LINES = 256 };
    const uintptr_t base = (uintptr_t)d / 64u;
    unsigned char locked[LINES] = {0}, written[LINES] = {0}, map[LINES] = {0}, busy[LINES] = {0};
    mark_lines(&d->locked_ops, sizeof d->locked_ops, base, locked, LINES);
    mark_lines(&d->loss_epoch, sizeof d->loss_epoch, base, written, LINES);
    mark_lines(&d->reserved_mask, sizeof d->reserved_mask, base, written, LINES);
    mark_lines(&d->debug_line, sizeof d->debug_line, base, written, LINES);
    mark_lines(d->reserved_map, sizeof d->reserved_map, base, map, LINES);
    mark_lines(d->busy_map, sizeof d->busy_map, base, busy, LINES);
    /* the four groups the lock-free stores read or the guarded operations write share no cache line, whatever address the object has */
    assert(!overlap(locked, written, LINES) && !overlap(locked, map, LINES) && !overlap(locked, busy, LINES));
    assert(!overlap(written, map, LINES) && !overlap(written, busy, LINES) && !overlap(map, busy, LINES));
    /* byte distances (what the static asserts in atomic_domain.c guarantee at build time) */
    assert((const char *)&d->locked_ops - ((const char *)&d->wait_port + sizeof d->wait_port) >= 64);
    assert((const char *)&d->loss_epoch - ((const char *)&d->locked_ops + sizeof d->locked_ops) >= 64);
    assert((const char *)d->reserved_map - ((const char *)&d->debug_line + sizeof d->debug_line) >= 64);
    assert((const char *)d->busy_map - ((const char *)d->reserved_map + sizeof d->reserved_map) >= 64);
    ps3_atomic_owner o = owner(&f);
    uint32_t value;
    int stored;
    put(&f, 0x1000, 1);
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x1000, &value) == PS3_GUEST_MEMORY_OK);
    assert(f.domain.reserved_mask == ps3_atomic_granule_bit(0x1000));
    assert(ps3_atomic_store_conditional32(&f.domain, o, 0x1000, 2, &stored) == PS3_GUEST_MEMORY_OK && stored);
    assert(f.domain.reserved_mask == 0);
    /* the diagnostic: on in a host build, off (field stays 0, counters answer 0) with -DPS3_ATOMIC_DEBUG_LINE=0 */
#if defined(PS3_ATOMIC_DEBUG_LINE) && !PS3_ATOMIC_DEBUG_LINE
    assert(f.domain.debug_line == 0);
    for (unsigned line = 0; line < 1024u; ++line) assert(ps3_atomic_domain_debug_enter_count(line) == 0);
#else
    assert(f.domain.debug_line != 0);
#endif
    assert(ps3_atomic_owner_release(&f.domain, &o) == PS3_GUEST_MEMORY_OK);
    finish(&f); ++tests;
}
static int slow_test(const ps3_atomic_domain *d, uint64_t a, uint64_t n) { return ps3_atomic_sticky_hit(a, n) || ps3_atomic_domain_write_conflicts(d, a, n); }
static int fast_test(const ps3_atomic_domain *d, uint64_t a, uint64_t n) { return ps3_atomic_sticky_hit_fast(a, n) || ps3_atomic_domain_write_conflicts_fast(d, a, n); }
/* both forms of the test must give the same answer in every state the domain can be in */
#define AGREE(f, a, n, expect) do { assert(slow_test(&(f)->domain, (a), (n)) == (expect)); assert(fast_test(&(f)->domain, (a), (n)) == (expect)); } while (0)
typedef struct { Fixture *f; ps3_atomic_owner owner; uint64_t address; atomic_int started, done; } BusyLoader;
static void *busy_load(void *context) {
    BusyLoader *b = context; uint32_t v; atomic_store(&b->started, 1);
    assert(ps3_atomic_load_reserve32(&b->f->domain, b->owner, b->address, &v) == PS3_GUEST_MEMORY_OK);
    atomic_store(&b->done, 1); return NULL;
}
static void fast_store_tests(void) {
    Fixture f; init(&f);
    assert(ps3_atomic_commit(&f.domain, 0x8000, 0x4000) == PS3_GUEST_MEMORY_OK);
    ps3_atomic_owner o = owner(&f), spu = owner(&f);
    const uint64_t a = 0x8010, other = 0x9010, straddle = 0x807c;   /* the straddling 8-byte store covers the granules of 0x8000 and 0x8080 */
    assert(ps3_atomic_granule_index(a) != ps3_atomic_granule_index(other) && ps3_atomic_granule_index(straddle) == ps3_atomic_granule_index(a));
    assert(ps3_atomic_granule_index(straddle + 7) != ps3_atomic_granule_index(a));
    AGREE(&f, a, 4, 0); AGREE(&f, other, 4, 0); AGREE(&f, straddle, 8, 0); AGREE(&f, a, 0, 0);
    /* a reservation held: the word's granule and a store that straddles into it conflict, another granule does not */
    uint32_t value; int stored;
    assert(ps3_atomic_load_reserve32(&f.domain, o, a, &value) == PS3_GUEST_MEMORY_OK);
    AGREE(&f, a, 4, 1); AGREE(&f, other, 4, 0); AGREE(&f, straddle, 8, 1); AGREE(&f, 0x8090, 4, 0);
    assert(ps3_atomic_store_conditional32(&f.domain, o, a, 1, &stored) == PS3_GUEST_MEMORY_OK && stored);
    AGREE(&f, a, 4, 0); AGREE(&f, straddle, 8, 0);
    /* a reservation on the LAST granule of a straddling store (the second map word is read too) */
    assert(ps3_atomic_load_reserve32(&f.domain, o, 0x8084, &value) == PS3_GUEST_MEMORY_OK);
    AGREE(&f, straddle, 8, 1); AGREE(&f, a, 4, 0);
    assert(ps3_atomic_owner_clear_reservation(&f.domain, o) == PS3_GUEST_MEMORY_OK);
    AGREE(&f, straddle, 8, 0);
    /* an operation not tied to an address: locked_ops raised */
    __atomic_fetch_add(&f.domain.locked_ops, 1u, __ATOMIC_SEQ_CST);
    AGREE(&f, a, 4, 1); AGREE(&f, other, 4, 1);
    __atomic_fetch_sub(&f.domain.locked_ops, 1u, __ATOMIC_SEQ_CST);
    AGREE(&f, a, 4, 0);
    /* a read-modify-write in flight (the backing read is blocked): only its own granule bucket conflicts */
    {
        pthread_t t; BusyLoader b = {.f = &f, .owner = o, .address = a};
        atomic_store(&f.block_read, 1);
        assert(!pthread_create(&t, NULL, busy_load, &b));
        while (!atomic_load(&f.read_observed)) sched_yield();
        AGREE(&f, a, 4, 1); AGREE(&f, other, 4, 0); AGREE(&f, straddle, 8, 1); AGREE(&f, 0x8200, 4, 0);
        atomic_store(&f.allow_read, 1);
        assert(!pthread_join(t, NULL));
        atomic_store(&f.block_read, 0);
        AGREE(&f, a, 4, 1);      /* the reservation is held after the load */
        assert(ps3_atomic_owner_clear_reservation(&f.domain, o) == PS3_GUEST_MEMORY_OK);
        AGREE(&f, a, 4, 0);
    }
    /* a line an SPU reserved is sticky for ever; the first granule of a straddling store may be the sticky one or the last */
    {
        unsigned char image[128]; int ok = 0;
        assert(ps3_atomic_load_reserve128(&f.domain, spu, 0x8800, image) == PS3_GUEST_MEMORY_OK);
        assert(ps3_atomic_store_conditional128(&f.domain, spu, 0x8800, image, &ok) == PS3_GUEST_MEMORY_OK && ok);
        AGREE(&f, 0x8808, 4, 1); AGREE(&f, 0x8880, 4, 0); AGREE(&f, 0x87fc, 8, 1); AGREE(&f, 0x887c, 8, 1); AGREE(&f, 0x8780, 4, 0);
    }
    assert(ps3_atomic_owner_release(&f.domain, &o) == 0 && ps3_atomic_owner_release(&f.domain, &spu) == 0);
    finish(&f); ++tests;
}
/* The store sequence of lean_store / fast_store (ppu_memory_context.cpp) on this domain: test, store, fence, test again; a hit goes through the domain. */
static unsigned g_litmus_second_hits;
static void protocol_store32(Fixture *f, uint64_t address, uint32_t value) {
    const unsigned char be[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8), (unsigned char)value};
#ifdef LITMUS_UNSAFE   /* control run (not part of the verifier): a store with no test at all must be caught by the litmus */
    assert(f->backing.write(f->backing.context, (uint32_t)address, be, 4) == PS3_GUEST_MEMORY_OK);
    return;
#endif
    if (ps3_atomic_sticky_hit_fast(address, 4) || ps3_atomic_domain_write_conflicts_fast(&f->domain, address, 4)) {
        assert(ps3_atomic_write(&f->domain, address, be, 4) == PS3_GUEST_MEMORY_OK);
        return;
    }
    assert(f->backing.write(f->backing.context, (uint32_t)address, be, 4) == PS3_GUEST_MEMORY_OK);   /* the unguarded store */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ps3_atomic_sticky_hit_fast(address, 4) || ps3_atomic_domain_write_conflicts_fast(&f->domain, address, 4)) {
        __atomic_fetch_add(&g_litmus_second_hits, 1u, __ATOMIC_RELAXED);
        assert(ps3_atomic_write(&f->domain, address, be, 4) == PS3_GUEST_MEMORY_OK);
    }
}
typedef struct { Fixture *f; uint64_t line; ps3_atomic_owner spu; uint32_t stores; atomic_int stop, spu_ok, spu_fail, ready, cpu_done; } Litmus;
static void *litmus_spu(void *context) {
    Litmus *l = context;
    unsigned char image[128]; int stored;
    atomic_store(&l->ready, 1);
    while (!atomic_load(&l->stop)) {
        assert(ps3_atomic_load_reserve128(&l->f->domain, l->spu, l->line, image) == PS3_GUEST_MEMORY_OK);
        memcpy(image + 4, image, 4);   /* word 1 := word 0 as the SPU read it; a stale word 0 written back would make the value go down */
        assert(ps3_atomic_store_conditional128(&l->f->domain, l->spu, l->line, image, &stored) == PS3_GUEST_MEMORY_OK);
        atomic_fetch_add(stored ? &l->spu_ok : &l->spu_fail, 1);
    }
    return NULL;
}
static void *litmus_cpu(void *context) {
    Litmus *l = context;
    while (!atomic_load(&l->ready)) sched_yield();
    for (uint32_t k = 1; k <= l->stores; ++k) protocol_store32(l->f, l->line, k);
    atomic_store(&l->cpu_done, 1);
    return NULL;
}
/* The Decision 87 property under the relaxed tests: an SPU store-conditional never succeeds on a line that a lock-free store wrote after its load, so the word the CPU counts up
 * never goes down. (A stale line written back by a store-conditional that should have failed is the only thing that makes it go down.) `warm`: the line is sticky before the CPU
 * thread starts; otherwise it becomes sticky while the CPU stores to it (the second test is what catches the stores that raced with the first load-reserve; not under TSan: the
 * unguarded stores race with the guarded reads on purpose). */
static void sticky_litmus(int warm) {
    Fixture f; init(&f);
    assert(ps3_atomic_commit(&f.domain, 0x8000, 0x4000) == PS3_GUEST_MEMORY_OK);
    Litmus l = {.f = &f, .line = warm ? 0xA000 : 0xA400, .spu = owner(&f), .stores = 20000};
    if (warm) {
        unsigned char image[128]; int ok;
        assert(ps3_atomic_load_reserve128(&f.domain, l.spu, l.line, image) == PS3_GUEST_MEMORY_OK);
        assert(ps3_atomic_store_conditional128(&f.domain, l.spu, l.line, image, &ok) == PS3_GUEST_MEMORY_OK && ok);
        assert(ps3_atomic_sticky_hit_fast(l.line, 4));
    } else assert(!ps3_atomic_sticky_hit_fast(l.line, 4));
    pthread_t cpu, spu;
    assert(!pthread_create(&spu, NULL, litmus_spu, &l) && !pthread_create(&cpu, NULL, litmus_cpu, &l));
    uint32_t last = 0, decreases = 0, samples = 0;
    for (;;) {
        const uint32_t now = get32(&f, (uint32_t)l.line);
        if (now < last) ++decreases;
        last = now; ++samples;
        if (atomic_load(&l.cpu_done)) break;
        sched_yield();
    }
    atomic_store(&l.stop, 1);
    assert(!pthread_join(cpu, NULL) && !pthread_join(spu, NULL));
    assert(decreases == 0);
    assert(get32(&f, (uint32_t)l.line) == l.stores);
    assert(ps3_atomic_sticky_hit_fast(l.line, 4));
    printf("STICKY_LITMUS warm=%d samples=%u spu_ok=%d spu_fail=%d late_hits=%u\n", warm, samples, atomic_load(&l.spu_ok), atomic_load(&l.spu_fail), g_litmus_second_hits);
    assert(ps3_atomic_owner_release(&f.domain, &l.spu) == 0);
    finish(&f); ++tests;
}
int main(void) {
    reservation_loss(); granule_reservations(); line_reservations(); owned_transfers();
    reservation_cases(); explicit_reservation_clear(); failures_and_lifetime(); concurrency();
    control_blocks_and_bounds(); cancellation_boundary(); reservation_loss_race();
    domain_layout(); fast_store_tests(); sticky_litmus(1);
#if !UNDER_TSAN
    sticky_litmus(0);
#endif
    printf("ATOMIC_DOMAIN_PASS tests=%u\n", tests);
    return 0;
}
