/* Instance-owned native GCM address space: the main-memory EA <-> RSX IO-offset mapping table and
 * the LOCAL (video) window, as a pure value core.
 *
 * Derived from the desktop reference source/ps3recomp/libs/video/cellGcmSys.c
 * (cellGcmInit :409, cellGcmMapMainMemory :3256, cellGcmMapEaIoAddress :3303, cellGcmUnmapEaIoAddress
 * :3341, cellGcmUnmapIoAddress :3357, cellGcmAddressToOffset :3124, cellGcmIoOffsetToAddress :3373,
 * cellGcmResolveLocated :3443; error values ps3emu/error_codes.h:160-165). Where this core is
 * deliberately NARROWER or different than that desktop code it is marked [P] (native admission policy);
 * see evidence/sonnet-native-gcm-address-space-r1/CHECKPOINT.md.
 *
 * Contract:
 *  - No globals, no guest memory, no vm_base, no getenv/printf, no platform headers. Mapping storage is
 *    caller-supplied and caller-owned; the config is copied at init. Do not share one slots[] array
 *    between instances.
 *  - Single owner: calls on one instance must be serialized by its owner. No locks, no atomics.
 *  - Every operation validates completely before it publishes anything. A non-OK status means the
 *    instance, the slot storage and every out-parameter are unchanged ("no effect"). (The desktop code
 *    writes 0 to the guest out-param on some failures; the guest-visible failure write is the wiring's job.)
 *  - 1 MiB page granularity, as the desktop offset tables. Whole-mapping unmap only; there is no
 *    partial unmap and no interior-address unmap.
 *  - [P] No auto-mapping of unmapped EAs (desktop AddressToOffset heuristic) and no table-first
 *    "ResolveOffset" guess: location is an explicit argument everywhere an offset is resolved.
 */
#ifndef NATIVE_GCM_ADDRESS_SPACE_H
#define NATIVE_GCM_ADDRESS_SPACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_AS_PAGE_SIZE 0x100000u      /* 1 MiB: cellGcmSys.c populate_offset_table (">> 20") */
#define GCM_AS_LOCAL_BASE 0xC0000000u   /* cellGcmSys.c:435 s_config.localAddress (caller still supplies it) */

/* Cell error values: guest-visible CELL_GCM_ERROR_* as retained from live RPCS3
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellGcmSys.h (git blob
 * 0d07bf62a71520d97a372791ad59ad5e9269944f; bytes in evidence/opus-native-gcm-map-review-r1/root-abi-verification.json):
 * FAILURE 0x802100ff, NO_IO_PAGE_TABLE 0x80210001, INVALID_ENUM 0x80210002, INVALID_VALUE 0x80210003,
 * INVALID_ALIGNMENT 0x80210004, ADDRESS_OVERWRAP 0x80210005. The desktop reference
 * (ps3emu/error_codes.h, base 0x80210700) uses 0x802107xx, which does NOT match; it is not used here and is
 * left unchanged. Only the four values this core can convert are defined. */
#define GCM_AS_CELL_OK 0u
#define GCM_AS_CELL_ERROR_FAILURE 0x802100FFu
#define GCM_AS_CELL_ERROR_INVALID_VALUE 0x80210003u
#define GCM_AS_CELL_ERROR_INVALID_ALIGNMENT 0x80210004u
#define GCM_AS_CELL_ERROR_ADDRESS_OVERWRAP 0x80210005u

/* Location of an RSX offset. 0 is deliberately INVALID: the desktop `int local` (nonzero = local) and a
 * raw cell location integer (0 = local) have opposite polarity, so no raw integer may alias silently.
 * Converting wire/method bits into this enum is the decoder wiring's job. */
#define GCM_AS_LOCATION_INVALID 0u
#define GCM_AS_LOCATION_LOCAL 1u
#define GCM_AS_LOCATION_MAIN 2u

typedef enum gcm_as_status {
    GCM_AS_OK = 0,
    GCM_AS_INVALID_ARGUMENT,   /* NULL pointer / bad storage argument */
    GCM_AS_NOT_INITIALIZED,    /* instance never successfully initialized */
    GCM_AS_CONFIG_INVALID,     /* init config refused */
    GCM_AS_INVALID_VALUE,      /* zero length, bad location, slot index out of range */
    GCM_AS_MISALIGNED,         /* ea / io / size not 1 MiB aligned (or size 0 for map) */
    GCM_AS_RANGE_OVERFLOW,     /* base+length leaves the 32-bit space */
    GCM_AS_EA_FORBIDDEN,       /* [P] EA page 0 or intersecting the local window */
    GCM_AS_EA_OVERLAP,         /* [P] EA range intersects an active mapping (native policy refusal) */
    GCM_AS_EA_ALREADY_MAPPED,  /* map_ea_io only: EA equals an active mapping's base (desktop ADDRESS_OVERWRAP case; precedes range/IO refusals) */
    GCM_AS_IO_RANGE,           /* [P] IO range ends beyond io_space_size */
    GCM_AS_IO_OVERLAP,         /* [P] IO range intersects an active mapping */
    GCM_AS_NO_SLOT,            /* mapping storage exhausted */
    GCM_AS_NO_IO_SPACE,        /* no free IO range of the requested size */
    GCM_AS_NOT_MAPPED,         /* translation/query: no active mapping at the start of the range / free slot */
    GCM_AS_UNMAP_NOT_FOUND,    /* unmap_*: no active mapping with that exact base (desktop FAILURE case) */
    GCM_AS_RANGE_SPANS,        /* [P] range starts mapped but leaves that single record. A native refusal of a
                                  whole-span translation, NOT evidence that the guest address is illegal. */
    GCM_AS_LOCAL_RANGE,        /* local range leaves the local window */
    GCM_AS_PROTECTED           /* [P] the init mapping cannot be unmapped */
} gcm_as_status;

typedef struct gcm_as_mapping {
    uint32_t ea, io, size;
    uint8_t active;
    uint8_t initial;           /* the cellGcmInit mapping (io 0) */
} gcm_as_mapping;

typedef struct gcm_as_config {
    uint32_t local_base, local_size;   /* LOCAL window [base, base+size) */
    uint32_t io_ea, io_size;           /* init mapping: EA io_ea -> IO offset 0 (cellGcmInit ioAddress/ioSize) */
    uint32_t io_space_size;            /* [P] caller-chosen size of the IO offset space */
    gcm_as_mapping *slots;             /* caller storage; slot 0 receives the init mapping */
    uint32_t slot_count;
} gcm_as_config;

typedef struct gcm_address_space {
    uint32_t magic;
    gcm_as_config cfg;
    uint32_t active;
} gcm_address_space;

/* Storage lifecycle and ownership (the core never validates arbitrary pointers; these are caller obligations):
 *  - A gcm_address_space is "initialized" only after gcm_as_init returned GCM_AS_OK. Before that, every other call
 *    recognises it only if the storage is ZERO-initialized (e.g. memset 0 / static) or left by a FAILED init
 *    (which writes nothing). The check reads the `magic` field, so it is NOT meaningful on arbitrary
 *    uninitialized storage: pass either zeroed storage or storage that was successfully initialized. gcm_as_init
 *    itself never reads the previous contents of *as and may be called on any bytes.
 *  - After a successful init, the stored `cfg` (including cfg.slots and cfg.slot_count) and the slots array
 *    are immutable to the caller: they change only through the gcm_as_* core calls. The structs are public only
 *    for sizing/embedding. Writing cfg.slot_count larger than the real slot array, repointing cfg.slots, or editing
 *    a slot makes later calls read/write outside the array or corrupt the tables; the core cannot detect that.
 *    To change geometry, call gcm_as_init again (a valid reset; see the alias contract below).
 *  - Output pointers (io_out, location_out, offset_out, ea_out, info_out, cell_out) must not alias the instance,
 *    its stored config, or the slots array: map_main_memory writes *io_out after publishing the mapping, and
 *    queries write outputs only on success. location_out and offset_out must be distinct objects.
 *    Outputs are written only when the call returns GCM_AS_OK (cell_out only when
 *    gcm_as_status_to_cell_error returns 1). */

/* Alias contract: *cfg may be the instance's own stored config (&as->cfg), i.e. re-init/reset from itself:
 * the config is copied before anything is written. Init never reads prior *as contents, so *as may be
 * uninitialized storage. Refused before effects (CONFIG_INVALID): slots[0..slot_count) overlapping the bytes of
 * *as or *cfg, or whose byte size/address range wraps.
 * [P] Init requires all of local_base/local_size/io_ea/io_size/io_space_size nonzero (local_base
 * and io_ea) / 1 MiB aligned, no 32-bit wrap (an end of exactly 2^32 is valid), io_ea >= 1 MiB,
 * io_size <= io_space_size, init EA range disjoint from the local window, slot_count >= 1.
 * Slots are zeroed only after validation. Re-initializing an instance is allowed. */
gcm_as_status gcm_as_init(gcm_address_space *as, const gcm_as_config *cfg);

/* cellGcmMapMainMemory: IO range chosen by the core (first fit, lowest 1 MiB-aligned free range at or
 * above io_size). [P] first fit instead of the desktop bump allocator, so unmapped ranges are reusable. */
gcm_as_status gcm_as_map_main_memory(gcm_address_space *as, uint32_t ea, uint32_t size, uint32_t *io_out);
/* cellGcmMapEaIoAddress: caller-chosen IO offset. Check order: alignment (MISALIGNED), then an exact-base duplicate
 * EA (EA_ALREADY_MAPPED, reported even if the range would overflow or the IO range is outside the config), then the
 * native range/IO/overlap policy refusals. */
gcm_as_status gcm_as_map_ea_io(gcm_address_space *as, uint32_t ea, uint32_t io, uint32_t size);
/* cellGcmUnmapEaIoAddress / cellGcmUnmapIoAddress: exact base of an active, non-initial mapping. */
gcm_as_status gcm_as_unmap_ea(gcm_address_space *as, uint32_t ea);
gcm_as_status gcm_as_unmap_io(gcm_address_space *as, uint32_t io);

/* cellGcmAddressToOffset generalised to a byte range [address, address+length). The whole range must lie in
 * the local window (LOCAL) or inside ONE mapping (MAIN); a range crossing two EA-adjacent mappings is
 * refused because their IO offsets are not contiguous. */
gcm_as_status gcm_as_address_to_offset(const gcm_address_space *as, uint32_t address, uint32_t length,
                                       uint32_t *location_out, uint32_t *offset_out);
/* cellGcmIoOffsetToAddress for a range: MAIN only, whole range inside one mapping. */
gcm_as_status gcm_as_io_to_address(const gcm_address_space *as, uint32_t io, uint32_t length, uint32_t *ea_out);
/* cellGcmResolveLocated with an explicit, validated location; [P] LOCAL offsets are bounds-checked. */
gcm_as_status gcm_as_resolve(const gcm_address_space *as, uint32_t location, uint32_t offset, uint32_t length,
                             uint32_t *ea_out);

/* Query: copies slot `slot` (active or not is reflected in info.active); INVALID_VALUE if out of range,
 * NOT_MAPPED if that slot is free. */
gcm_as_status gcm_as_get_mapping(const gcm_address_space *as, uint32_t slot, gcm_as_mapping *info_out);
uint32_t gcm_as_active_count(const gcm_address_space *as);   /* 0 if uninitialized */

/* Status -> cell error value, ONLY where the desktop reference retains that exact failure:
 *   OK -> 0; MISALIGNED -> INVALID_ALIGNMENT (MapMainMemory/MapEaIoAddress alignment checks);
 *   EA_ALREADY_MAPPED -> ADDRESS_OVERWRAP (MapEaIoAddress exact-ea duplicate);
 *   UNMAP_NOT_FOUND -> FAILURE (UnmapEaIoAddress/UnmapIoAddress miss).
 * Returns 1 and writes *cell_out for those. Every other status (native policy refusals, host API misuse,
 * capacity, translation misses, PROTECTED, unknown values) is NOT convertible: returns 0 and leaves *cell_out
 * unchanged. A guest-visible adapter must STOP on a non-convertible status; it must not invent a cell error.
 * (E.g. PROTECTED is a native restriction: the desktop reference allows exact-base unmap of the init mapping.
 * NO_SLOT maps to the reference FAILURE only if the adapter supplied the reference's 16 slots,
 * CELL_GCM_MAX_IO_MAPPINGS, which this core cannot know.) */
int gcm_as_status_to_cell_error(gcm_as_status status, uint32_t *cell_out);
/* Stable name for evidence logs. */
const char *gcm_as_status_name(gcm_as_status status);

#ifdef __cplusplus
}
#endif
#endif
