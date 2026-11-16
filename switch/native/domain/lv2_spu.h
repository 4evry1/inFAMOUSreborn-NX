#ifndef NATIVE_LV2_SPU_H
#define NATIVE_LV2_SPU_H
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/sync/guard.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Decision 33 / B2b: SPU images and SPU thread groups as native lv2 objects, on the CPU.
 *
 * This module owns every ppu-side fact about an SPU group: the imported images, the groups, their
 * thread slots, each thread's 256 KiB local store, the port bindings and the START snapshot. It
 * NEVER runs an SPU. sys_spu_thread_group_start records the snapshot and answers "recorded"; running
 * the group is the native SPU executor's job (phase C1), not this module's.
 *
 * Local-store storage is CALLER-provided (the composition owns N x 256 KiB), exactly like every other
 * buffer in this layer: this module allocates nothing.
 *
 * Guard: same non-recursive ps3_instance_guard contract as lv2_objects. No callback takes a second
 * guard and none runs consumer code. */
enum {
    PS3_LV2_SPU_OK = 0, PS3_LV2_SPU_EAGAIN = 0x80010001u, PS3_LV2_SPU_EINVAL = 0x80010002u, PS3_LV2_SPU_ENOMEM = 0x80010004u,
    PS3_LV2_SPU_ESRCH = 0x80010005u, PS3_LV2_SPU_EPERM = 0x80010009u, PS3_LV2_SPU_EBUSY = 0x8001000Au,
    PS3_LV2_SPU_EFAULT = 0x8001000Du, PS3_LV2_SPU_ESTAT = 0x8001000Fu,
    /* Internal outcomes the adapter maps to a named stop, never to a guest error. These MUST NOT
     * reuse lv2_objects' 0xFFFF0001..0xFFFF0005 (FENCED, CANCELLED, PROVIDER_FAILURE, UNSUPPORTED,
     * TIMED_OUT) -- they deliberately sit in their own range so a code can never be mistaken for
     * another layer's outcome. */
    PS3_LV2_SPU_UNSUPPORTED = 0xFFFF0010u, PS3_LV2_SPU_PROVIDER_FAILURE = 0xFFFF0011u
};
enum {
    PS3_LV2_SPU_LS_BYTES = 0x40000,          /* 256 KiB local store per thread */
    PS3_LV2_SPU_SEGMENT_BYTES = 0x18,        /* sys_spu_segment_t */
    PS3_LV2_SPU_IMAGE_SEGMENTS_MAX = 32,
    PS3_LV2_SPU_GROUP_CAP = 8,
    PS3_LV2_SPU_THREADS_MAX = 8,             /* threads in one group */
    PS3_LV2_SPU_GROUP_BASE = 0x9E000000u,    /* id space, disjoint from semaphores/queues/ports/flags */
    PS3_LV2_SPU_THREAD_BASE = 0x9D000000u,
    PS3_LV2_SPU_ID_STEP = 0x100u,
    PS3_LV2_SPU_PRIORITY_MIN = 16,
    PS3_LV2_SPU_PRIORITY_MAX = 255,
    /* Decision 33 / B2b-2: the SYSTEM-OWNED guest window the image segment arrays live in. A console
     * keeps libsre's segs array in kernel/system memory, not in the title's user-memory quota, so it
     * is a fixed window with fixed slots and its own bitmap. It sits just past the libsre extent
     * (0x30000000 + 0x50000) and the composition must declare it as RAM. */
    PS3_LV2_SPU_ARENA_BASE = 0x30060000,
    PS3_LV2_SPU_ARENA_SIZE = 0x10000,
    PS3_LV2_SPU_ARENA_SLOT = 256,
    PS3_LV2_SPU_ARENA_SLOTS = PS3_LV2_SPU_ARENA_SIZE / PS3_LV2_SPU_ARENA_SLOT
};
/* sys_spu_image_t as the guest sees it: { u32 type; u32 entry_point; u32 segs; s32 nsegs }. */
typedef struct ps3_lv2_spu_image {
    uint32_t type, entry_point, segs_ea, nsegs;
    unsigned active;
    uint32_t source_ea;          /* the SPU ELF it was parsed from (identification only) */
} ps3_lv2_spu_image;
typedef struct ps3_lv2_spu_thread {
    uint32_t id, spu_number;
    unsigned initialized, connected;
    uint32_t entry_point;        /* copied from the image at initialize */
    uint64_t arg1, arg2, arg3, arg4;
    unsigned char *ls;           /* borrowed: this thread's 256 KiB local store */
    /* Event ports bound by thread_connect_event, one per event type (1 run, 2 exception, 4 system). */
    uint32_t port[PS3_LV2_SPU_THREADS_MAX];
    /* Root fix (B2b Eden): SPU event ports (spup 0..63) bound to an event queue by
     * sys_spu_thread_group_connect_event_all_threads; bit n of spup_bound set means spup_queue[n] holds. */
    uint64_t spup_bound;
    uint32_t spup_queue[64];
    /* Decision 33 / C1b: what the thread did once a runner executed it. `ended` is set exactly once, by the runner,
     * when the thread reaches a terminal; exit_status is what stop 0x102 reported. `queue_bound` is the queue a
     * sys_spu_thread_bind_queue (193) bound for stop 0x110's receive. */
    unsigned ended, exited;
    uint32_t exit_status, bound_queue;
} ps3_lv2_spu_thread;
enum { PS3_LV2_SPU_EISCONN = 0x80010015u };   /* every requested SPU port already bound */
typedef struct ps3_lv2_spu_group {
    uint32_t id, num, priority, type, num_usable;
    unsigned active, started, terminated;
    /* Decision 33 / C1b: filled by the runner. `ended_count` is how many initialized threads reached a terminal;
     * join_cause is PS3_LV2_SPU_GROUP_JOIN_* once every one of them has. */
    uint32_t ended_count, join_cause;
    uint32_t port_run, port_exception, port_system;   /* group-level connects */
    ps3_lv2_spu_thread threads[PS3_LV2_SPU_THREADS_MAX];
} ps3_lv2_spu_group;
/* What sys_spu_thread_group_start recorded, for phase C1 and for the adapter's printer. */
typedef struct ps3_lv2_spu_start_snapshot {
    uint32_t valid, group, num, priority;
    uint32_t thread_id[PS3_LV2_SPU_THREADS_MAX];
    uint32_t entry[PS3_LV2_SPU_THREADS_MAX];
    uint64_t arg1[PS3_LV2_SPU_THREADS_MAX];
    uint64_t ls_hash[PS3_LV2_SPU_THREADS_MAX];   /* FNV-1a over the first 64 bytes of that thread's LS */
} ps3_lv2_spu_start_snapshot;

typedef struct ps3_lv2_spu_images {
    ps3_instance_guard guard;
    uint32_t serial;
    ps3_lv2_spu_image images[PS3_LV2_SPU_IMAGE_SEGMENTS_MAX];
    /* The system arena's slot bitmap: one bit per PS3_LV2_SPU_ARENA_SLOT. */
    uint32_t arena_used[PS3_LV2_SPU_ARENA_SLOTS / 32];
    unsigned initialized;
} ps3_lv2_spu_images;

typedef struct ps3_lv2_spu_groups {
    ps3_instance_guard guard;
    uint32_t serial_group, serial_thread;
    uint32_t usable_spus;        /* from ps3_spu_resource_policy: how many the title may use */
    unsigned char *local_stores; /* borrowed: PS3_LV2_SPU_GROUP_CAP * PS3_LV2_SPU_THREADS_MAX * PS3_LV2_SPU_LS_BYTES */
    ps3_lv2_spu_group groups[PS3_LV2_SPU_GROUP_CAP];
    ps3_lv2_spu_start_snapshot snapshot;
    unsigned inject_provider_failure;   /* test-only: drives the adapter's internal-code mapping */
    unsigned initialized;
} ps3_lv2_spu_groups;

/* Storage: zero-filled caller storage, one-shot init (a second init is EINVAL and changes nothing). */
uint32_t ps3_lv2_spu_images_init(ps3_lv2_spu_images *, ps3_instance_guard);
/* `local_stores` must hold group_cap * threads_max * PS3_LV2_SPU_LS_BYTES bytes and outlive the groups. */
uint32_t ps3_lv2_spu_groups_init(ps3_lv2_spu_groups *, ps3_instance_guard, unsigned char *local_stores, uint32_t usable_spus);
void ps3_lv2_spu_groups_set_usable(ps3_lv2_spu_groups *, uint32_t usable_spus);

/* ---- images --------------------------------------------------------------------------------- */
/* Parse the SPU ELF a guest already read (`elf`, `elf_length` bytes) and fill `image`.
 * `segments` is the caller's buffer for the guest-visible segs array; both it and the ELF stay the
 * caller's. Returns PS3_LV2_SPU_OK, or EINVAL for a non-ELF / wrong e_machine / truncated headers, or
 * ENOMEM when the image has more PT_LOAD pairs than PS3_LV2_SPU_IMAGE_SEGMENTS_MAX.
 * On success `*written` is the number of ps3_spu_segment_t records filled. */
uint32_t ps3_lv2_spu_image_parse(ps3_lv2_spu_images *, const unsigned char *elf, size_t elf_length,
                             ps3_lv2_spu_image *image, unsigned char *segments, size_t segments_capacity,
                             uint32_t *written);
/* Register a successfully parsed image so close can retire it. Idempotent per source address. */
uint32_t ps3_lv2_spu_image_register(ps3_lv2_spu_images *, const ps3_lv2_spu_image *image);
uint32_t ps3_lv2_spu_image_close(ps3_lv2_spu_images *, uint32_t image_ea);
/* Reserve `size` bytes in the SYSTEM arena and return the guest EA. Only 256-byte-aligned sizes from
 * 1 to PS3_LV2_SPU_ARENA_SLOT are covered by one slot; a larger request takes several contiguous
 * slots. Exhaustion is ENOMEM (the guest's own error); nothing is fabricated. */
uint32_t ps3_lv2_spu_arena_allocate(ps3_lv2_spu_images *, uint32_t size, uint32_t *ea);
uint32_t ps3_lv2_spu_arena_free(ps3_lv2_spu_images *, uint32_t ea);
uint32_t ps3_lv2_spu_arena_used_count(ps3_lv2_spu_images *);
/* Test-only provider-failure injection: when `armed`, the next read_ls/write_ls answers
 * PS3_LV2_SPU_PROVIDER_FAILURE so the adapter's internal-code mapping can be driven. Inert otherwise. */
void ps3_lv2_spu_inject_provider_failure(ps3_lv2_spu_groups *, int armed);
uint32_t ps3_lv2_spu_image_find(ps3_lv2_spu_images *, uint32_t image_ea, ps3_lv2_spu_image *out);

/* ---- groups --------------------------------------------------------------------------------- */
uint32_t ps3_lv2_spu_group_create(ps3_lv2_spu_groups *, uint32_t *id, uint32_t num, uint32_t priority, uint32_t type);
/* Allowed before start; EBUSY once started; ESRCH when absent. */
uint32_t ps3_lv2_spu_group_destroy(ps3_lv2_spu_groups *, uint32_t id);
/* Copy `segments` (nsegs records of 24 bytes) into this thread's local store: COPY records copy their
 * source bytes, FILL records write their fill byte. Anything past the copied bytes stays zero (bss).
 * `source` is the guest image the COPY records index into. */
uint32_t ps3_lv2_spu_thread_initialize(ps3_lv2_spu_groups *, uint32_t *thread_id, uint32_t group_id, uint32_t spu_number,
                                   const ps3_lv2_spu_image *image, const unsigned char *segments, uint32_t nsegs,
                                   const unsigned char *source, size_t source_length,
                                   const uint64_t arg[4]);
/* The guest-memory form: a COPY record's source is an EA, so each record is fetched through `memory`
 * (the composition's coherent guest port) straight into the thread's local store. Same rules and the
 * same refusals as the contiguous form above; nothing is copied when a record is refused. */
uint32_t ps3_lv2_spu_thread_initialize_gather(ps3_lv2_spu_groups *, uint32_t *thread_id, uint32_t group_id,
                                          uint32_t spu_number, const ps3_lv2_spu_image *image,
                                          const unsigned char *segments, uint32_t nsegs,
                                          const ps3_guest_memory *memory, const uint64_t arg[4]);
uint32_t ps3_lv2_spu_thread_get_exit_status(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t *status);
uint32_t ps3_lv2_spu_thread_set_argument(ps3_lv2_spu_groups *, uint32_t thread_id, const uint64_t arg[4]);
/* size must be 1, 2, 4 or 8 and the local-store address must be aligned to it. */
uint32_t ps3_lv2_spu_thread_read_ls(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t ls_address, uint64_t *value, size_t size);
uint32_t ps3_lv2_spu_thread_write_ls(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t ls_address, uint64_t value, size_t size);
uint32_t ps3_lv2_spu_group_set_priority(ps3_lv2_spu_groups *, uint32_t id, uint32_t priority);
uint32_t ps3_lv2_spu_group_get_priority(ps3_lv2_spu_groups *, uint32_t id, uint32_t *priority);
uint32_t ps3_lv2_spu_group_terminate(ps3_lv2_spu_groups *, uint32_t id);
uint32_t ps3_lv2_spu_group_join(ps3_lv2_spu_groups *, uint32_t id);

/* ---- Decision 33 / C1b: the run outcome the runner records ------------------------------------- */
/* sys_spu_thread_group_join causes (lv2). */
enum { PS3_LV2_SPU_GROUP_JOIN_ALL_THREADS_EXIT = 0x2, PS3_LV2_SPU_GROUP_JOIN_GROUP_EXIT = 0x1 };
/* Records that `thread_id` reached a terminal. `exited` marks stop 0x102 (exit_status is meaningful then);
 * `group_exit` marks stop 0x101, which makes the whole group's join cause GROUP_EXIT. Idempotent per thread: the
 * FIRST terminal wins, so a later cancel never overwrites a real exit. Returns OK, or ESRCH/ESTAT. */
uint32_t ps3_lv2_spu_thread_record_end(ps3_lv2_spu_groups *, uint32_t thread_id, unsigned exited,
                                       uint32_t exit_status, unsigned group_exit);
/* Binds a queue to this thread for stop 0x110 (sys_spu_thread_bind_queue, 193). EBUSY when one is already bound. */
uint32_t ps3_lv2_spu_thread_bind_queue(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t queue_id);
uint32_t ps3_lv2_spu_thread_unbind_queue(ps3_lv2_spu_groups *, uint32_t thread_id);
/* The queue bound by 193/194 (0 = none). */
uint32_t ps3_lv2_spu_thread_bound_queue(const ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t *queue_id);
/* How many initialized threads of `id` have not reached a terminal, and the join cause once none have. */
uint32_t ps3_lv2_spu_group_pending(const ps3_lv2_spu_groups *, uint32_t id, uint32_t *pending, uint32_t *cause);
/* Event connects. `port` is a ps3_lv2_objects port id (0 = none). */
uint32_t ps3_lv2_spu_group_connect_event(ps3_lv2_spu_groups *, uint32_t id, uint32_t port, uint32_t event);
uint32_t ps3_lv2_spu_group_disconnect_event(ps3_lv2_spu_groups *, uint32_t id);
uint32_t ps3_lv2_spu_thread_connect_event(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t port, uint32_t event, int bind);
uint32_t ps3_lv2_spu_thread_disconnect_event(ps3_lv2_spu_groups *, uint32_t thread_id);
uint32_t ps3_lv2_spu_thread_user_port(ps3_lv2_spu_groups *, uint32_t thread_id, uint32_t queue, uint32_t spup);
/* Pick an SPU number (0..63) free across every thread of the group and hand it back. */
uint32_t ps3_lv2_spu_group_connect_event_all_threads(ps3_lv2_spu_groups *, uint32_t id, uint32_t port, uint64_t request,
                                                 uint32_t *spu_number);
/* Validate, record the snapshot, mark started. Never runs anything. ESTAT when a thread is not
 * initialized, ESRCH when absent, EBUSY when already started. */
uint32_t ps3_lv2_spu_group_start(ps3_lv2_spu_groups *, uint32_t id);
uint32_t ps3_lv2_spu_group_snapshot(const ps3_lv2_spu_groups *, ps3_lv2_spu_start_snapshot *out);
/* FNV-1a over the first `length` bytes; exposed so a caller can hash the same way the snapshot does. */
uint64_t ps3_lv2_spu_ls_hash(const unsigned char *bytes, size_t length);
#ifdef __cplusplus
}
#endif
#endif
