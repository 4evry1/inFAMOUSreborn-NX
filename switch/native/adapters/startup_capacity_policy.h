#ifndef NATIVE_STARTUP_CAPACITY_POLICY_H
#define NATIVE_STARTUP_CAPACITY_POLICY_H

enum {
#ifdef PS3_NATIVE_LLE_PROFILE
    /* C1b (2026-11-11): the game builds a worker pool after SPURS init; on Eden the 64th sys_lwmutex_create and the
     * 33rd sys_lwcond_create were refused (STARTUP_MUTEX_CAPABILITY_STOP status=9). Object tables only; waiter,
     * binding and pin capacities are unchanged. */
    STARTUP_LWMUTEX_OBJECT_CAPACITY = 1024,
#else
    STARTUP_LWMUTEX_OBJECT_CAPACITY = 64,   /* bounded doubling: 33 required (31 ordinary + 2 SPURS), 31 spare; not a whole-game claim */
#endif
    /* Decision 33 / C1b: PS3_NATIVE_LLE_PROFILE is a BUILD-WIDE define (evidence/lle-sre-b1b/lle.mk), never a
     * per-object one: it sizes ps3_ppu_threads and the child composition, so every object must agree. Under LLE
     * Sony's libsre creates two PPU threads per SPURS instance through sys_ppu_thread_create (the game has two
     * instances), and the game then creates its own. 8 = 4 libsre + 3 game (249, 0x10000) + the (2000, 0x4000)
     * run-to-exit slot. Without the define every value is the three-child profile, unchanged. */
#ifdef PS3_NATIVE_LLE_PROFILE
    STARTUP_CHILD_SLOT_CAPACITY = 64,   /* 2026-11-11: 8 -> 12 -> 24 -> 64 (Eden r2 run: refused_no_slot=1 at the 25th create); after FIOS starts the game asked for a 13th thread (priority 100) */
#else
    STARTUP_CHILD_SLOT_CAPACITY = 3,
#endif
    STARTUP_PRIMARY_OWNER_CAPACITY = 1,
    STARTUP_SPU_OWNER_CAPACITY = 6,
    STARTUP_ATOMIC_OWNER_CAPACITY = STARTUP_PRIMARY_OWNER_CAPACITY +
        STARTUP_SPU_OWNER_CAPACITY + STARTUP_CHILD_SLOT_CAPACITY,
    /* Value-neutral names for the capacities that deliberately stay unchanged in the three-child profile
     * (mutex waiters, condition objects/waiters/bindings, atomic pins) so every array size and init count
     * in the native target is bound to ONE named constant. */
    STARTUP_LWMUTEX_WAITER_CAPACITY = 32,
#ifdef PS3_NATIVE_LLE_PROFILE
    STARTUP_LWCOND_OBJECT_CAPACITY = 512,
#else
    STARTUP_LWCOND_OBJECT_CAPACITY = 32,
#endif
    STARTUP_LWCOND_WAITER_CAPACITY = 32,
#ifdef PS3_NATIVE_LLE_PROFILE
    STARTUP_LWCOND_BINDING_CAPACITY = 512,   /* one per lwcond object: the 33rd sys_lwcond_create got EAGAIN from this table */
#else
    STARTUP_LWCOND_BINDING_CAPACITY = 32,
#endif
#ifdef PS3_NATIVE_LLE_PROFILE
    /* Root (2026-11-11, hardware run r9): every live lwmutex and lwcond holds one pin, so 128 pins ended the
     * primary at about its 128th sys_lwmutex_create (status MEMORY_FAILURE, memory NO_MEMORY) although the
     * lwmutex table holds 1024. Sized for both object tables plus margin. */
    STARTUP_ATOMIC_PIN_CAPACITY = 2048,
#else
    STARTUP_ATOMIC_PIN_CAPACITY = 128,
#endif
    /* ps3_ppu_threads service records: two SPURS helper + two SPURS handler + one record per child slot (the adopted
     * main thread is not a slot). Must equal the domain PS3_PPU_SERVICE_CAP (domain/ppu_threads.h cannot include this
     * adapter header; the native target static_asserts the equality and the SPURS-instance relation). */
    STARTUP_SPURS_SERVICE_RECORD_CAPACITY = 4,
    STARTUP_PPU_SERVICE_CAPACITY = STARTUP_SPURS_SERVICE_RECORD_CAPACITY + STARTUP_CHILD_SLOT_CAPACITY
};

#endif
