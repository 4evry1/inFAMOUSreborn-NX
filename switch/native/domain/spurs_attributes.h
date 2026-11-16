#ifndef NATIVE_SPURS_ATTRIBUTES_H
#define NATIVE_SPURS_ATTRIBUTES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_SPURS_ATTRIBUTE_SIZE = 512,
    PS3_SPURS_ATTRIBUTE_ALIGNMENT = 8,
    PS3_SPURS_ATTRIBUTE_PREFIX = 0x15,
    PS3_SPURS_ATTRIBUTE_PREFIX_CAPACITY = 15,
    PS3_SPURS_ATTRIBUTE_PREFIX_SIZE = 0x24,
    PS3_SPURS_ATTRIBUTE_FLAGS = 0x28,
    PS3_SPURS_ATTRIBUTE_CONTAINER = 0x2c,
    PS3_SPURS_ATTRIBUTE_GROUP_NORMAL = 0,
    PS3_SPURS_ATTRIBUTE_GROUP_EXCLUSIVE = 0x18
};
#define PS3_SPURS_ATTRIBUTE_PRINTF_FLAG UINT32_C(0x10000000)
#define PS3_SPURS_ATTRIBUTE_EXCLUSIVE_FLAG UINT32_C(0x20000000)
#define PS3_SPURS_ATTRIBUTE_CONTAINER_FLAG UINT32_C(0x40000000)

typedef enum ps3_spurs_attribute_status {
    PS3_SPURS_ATTRIBUTE_OK = 0,
    PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT = 1,
    PS3_SPURS_ATTRIBUTE_NULL = 0x80410711u,
    PS3_SPURS_ATTRIBUTE_ALIGN = 0x80410710u,
    PS3_SPURS_ATTRIBUTE_INVAL = 0x80410702u,
    PS3_SPURS_ATTRIBUTE_STAT = 0x8041070fu
} ps3_spurs_attribute_status;

typedef struct ps3_spurs_attribute_initialization {
    uint32_t revision, sdk_version, n_spus;
    int32_t spu_priority, ppu_priority;
    uint32_t exit_if_no_work;
} ps3_spurs_attribute_initialization;

/* Validation is separate from flag transformation so guest semantic failures
 * precede unnecessary memory access. No function owns or caches guest state. */
ps3_spurs_attribute_status ps3_spurs_attribute_validate(uint32_t address);
ps3_spurs_attribute_status ps3_spurs_attribute_validate_prefix(
    uint32_t address, uint32_t prefix, uint32_t size);
ps3_spurs_attribute_status ps3_spurs_attribute_validate_group_type(
    uint32_t address, int32_t type);
ps3_spurs_attribute_status ps3_spurs_attribute_initialize(uint32_t address,
    const ps3_spurs_attribute_initialization *arguments,
    unsigned char image[PS3_SPURS_ATTRIBUTE_SIZE]);
ps3_spurs_attribute_status ps3_spurs_attribute_group_flags(
    int32_t type, uint32_t flags, uint32_t *updated);
ps3_spurs_attribute_status ps3_spurs_attribute_container_flags(
    uint32_t flags, uint32_t *updated);
uint32_t ps3_spurs_attribute_printf_flags(uint32_t flags);

/* ---- _cellSpursWorkloadAttributeInitialize / cellSpursWorkloadAttributeSetName ----
 * Pure domain: no guest memory, no policy-module dereference, no state, no I/O.
 * Policy codes are 804108xx, distinct from the instance-attribute 804107xx codes. */
enum {
    PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE = 512,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_ALIGNMENT = 8,
    PS3_SPURS_WORKLOAD_POLICY_MODULE_ALIGNMENT = 16,
    PS3_SPURS_WORKLOAD_PRIORITY_COUNT = 8,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_CLASS_OFFSET = 0x28,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_INSTANCE_OFFSET = 0x2c,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE = 8
};
typedef enum ps3_spurs_workload_attribute_status {
    PS3_SPURS_WORKLOAD_ATTRIBUTE_OK = 0,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT = 1,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL  = 0x80410811u,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN = 0x80410810u,
    PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL = 0x80410802u
} ps3_spurs_workload_attribute_status;
typedef struct ps3_spurs_workload_attribute_request {
    uint32_t attribute, revision, sdk_version, policy_module, size;
    uint64_t data;
    unsigned char priority[PS3_SPURS_WORKLOAD_PRIORITY_COUNT]; /* snapshot taken before zeroing */
    uint32_t min_contention, max_contention;
} ps3_spurs_workload_attribute_request;
/* SDK steps 1-4: attr NULL, attr%8, pm NULL, pm%16. Runs BEFORE the priority read. Pure; pm is never dereferenced. */
ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_validate_pointers(
    uint32_t attribute, uint32_t policy_module);
/* SDK step 5: runs AFTER the 8-byte priority snapshot. Result is INVAL if min==0 || any (byte & 0xF0).
 * A NULL priority pointer gives INVALID_HOST_ARGUMENT. */
ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_validate_contention(
    uint32_t min_contention, const unsigned char priority[PS3_SPURS_WORKLOAD_PRIORITY_COUNT]);
/* Pure 512-byte builder. Order: (!request||!image) gives HOST; then validate_pointers; then validate_contention.
 * Any non-OK result leaves the image UNTOUCHED. On OK: memset 0 over all 512 bytes, then encode
 * BE32 0x00 revision, 0x04 sdk, 0x08 pm, 0x0C size; BE64 0x10 data; raw 0x18..0x1F priority;
 * BE32 0x20 min, 0x24 max. Bytes 0x28..0x1FF stay 0.
 * No revision/sdk/size/max or min<=max checks.
 * The request is snapshotted locally before the image is zeroed, so valid host aliasing is safe. */
ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_build(
    const ps3_spurs_workload_attribute_request *request,
    unsigned char image[PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE]);
/* cellSpursWorkloadAttributeSetName 8-byte patch: BE32 name_class at +0x28 then BE32 name_instance at +0x2c
 * (offsets are the NAME_*_OFFSET constants; the patch itself is 8 bytes: class then instance).
 * NULL patch => INVALID_HOST_ARGUMENT; then attr NULL => POLICY_NULL; attr%8 => POLICY_ALIGN; else encode.
 * Errors leave patch untouched. No initialization/revision/SDK/name-pointer/string validation: names are
 * opaque numbers (may be NULL, equal, unmapped or inside the attribute) and are never dereferenced. */
ps3_spurs_workload_attribute_status ps3_spurs_workload_attribute_name_patch(
    uint32_t attribute, uint32_t name_class, uint32_t name_instance,
    unsigned char patch[PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE]);

#ifdef __cplusplus
}
#endif
#endif
