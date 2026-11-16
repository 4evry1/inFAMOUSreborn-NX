/* Native GCM address space core. See gcm_address_space.h for the contract and CHECKPOINT.md for sources. */
#include "gcm_address_space.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GCM_AS_MAGIC 0x47434D41u /* "GCMA" */
#define PAGE_MASK (GCM_AS_PAGE_SIZE - 1u)
#define SPACE_END ((uint64_t)1 << 32)

static int aligned(uint32_t v) { return (v & PAGE_MASK) == 0; }

static int ranges_overlap_bytes(const void *a, size_t an, const void *b, size_t bn) {
    uintptr_t a0 = (uintptr_t)a, b0 = (uintptr_t)b;
    if (a0 + an < a0 || b0 + bn < b0) return 1;   /* wrapping range: treat as aliasing */
    return a0 < b0 + bn && b0 < a0 + an;
}

static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }

static int ready(const gcm_address_space *as) {
    return as && as->magic == GCM_AS_MAGIC && as->cfg.slots && as->cfg.slot_count >= 1;
}

gcm_as_status gcm_as_init(gcm_address_space *as, const gcm_as_config *cfg) {
    if (!as || !cfg) return GCM_AS_INVALID_ARGUMENT;
    gcm_as_config c = *cfg;   /* validate a private copy so cfg aliasing *as cannot matter */
    if (!c.slots || c.slot_count < 1) return GCM_AS_CONFIG_INVALID;
    if ((uint64_t)c.slot_count * sizeof(gcm_as_mapping) > (uint64_t)SIZE_MAX) return GCM_AS_CONFIG_INVALID;
    {
        size_t slot_bytes = sizeof(gcm_as_mapping) * (size_t)c.slot_count;
        if (ranges_overlap_bytes(c.slots, slot_bytes, as, sizeof *as) ||
            ranges_overlap_bytes(c.slots, slot_bytes, cfg, sizeof *cfg)) return GCM_AS_CONFIG_INVALID;
    }
    if (c.local_base == 0 || !aligned(c.local_base) || c.local_size == 0 || !aligned(c.local_size)) return GCM_AS_CONFIG_INVALID;
    if ((uint64_t)c.local_base + c.local_size > SPACE_END) return GCM_AS_CONFIG_INVALID;
    if (c.io_ea < GCM_AS_PAGE_SIZE || !aligned(c.io_ea) || c.io_size == 0 || !aligned(c.io_size)) return GCM_AS_CONFIG_INVALID;
    if ((uint64_t)c.io_ea + c.io_size > SPACE_END) return GCM_AS_CONFIG_INVALID;
    if (c.io_space_size == 0 || !aligned(c.io_space_size) || c.io_size > c.io_space_size) return GCM_AS_CONFIG_INVALID;
    if (overlaps(c.io_ea, (uint64_t)c.io_ea + c.io_size, c.local_base, (uint64_t)c.local_base + c.local_size))
        return GCM_AS_CONFIG_INVALID;
    /* Validated: publish. */
    memset(c.slots, 0, sizeof(gcm_as_mapping) * (size_t)c.slot_count);
    c.slots[0].ea = c.io_ea; c.slots[0].io = 0; c.slots[0].size = c.io_size;
    c.slots[0].active = 1; c.slots[0].initial = 1;
    as->magic = GCM_AS_MAGIC;
    as->cfg = c;
    as->active = 1;
    return GCM_AS_OK;
}

/* Shared EA admission. `exact_status` is what an exact-base duplicate reports: only map_ea_io has a retained
 * reference error (ADDRESS_OVERWRAP) for it; map_main_memory reports the native-policy EA_OVERLAP. */
static int ea_exact_mapped(const gcm_address_space *as, uint32_t ea) {
    for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
        const gcm_as_mapping *m = &as->cfg.slots[i];
        if (m->active && m->ea == ea) return 1;
    }
    return 0;
}

static gcm_as_status check_ea(const gcm_address_space *as, uint32_t ea, uint32_t size, gcm_as_status exact_status) {
    uint64_t end = (uint64_t)ea + size;
    if (end > SPACE_END) return GCM_AS_RANGE_OVERFLOW;
    if (ea < GCM_AS_PAGE_SIZE) return GCM_AS_EA_FORBIDDEN;
    if (overlaps(ea, end, as->cfg.local_base, (uint64_t)as->cfg.local_base + as->cfg.local_size)) return GCM_AS_EA_FORBIDDEN;
    if (ea_exact_mapped(as, ea)) return exact_status;
    for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
        const gcm_as_mapping *m = &as->cfg.slots[i];
        if (m->active && overlaps(ea, end, m->ea, (uint64_t)m->ea + m->size)) return GCM_AS_EA_OVERLAP;
    }
    return GCM_AS_OK;
}

static int find_free_slot(const gcm_address_space *as, uint32_t *slot) {
    for (uint32_t i = 0; i < as->cfg.slot_count; i++)
        if (!as->cfg.slots[i].active) { *slot = i; return 1; }
    return 0;
}

static int io_overlaps_active(const gcm_address_space *as, uint64_t io, uint64_t end) {
    for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
        const gcm_as_mapping *m = &as->cfg.slots[i];
        if (m->active && overlaps(io, end, m->io, (uint64_t)m->io + m->size)) return 1;
    }
    return 0;
}

static void publish(gcm_address_space *as, uint32_t slot, uint32_t ea, uint32_t io, uint32_t size) {
    gcm_as_mapping *m = &as->cfg.slots[slot];
    m->ea = ea; m->io = io; m->size = size; m->active = 1; m->initial = 0;
    as->active++;
}

gcm_as_status gcm_as_map_main_memory(gcm_address_space *as, uint32_t ea, uint32_t size, uint32_t *io_out) {
    if (!as || !io_out) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    if (size == 0 || !aligned(size) || !aligned(ea)) return GCM_AS_MISALIGNED;
    gcm_as_status st = check_ea(as, ea, size, GCM_AS_EA_OVERLAP);
    if (st != GCM_AS_OK) return st;
    uint32_t slot;
    if (!find_free_slot(as, &slot)) return GCM_AS_NO_SLOT;
    /* First fit: lowest candidate >= io_size that intersects no active IO range. Each pass moves the
     * candidate past one blocking mapping, so it terminates after at most slot_count moves. */
    uint64_t cand = as->cfg.io_size;
    for (;;) {
        int moved = 0;
        for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
            const gcm_as_mapping *m = &as->cfg.slots[i];
            if (!m->active) continue;
            uint64_t m_end = (uint64_t)m->io + m->size;
            if (overlaps(cand, cand + size, m->io, m_end)) { cand = m_end; moved = 1; }
        }
        if (!moved) break;
    }
    if (cand + size > as->cfg.io_space_size) return GCM_AS_NO_IO_SPACE;
    publish(as, slot, ea, (uint32_t)cand, size);
    *io_out = (uint32_t)cand;
    return GCM_AS_OK;
}

gcm_as_status gcm_as_map_ea_io(gcm_address_space *as, uint32_t ea, uint32_t io, uint32_t size) {
    if (!as) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    if (!aligned(ea) || !aligned(io) || size == 0 || !aligned(size)) return GCM_AS_MISALIGNED;
    /* Exact duplicate EA is the one reference-backed refusal after alignment (ADDRESS_OVERWRAP): report it before
     * the native range/IO-range policy refusals, so overflow or an out-of-config IO cannot mask it. */
    if (ea_exact_mapped(as, ea)) return GCM_AS_EA_ALREADY_MAPPED;
    if ((uint64_t)ea + size > SPACE_END || (uint64_t)io + size > SPACE_END) return GCM_AS_RANGE_OVERFLOW;
    gcm_as_status st = check_ea(as, ea, size, GCM_AS_EA_ALREADY_MAPPED);
    if (st == GCM_AS_RANGE_OVERFLOW || st == GCM_AS_EA_FORBIDDEN) return st;
    if ((uint64_t)io + size > as->cfg.io_space_size) return GCM_AS_IO_RANGE;
    if (st != GCM_AS_OK) return st;                       /* EA_ALREADY_MAPPED / EA_OVERLAP */
    if (io_overlaps_active(as, io, (uint64_t)io + size)) return GCM_AS_IO_OVERLAP;
    uint32_t slot;
    if (!find_free_slot(as, &slot)) return GCM_AS_NO_SLOT;
    publish(as, slot, ea, io, size);
    return GCM_AS_OK;
}

static gcm_as_status unmap_key(gcm_address_space *as, uint32_t key, int by_io) {
    if (!as) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
        gcm_as_mapping *m = &as->cfg.slots[i];
        if (!m->active || (by_io ? m->io : m->ea) != key) continue;
        if (m->initial) return GCM_AS_PROTECTED;
        memset(m, 0, sizeof *m);
        as->active--;
        return GCM_AS_OK;
    }
    return GCM_AS_UNMAP_NOT_FOUND;
}
gcm_as_status gcm_as_unmap_ea(gcm_address_space *as, uint32_t ea) { return unmap_key(as, ea, 0); }
gcm_as_status gcm_as_unmap_io(gcm_address_space *as, uint32_t io) { return unmap_key(as, io, 1); }

/* Find the mapping holding the first byte of [base, base+length) in the EA (by_io==0) or IO space and
 * require the whole range inside it. */
static gcm_as_status lookup(const gcm_address_space *as, uint32_t base, uint32_t length, int by_io, const gcm_as_mapping **out) {
    uint64_t end = (uint64_t)base + length;
    for (uint32_t i = 0; i < as->cfg.slot_count; i++) {
        const gcm_as_mapping *m = &as->cfg.slots[i];
        if (!m->active) continue;
        uint64_t k0 = by_io ? m->io : m->ea;
        if (base < k0 || base >= k0 + m->size) continue;
        if (end > k0 + m->size) return GCM_AS_RANGE_SPANS;
        *out = m;
        return GCM_AS_OK;
    }
    return GCM_AS_NOT_MAPPED;
}

static gcm_as_status check_range(uint32_t base, uint32_t length) {
    if (length == 0) return GCM_AS_INVALID_VALUE;
    if ((uint64_t)base + length > SPACE_END) return GCM_AS_RANGE_OVERFLOW;
    return GCM_AS_OK;
}

gcm_as_status gcm_as_address_to_offset(const gcm_address_space *as, uint32_t address, uint32_t length,
                                       uint32_t *location_out, uint32_t *offset_out) {
    if (!as || !location_out || !offset_out) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    gcm_as_status st = check_range(address, length);
    if (st != GCM_AS_OK) return st;
    if (address >= as->cfg.local_base && address - as->cfg.local_base < as->cfg.local_size) {
        uint32_t off = address - as->cfg.local_base;
        if ((uint64_t)off + length > as->cfg.local_size) return GCM_AS_LOCAL_RANGE;
        *location_out = GCM_AS_LOCATION_LOCAL;
        *offset_out = off;
        return GCM_AS_OK;
    }
    const gcm_as_mapping *m = NULL;
    st = lookup(as, address, length, 0, &m);
    if (st != GCM_AS_OK) return st;
    *location_out = GCM_AS_LOCATION_MAIN;
    *offset_out = m->io + (address - m->ea);
    return GCM_AS_OK;
}

gcm_as_status gcm_as_io_to_address(const gcm_address_space *as, uint32_t io, uint32_t length, uint32_t *ea_out) {
    if (!as || !ea_out) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    gcm_as_status st = check_range(io, length);
    if (st != GCM_AS_OK) return st;
    const gcm_as_mapping *m = NULL;
    st = lookup(as, io, length, 1, &m);
    if (st != GCM_AS_OK) return st;
    *ea_out = m->ea + (io - m->io);
    return GCM_AS_OK;
}

gcm_as_status gcm_as_resolve(const gcm_address_space *as, uint32_t location, uint32_t offset, uint32_t length,
                             uint32_t *ea_out) {
    if (!as || !ea_out) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    if (location != GCM_AS_LOCATION_LOCAL && location != GCM_AS_LOCATION_MAIN) return GCM_AS_INVALID_VALUE;
    gcm_as_status st = check_range(offset, length);
    if (st != GCM_AS_OK) return st;
    if (location == GCM_AS_LOCATION_MAIN) return gcm_as_io_to_address(as, offset, length, ea_out);
    if ((uint64_t)offset + length > as->cfg.local_size) return GCM_AS_LOCAL_RANGE;
    *ea_out = as->cfg.local_base + offset;
    return GCM_AS_OK;
}

gcm_as_status gcm_as_get_mapping(const gcm_address_space *as, uint32_t slot, gcm_as_mapping *info_out) {
    if (!as || !info_out) return GCM_AS_INVALID_ARGUMENT;
    if (!ready(as)) return GCM_AS_NOT_INITIALIZED;
    if (slot >= as->cfg.slot_count) return GCM_AS_INVALID_VALUE;
    if (!as->cfg.slots[slot].active) return GCM_AS_NOT_MAPPED;
    *info_out = as->cfg.slots[slot];
    return GCM_AS_OK;
}

uint32_t gcm_as_active_count(const gcm_address_space *as) { return ready(as) ? as->active : 0; }

int gcm_as_status_to_cell_error(gcm_as_status s, uint32_t *cell_out) {
    uint32_t v;
    if (!cell_out) return 0;
    switch (s) {
    case GCM_AS_OK: v = GCM_AS_CELL_OK; break;
    case GCM_AS_MISALIGNED: v = GCM_AS_CELL_ERROR_INVALID_ALIGNMENT; break;
    case GCM_AS_EA_ALREADY_MAPPED: v = GCM_AS_CELL_ERROR_ADDRESS_OVERWRAP; break;
    case GCM_AS_UNMAP_NOT_FOUND: v = GCM_AS_CELL_ERROR_FAILURE; break;
    default: return 0;
    }
    *cell_out = v;
    return 1;
}

const char *gcm_as_status_name(gcm_as_status s) {
    switch (s) {
    case GCM_AS_OK: return "OK";
    case GCM_AS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case GCM_AS_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case GCM_AS_CONFIG_INVALID: return "CONFIG_INVALID";
    case GCM_AS_INVALID_VALUE: return "INVALID_VALUE";
    case GCM_AS_MISALIGNED: return "MISALIGNED";
    case GCM_AS_RANGE_OVERFLOW: return "RANGE_OVERFLOW";
    case GCM_AS_EA_FORBIDDEN: return "EA_FORBIDDEN";
    case GCM_AS_EA_OVERLAP: return "EA_OVERLAP";
    case GCM_AS_EA_ALREADY_MAPPED: return "EA_ALREADY_MAPPED";
    case GCM_AS_IO_RANGE: return "IO_RANGE";
    case GCM_AS_IO_OVERLAP: return "IO_OVERLAP";
    case GCM_AS_NO_SLOT: return "NO_SLOT";
    case GCM_AS_NO_IO_SPACE: return "NO_IO_SPACE";
    case GCM_AS_NOT_MAPPED: return "NOT_MAPPED";
    case GCM_AS_UNMAP_NOT_FOUND: return "UNMAP_NOT_FOUND";
    case GCM_AS_RANGE_SPANS: return "RANGE_SPANS";
    case GCM_AS_LOCAL_RANGE: return "LOCAL_RANGE";
    case GCM_AS_PROTECTED: return "PROTECTED";
    }
    return "UNKNOWN";
}
