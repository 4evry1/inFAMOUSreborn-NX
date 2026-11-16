#ifndef PS3_USER_MEMORY_H
#define PS3_USER_MEMORY_H
#include "guest_space.h"
#include "../sync/guard.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PS3_USER_MEMORY_ALIGN_64K UINT64_C(65536)
#define PS3_USER_MEMORY_ALIGN_1M UINT64_C(1048576)
typedef enum ps3_user_memory_status {
 PS3_USER_MEMORY_OK=0, PS3_USER_MEMORY_INVALID_ARGUMENT,
 PS3_USER_MEMORY_QUOTA_EXCEEDED, PS3_USER_MEMORY_CAPACITY_EXCEEDED,
 PS3_USER_MEMORY_NO_SPACE, PS3_USER_MEMORY_NOT_OWNED,
 PS3_USER_MEMORY_MEMORY_FAILURE, PS3_USER_MEMORY_CHARGES_OUTSTANDING
} ps3_user_memory_status;
typedef struct ps3_user_memory_result {
    ps3_user_memory_status status;
    ps3_guest_memory_status memory_status;
} ps3_user_memory_result;
typedef struct ps3_user_memory_slot { uint32_t address; uint64_t size; } ps3_user_memory_slot;
/* Composition grants exclusive ownership of an initially uncommitted window.
 * The allocator guard must differ from any guard used by memory callbacks.
 * Storage, ports and instance remain alive until all callers have joined.
 * Init requires fresh/quiescent storage; it must not replace a live instance.
 * Instance, slots, config and operation output storage must not overlap each
 * other or the managed guest window; callers must not mutate live state. */
typedef struct ps3_user_memory_config {
 uint64_t window_begin, window_end, quota_bytes;
 ps3_guest_memory memory;
 ps3_instance_guard guard;
} ps3_user_memory_config;
/* Address-less quota charge (e.g. lv2 container/stack accounting). id 0 = free. */
typedef struct ps3_user_memory_charge_record { uint64_t id, size; } ps3_user_memory_charge_record;
struct ps3_user_memory;
typedef struct ps3_user_memory_charge_token { const struct ps3_user_memory *owner; size_t slot; uint64_t id; } ps3_user_memory_charge_token;
typedef struct ps3_user_memory {
 ps3_user_memory_config config;
 ps3_user_memory_slot *slots;
 size_t capacity;
 uint64_t used_bytes;
 size_t allocation_count;
 ps3_guest_memory_status last_memory_status;
 ps3_user_memory_charge_record *charges;
 size_t charge_capacity;
 uint64_t next_charge_id;
} ps3_user_memory;
typedef struct ps3_user_memory_info {
 uint64_t quota_bytes, used_bytes, available_bytes;
 size_t allocation_count;
 ps3_guest_memory_status last_memory_status;
} ps3_user_memory_info;
ps3_user_memory_status ps3_user_memory_init(
    ps3_user_memory *,
    const ps3_user_memory_config *,
    ps3_user_memory_slot *,
    size_t);
/* Only these two exact alignments are supported; size must be a nonzero exact
 * multiple. Address output is untouched on failure. Callback failures preserve
 * owned ranges/accounting; the returned memory_status is correlated to this operation (OK when no
 * callback failed). Query status is diagnostic only. */
ps3_user_memory_result ps3_user_memory_allocate(
    ps3_user_memory *,
    uint64_t,
    uint64_t,
    uint32_t *);
ps3_user_memory_result ps3_user_memory_free(
    ps3_user_memory *,
    uint32_t);
ps3_user_memory_status ps3_user_memory_query(
    ps3_user_memory *,
    ps3_user_memory_info *);
/* Charges share used_bytes/quota with allocations. Attach a caller-owned table
 * once before concurrent use. charge: QUOTA_EXCEEDED/CAPACITY_EXCEEDED change
 * nothing (token untouched). uncharge: only the live token releases exactly its
 * size; stale, forged, repeated or other-allocator tokens return NOT_OWNED
 * without effect (token untouched). cleanup() refuses with CHARGES_OUTSTANDING
 * (nothing freed) until every charge owner has joined and uncharged. */
ps3_user_memory_status ps3_user_memory_attach_charges(ps3_user_memory *, ps3_user_memory_charge_record *, size_t);
ps3_user_memory_status ps3_user_memory_charge(ps3_user_memory *, uint64_t, ps3_user_memory_charge_token *);
ps3_user_memory_status ps3_user_memory_uncharge(ps3_user_memory *, ps3_user_memory_charge_token *);
/* Attempts every allocation once; successful frees remain freed, failed ranges
 * remain owned. Return failure if any decommit failed; retry is supported. */
ps3_user_memory_result ps3_user_memory_cleanup(
    ps3_user_memory *);
#ifdef __cplusplus
}
#endif
#endif
