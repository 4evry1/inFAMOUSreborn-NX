#define _POSIX_C_SOURCE 200809L
#include "lv2_spu.h"
#include <string.h>

/* SPU ELF constants (ELF32, big-endian, EM_SPU). */
enum {
    ELF_HEADER_BYTES = 0x34, ELF_HEADER_PHOFF = 0x1C, ELF_HEADER_PHENTSIZE = 0x2A,
    ELF_HEADER_PHNUM = 0x2C, ELF_HEADER_ENTRY = 0x18, ELF_HEADER_MACHINE = 0x12,
    ELF_MACHINE_SPU = 0x17, PT_LOAD = 1,
    PH_TYPE = 0x00, PH_OFFSET = 0x04, PH_VADDR = 0x08, PH_FILESZ = 0x10, PH_MEMSZ = 0x14,
    SEGMENT_TYPE_COPY = 1, SEGMENT_TYPE_FILL = 2
};
/* sys_spu_segment_t, guest byte layout: s32 type; u32 ls_start; s32 size; u64 src (only its low
 * word is used by every 32-bit reader, so both words carry the same value). */
enum { SEG_TYPE = 0x00, SEG_LS_START = 0x04, SEG_SIZE = 0x08, SEG_SRC_LO = 0x10, SEG_SRC_HI = 0x14 };
enum { EVENT_RUN = 1, EVENT_EXCEPTION = 2, EVENT_SYSTEM = 4, EVENT_ANY = 0xFF };

static void locki(ps3_lv2_spu_images *i) { i->guard.lock(i->guard.context); }
static uint32_t unlocki(ps3_lv2_spu_images *i, uint32_t rc) { i->guard.unlock(i->guard.context); return rc; }
static void lockg(ps3_lv2_spu_groups *g) { g->guard.lock(g->guard.context); }
static uint32_t unlockg(ps3_lv2_spu_groups *g, uint32_t rc) { g->guard.unlock(g->guard.context); return rc; }

static uint16_t be16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

uint64_t ps3_lv2_spu_ls_hash(const unsigned char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (size_t index = 0; index < length; ++index) { hash ^= bytes[index]; hash *= UINT64_C(0x100000001B3); }
    return hash;
}

/* ---- images --------------------------------------------------------------------------------- */

uint32_t ps3_lv2_spu_images_init(ps3_lv2_spu_images *i, ps3_instance_guard guard)
{
    if (!i || !guard.lock || !guard.unlock) return PS3_LV2_SPU_EINVAL;
    if (i->initialized) return PS3_LV2_SPU_EINVAL;
    memset(i, 0, sizeof *i);
    i->guard = guard;
    i->initialized = 1;
    return PS3_LV2_SPU_OK;
}

uint32_t ps3_lv2_spu_image_parse(ps3_lv2_spu_images *images, const unsigned char *elf, size_t elf_length,
                             ps3_lv2_spu_image *image, unsigned char *segments, size_t segments_capacity,
                             uint32_t *written)
{
    if (!image || !written) return PS3_LV2_SPU_EFAULT;
    if (!images || !images->initialized) return PS3_LV2_SPU_EINVAL;
    if (!elf || elf_length < ELF_HEADER_BYTES) return PS3_LV2_SPU_EINVAL;
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return PS3_LV2_SPU_EINVAL;
    if (be16(elf + ELF_HEADER_MACHINE) != ELF_MACHINE_SPU) return PS3_LV2_SPU_EINVAL;
    const uint32_t entry = be32(elf + ELF_HEADER_ENTRY);
    const uint32_t phoff = be32(elf + ELF_HEADER_PHOFF);
    uint16_t phentsize = be16(elf + ELF_HEADER_PHENTSIZE);
    const uint16_t phnum = be16(elf + ELF_HEADER_PHNUM);
    if (!phentsize) phentsize = 0x20;
    if (phoff > elf_length || (size_t)phentsize * phnum > elf_length - phoff) return PS3_LV2_SPU_EINVAL;
    uint32_t count = 0;
    for (uint16_t index = 0; index < phnum; ++index) {
        const unsigned char *ph = elf + phoff + (uint32_t)index * phentsize;
        if (be32(ph + PH_TYPE) != PT_LOAD) continue;
        const uint32_t offset = be32(ph + PH_OFFSET), vaddr = be32(ph + PH_VADDR);
        const uint32_t filesz = be32(ph + PH_FILESZ), memsz = be32(ph + PH_MEMSZ);
        if (filesz > memsz) return PS3_LV2_SPU_EINVAL;
        if (offset > elf_length || filesz > elf_length - offset) return PS3_LV2_SPU_EINVAL;
        if ((uint64_t)vaddr + memsz > PS3_LV2_SPU_LS_BYTES) return PS3_LV2_SPU_EINVAL;   /* cannot fit in an LS */
        const uint32_t needed = filesz ? (memsz > filesz ? 2u : 1u) : (memsz ? 1u : 0u);
        if (count + needed > PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) return PS3_LV2_SPU_ENOMEM;
        if ((count + needed) * PS3_LV2_SPU_SEGMENT_BYTES > segments_capacity) return PS3_LV2_SPU_ENOMEM;
        if (!segments && needed) return PS3_LV2_SPU_EFAULT;
        unsigned char *record = segments + count * PS3_LV2_SPU_SEGMENT_BYTES;
        if (filesz) {
            memset(record, 0, PS3_LV2_SPU_SEGMENT_BYTES);
            put32(record + SEG_TYPE, SEGMENT_TYPE_COPY);
            put32(record + SEG_LS_START, vaddr);
            put32(record + SEG_SIZE, filesz);
            put32(record + SEG_SRC_LO, offset);      /* the caller adds the guest base */
            put32(record + SEG_SRC_HI, offset);
            record += PS3_LV2_SPU_SEGMENT_BYTES;
            ++count;
        }
        if (memsz > filesz) {   /* the bss tail, always a zero FILL */
            memset(record, 0, PS3_LV2_SPU_SEGMENT_BYTES);
            put32(record + SEG_TYPE, SEGMENT_TYPE_FILL);
            put32(record + SEG_LS_START, vaddr + filesz);
            put32(record + SEG_SIZE, memsz - filesz);
            ++count;
        }
    }
    image->type = 0;                 /* USER */
    image->entry_point = entry;
    image->segs_ea = 0;              /* the adapter fills the guest address it placed them at */
    image->nsegs = count;
    image->source_ea = 0;
    image->active = 1;
    *written = count;
    return PS3_LV2_SPU_OK;
}

uint32_t ps3_lv2_spu_image_register(ps3_lv2_spu_images *images, const ps3_lv2_spu_image *image)
{
    if (!images || !images->initialized) return PS3_LV2_SPU_EINVAL;
    if (!image) return PS3_LV2_SPU_EFAULT;
    locki(images);
    for (size_t index = 0; index < PS3_LV2_SPU_IMAGE_SEGMENTS_MAX; ++index) {
        ps3_lv2_spu_image *slot = &images->images[index];
        if (slot->active && slot->source_ea == image->source_ea && slot->segs_ea == image->segs_ea) {
            return unlocki(images, PS3_LV2_SPU_OK);   /* idempotent for the same (source, segs) pair */
        }
    }
    for (size_t index = 0; index < PS3_LV2_SPU_IMAGE_SEGMENTS_MAX; ++index) {
        ps3_lv2_spu_image *slot = &images->images[index];
        if (slot->active) continue;
        *slot = *image;
        slot->active = 1;
        return unlocki(images, PS3_LV2_SPU_OK);
    }
    return unlocki(images, PS3_LV2_SPU_EAGAIN);
}
uint32_t ps3_lv2_spu_image_find(ps3_lv2_spu_images *images, uint32_t image_ea, ps3_lv2_spu_image *out)
{
    if (!out) return PS3_LV2_SPU_EFAULT;
    if (!images || !images->initialized) return PS3_LV2_SPU_EINVAL;
    locki(images);
    for (size_t index = 0; index < PS3_LV2_SPU_IMAGE_SEGMENTS_MAX; ++index)
        if (images->images[index].active && images->images[index].segs_ea == image_ea) {
            *out = images->images[index];
            return unlocki(images, PS3_LV2_SPU_OK);
        }
    return unlocki(images, PS3_LV2_SPU_ESRCH);
}
uint32_t ps3_lv2_spu_image_close(ps3_lv2_spu_images *images, uint32_t image_ea)
{
    if (!images || !images->initialized) return PS3_LV2_SPU_EINVAL;
    locki(images);
    for (size_t index = 0; index < PS3_LV2_SPU_IMAGE_SEGMENTS_MAX; ++index)
        if (images->images[index].active && images->images[index].segs_ea == image_ea) {
            images->images[index].active = 0;
            return unlocki(images, PS3_LV2_SPU_OK);
        }
    return unlocki(images, PS3_LV2_SPU_ESRCH);
}

/* ---- the system segs arena -------------------------------------------------------------------
 * A fixed window of PS3_LV2_SPU_ARENA_SLOTS slots of PS3_LV2_SPU_ARENA_SLOT bytes, tracked by a
 * bitmap. A request needing `count` contiguous slots gets the lowest run that fits, so a close that
 * frees one image never fragments the window for the next. */
static uint32_t arena_run_count(uint32_t size)
{
    if (!size) return 0;
    return (size + PS3_LV2_SPU_ARENA_SLOT - 1) / PS3_LV2_SPU_ARENA_SLOT;
}
static int arena_slot_used(const ps3_lv2_spu_images *i, uint32_t slot)
{
    return (i->arena_used[slot / 32] >> (slot % 32)) & 1u;
}
static void arena_set(ps3_lv2_spu_images *i, uint32_t slot, int used)
{
    const uint32_t bit = 1u << (slot % 32);
    if (used) i->arena_used[slot / 32] |= bit;
    else i->arena_used[slot / 32] &= ~bit;
}
uint32_t ps3_lv2_spu_arena_allocate(ps3_lv2_spu_images *i, uint32_t size, uint32_t *ea)
{
    if (!i || !i->initialized) return PS3_LV2_SPU_EINVAL;
    if (!ea) return PS3_LV2_SPU_EFAULT;
    const uint32_t need = arena_run_count(size);
    if (!need || need > PS3_LV2_SPU_ARENA_SLOTS) return PS3_LV2_SPU_ENOMEM;
    locki(i);
    for (uint32_t first = 0; first + need <= PS3_LV2_SPU_ARENA_SLOTS; ++first) {
        uint32_t taken = 0;
        while (taken < need && !arena_slot_used(i, first + taken)) ++taken;
        if (taken != need) { first += taken; continue; }
        for (uint32_t index = 0; index < need; ++index) arena_set(i, first + index, 1);
        *ea = PS3_LV2_SPU_ARENA_BASE + first * PS3_LV2_SPU_ARENA_SLOT;
        return unlocki(i, PS3_LV2_SPU_OK);
    }
    return unlocki(i, PS3_LV2_SPU_ENOMEM);   /* every run refused: the guest's own error, no fabrication */
}
uint32_t ps3_lv2_spu_arena_free(ps3_lv2_spu_images *i, uint32_t ea)
{
    if (!i || !i->initialized) return PS3_LV2_SPU_EINVAL;
    if (ea < PS3_LV2_SPU_ARENA_BASE || ea - PS3_LV2_SPU_ARENA_BASE >= PS3_LV2_SPU_ARENA_SIZE ||
        (ea - PS3_LV2_SPU_ARENA_BASE) % PS3_LV2_SPU_ARENA_SLOT)
        return PS3_LV2_SPU_EINVAL;
    locki(i);
    const uint32_t first = (ea - PS3_LV2_SPU_ARENA_BASE) / PS3_LV2_SPU_ARENA_SLOT;
    if (!arena_slot_used(i, first)) return unlocki(i, PS3_LV2_SPU_ESRCH);
    /* Release the whole run: every following slot that is set but whose predecessor is set belongs to
     * the same reservation (the allocator never splits a run). */
    for (uint32_t slot = first; slot < PS3_LV2_SPU_ARENA_SLOTS && arena_slot_used(i, slot); ++slot)
        arena_set(i, slot, 0);
    return unlocki(i, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_arena_used_count(ps3_lv2_spu_images *i)
{
    if (!i || !i->initialized) return PS3_LV2_SPU_EINVAL;
    locki(i);
    uint32_t used = 0;
    for (uint32_t slot = 0; slot < PS3_LV2_SPU_ARENA_SLOTS; ++slot) used += arena_slot_used(i, slot) ? 1u : 0u;
    unlocki(i, 0);
    return used;
}

/* ---- groups --------------------------------------------------------------------------------- */

uint32_t ps3_lv2_spu_groups_init(ps3_lv2_spu_groups *g, ps3_instance_guard guard, unsigned char *local_stores,
                             uint32_t usable_spus)
{
    if (!g || !guard.lock || !guard.unlock || !local_stores) return PS3_LV2_SPU_EINVAL;
    if (g->initialized) return PS3_LV2_SPU_EINVAL;
    memset(g, 0, sizeof *g);
    g->guard = guard;
    g->local_stores = local_stores;
    g->usable_spus = usable_spus;
    g->initialized = 1;
    return PS3_LV2_SPU_OK;
}
void ps3_lv2_spu_groups_set_usable(ps3_lv2_spu_groups *g, uint32_t usable_spus)
{
    if (!g || !g->initialized) return;
    lockg(g);
    g->usable_spus = usable_spus;
    unlockg(g, 0);
}
static ps3_lv2_spu_group *find_group(ps3_lv2_spu_groups *g, uint32_t id)
{
    for (size_t index = 0; index < PS3_LV2_SPU_GROUP_CAP; ++index)
        if (g->groups[index].active && g->groups[index].id == id) return &g->groups[index];
    return NULL;
}
/* A thread id is unique across groups; the search returns both the thread and its owning group. */
static ps3_lv2_spu_thread *find_thread(ps3_lv2_spu_groups *g, uint32_t thread_id, ps3_lv2_spu_group **owner)
{
    for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi) {
        ps3_lv2_spu_group *group = &g->groups[gi];
        if (!group->active) continue;
        for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
            if (group->threads[ti].id == thread_id) {
                if (owner) *owner = group;
                return &group->threads[ti];
            }
    }
    return NULL;
}
static uint32_t next_group_id(ps3_lv2_spu_groups *g)
{
    for (uint32_t attempt = 0; attempt < 0xFFFFFFu; ++attempt) {
        const uint32_t id = PS3_LV2_SPU_GROUP_BASE + PS3_LV2_SPU_ID_STEP * g->serial_group++;
        if (!find_group(g, id)) return id;
    }
    return 0;
}
static uint32_t next_thread_id(ps3_lv2_spu_groups *g)
{
    for (uint32_t attempt = 0; attempt < 0xFFFFFFu; ++attempt) {
        const uint32_t id = PS3_LV2_SPU_THREAD_BASE + PS3_LV2_SPU_ID_STEP * g->serial_thread++;
        if (!find_thread(g, id, NULL)) return id;
    }
    return 0;
}

uint32_t ps3_lv2_spu_group_create(ps3_lv2_spu_groups *g, uint32_t *id, uint32_t num, uint32_t priority, uint32_t type)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!id) return PS3_LV2_SPU_EFAULT;
    if (num == 0 || num > PS3_LV2_SPU_THREADS_MAX) return PS3_LV2_SPU_EINVAL;
    if (priority < PS3_LV2_SPU_PRIORITY_MIN || priority > PS3_LV2_SPU_PRIORITY_MAX) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    /* The policy's usable-SPU count bounds what this title may create (syscall 169 configured it). */
    if (num > g->usable_spus) return unlockg(g, PS3_LV2_SPU_EAGAIN);
    for (size_t index = 0; index < PS3_LV2_SPU_GROUP_CAP; ++index) {
        if (g->groups[index].active) continue;
        const uint32_t new_id = next_group_id(g);
        if (!new_id) return unlockg(g, PS3_LV2_SPU_EAGAIN);
        ps3_lv2_spu_group *group = &g->groups[index];
        memset(group, 0, sizeof *group);
        group->id = new_id;
        group->num = num;
        group->priority = priority;
        group->type = type;
        group->num_usable = num;
        group->active = 1;
        *id = new_id;
        return unlockg(g, PS3_LV2_SPU_OK);
    }
    return unlockg(g, PS3_LV2_SPU_EAGAIN);
}
uint32_t ps3_lv2_spu_group_destroy(ps3_lv2_spu_groups *g, uint32_t id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (group->started) return unlockg(g, PS3_LV2_SPU_EBUSY);   /* running groups stay until terminated */
    /* Every thread's local store is cleared so a later group cannot read the dead one's bytes. */
    for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
        if (group->threads[ti].ls) memset(group->threads[ti].ls, 0, PS3_LV2_SPU_LS_BYTES);
    group->active = 0;
    return unlockg(g, PS3_LV2_SPU_OK);
}

uint32_t ps3_lv2_spu_thread_initialize(ps3_lv2_spu_groups *g, uint32_t *thread_id, uint32_t group_id, uint32_t spu_number,
                                   const ps3_lv2_spu_image *image, const unsigned char *segments, uint32_t nsegs,
                                   const unsigned char *source, size_t source_length,
                                   const uint64_t arg[4])
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!thread_id || !image || !segments) return PS3_LV2_SPU_EFAULT;
    if (nsegs > PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, group_id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (group->started) return unlockg(g, PS3_LV2_SPU_EBUSY);
    if (spu_number >= group->num) return unlockg(g, PS3_LV2_SPU_EINVAL);
    ps3_lv2_spu_thread *thread = &group->threads[spu_number];
    const uint32_t new_id = thread->id ? thread->id : next_thread_id(g);
    if (!new_id) return unlockg(g, PS3_LV2_SPU_EAGAIN);
    unsigned char *ls = g->local_stores +
        ((size_t)(group - g->groups) * PS3_LV2_SPU_THREADS_MAX + spu_number) * PS3_LV2_SPU_LS_BYTES;
    /* Build the local store in place so a refused segment leaves nothing half-written. */
    memset(ls, 0, PS3_LV2_SPU_LS_BYTES);
    for (uint32_t index = 0; index < nsegs; ++index) {
        const unsigned char *record = segments + (size_t)index * PS3_LV2_SPU_SEGMENT_BYTES;
        const int32_t type = (int32_t)be32(record + SEG_TYPE);
        const uint32_t ls_start = be32(record + SEG_LS_START);
        const uint32_t size = be32(record + SEG_SIZE);
        if (ls_start > PS3_LV2_SPU_LS_BYTES || size > PS3_LV2_SPU_LS_BYTES - ls_start)
            return unlockg(g, PS3_LV2_SPU_EINVAL);
        if (type == SEGMENT_TYPE_FILL) {
            memset(ls + ls_start, 0, size);   /* the only fill this layer emits is a zero bss tail */
        } else if (type == SEGMENT_TYPE_COPY) {
            const uint32_t offset = be32(record + SEG_SRC_LO);
            if (offset > source_length || size > source_length - offset) return unlockg(g, PS3_LV2_SPU_EINVAL);
            memcpy(ls + ls_start, source + offset, size);
        } else {
            return unlockg(g, PS3_LV2_SPU_EINVAL);
        }
    }
    thread->id = new_id;
    thread->spu_number = spu_number;
    thread->ls = ls;   // read_ls/write_ls and the start hash address this thread's own store
    thread->entry_point = image->entry_point;
    thread->initialized = 1;
    if (arg) { thread->arg1 = arg[0]; thread->arg2 = arg[1]; thread->arg3 = arg[2]; thread->arg4 = arg[3]; }
    *thread_id = new_id;
    return unlockg(g, PS3_LV2_SPU_OK);
}

/* Shared body: `ls` is already this thread's own store. Copies each record, then commits the
 * thread. A refused record leaves `ls` zeroed and the thread uninitialized. */
static uint32_t thread_commit(ps3_lv2_spu_groups *g, uint32_t *thread_id, ps3_lv2_spu_group *group,
                              uint32_t slot, uint32_t spu_number, const ps3_lv2_spu_image *image, unsigned char *ls,
                              const uint64_t arg[4])
{
    ps3_lv2_spu_thread *thread = &group->threads[slot];
    const uint32_t new_id = thread->id ? thread->id : next_thread_id(g);
    if (!new_id) return unlockg(g, PS3_LV2_SPU_EAGAIN);
    /* Root fix (C1b Eden): the caller (initialize_gather) has already zeroed `ls` and copied the image's segments
     * into it; zeroing it again here wiped every SPU thread's program (Eden: LS all zero, stop 0 at the entry). */
    thread->id = new_id;
    thread->spu_number = spu_number;
    thread->ls = ls;
    thread->entry_point = image->entry_point;
    thread->initialized = 1;
    if (arg) { thread->arg1 = arg[0]; thread->arg2 = arg[1]; thread->arg3 = arg[2]; thread->arg4 = arg[3]; }
    *thread_id = new_id;
    return unlockg(g, PS3_LV2_SPU_OK);
}
/* Validate one record's local-store target. Returns 0 when it does not fit. */
static int record_target(const unsigned char *record, uint32_t *ls_start, uint32_t *size, uint32_t *offset)
{
    *ls_start = be32(record + SEG_LS_START);
    *size = be32(record + SEG_SIZE);
    *offset = be32(record + SEG_SRC_LO);
    return *ls_start <= PS3_LV2_SPU_LS_BYTES && *size <= PS3_LV2_SPU_LS_BYTES - *ls_start;
}

uint32_t ps3_lv2_spu_thread_initialize_gather(ps3_lv2_spu_groups *g, uint32_t *thread_id, uint32_t group_id,
                                          uint32_t spu_number, const ps3_lv2_spu_image *image,
                                          const unsigned char *segments, uint32_t nsegs,
                                          const ps3_guest_memory *memory, const uint64_t arg[4])
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!thread_id || !image || !segments) return PS3_LV2_SPU_EFAULT;
    if (nsegs > PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) return PS3_LV2_SPU_EINVAL;
    if (nsegs && (!memory || !memory->read)) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, group_id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (group->started) return unlockg(g, PS3_LV2_SPU_EBUSY);
    /* Root (2026-11-12): lv2 takes any spu_num below 8, whatever the group's thread count (inFAMOUS's movie decoder
     * initializes spu_num 5 in a group of ONE thread; refusing it left the movie without a decoder). The group's
     * threads are kept dense in [0, num): the number's own slot when it is inside and free, else the first free one. */
    if (spu_number >= PS3_LV2_SPU_THREADS_MAX) return unlockg(g, PS3_LV2_SPU_EINVAL);
    uint32_t slot = PS3_LV2_SPU_THREADS_MAX;
    for (uint32_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
        if (group->threads[ti].initialized && group->threads[ti].spu_number == spu_number) { slot = ti; break; }
    if (slot == PS3_LV2_SPU_THREADS_MAX && spu_number < group->num && !group->threads[spu_number].initialized)
        slot = spu_number;
    for (uint32_t ti = 0; slot == PS3_LV2_SPU_THREADS_MAX && ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
        if (!group->threads[ti].initialized) slot = ti;
    if (slot == PS3_LV2_SPU_THREADS_MAX) return unlockg(g, PS3_LV2_SPU_EBUSY);
    unsigned char *ls = g->local_stores +
        ((size_t)(group - g->groups) * PS3_LV2_SPU_THREADS_MAX + slot) * PS3_LV2_SPU_LS_BYTES;
    memset(ls, 0, PS3_LV2_SPU_LS_BYTES);
    for (uint32_t index = 0; index < nsegs; ++index) {
        const unsigned char *record = segments + (size_t)index * PS3_LV2_SPU_SEGMENT_BYTES;
        const int32_t type = (int32_t)be32(record + SEG_TYPE);
        uint32_t ls_start = 0, size = 0, offset = 0;
        if (!record_target(record, &ls_start, &size, &offset)) return unlockg(g, PS3_LV2_SPU_EINVAL);
        if (type == SEGMENT_TYPE_FILL) {
            memset(ls + ls_start, 0, size);
            continue;
        }
        if (type != SEGMENT_TYPE_COPY) return unlockg(g, PS3_LV2_SPU_EINVAL);
        unlockg(g, 0);   /* the guest read may block on the memory port: never hold the guard */
        const ps3_guest_memory_status status = size
            ? memory->read(memory->context, offset, ls + ls_start, size) : PS3_GUEST_MEMORY_OK;
        lockg(g);
        if (status != PS3_GUEST_MEMORY_OK) {
            memset(ls, 0, PS3_LV2_SPU_LS_BYTES);
            return unlockg(g, PS3_LV2_SPU_PROVIDER_FAILURE);
        }
    }
    return thread_commit(g, thread_id, group, slot, spu_number, image, ls, arg);
}

uint32_t ps3_lv2_spu_thread_get_exit_status(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t *status)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!status) return PS3_LV2_SPU_EFAULT;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->initialized) return unlockg(g, PS3_LV2_SPU_ESTAT);
    /* Decision 33 / C1b: a thread that ran and exited through stop 0x102 has a real status. Anything else has
     * none -- ESTAT, never a fabricated 0. */
    if (thread->ended && thread->exited) { *status = thread->exit_status; return unlockg(g, PS3_LV2_SPU_OK); }
    return unlockg(g, PS3_LV2_SPU_ESTAT);
}

/* ---- Decision 33 / C1b: the run outcome the runner records ------------------------------------- */
uint32_t ps3_lv2_spu_thread_record_end(ps3_lv2_spu_groups *g, uint32_t thread_id, unsigned exited,
                                       uint32_t exit_status, unsigned group_exit)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = NULL;
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, &group);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->initialized || !group) return unlockg(g, PS3_LV2_SPU_ESTAT);
    if (thread->ended) return unlockg(g, PS3_LV2_SPU_OK);   /* the FIRST terminal wins; a later cancel never overwrites */
    thread->ended = 1;
    thread->exited = exited ? 1u : 0u;
    thread->exit_status = exited ? exit_status : 0u;
    ++group->ended_count;
    if (group_exit) group->join_cause = PS3_LV2_SPU_GROUP_JOIN_GROUP_EXIT;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_bind_queue(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t queue_id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!queue_id) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->initialized) return unlockg(g, PS3_LV2_SPU_ESTAT);
    if (thread->bound_queue) return unlockg(g, PS3_LV2_SPU_EBUSY);   /* lv2: one queue per thread */
    thread->bound_queue = queue_id;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_unbind_queue(ps3_lv2_spu_groups *g, uint32_t thread_id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->bound_queue) return unlockg(g, PS3_LV2_SPU_ESRCH);   /* nothing bound: lv2 answers ESRCH */
    thread->bound_queue = 0;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_bound_queue(const ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t *queue_id)
{
    if (!g || !g->initialized || !queue_id) return PS3_LV2_SPU_EINVAL;
    for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi) {
        const ps3_lv2_spu_group *group = &g->groups[gi];
        if (!group->active) continue;
        for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
            if (group->threads[ti].id == thread_id) {
                *queue_id = group->threads[ti].bound_queue;
                return PS3_LV2_SPU_OK;
            }
    }
    return PS3_LV2_SPU_ESRCH;
}
uint32_t ps3_lv2_spu_group_pending(const ps3_lv2_spu_groups *g, uint32_t id, uint32_t *pending, uint32_t *cause)
{
    if (!g || !g->initialized || !pending || !cause) return PS3_LV2_SPU_EINVAL;
    for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi) {
        const ps3_lv2_spu_group *group = &g->groups[gi];
        if (!group->active || group->id != id) continue;
        uint32_t waiting = 0;
        for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
            if (group->threads[ti].initialized && !group->threads[ti].ended) ++waiting;
        *pending = waiting;
        *cause = group->join_cause ? group->join_cause : PS3_LV2_SPU_GROUP_JOIN_ALL_THREADS_EXIT;
        return PS3_LV2_SPU_OK;
    }
    return PS3_LV2_SPU_ESRCH;
}
uint32_t ps3_lv2_spu_thread_set_argument(ps3_lv2_spu_groups *g, uint32_t thread_id, const uint64_t arg[4])
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!arg) return PS3_LV2_SPU_EFAULT;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->initialized) return unlockg(g, PS3_LV2_SPU_ESTAT);
    thread->arg1 = arg[0]; thread->arg2 = arg[1]; thread->arg3 = arg[2]; thread->arg4 = arg[3];
    return unlockg(g, PS3_LV2_SPU_OK);
}

void ps3_lv2_spu_inject_provider_failure(ps3_lv2_spu_groups *g, int armed)
{
    if (!g || !g->initialized) return;
    lockg(g);
    g->inject_provider_failure = armed ? 1u : 0u;
    unlockg(g, 0);
}
uint32_t ps3_lv2_spu_thread_read_ls(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t ls_address, uint64_t *value, size_t size)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!value) return PS3_LV2_SPU_EFAULT;
    if (size != 1 && size != 2 && size != 4 && size != 8) return PS3_LV2_SPU_EINVAL;
    if (ls_address % size || ls_address > PS3_LV2_SPU_LS_BYTES || size > PS3_LV2_SPU_LS_BYTES - ls_address)
        return PS3_LV2_SPU_EINVAL;
    lockg(g);
    if (g->inject_provider_failure) return unlockg(g, PS3_LV2_SPU_PROVIDER_FAILURE);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->ls) return unlockg(g, PS3_LV2_SPU_ESTAT);
    uint64_t result = 0;
    for (size_t index = 0; index < size; ++index) result = result << 8 | thread->ls[ls_address + index];
    *value = result;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_write_ls(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t ls_address, uint64_t value, size_t size)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (size != 1 && size != 2 && size != 4 && size != 8) return PS3_LV2_SPU_EINVAL;
    if (ls_address % size || ls_address > PS3_LV2_SPU_LS_BYTES || size > PS3_LV2_SPU_LS_BYTES - ls_address)
        return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->ls) return unlockg(g, PS3_LV2_SPU_ESTAT);
    for (size_t index = 0; index < size; ++index)
        thread->ls[ls_address + index] = (unsigned char)(value >> (8 * (size - 1 - index)));
    return unlockg(g, PS3_LV2_SPU_OK);
}

uint32_t ps3_lv2_spu_group_set_priority(ps3_lv2_spu_groups *g, uint32_t id, uint32_t priority)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (priority < PS3_LV2_SPU_PRIORITY_MIN || priority > PS3_LV2_SPU_PRIORITY_MAX) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    group->priority = priority;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_group_get_priority(ps3_lv2_spu_groups *g, uint32_t id, uint32_t *priority)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!priority) return PS3_LV2_SPU_EFAULT;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    *priority = group->priority;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_group_terminate(ps3_lv2_spu_groups *g, uint32_t id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    /* Nothing ran, so there is nothing to stop: this only records the request. A group that never
     * started is ESTAT, exactly as lv2 answers for a group in the idle state. */
    if (!group->started) return unlockg(g, PS3_LV2_SPU_ESTAT);
    group->terminated = 1;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_group_join(ps3_lv2_spu_groups *g, uint32_t id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    /* A join before start would block forever on work that does not exist. Report the state instead
     * of hanging; the adapter turns this into a named stop. */
    if (!group->started) return unlockg(g, PS3_LV2_SPU_ESTAT);
    return unlockg(g, PS3_LV2_SPU_OK);
}

uint32_t ps3_lv2_spu_group_connect_event(ps3_lv2_spu_groups *g, uint32_t id, uint32_t port, uint32_t event)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    switch (event & EVENT_ANY) {
    case EVENT_RUN: group->port_run = port; break;
    case EVENT_EXCEPTION: group->port_exception = port; break;
    case EVENT_SYSTEM: group->port_system = port; break;
    default: return unlockg(g, PS3_LV2_SPU_EINVAL);
    }
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_group_disconnect_event(ps3_lv2_spu_groups *g, uint32_t id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!group->port_run && !group->port_exception && !group->port_system) return unlockg(g, PS3_LV2_SPU_EINVAL);
    group->port_run = group->port_exception = group->port_system = 0;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_connect_event(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t port, uint32_t event, int bind)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (!thread->initialized) return unlockg(g, PS3_LV2_SPU_ESTAT);
    if (!bind) {
        if (!thread->connected) return unlockg(g, PS3_LV2_SPU_EINVAL);
        thread->connected = 0;
        memset(thread->port, 0, sizeof thread->port);
        return unlockg(g, PS3_LV2_SPU_OK);
    }
    switch (event & EVENT_ANY) {
    case EVENT_RUN: thread->port[0] = port; break;
    case EVENT_EXCEPTION: thread->port[1] = port; break;
    case EVENT_SYSTEM: thread->port[2] = port; break;
    default: return unlockg(g, PS3_LV2_SPU_EINVAL);
    }
    thread->connected = 1;
    return unlockg(g, PS3_LV2_SPU_OK);
}
/* Root (2026-11-12): sys_spu_thread_connect_event(id, queue, SYS_SPU_THREAD_EVENT_USER, spup) -- the SPU thread's own
 * user port `spup` (0..63), what its send_event/throw_event words name. queue 0 disconnects. */
uint32_t ps3_lv2_spu_thread_user_port(ps3_lv2_spu_groups *g, uint32_t thread_id, uint32_t queue, uint32_t spup)
{
    if (!g || !g->initialized || spup > 63u) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_thread *thread = find_thread(g, thread_id, NULL);
    if (!thread) return unlockg(g, PS3_LV2_SPU_ESRCH);
    const uint64_t bit = UINT64_C(1) << spup;
    if (queue) {
        if (thread->spup_bound & bit) return unlockg(g, PS3_LV2_SPU_EISCONN);
        thread->spup_bound |= bit;
        thread->spup_queue[spup] = queue;
    } else {
        if (!(thread->spup_bound & bit)) return unlockg(g, PS3_LV2_SPU_EINVAL);
        thread->spup_bound &= ~bit;
        thread->spup_queue[spup] = 0;
    }
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_thread_disconnect_event(ps3_lv2_spu_groups *g, uint32_t thread_id)
{
    return ps3_lv2_spu_thread_connect_event(g, thread_id, 0, EVENT_RUN, 0);
}
uint32_t ps3_lv2_spu_group_connect_event_all_threads(ps3_lv2_spu_groups *g, uint32_t id, uint32_t port, uint64_t request,
                                                 uint32_t *spu_number)
{
    /* Root fix (B2b Eden): lv2 semantics. `request` is a BITMASK of acceptable SPU port numbers
     * (spup 0..63). The answer is the lowest requested spup that no thread of the group has bound;
     * every initialized thread then binds that spup to the event queue `port`. All requested spups
     * taken -> EISCONN (0x80010015), which libsre retries on (ppu_recomp_000.cpp:13334-13336). The
     * worker's version ignored both the mask and the queue. */
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    if (!spu_number) return PS3_LV2_SPU_EFAULT;
    if (!request) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    for (uint32_t candidate = 0; candidate < 64; ++candidate) {
        const uint64_t bit = UINT64_C(1) << candidate;
        if (!(request & bit)) continue;
        int taken = 0;
        for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
            if (group->threads[ti].initialized && (group->threads[ti].spup_bound & bit)) taken = 1;
        if (taken) continue;
        for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti) {
            if (!group->threads[ti].initialized) continue;
            group->threads[ti].spup_bound |= bit;
            group->threads[ti].spup_queue[candidate] = port;
        }
        *spu_number = candidate;
        return unlockg(g, PS3_LV2_SPU_OK);
    }
    return unlockg(g, PS3_LV2_SPU_EISCONN);
}

uint32_t ps3_lv2_spu_group_start(ps3_lv2_spu_groups *g, uint32_t id)
{
    if (!g || !g->initialized) return PS3_LV2_SPU_EINVAL;
    lockg(g);
    ps3_lv2_spu_group *group = find_group(g, id);
    if (!group) return unlockg(g, PS3_LV2_SPU_ESRCH);
    if (group->started) return unlockg(g, PS3_LV2_SPU_EBUSY);
    for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti)
        if (!group->threads[ti].initialized) return unlockg(g, PS3_LV2_SPU_ESTAT);
    /* Decision 33 / C1b: a fresh run. Every thread starts un-ended, so a runner's record is the only one. */
    group->ended_count = 0;
    group->join_cause = 0;
    for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti) {
        group->threads[ti].ended = 0;
        group->threads[ti].exited = 0;
        group->threads[ti].exit_status = 0;
    }
    /* Record what phase C1 will run. This module never runs it. */
    ps3_lv2_spu_start_snapshot snapshot = {};
    snapshot.valid = 1;
    snapshot.group = group->id;
    snapshot.num = group->num;
    snapshot.priority = group->priority;
    for (size_t ti = 0; ti < group->num && ti < PS3_LV2_SPU_THREADS_MAX; ++ti) {
        const ps3_lv2_spu_thread *thread = &group->threads[ti];
        snapshot.thread_id[ti] = thread->id;
        snapshot.entry[ti] = thread->entry_point;
        snapshot.arg1[ti] = thread->arg1;
        snapshot.ls_hash[ti] = thread->ls ? ps3_lv2_spu_ls_hash(thread->ls, 64) : 0;
    }
    g->snapshot = snapshot;
    group->started = 1;
    return unlockg(g, PS3_LV2_SPU_OK);
}
uint32_t ps3_lv2_spu_group_snapshot(const ps3_lv2_spu_groups *g, ps3_lv2_spu_start_snapshot *out)
{
    if (!g || !g->initialized || !out) return PS3_LV2_SPU_EINVAL;
    *out = g->snapshot;
    return PS3_LV2_SPU_OK;
}
