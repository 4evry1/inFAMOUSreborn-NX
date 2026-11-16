#ifndef PS3_USER_MEMORY_SERVICE_H
#define PS3_USER_MEMORY_SERVICE_H
#include "atomic_domain.h"
#include "user_memory.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ps3_user_memory *allocator;
    ps3_atomic_domain *domain;
} ps3_user_memory_service;
typedef enum {
    PS3_USER_MEMORY_SERVICE_OK = 0,
    PS3_USER_MEMORY_SERVICE_ARGUMENT,
    PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE,
    PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE,
    PS3_USER_MEMORY_SERVICE_OVERFLOW,
    PS3_USER_MEMORY_SERVICE_ROLLBACK_FAILURE,
    PS3_USER_MEMORY_SERVICE_PIN_RELEASE_FAILURE
} ps3_user_memory_service_status;
typedef struct {
    ps3_user_memory_service_status status;
    ps3_guest_memory_status memory_status;
    ps3_guest_memory_status pin_release_status;
    ps3_user_memory_result allocator, rollback;
    int published;
    int retained_allocation;
    uint32_t retained_address;
    int retained_pin;
    ps3_atomic_pin pin;
} ps3_user_memory_service_result;

/* Borrowed live instances: allocator memory must be this domain's coherent port.
 * Composition owns both and all guards; allocator and domain guards are distinct.
 * Calls never hold the domain guard across an allocator operation. All mapping
 * retirement and guest writes must use that domain. The domain validates physical
 * mapping/lifetime, not ELF or guest ABI write permissions: the native ABI must
 * validate the writable guest scope before invoking publication. Host cancellation/nonlocal
 * unwind through these operations is forbidden. Service/result storage must not
 * alias guest backing or collaborator state. No service-wide serialization.
 *
 * The complete output range is pinned before allocation/query. Publication is
 * one all-or-nothing coherent write; an allocation is rolled back on write
 * failure. A retained allocation remains charged and caller-owned: retry
 * free_owned or allocator cleanup. A retained pin must be explicitly released
 * on the original live domain before its destruction; never discard the result.
 * Valid pin release is total under the domain contract; its unexpected failure
 * is reported without hiding the handle. Outputs can have changed only when
 * publication succeeded (including an unexpected subsequent release failure).
 * Query snapshots linearize under the allocator guard and may age afterward.
 * Byte addresses may be unaligned; full range validity is checked by the domain.
 * Query publishes quota followed by available bytes as two BE32 words; values
 * exceeding UINT32_MAX fail without truncation. memory_status distinguishes
 * invalid output ranges from backing/pin capacity failures. Raw syscall decoding
 * and CELL error policy belong to composition. */
ps3_user_memory_service_result ps3_user_memory_allocate_and_publish_be32(
    ps3_user_memory_service *, uint64_t size, uint64_t alignment, uint64_t output);
ps3_user_memory_service_result ps3_user_memory_free_owned(
    ps3_user_memory_service *, uint32_t address);
ps3_user_memory_service_result ps3_user_memory_query_and_publish_be32(
    ps3_user_memory_service *, uint64_t output);
#ifdef __cplusplus
}
#endif
#endif
