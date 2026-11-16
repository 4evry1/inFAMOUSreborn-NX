/* Behavioural tests for native/domain/gcm_address_space. All expected values are computed by hand
 * from the 1 MiB page rule and the cellGcmSys.c semantics cited in the header, not by calling the
 * code under test. CPU only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../domain/gcm_address_space.h"
#include "../domain/rsx_fifo_walker.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); g_checks++; \
    if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (0x%llX vs 0x%llX)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define MB 0x100000u
#define SENT 0xA5A5A5A5u

/* Reference geometry: local 256 MiB at 0xC0000000 (ends 0xD0000000); init mapping EA 0x30100000 size 4 MiB -> io 0;
 * 256 MiB IO space (test-chosen policy value). */
#define LB 0xC0000000u
#define LS 0x10000000u
#define IOEA 0x30100000u
#define IOSZ 0x400000u
#define SPACE 0x10000000u

typedef struct fx { gcm_address_space as; gcm_as_mapping slots[8]; } fx;

static gcm_as_config cfg_for(fx *f, uint32_t slot_count, uint32_t space) {
    gcm_as_config c; memset(&c, 0, sizeof c);
    c.local_base = LB; c.local_size = LS; c.io_ea = IOEA; c.io_size = IOSZ; c.io_space_size = space;
    c.slots = f->slots; c.slot_count = slot_count;
    return c;
}
static void fx_init(fx *f, uint32_t slot_count, uint32_t space) {
    memset(f, 0, sizeof *f);
    gcm_as_config c = cfg_for(f, slot_count, space);
    EQ(gcm_as_init(&f->as, &c), GCM_AS_OK);
}
/* Snapshot for no-effect proofs. */
typedef struct snap { fx f; } snap;
static void take(snap *s, const fx *f) { memcpy(&s->f, f, sizeof *f); }
/* Explicit full-field comparison (no padding assumptions): every instance field, every config field (the
 * slots pointer included: the snapshot copy still points at the ORIGINAL f->slots) and every field of every slot. */
static int same(const snap *s, const fx *f) {
    const gcm_as_config *a = &s->f.as.cfg, *b = &f->as.cfg;
    if (s->f.as.magic != f->as.magic || s->f.as.active != f->as.active) return 0;
    if (a->local_base != b->local_base || a->local_size != b->local_size || a->io_ea != b->io_ea ||
        a->io_size != b->io_size || a->io_space_size != b->io_space_size || a->slots != b->slots ||
        a->slot_count != b->slot_count) return 0;
    for (size_t i = 0; i < sizeof f->slots / sizeof f->slots[0]; i++) {
        const gcm_as_mapping *x = &s->f.slots[i], *y = &f->slots[i];
        if (x->ea != y->ea || x->io != y->io || x->size != y->size || x->active != y->active || x->initial != y->initial) return 0;
    }
    return 1;
}
#define REFUSE_MAIN(f, st, ea, size) do { snap _s; uint32_t _io = SENT; take(&_s, (f)); \
    EQ(gcm_as_map_main_memory(&(f)->as, (ea), (size), &_io), (st)); EQ(_io, SENT); CHECK(same(&_s, (f))); } while (0)
#define REFUSE_EAIO(f, st, ea, io, size) do { snap _s; take(&_s, (f)); \
    EQ(gcm_as_map_ea_io(&(f)->as, (ea), (io), (size)), (st)); CHECK(same(&_s, (f))); } while (0)

/* ---- init ---------------------------------------------------------------------------------- */
static void test_init(void) {
    fx f; gcm_as_config c; memset(&c, 0, sizeof c);
    EQ(gcm_as_init(NULL, &c), GCM_AS_INVALID_ARGUMENT);
    EQ(gcm_as_init(&f.as, NULL), GCM_AS_INVALID_ARGUMENT);

    struct { const char *what; int field; uint32_t value; } bad[] = {
        {"local_base unaligned", 0, LB + 1}, {"local_size 0", 1, 0}, {"local_size unaligned", 1, LS + 0x1000},
        {"local end wraps", 0, 0xF0000000u}, /* + 0x10000000 == 2^32 exactly: valid, patched below */
        {"io_ea unaligned", 2, IOEA + 0x10000}, {"io_ea page 0", 2, 0}, {"io_size 0", 3, 0}, {"io_size unaligned", 3, IOSZ + 4},
        {"io_space 0", 4, 0}, {"io_space unaligned", 4, SPACE + 1}, {"io_space < io_size", 4, IOSZ - MB},
        {"io range wraps", 2, 0xFFF00000u}, {"io range in local window", 2, 0xBFF00000u},
        {"io range inside local window", 2, 0xC8000000u}, {"local_base 0", 0, 0},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&f, 0xA5, sizeof f);
        c = cfg_for(&f, 4, SPACE);
        switch (bad[i].field) { case 0: c.local_base = bad[i].value; break; case 1: c.local_size = bad[i].value; break;
            case 2: c.io_ea = bad[i].value; break; case 3: c.io_size = bad[i].value; break; default: c.io_space_size = bad[i].value; }
        if (!strcmp(bad[i].what, "local end wraps")) c.local_size = 0x20000000u; /* 0xF0000000+0x20000000 > 2^32 */
        if (!strcmp(bad[i].what, "io range wraps")) c.io_size = 2 * MB;
        if (!strcmp(bad[i].what, "io range in local window")) c.io_size = 2 * MB; /* [0xBFF00000,0xC0100000) */
        if (!strcmp(bad[i].what, "io range inside local window")) c.io_size = MB;
        gcm_as_status st = gcm_as_init(&f.as, &c);
        if (st != GCM_AS_CONFIG_INVALID) printf("  case: %s -> %s\n", bad[i].what, gcm_as_status_name(st));
        EQ(st, GCM_AS_CONFIG_INVALID);
        /* no effect: storage still the 0xA5 fill (as AND slots) */
        unsigned char *p = (unsigned char *)&f; int clean = 1;
        for (size_t k = 0; k < sizeof f; k++) if (p[k] != 0xA5) { clean = 0; break; }
        if (!clean && st == GCM_AS_CONFIG_INVALID) { printf("  case: %s touched storage\n", bad[i].what); }
        CHECK(clean);
    }
    memset(&f, 0xA5, sizeof f); c = cfg_for(&f, 4, SPACE); c.slots = NULL;
    EQ(gcm_as_init(&f.as, &c), GCM_AS_CONFIG_INVALID);
    c = cfg_for(&f, 0, SPACE); EQ(gcm_as_init(&f.as, &c), GCM_AS_CONFIG_INVALID);

    /* Boundary: valid configs that sit exactly on the limits. */
    memset(&f, 0, sizeof f); c = cfg_for(&f, 1, IOSZ);        /* io_space == io_size, one slot */
    c.local_base = 0xFFF00000u; c.local_size = MB;            /* local ends at exactly 2^32 */
    c.io_ea = 0xFFE00000u; c.io_size = IOSZ;                  /* [0xFFE00000, 0x100200000) wraps -> invalid */
    EQ(gcm_as_init(&f.as, &c), GCM_AS_CONFIG_INVALID);
    c.io_ea = 0xFFB00000u; c.io_size = IOSZ;                  /* ends exactly at 0xFFF00000 = local base: valid */
    EQ(gcm_as_init(&f.as, &c), GCM_AS_OK);
    c.io_ea = 0xFFC00000u;                                    /* ends at 2^32 inside the local page: overlap */
    EQ(gcm_as_init(&f.as, &c), GCM_AS_CONFIG_INVALID);
    c.io_ea = 0xFFB00000u;
    EQ(gcm_as_init(&f.as, &c), GCM_AS_OK);
    c.io_ea = IOEA; c.local_base = 0x30500000u; c.local_size = MB; /* local starts exactly where io ea range ends */
    EQ(gcm_as_init(&f.as, &c), GCM_AS_OK);
    c.local_base = 0x30400000u; EQ(gcm_as_init(&f.as, &c), GCM_AS_CONFIG_INVALID); /* one page of overlap */

    /* Successful init content. */
    fx_init(&f, 4, SPACE);
    EQ(gcm_as_active_count(&f.as), 1);
    gcm_as_mapping m; EQ(gcm_as_get_mapping(&f.as, 0, &m), GCM_AS_OK);
    EQ(m.ea, IOEA); EQ(m.io, 0); EQ(m.size, IOSZ); EQ(m.active, 1); EQ(m.initial, 1);
    EQ(gcm_as_get_mapping(&f.as, 1, &m), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_get_mapping(&f.as, 3, &m), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_get_mapping(&f.as, 4, &m), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_get_mapping(&f.as, 0, NULL), GCM_AS_INVALID_ARGUMENT);
    /* Re-init resets everything (stale slots zeroed). */
    uint32_t io; EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, &io), GCM_AS_OK);
    fx_init(&f, 4, SPACE); EQ(gcm_as_active_count(&f.as), 1);
    /* init zeroes dirty caller slots */
    memset(f.slots, 0xFF, sizeof f.slots); c = cfg_for(&f, 4, SPACE);
    EQ(gcm_as_init(&f.as, &c), GCM_AS_OK); EQ(gcm_as_get_mapping(&f.as, 2, &m), GCM_AS_NOT_MAPPED);
}

static void test_uninitialized(void) {
    gcm_address_space z; memset(&z, 0, sizeof z);
    uint32_t a = SENT, b = SENT; gcm_as_mapping m;
    EQ(gcm_as_map_main_memory(&z, 0x20000000u, MB, &a), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_map_ea_io(&z, 0x20000000u, 0x1000000u, MB), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_unmap_ea(&z, 0x20000000u), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_unmap_io(&z, 0x1000000u), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_address_to_offset(&z, 0xC0000000u, 1, &a, &b), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_io_to_address(&z, 0, 4, &a), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_resolve(&z, GCM_AS_LOCATION_LOCAL, 0, 4, &a), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_get_mapping(&z, 0, &m), GCM_AS_NOT_INITIALIZED);
    EQ(gcm_as_active_count(&z), 0); EQ(a, SENT); EQ(b, SENT);
    EQ(gcm_as_map_main_memory(NULL, 0x20000000u, MB, &a), GCM_AS_INVALID_ARGUMENT);
    EQ(gcm_as_active_count(NULL), 0);
}

/* ---- translation of the init mapping and local window ----------------------------------------- */
static void test_translation_basic(void) {
    fx f; fx_init(&f, 4, SPACE);
    uint32_t loc, off, ea;
    /* Init mapping boundaries: EA [0x30100000, 0x30500000) <-> IO [0, 0x400000). */
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 1, &loc, &off), GCM_AS_OK); EQ(loc, GCM_AS_LOCATION_MAIN); EQ(off, 0);
    EQ(gcm_as_address_to_offset(&f.as, 0x304FFFFFu, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x3FFFFF);
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, IOSZ, &loc, &off), GCM_AS_OK); EQ(off, 0);
    EQ(gcm_as_address_to_offset(&f.as, 0x304FFFFCu, 4, &loc, &off), GCM_AS_OK); EQ(off, 0x3FFFFC);
    loc = off = SENT;
    EQ(gcm_as_address_to_offset(&f.as, 0x304FFFFFu, 2, &loc, &off), GCM_AS_RANGE_SPANS); EQ(loc, SENT); EQ(off, SENT);
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, IOSZ + 1, &loc, &off), GCM_AS_RANGE_SPANS);
    EQ(gcm_as_address_to_offset(&f.as, 0x30500000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x300FFFFFu, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x300FFFFFu, 2, &loc, &off), GCM_AS_NOT_MAPPED); /* starts unmapped */
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 0, &loc, &off), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_address_to_offset(&f.as, 0xFFFFFFFFu, 2, &loc, &off), GCM_AS_RANGE_OVERFLOW);
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 0xCFF00000u, &loc, &off), GCM_AS_RANGE_SPANS);   /* ends exactly at 2^32 */
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 0xCFF00001u, &loc, &off), GCM_AS_RANGE_OVERFLOW);
    EQ(gcm_as_address_to_offset(&f.as, 0xFFFFFFFFu, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 1, NULL, &off), GCM_AS_INVALID_ARGUMENT);
    EQ(gcm_as_address_to_offset(&f.as, 0x30100000u, 1, &loc, NULL), GCM_AS_INVALID_ARGUMENT);

    /* Local window [0xC0000000, 0xD0000000). */
    EQ(gcm_as_address_to_offset(&f.as, 0xC0000000u, 1, &loc, &off), GCM_AS_OK); EQ(loc, GCM_AS_LOCATION_LOCAL); EQ(off, 0);
    EQ(gcm_as_address_to_offset(&f.as, 0xC0123456u, 4, &loc, &off), GCM_AS_OK); EQ(off, 0x123456);
    EQ(gcm_as_address_to_offset(&f.as, 0xCFFFFFFFu, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x0FFFFFFF);
    EQ(gcm_as_address_to_offset(&f.as, 0xC0000000u, LS, &loc, &off), GCM_AS_OK);
    EQ(gcm_as_address_to_offset(&f.as, 0xCFFFFFFFu, 2, &loc, &off), GCM_AS_LOCAL_RANGE);
    EQ(gcm_as_address_to_offset(&f.as, 0xC0000000u, LS + 1, &loc, &off), GCM_AS_LOCAL_RANGE);
    EQ(gcm_as_address_to_offset(&f.as, 0xD0000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0xBFFFFFFFu, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0xBFFFFFFFu, 2, &loc, &off), GCM_AS_NOT_MAPPED); /* starts below the window */

    /* IO -> EA. */
    EQ(gcm_as_io_to_address(&f.as, 0, 4, &ea), GCM_AS_OK); EQ(ea, IOEA);
    EQ(gcm_as_io_to_address(&f.as, 0x3FFFFC, 4, &ea), GCM_AS_OK); EQ(ea, 0x304FFFFCu);
    EQ(gcm_as_io_to_address(&f.as, 0x3FFFFD, 4, &ea), GCM_AS_RANGE_SPANS);
    EQ(gcm_as_io_to_address(&f.as, 0x400000, 1, &ea), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_io_to_address(&f.as, 0, 0, &ea), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_io_to_address(&f.as, 0xFFFFFFFFu, 2, &ea), GCM_AS_RANGE_OVERFLOW);
    EQ(gcm_as_io_to_address(&f.as, 0, 4, NULL), GCM_AS_INVALID_ARGUMENT);
    /* Local-window offsets are not IO offsets: io 0xC0000000 is not mapped. */
    EQ(gcm_as_io_to_address(&f.as, 0xC0000000u, 1, &ea), GCM_AS_NOT_MAPPED);
}

static void test_resolve_location(void) {
    fx f; fx_init(&f, 4, SPACE);
    uint32_t ea = SENT;
    /* The same numeric offset means different memory by location. */
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, 0x100, 4, &ea), GCM_AS_OK); EQ(ea, 0xC0000100u);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0x100, 4, &ea), GCM_AS_OK); EQ(ea, 0x30100100u);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, 0x0FFFFFFC, 4, &ea), GCM_AS_OK); EQ(ea, 0xCFFFFFFCu);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, 0x0FFFFFFD, 4, &ea), GCM_AS_LOCAL_RANGE);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, LS, 1, &ea), GCM_AS_LOCAL_RANGE);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, 0xFFFFFFFFu, 2, &ea), GCM_AS_RANGE_OVERFLOW);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0x400000, 4, &ea), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0x3FFFFE, 4, &ea), GCM_AS_RANGE_SPANS);
    EQ(ea, 0xCFFFFFFCu);   /* still the last successful result */
    EQ(gcm_as_resolve(&f.as, 0, 0, 4, &ea), GCM_AS_INVALID_VALUE);     /* desktop `local==0` polarity must not alias */
    EQ(gcm_as_resolve(&f.as, 3, 0, 4, &ea), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_resolve(&f.as, 0xFFFFFFFFu, 0, 4, &ea), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0, 0, &ea), GCM_AS_INVALID_VALUE);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0, 4, NULL), GCM_AS_INVALID_ARGUMENT);
    EQ(ea, 0xCFFFFFFCu);   /* never touched by any failure above */
    /* Alias hazard: a main mapping whose IO offsets coincide with local offsets still resolves by location. */
    uint32_t io; EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_MAIN, 0x400010, 4, &ea), GCM_AS_OK); EQ(ea, 0x20000010u);
    EQ(gcm_as_resolve(&f.as, GCM_AS_LOCATION_LOCAL, 0x400010, 4, &ea), GCM_AS_OK); EQ(ea, 0xC0400010u);
}

/* ---- map_main_memory --------------------------------------------------------------------------- */
static void test_map_main(void) {
    fx f; fx_init(&f, 4, SPACE);
    uint32_t io = SENT, loc, off, ea;
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, 0x300000, &io), GCM_AS_OK); EQ(io, 0x400000); /* first free IO page == io_size */
    EQ(gcm_as_map_main_memory(&f.as, 0x21000000u, MB, &io), GCM_AS_OK); EQ(io, 0x700000);       /* after 3 MiB */
    EQ(gcm_as_active_count(&f.as), 3);
    gcm_as_mapping m; EQ(gcm_as_get_mapping(&f.as, 1, &m), GCM_AS_OK);
    EQ(m.ea, 0x20000000u); EQ(m.io, 0x400000); EQ(m.size, 0x300000); EQ(m.active, 1); EQ(m.initial, 0);
    EQ(gcm_as_get_mapping(&f.as, 2, &m), GCM_AS_OK); EQ(m.ea, 0x21000000u); EQ(m.io, 0x700000);
    /* Translation: last byte of first mapping is io 0x6FFFFF; io 0x700000 is the SECOND mapping. */
    EQ(gcm_as_address_to_offset(&f.as, 0x202FFFFFu, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x6FFFFF);
    EQ(gcm_as_address_to_offset(&f.as, 0x21000000u, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x700000);
    EQ(gcm_as_address_to_offset(&f.as, 0x20000000u, 0x300000, &loc, &off), GCM_AS_OK); EQ(off, 0x400000);
    EQ(gcm_as_io_to_address(&f.as, 0x6FFFFF, 1, &ea), GCM_AS_OK); EQ(ea, 0x202FFFFFu);
    EQ(gcm_as_io_to_address(&f.as, 0x700000, 1, &ea), GCM_AS_OK); EQ(ea, 0x21000000u);
    /* IO-adjacent but EA-discontiguous mappings: a range across them must be refused. */
    EQ(gcm_as_io_to_address(&f.as, 0x6FFFFF, 2, &ea), GCM_AS_RANGE_SPANS);
    /* EA-adjacent mapping with a non-contiguous IO range likewise. */
    EQ(gcm_as_map_main_memory(&f.as, 0x20300000u, MB, &io), GCM_AS_OK); EQ(io, 0x800000);
    EQ(gcm_as_active_count(&f.as), 4);
    EQ(gcm_as_address_to_offset(&f.as, 0x202FFFFFu, 2, &loc, &off), GCM_AS_RANGE_SPANS);
    EQ(gcm_as_address_to_offset(&f.as, 0x20300000u, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x800000);
    /* Slot exhaustion (4 slots, all used): no effect. */
    REFUSE_MAIN(&f, GCM_AS_NO_SLOT, 0x22000000u, MB);
    REFUSE_EAIO(&f, GCM_AS_NO_SLOT, 0x22000000u, 0x2000000u, MB);
}

static void test_map_main_refusals(void) {
    fx f; fx_init(&f, 8, SPACE);
    uint32_t io = SENT;
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, 0x200000, &io), GCM_AS_OK); EQ(io, 0x400000);
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, NULL), GCM_AS_INVALID_ARGUMENT);
    REFUSE_MAIN(&f, GCM_AS_MISALIGNED, 0x28000000u, 0);
    REFUSE_MAIN(&f, GCM_AS_MISALIGNED, 0x28000000u, 0x80000);
    REFUSE_MAIN(&f, GCM_AS_MISALIGNED, 0x28000000u, MB + 0x1000);
    REFUSE_MAIN(&f, GCM_AS_MISALIGNED, 0x28080000u, MB);
    REFUSE_MAIN(&f, GCM_AS_MISALIGNED, 0x28000001u, MB);
    REFUSE_MAIN(&f, GCM_AS_RANGE_OVERFLOW, 0xFFF00000u, 2 * MB);          /* 0x100100000 > 2^32 */
    REFUSE_MAIN(&f, GCM_AS_RANGE_OVERFLOW, 0xFFF00000u, 0xFFF00000u);
    REFUSE_MAIN(&f, GCM_AS_EA_FORBIDDEN, 0, MB);                          /* page 0 */
    REFUSE_MAIN(&f, GCM_AS_EA_FORBIDDEN, 0xBFF00000u, 2 * MB);            /* tail enters local window */
    REFUSE_MAIN(&f, GCM_AS_EA_FORBIDDEN, 0xC0000000u, MB);
    REFUSE_MAIN(&f, GCM_AS_EA_FORBIDDEN, 0xCFF00000u, MB);                /* last local page */
    REFUSE_MAIN(&f, GCM_AS_EA_FORBIDDEN, 0xB0000000u, 0x30000000u);       /* spans the whole window */
    /* Overlap in EA: tail, head, containing, contained, init mapping, exact. */
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x20100000u, 2 * MB);
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x1FF00000u, 2 * MB);
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x1F000000u, 0x2000000u);
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x20100000u, MB);
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x20000000u, 0x200000);
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x30000000u, 2 * MB);              /* head of init mapping */
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x30400000u, MB);                  /* last page of init mapping */
    /* Adjacent / boundary-touching mappings are accepted. */
    EQ(gcm_as_map_main_memory(&f.as, 0x20200000u, MB, &io), GCM_AS_OK); EQ(io, 0x600000);   /* right after first */
    EQ(gcm_as_map_main_memory(&f.as, 0x1FF00000u, MB, &io), GCM_AS_OK); EQ(io, 0x700000);   /* right before first */
    EQ(gcm_as_map_main_memory(&f.as, 0xBFF00000u, MB, &io), GCM_AS_OK);                     /* ends where local begins */
    EQ(gcm_as_map_main_memory(&f.as, 0xD0000000u, MB, &io), GCM_AS_OK);                     /* starts where local ends */
    EQ(gcm_as_map_main_memory(&f.as, 0xFFF00000u, MB, &io), GCM_AS_OK);                     /* ends at exactly 2^32 */
    EQ(gcm_as_active_count(&f.as), 7);
}

static void test_io_space_exhaustion(void) {
    fx f; fx_init(&f, 8, IOSZ + 2 * MB);                 /* only 2 MiB above the init mapping */
    uint32_t io = SENT;
    REFUSE_MAIN(&f, GCM_AS_NO_IO_SPACE, 0x20000000u, 3 * MB);
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, 2 * MB, &io), GCM_AS_OK); EQ(io, 0x400000);   /* exact fit */
    REFUSE_MAIN(&f, GCM_AS_NO_IO_SPACE, 0x21000000u, MB);
    EQ(gcm_as_unmap_ea(&f.as, 0x20000000u), GCM_AS_OK);
    EQ(gcm_as_map_main_memory(&f.as, 0x21000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);       /* freed IO reused */
    EQ(gcm_as_map_main_memory(&f.as, 0x22000000u, MB, &io), GCM_AS_OK); EQ(io, 0x500000);
    REFUSE_MAIN(&f, GCM_AS_NO_IO_SPACE, 0x23000000u, MB);
    /* io_space exactly io_size: nothing can be allocated automatically. */
    fx g; fx_init(&g, 4, IOSZ);
    REFUSE_MAIN(&g, GCM_AS_NO_IO_SPACE, 0x20000000u, MB);
    /* size so large that io+size would wrap u32 must be NO_IO_SPACE, not a wrapped success. */
    fx h; fx_init(&h, 4, SPACE);
    REFUSE_MAIN(&h, GCM_AS_NO_IO_SPACE, 0x40000000u, 0x70000000u);
}

static void test_first_fit(void) {
    fx f; fx_init(&f, 8, SPACE);
    EQ(gcm_as_map_ea_io(&f.as, 0x20000000u, 0x400000, MB), GCM_AS_OK);       /* A: io [4,5) MiB */
    EQ(gcm_as_map_ea_io(&f.as, 0x21000000u, 0x700000, MB), GCM_AS_OK);       /* B: io [7,8) MiB */
    uint32_t io = SENT;
    EQ(gcm_as_map_main_memory(&f.as, 0x22000000u, 2 * MB, &io), GCM_AS_OK); EQ(io, 0x500000);   /* gap [5,7) fits exactly */
    EQ(gcm_as_map_main_memory(&f.as, 0x23000000u, MB, &io), GCM_AS_OK); EQ(io, 0x800000);       /* gap now full */
    EQ(gcm_as_unmap_io(&f.as, 0x500000), GCM_AS_OK);
    EQ(gcm_as_map_main_memory(&f.as, 0x24000000u, 3 * MB, &io), GCM_AS_OK); EQ(io, 0x900000);   /* 2 MiB hole too small */
    EQ(gcm_as_map_main_memory(&f.as, 0x25000000u, MB, &io), GCM_AS_OK); EQ(io, 0x500000);       /* lowest free page */
    /* An explicit mapping placed far above never raises the automatic floor (no bump cursor). */
    fx g; fx_init(&g, 4, SPACE);
    EQ(gcm_as_map_ea_io(&g.as, 0x20000000u, 0x8000000u, MB), GCM_AS_OK);
    EQ(gcm_as_map_main_memory(&g.as, 0x21000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
}

/* ---- map_ea_io --------------------------------------------------------------------------------- */
static void test_map_ea_io(void) {
    fx f; fx_init(&f, 8, SPACE);
    uint32_t loc, off, ea;
    EQ(gcm_as_map_ea_io(&f.as, 0x40000000u, 0x1000000u, 2 * MB), GCM_AS_OK);
    EQ(gcm_as_address_to_offset(&f.as, 0x401FFFFFu, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x11FFFFF);
    EQ(gcm_as_io_to_address(&f.as, 0x1000000u, 1, &ea), GCM_AS_OK); EQ(ea, 0x40000000u);
    EQ(gcm_as_io_to_address(&f.as, 0x11FFFFF, 1, &ea), GCM_AS_OK); EQ(ea, 0x401FFFFFu);
    EQ(gcm_as_io_to_address(&f.as, 0x1200000u, 1, &ea), GCM_AS_NOT_MAPPED);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x41000001u, 0x2000000u, MB);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x41000000u, 0x2000800u, MB);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x41000000u, 0x2000000u, 0);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x41000000u, 0x2000000u, 0x180000);
    REFUSE_EAIO(&f, GCM_AS_RANGE_OVERFLOW, 0xFFF00000u, 0x2000000u, 2 * MB);   /* ea wraps */
    REFUSE_EAIO(&f, GCM_AS_RANGE_OVERFLOW, 0x41000000u, 0xFFF00000u, 2 * MB);  /* io wraps */
    REFUSE_EAIO(&f, GCM_AS_IO_RANGE, 0x41000000u, SPACE - MB, 2 * MB);         /* ends one page past the space */
    REFUSE_EAIO(&f, GCM_AS_IO_RANGE, 0x41000000u, SPACE, MB);
    REFUSE_EAIO(&f, GCM_AS_EA_FORBIDDEN, 0, 0x2000000u, MB);
    REFUSE_EAIO(&f, GCM_AS_EA_FORBIDDEN, 0xC0000000u, 0x2000000u, MB);
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, 0x2000000u, MB);    /* exact desktop OVERWRAP case */
    REFUSE_EAIO(&f, GCM_AS_EA_OVERLAP, 0x40100000u, 0x2000000u, MB);           /* interior: desktop would allow */
    REFUSE_EAIO(&f, GCM_AS_EA_OVERLAP, 0x3FF00000u, 0x2000000u, 2 * MB);
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x30100000u, 0x2000000u, MB);    /* init mapping EA, exact base */
    REFUSE_EAIO(&f, GCM_AS_EA_OVERLAP, 0x30200000u, 0x2000000u, MB);          /* init mapping interior */
    /* F2: an exact duplicate EA is reported right after the alignment checks, before the range/IO policy refusals
     * (reference order: alignment, then exact-EA ADDRESS_OVERWRAP). Mapped EAs here: 0x40000000 and IOEA. */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, 0x2000000u, 0xC0100000u);  /* ea+size = 2^32+1MiB wraps */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x30100000u, 0x2000000u, 0xD0000000u);  /* init EA, ea+size wraps */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, 0xFFF00000u, 2 * MB);      /* io+size wraps */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, SPACE, MB);                /* io beyond io_space_size */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, SPACE - MB, 2 * MB);       /* io ends one page past */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x30100000u, 0x300000, MB);             /* io overlaps too: still dup */
    REFUSE_EAIO(&f, GCM_AS_EA_ALREADY_MAPPED, 0x40000000u, 0x2000000u, 0x7000000u);   /* larger than the mapping */
    /* Alignment keeps precedence over the duplicate-EA report. */
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x40000000u, 0x2000800u, 0xC0100000u);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x40000000u, SPACE, 0x180000);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x40000000u, SPACE, 0);
    REFUSE_EAIO(&f, GCM_AS_MISALIGNED, 0x40000001u, SPACE, MB);                       /* unaligned ea is not an exact base */
    /* Non-exact EA conflicts keep their existing order: the IO range policy comes before EA_OVERLAP. */
    REFUSE_EAIO(&f, GCM_AS_IO_RANGE, 0x40100000u, SPACE, MB);
    REFUSE_EAIO(&f, GCM_AS_RANGE_OVERFLOW, 0x40100000u, 0x2000000u, 0xC0000000u);     /* ea+size wraps, interior ea */
    /* map_main_memory has no retained reference error for an exact EA: it stays the native EA_OVERLAP (not convertible). */
    REFUSE_MAIN(&f, GCM_AS_EA_OVERLAP, 0x40000000u, MB);
    { uint32_t v = SENT; EQ(gcm_as_status_to_cell_error(GCM_AS_EA_OVERLAP, &v), 0); EQ(v, SENT); }
    REFUSE_EAIO(&f, GCM_AS_IO_OVERLAP, 0x41000000u, 0x300000, MB);             /* inside init IO [0,4MiB) */
    REFUSE_EAIO(&f, GCM_AS_IO_OVERLAP, 0x41000000u, 0, MB);                    /* io 0: init start */
    REFUSE_EAIO(&f, GCM_AS_IO_OVERLAP, 0x41000000u, 0x1100000u, 2 * MB);       /* overlaps tail of [0x1000000,0x1200000) */
    REFUSE_EAIO(&f, GCM_AS_IO_OVERLAP, 0x41000000u, 0xF00000u, 2 * MB);        /* overlaps head */
    REFUSE_EAIO(&f, GCM_AS_IO_OVERLAP, 0x41000000u, 0xF00000u, 8 * MB);        /* contains it */
    /* Touching IO ranges are accepted, including the last page of the space. */
    EQ(gcm_as_map_ea_io(&f.as, 0x41000000u, 0x1200000u, MB), GCM_AS_OK);
    EQ(gcm_as_map_ea_io(&f.as, 0x42000000u, 0x400000, MB), GCM_AS_OK);          /* touches init mapping end */
    EQ(gcm_as_map_ea_io(&f.as, 0x43000000u, 0xF00000u, MB), GCM_AS_OK);         /* touches [0x1000000,..) start */
    EQ(gcm_as_map_ea_io(&f.as, 0x44000000u, SPACE - MB, MB), GCM_AS_OK);
    EQ(gcm_as_io_to_address(&f.as, SPACE - 4, 4, &ea), GCM_AS_OK); EQ(ea, 0x440FFFFCu);
    EQ(gcm_as_io_to_address(&f.as, SPACE - 4, 5, &ea), GCM_AS_RANGE_SPANS);
    /* Two mappings sharing one EA (or overlapping IO) is the failure mode that would corrupt a page-table design:
     * after all of the above, every pair of active slots has distinct EA bases and disjoint EA and IO ranges. */
    EQ(gcm_as_active_count(&f.as), 6);
    {
        gcm_as_mapping mi, mj; int n = 0;
        for (uint32_t i = 0; i < 8; i++) {
            if (gcm_as_get_mapping(&f.as, i, &mi) != GCM_AS_OK) continue;
            n++;
            for (uint32_t j = i + 1; j < 8; j++) {
                if (gcm_as_get_mapping(&f.as, j, &mj) != GCM_AS_OK) continue;
                CHECK(mi.ea != mj.ea);
                CHECK((uint64_t)mi.ea + mi.size <= mj.ea || (uint64_t)mj.ea + mj.size <= mi.ea);
                CHECK((uint64_t)mi.io + mi.size <= mj.io || (uint64_t)mj.io + mj.size <= mi.io);
            }
        }
        EQ(n, 6);
    }
}

/* ---- unmap ------------------------------------------------------------------------------------- */
static void test_unmap(void) {
    fx f; fx_init(&f, 4, SPACE);
    uint32_t io = SENT, loc, off, ea;
    snap s;
    /* Field-swap trap: mapping X has ea == mapping Y's io value and vice versa. */
    EQ(gcm_as_map_ea_io(&f.as, 0x01000000u, 0x02000000u, MB), GCM_AS_OK);   /* X: ea 16MiB -> io 32MiB */
    EQ(gcm_as_map_ea_io(&f.as, 0x02000000u, 0x03000000u, MB), GCM_AS_OK);   /* Y: ea 32MiB -> io 48MiB */
    take(&s, &f);
    EQ(gcm_as_unmap_ea(&f.as, 0x01000800u), GCM_AS_UNMAP_NOT_FOUND);             /* inside X, not its base */
    EQ(gcm_as_unmap_ea(&f.as, 0x01100000u), GCM_AS_UNMAP_NOT_FOUND);
    EQ(gcm_as_unmap_ea(&f.as, 0x03000000u), GCM_AS_UNMAP_NOT_FOUND);             /* is Y's io, not an ea */
    EQ(gcm_as_unmap_io(&f.as, 0x01000000u), GCM_AS_UNMAP_NOT_FOUND);             /* is X's ea, not an io */
    EQ(gcm_as_unmap_io(&f.as, 0x02080000u), GCM_AS_UNMAP_NOT_FOUND);
    EQ(gcm_as_unmap_ea(&f.as, 0), GCM_AS_UNMAP_NOT_FOUND);
    CHECK(same(&s, &f));
    /* Init mapping is protected by either key. */
    EQ(gcm_as_unmap_ea(&f.as, IOEA), GCM_AS_PROTECTED);
    EQ(gcm_as_unmap_io(&f.as, 0), GCM_AS_PROTECTED);
    CHECK(same(&s, &f));
    /* unmap_io(0x02000000) removes X (io 32MiB), leaving Y. */
    EQ(gcm_as_unmap_io(&f.as, 0x02000000u), GCM_AS_OK);
    EQ(gcm_as_active_count(&f.as), 2);
    EQ(gcm_as_address_to_offset(&f.as, 0x01000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_io_to_address(&f.as, 0x02000000u, 1, &ea), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x02000000u, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x03000000u);
    EQ(gcm_as_unmap_io(&f.as, 0x02000000u), GCM_AS_UNMAP_NOT_FOUND);            /* double unmap */
    /* unmap_ea(0x02000000) removes Y. */
    EQ(gcm_as_unmap_ea(&f.as, 0x02000000u), GCM_AS_OK);
    EQ(gcm_as_active_count(&f.as), 1);
    EQ(gcm_as_unmap_ea(&f.as, 0x02000000u), GCM_AS_UNMAP_NOT_FOUND);
    /* init mapping still translates. */
    EQ(gcm_as_address_to_offset(&f.as, IOEA, 1, &loc, &off), GCM_AS_OK); EQ(off, 0);
    /* Partial unmap does not exist: a multi-page mapping survives interior keys and unmaps whole. */
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, 4 * MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    EQ(gcm_as_unmap_ea(&f.as, 0x20100000u), GCM_AS_UNMAP_NOT_FOUND);
    EQ(gcm_as_unmap_io(&f.as, 0x500000), GCM_AS_UNMAP_NOT_FOUND);
    EQ(gcm_as_address_to_offset(&f.as, 0x203FFFFFu, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x7FFFFF);
    EQ(gcm_as_unmap_ea(&f.as, 0x20000000u), GCM_AS_OK);
    EQ(gcm_as_address_to_offset(&f.as, 0x20000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x203FFFFFu, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_io_to_address(&f.as, 0x7FFFFF, 1, &ea), GCM_AS_NOT_MAPPED);
    /* Reuse: freed slot and IO range both come back, with no stale size. */
    EQ(gcm_as_map_main_memory(&f.as, 0x50000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    gcm_as_mapping m; EQ(gcm_as_get_mapping(&f.as, 1, &m), GCM_AS_OK);
    EQ(m.ea, 0x50000000u); EQ(m.size, MB); EQ(m.io, 0x400000);
    EQ(gcm_as_address_to_offset(&f.as, 0x50100000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, 0x20000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);   /* old EA not resurrected */
    /* Remap of the previously unmapped EA is allowed again. */
    EQ(gcm_as_map_ea_io(&f.as, 0x20000000u, 0x900000u, MB), GCM_AS_OK);
    /* Repeated map/unmap cycles must not leak slots (all 4 slots: init + 3). */
    for (int i = 0; i < 20; i++) {
        EQ(gcm_as_unmap_ea(&f.as, 0x20000000u), GCM_AS_OK);
        EQ(gcm_as_map_ea_io(&f.as, 0x20000000u, 0x900000u, MB), GCM_AS_OK);
    }
    EQ(gcm_as_active_count(&f.as), 3);
}

static void test_isolation(void) {
    fx a, b; fx_init(&a, 4, SPACE); fx_init(&b, 4, SPACE);
    uint32_t io = SENT, loc, off;
    EQ(gcm_as_map_main_memory(&a.as, 0x20000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    EQ(gcm_as_address_to_offset(&b.as, 0x20000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_active_count(&b.as), 1);
    io = SENT; EQ(gcm_as_map_main_memory(&b.as, 0x21000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    EQ(gcm_as_unmap_io(&b.as, 0x400000), GCM_AS_OK);
    EQ(gcm_as_address_to_offset(&a.as, 0x20000000u, 1, &loc, &off), GCM_AS_OK); EQ(off, 0x400000);
    EQ(gcm_as_active_count(&a.as), 2);
    /* Different configs do not leak into each other. */
    fx c; memset(&c, 0, sizeof c); gcm_as_config cc = cfg_for(&c, 2, SPACE);
    cc.local_base = 0x80000000u; cc.local_size = 0x2000000u;
    EQ(gcm_as_init(&c.as, &cc), GCM_AS_OK);
    EQ(gcm_as_address_to_offset(&c.as, 0xC0000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&c.as, 0x80000000u, 1, &loc, &off), GCM_AS_OK); EQ(loc, GCM_AS_LOCATION_LOCAL);
    EQ(gcm_as_address_to_offset(&a.as, 0x80000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
}

static void test_status_mapping(void) {
    /* Convertible ONLY where the desktop reference retains the exact failure. */
    uint32_t v = SENT;
    /* F1: expected values are independent literals from the retained live RPCS3 header (cellGcmSys.h, blob
     * 0d07bf62a71520d97a372791ad59ad5e9269944f; evidence/opus-native-gcm-map-review-r1/root-abi-verification.json):
     * FAILURE 0x802100ff, INVALID_VALUE 0x80210003, INVALID_ALIGNMENT 0x80210004, ADDRESS_OVERWRAP 0x80210005.
     * They are NOT derived from the native header macros nor from the desktop reference (0x802107xx, which is wrong). */
    EQ(GCM_AS_CELL_ERROR_FAILURE, 0x802100FFu);
    EQ(GCM_AS_CELL_ERROR_INVALID_VALUE, 0x80210003u);
    EQ(GCM_AS_CELL_ERROR_INVALID_ALIGNMENT, 0x80210004u);
    EQ(GCM_AS_CELL_ERROR_ADDRESS_OVERWRAP, 0x80210005u);
    EQ(GCM_AS_CELL_OK, 0);
    EQ(gcm_as_status_to_cell_error(GCM_AS_OK, &v), 1); EQ(v, 0);
    EQ(gcm_as_status_to_cell_error(GCM_AS_MISALIGNED, &v), 1); EQ(v, 0x80210004u);
    EQ(gcm_as_status_to_cell_error(GCM_AS_EA_ALREADY_MAPPED, &v), 1); EQ(v, 0x80210005u);
    EQ(gcm_as_status_to_cell_error(GCM_AS_UNMAP_NOT_FOUND, &v), 1); EQ(v, 0x802100FFu);
    /* No converted status yields the desktop-reference 0x802107xx values. */
    { gcm_as_status conv[] = {GCM_AS_MISALIGNED, GCM_AS_EA_ALREADY_MAPPED, GCM_AS_UNMAP_NOT_FOUND};
      for (unsigned i = 0; i < 3; i++) { v = SENT; EQ(gcm_as_status_to_cell_error(conv[i], &v), 1); CHECK((v & 0xFFFFFF00u) == 0x80210000u); } }
    /* Everything else: not convertible, output untouched (an adapter must stop, not invent a cell error). */
    gcm_as_status none[] = {GCM_AS_INVALID_ARGUMENT, GCM_AS_NOT_INITIALIZED, GCM_AS_CONFIG_INVALID, GCM_AS_INVALID_VALUE,
        GCM_AS_RANGE_OVERFLOW, GCM_AS_EA_FORBIDDEN, GCM_AS_EA_OVERLAP, GCM_AS_IO_RANGE, GCM_AS_IO_OVERLAP, GCM_AS_NO_SLOT,
        GCM_AS_NO_IO_SPACE, GCM_AS_NOT_MAPPED, GCM_AS_RANGE_SPANS, GCM_AS_LOCAL_RANGE, GCM_AS_PROTECTED, (gcm_as_status)9999,
        (gcm_as_status)-1};
    for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) { v = SENT; EQ(gcm_as_status_to_cell_error(none[i], &v), 0); EQ(v, SENT); }
    EQ(gcm_as_status_to_cell_error(GCM_AS_OK, NULL), 0);
    /* Every defined status has a distinct non-empty name. */
    for (int a = 0; a <= GCM_AS_PROTECTED; a++) {
        const char *n = gcm_as_status_name((gcm_as_status)a); CHECK(n && n[0] && strcmp(n, "UNKNOWN") && strcmp(n, "stub"));
        for (int b = a + 1; b <= GCM_AS_PROTECTED; b++) CHECK(strcmp(n, gcm_as_status_name((gcm_as_status)b)) != 0);
    }
    CHECK(gcm_as_status_name((gcm_as_status)9999) != NULL);
}

/* ---- init alias / reset contracts ---------------------------------------------------------------- */
static void test_init_alias(void) {
    fx f; fx_init(&f, 4, SPACE);
    uint32_t io, loc, off;
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, &io), GCM_AS_OK);
    EQ(gcm_as_map_ea_io(&f.as, 0x21000000u, 0x2000000u, MB), GCM_AS_OK);
    /* Valid alias: reset from the instance's own stored config. Slots are cleared (only the init mapping remains)
     * and the stored config is preserved, not zeroed by the reset. */
    gcm_as_mapping *slots = f.as.cfg.slots;
    EQ(gcm_as_init(&f.as, &f.as.cfg), GCM_AS_OK);
    EQ(gcm_as_active_count(&f.as), 1);
    CHECK(f.as.cfg.slots == slots); EQ(f.as.cfg.local_base, LB); EQ(f.as.cfg.local_size, LS); EQ(f.as.cfg.io_ea, IOEA);
    EQ(f.as.cfg.io_size, IOSZ); EQ(f.as.cfg.io_space_size, SPACE); EQ(f.as.cfg.slot_count, 4);
    EQ(f.slots[1].active, 0); EQ(f.slots[2].active, 0); EQ(f.slots[1].ea, 0);
    EQ(gcm_as_address_to_offset(&f.as, 0x20000000u, 1, &loc, &off), GCM_AS_NOT_MAPPED);
    EQ(gcm_as_address_to_offset(&f.as, IOEA, 1, &loc, &off), GCM_AS_OK); EQ(off, 0);
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);   /* allocator state reset too */
    /* Invalid stored config through the alias: refused before any effect (slots, as, config all unchanged). */
    f.as.cfg.local_size = 0;
    snap s; take(&s, &f);
    EQ(gcm_as_init(&f.as, &f.as.cfg), GCM_AS_CONFIG_INVALID);
    CHECK(same(&s, &f)); EQ(f.as.cfg.local_size, 0);
    /* Slot storage overlapping *as or *cfg is refused before effects. */
    fx g; memset(&g, 0xA5, sizeof g);
    gcm_as_config c = cfg_for(&g, 4, SPACE);
    c.slots = (gcm_as_mapping *)(void *)&g.as;                        /* overlaps the instance */
    EQ(gcm_as_init(&g.as, &c), GCM_AS_CONFIG_INVALID);
    { unsigned char *p = (unsigned char *)&g; int clean = 1; for (size_t k = 0; k < sizeof g; k++) if (p[k] != 0xA5) clean = 0; CHECK(clean); }
    struct { gcm_as_config c; gcm_as_mapping s[4]; } blob; memset(&blob, 0, sizeof blob);
    blob.c = cfg_for(&g, 4, SPACE); blob.c.slots = (gcm_as_mapping *)(void *)&blob;   /* overlaps the caller's cfg */
    memset(&g, 0xA5, sizeof g);
    EQ(gcm_as_init(&g.as, &blob.c), GCM_AS_CONFIG_INVALID);
    { unsigned char *p = (unsigned char *)&g; int clean = 1; for (size_t k = 0; k < sizeof g; k++) if (p[k] != 0xA5) clean = 0; CHECK(clean); }
    EQ(blob.c.slot_count, 4);
    blob.c.slots = blob.s;                                             /* adjacent but disjoint: fine */
    EQ(gcm_as_init(&g.as, &blob.c), GCM_AS_OK);
    EQ(gcm_as_active_count(&g.as), 1); EQ(blob.s[0].initial, 1);
    /* Init reads nothing from prior *as: arbitrary previous bytes (including a stale magic) are overwritten. */
    fx h; memset(&h, 0xFF, sizeof h); c = cfg_for(&h, 4, SPACE);
    EQ(gcm_as_init(&h.as, &c), GCM_AS_OK); EQ(gcm_as_active_count(&h.as), 1);
}

/* ---- real FIFO walker driven through the address space ------------------------------------------ */
typedef struct host { const gcm_address_space *as; uint8_t *mem[2]; uint32_t base[2]; uint32_t len[2]; } host;
static rsx_fifo_read_result host_read(void *u, uint32_t io, uint8_t out[4]) {
    host *h = u; uint32_t ea;
    gcm_as_status st = gcm_as_io_to_address(h->as, io, 4, &ea);
    if (st != GCM_AS_OK) return RSX_FIFO_IO_UNMAPPED;
    for (int i = 0; i < 2; i++)
        if (h->mem[i] && ea >= h->base[i] && (uint64_t)ea + 4 <= (uint64_t)h->base[i] + h->len[i]) { memcpy(out, h->mem[i] + (ea - h->base[i]), 4); return RSX_FIFO_IO_OK; }
    return RSX_FIFO_IO_FAULT;
}
typedef struct sinkrec { int n; uint32_t method[4], data[4]; } sinkrec;
static int sink_method(void *u, uint32_t sub, uint32_t method, uint32_t data, uint32_t io, rsx_fifo_effect *e) {
    sinkrec *r = u; (void)sub; (void)io; (void)e; if (r->n < 4) { r->method[r->n] = method; r->data[r->n] = data; } r->n++; return 0;
}
static void put_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void test_walker_integration(void) {
    fx f; fx_init(&f, 4, SPACE);
    static uint8_t ring[0x100], second[0x100];
    memset(ring, 0, sizeof ring); memset(second, 0, sizeof second);
    host h; memset(&h, 0, sizeof h); h.as = &f.as;
    h.mem[0] = ring; h.base[0] = IOEA; h.len[0] = sizeof ring;       /* init mapping, io 0 */
    h.mem[1] = second; h.base[1] = 0x20000000u; h.len[1] = sizeof second;
    uint32_t io;
    EQ(gcm_as_map_main_memory(&f.as, 0x20000000u, MB, &io), GCM_AS_OK); EQ(io, 0x400000);
    /* io 0: [hdr count1 method 0x50][data][JUMP 0x400000]; at 0x400000: [hdr count1 method 0x50][data] */
    put_be(ring + 0, (1u << 18) | 0x50); put_be(ring + 4, 0x11111111u); put_be(ring + 8, 0x20000000u | 0x400000);
    put_be(second + 0, (1u << 18) | 0x50); put_be(second + 4, 0x22222222u);
    rsx_fifo_sink sink; sinkrec rec; memset(&rec, 0, sizeof rec); sink.user = &rec; sink.method = sink_method;
    rsx_fifo_reader rd = {&h, host_read};
    rsx_fifo_walker w; memset(&w, 0, sizeof w);
    EQ(rsx_fifo_walker_init(&w, rd, sink, 0), RSX_FIFO_REACHED_PUT);
    EQ(rsx_fifo_walker_publish_put(&w, 0x400008), RSX_FIFO_REACHED_PUT);
    rsx_fifo_pump_report rep; memset(&rep, 0, sizeof rep);
    EQ(rsx_fifo_walker_pump(&w, 100, NULL, &rep), RSX_FIFO_REACHED_PUT);
    EQ(rec.n, 2); EQ(rec.method[0], 0x50); EQ(rec.data[0], 0x11111111u); EQ(rec.data[1], 0x22222222u); EQ(rep.get, 0x400008);
    /* Unmap the branch target live: a fresh walker over the same ring now stops at an unmapped branch with the
     * first packet delivered and nothing from the unmapped region. */
    EQ(gcm_as_unmap_io(&f.as, 0x400000), GCM_AS_OK);
    memset(&rec, 0, sizeof rec); memset(&w, 0, sizeof w);
    EQ(rsx_fifo_walker_init(&w, rd, sink, 0), RSX_FIFO_REACHED_PUT);
    EQ(rsx_fifo_walker_publish_put(&w, 0x400008), RSX_FIFO_REACHED_PUT);
    EQ(rsx_fifo_walker_pump(&w, 100, NULL, &rep), RSX_FIFO_UNMAPPED_BRANCH);
    EQ(rec.n, 1); EQ(rec.data[0], 0x11111111u);
    EQ(rsx_fifo_walker_failure(&w)->detail, 0x400000);
    /* Mapped (so translation succeeds) but the host has no bytes there: FAULT, not UNMAPPED. */
    h.mem[1] = NULL;
    EQ(gcm_as_map_ea_io(&f.as, 0x20000000u, 0x400000, MB), GCM_AS_OK);
    memset(&rec, 0, sizeof rec); memset(&w, 0, sizeof w);
    EQ(rsx_fifo_walker_init(&w, rd, sink, 0), RSX_FIFO_REACHED_PUT);
    EQ(rsx_fifo_walker_publish_put(&w, 0x400008), RSX_FIFO_REACHED_PUT);
    EQ(rsx_fifo_walker_pump(&w, 100, NULL, &rep), RSX_FIFO_BRANCH_READ_FAULT);
}

int main(void) {
    test_init(); test_uninitialized(); test_translation_basic(); test_resolve_location(); test_map_main();
    test_map_main_refusals(); test_io_space_exhaustion(); test_first_fit(); test_map_ea_io(); test_unmap();
    test_isolation(); test_status_mapping(); test_init_alias(); test_walker_integration();
    printf("gcm_address_space: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
