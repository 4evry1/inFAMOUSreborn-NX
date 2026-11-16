/* Host tests for native/domain/rsx_fifo_walker (CPU semantics only).
 * Guest bytes live in the real ps3_guest_memory host implementation
 * (guest_space_host.c); a small test IO table maps RSX IO offsets onto it. */
#include "../domain/rsx_fifo_walker.h"
#include "guest_space_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

/* ---- encoders (NV40 FIFO words) ---- */
static uint32_t INC(uint32_t s, uint32_t m, uint32_t n) { return (n << 18) | (s << 13) | m; }
static uint32_t NI(uint32_t s, uint32_t m, uint32_t n) { return 0x40000000u | INC(s, m, n); }
static uint32_t JMP(uint32_t t) { return 0x20000000u | t; }
static uint32_t CALL(uint32_t t) { return t | 2u; }
#define RET 0x00020000u

/* ---- guest memory + IO map ---- */
typedef struct seg { uint32_t io, size, ea; } seg;
static const seg k_segs[] = {
    {0x00000000u, 0x10000u, 0x10000u},   /* A: main ring */
    {0x00100000u, 0x10000u, 0x20000u},   /* B: far region, numerically beyond PUT */
    {0xFFFFF000u, 0x01000u, 0x30000u},   /* C: top of IO space (overflow) */
    {0x00200000u, 0x01000u, 0x40000u},   /* D: IO-mapped but guest page uncommitted */
    {0x00300000u, 0x01000u, 0x00200000u} /* E: EA outside host space -> INVALID_RANGE */
};
static ElfTestHostSpace *g_space;
static ps3_guest_memory g_mem;

typedef struct io_port {
    unsigned reads;
    uint32_t log[64];          /* offsets of the first 64 reads */
    unsigned fail_at;          /* 1-based read index to fail, 0 = never */
    rsx_fifo_read_result fail_as;
} io_port;

static int io_translate(uint32_t io, uint32_t *ea) {
    for (size_t i = 0; i < sizeof k_segs / sizeof k_segs[0]; ++i) {
        uint64_t lo = k_segs[i].io, hi = lo + k_segs[i].size;
        if ((uint64_t)io >= lo && (uint64_t)io + 4u <= hi) { *ea = k_segs[i].ea + (io - k_segs[i].io); return 1; }
    }
    return 0;
}
static rsx_fifo_read_result io_read(void *user, uint32_t io, uint8_t out[4]) {
    io_port *p = user;
    if (p->reads < 64) p->log[p->reads] = io;
    ++p->reads;
    if (p->fail_at && p->reads == p->fail_at) return p->fail_as;
    uint32_t ea;
    if (!io_translate(io, &ea)) return RSX_FIFO_IO_UNMAPPED;
    ps3_guest_memory_status s = g_mem.read(g_mem.context, ea, out, 4);
    if (s == PS3_GUEST_MEMORY_OK) return RSX_FIFO_IO_OK;
    if (s == PS3_GUEST_MEMORY_UNMAPPED) return RSX_FIFO_IO_UNMAPPED;
    return RSX_FIFO_IO_FAULT;
}
static void wr(uint32_t io, uint32_t w) {
    uint32_t ea; CHECK(io_translate(io, &ea));
    uint8_t b[4] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
    CHECK(g_mem.write(g_mem.context, ea, b, 4) == PS3_GUEST_MEMORY_OK);
}

/* ---- recording sink ---- */
typedef struct tuple { uint32_t s, m, d, off; } tuple;
typedef struct rec {
    void *self;
    tuple t[512];
    unsigned n;
    unsigned fail_at;      /* 1-based call index to refuse, 0 = never */
    int fail_code;
    rsx_fifo_walker *reenter;   /* if set: try to re-enter this walker */
    rsx_fifo_status reenter_pump, reenter_put;
    int fail_effect;       /* -1: leave the walker's preset; else value written on refusal */
    int apply_then_fail;   /* refusal after the word's effect was applied (recorded) */
    rsx_fifo_pump_report reenter_rep;   /* report of the re-entrant pump (F3) */
    rsx_fifo_walker *reinit;            /* if set: call the public init on it from inside the sink (F1) */
    unsigned reinit_calls;
    rsx_fifo_status reinit_ok, reinit_bad;
} rec;
static int rec_method(void *user, uint32_t s, uint32_t m, uint32_t d, uint32_t off, rsx_fifo_effect *eff) {
    rec *r = user;
    CHECK(r->self == r);
    CHECK(eff && *eff == RSX_FIFO_EFFECT_UNKNOWN);   /* walker presets UNKNOWN */
    if (r->reenter) {
        memset(&r->reenter_rep, 0xA5, sizeof r->reenter_rep);
        r->reenter_pump = rsx_fifo_walker_pump(r->reenter, 100, NULL, &r->reenter_rep);
        r->reenter_put = rsx_fifo_walker_publish_put(r->reenter, 0x40);
    }
    if (r->reinit) {
        /* Valid arguments (a different start) and invalid ones: both must be refused while busy. */
        static io_port other;
        r->reinit_ok = rsx_fifo_walker_init(r->reinit, (rsx_fifo_reader){&other, io_read},
                                            (rsx_fifo_sink){r, rec_method}, 0x800);
        r->reinit_bad = rsx_fifo_walker_init(r->reinit, (rsx_fifo_reader){&other, NULL},
                                             (rsx_fifo_sink){r, rec_method}, 0x802);
        ++r->reinit_calls;
    }
    if (r->fail_at && r->n + 1 == r->fail_at) {
        if (r->apply_then_fail) { CHECK(r->n < 512); r->t[r->n++] = (tuple){s, m, d, off}; r->fail_at = 0; }
        if (r->fail_effect >= 0) *eff = (rsx_fifo_effect)r->fail_effect;
        return r->fail_code;
    }
    CHECK(r->n < 512);
    r->t[r->n++] = (tuple){s, m, d, off};
    return 0;
}

static void walker_new(rsx_fifo_walker *w, io_port *p, rec *r, uint32_t get) {
    memset(p, 0, sizeof *p); memset(r, 0, sizeof *r); r->self = r; r->fail_effect = -1;
    rsx_fifo_reader rd = {p, io_read};
    rsx_fifo_sink sk = {r, rec_method};
    CHECK(rsx_fifo_walker_init(w, rd, sk, get) == RSX_FIFO_REACHED_PUT);
}
static int same_tuples(const rec *a, const rec *b) {
    if (a->n != b->n) return 0;
    for (unsigned i = 0; i < a->n; ++i)
        if (a->t[i].s != b->t[i].s || a->t[i].m != b->t[i].m || a->t[i].d != b->t[i].d || a->t[i].off != b->t[i].off) return 0;
    return 1;
}

/* ---- reference branched stream ---- */
#define REF_END 0x138u
static void write_ref_stream(void) {
    wr(0x100, INC(0, 0x1A00, 3)); wr(0x104, 0x11223344u); wr(0x108, 0x55667788u); wr(0x10C, 0x99AABBCCu);
    wr(0x110, 0);                                  /* NOP header (count 0) */
    wr(0x114, NI(2, 0x0310, 2)); wr(0x118, 0xA1B2C3D4u); wr(0x11C, 0x01020304u);
    wr(0x120, CALL(0x00100000u));                  /* target numerically beyond PUT */
    wr(0x124, JMP(0x00100100u));
    wr(0x00100000u, INC(7, 0x0B80, 2)); wr(0x00100004u, 0xDEADBEEFu); wr(0x00100008u, 0xFEEDF00Du);
    wr(0x0010000Cu, RET);
    wr(0x00100100u, NI(1, 0x1D70, 1)); wr(0x00100104u, 0xCAFEBABEu);
    wr(0x00100108u, JMP(0x130));
    wr(0x130, INC(0, 0x0200, 1)); wr(0x134, 0x0BADF00Du);
}
/* Walk path order and whether the word there is payload (put there => NEED_INPUT). */
static const uint32_t k_path[] = {0x100,0x104,0x108,0x10C,0x110,0x114,0x118,0x11C,0x120,
    0x00100000u,0x00100004u,0x00100008u,0x0010000Cu,0x124,0x00100100u,0x00100104u,0x00100108u,
    0x130,0x134,REF_END};
static const int k_payload[] = {0,1,1,1,0,0,1,1,0, 0,1,1,0,0,0,1,0, 0,1,0};
#define PATH_N (sizeof k_path / sizeof k_path[0])
static const tuple k_ref[] = {
    {0,0x1A00,0x11223344u,0x104},{0,0x1A04,0x55667788u,0x108},{0,0x1A08,0x99AABBCCu,0x10C},
    {2,0x0310,0xA1B2C3D4u,0x118},{2,0x0310,0x01020304u,0x11C},
    {7,0x0B80,0xDEADBEEFu,0x00100004u},{7,0x0B84,0xFEEDF00Du,0x00100008u},
    {1,0x1D70,0xCAFEBABEu,0x00100104u},{0,0x0200,0x0BADF00Du,0x134}};
#define REF_N (sizeof k_ref / sizeof k_ref[0])

static void check_ref(const rec *r) {
    CHECK(r->n == REF_N);
    for (unsigned i = 0; i < REF_N; ++i)
        CHECK(r->t[i].s == k_ref[i].s && r->t[i].m == k_ref[i].m && r->t[i].d == k_ref[i].d && r->t[i].off == k_ref[i].off);
}

static void test_arguments(void) {
    rsx_fifo_walker w = {0}; io_port p = {0}; rec r = {0}; r.self = &r;
    rsx_fifo_reader rd = {&p, io_read}; rsx_fifo_sink sk = {&r, rec_method};
    rsx_fifo_reader bad_rd = {&p, NULL}; rsx_fifo_sink bad_sk = {&r, NULL};
    CHECK(rsx_fifo_walker_init(NULL, rd, sk, 0) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rsx_fifo_walker_init(&w, bad_rd, sk, 0) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rsx_fifo_walker_init(&w, rd, bad_sk, 0) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rsx_fifo_walker_init(&w, rd, sk, 2) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rsx_fifo_walker_pump(NULL, 1, NULL, NULL) == RSX_FIFO_INVALID_ARGUMENT);
    memset(&w, 0, sizeof w);   /* never initialised */
    CHECK(rsx_fifo_walker_pump(&w, 1, NULL, NULL) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rsx_fifo_walker_publish_put(&w, 0x10) == RSX_FIFO_INVALID_ARGUMENT);
    walker_new(&w, &p, &r, 0x100);
    CHECK(rsx_fifo_walker_publish_put(&w, 0x102) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(w.put == 0x100);
    rsx_fifo_pump_report rep;
    CHECK(rsx_fifo_walker_pump(&w, 5, NULL, &rep) == RSX_FIFO_REACHED_PUT);   /* nothing published */
    CHECK(rep.words_processed == 0 && p.reads == 0 && r.n == 0);
    CHECK(rsx_fifo_walker_publish_put(&w, REF_END) == RSX_FIFO_REACHED_PUT);
    CHECK(rsx_fifo_walker_pump(&w, 0, NULL, &rep) == RSX_FIFO_BUDGET_EXHAUSTED);
    CHECK(p.reads == 0 && r.n == 0 && w.get == 0x100);
    CHECK(!rsx_fifo_status_is_terminal(RSX_FIFO_PARKED) && rsx_fifo_status_is_terminal(RSX_FIFO_SINK_FAILED));
    CHECK(!rsx_fifo_status_is_terminal(RSX_FIFO_NEED_INPUT) && rsx_fifo_status_is_terminal(RSX_FIFO_INVALID_HEADER));
}

static void test_tuples_endian_increment(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r;
    write_ref_stream();
    walker_new(&w, &p, &r, 0x100);
    CHECK(rsx_fifo_walker_publish_put(&w, REF_END) == RSX_FIFO_REACHED_PUT);
    rsx_fifo_pump_report rep;
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, &rep) == RSX_FIFO_REACHED_PUT);
    check_ref(&r);
    CHECK(rep.get == REF_END && rep.put == REF_END && w.get == REF_END);
    CHECK(rep.words_processed == PATH_N - 1);
    CHECK(!w.in_call && !w.packet.active);
    CHECK(w.methods_committed == REF_N && w.words_committed == PATH_N - 1);
    /* idle pump: no reads */
    unsigned reads = p.reads;
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, &rep) == RSX_FIFO_REACHED_PUT);
    CHECK(p.reads == reads && rep.words_processed == 0);
}

/* Budget splits, cancel splits and partial publication at every boundary. */
static int cancel_every;
static unsigned cancel_polls;
static int cancel_fn(void *u) { (void)u; return (++cancel_polls % (unsigned)cancel_every) == 0; }

static void test_splits(void) {
    write_ref_stream();
    for (uint32_t budget = 1; budget <= PATH_N; ++budget) {
        rsx_fifo_walker w = {0}; io_port p; rec r;
        walker_new(&w, &p, &r, 0x100);
        rsx_fifo_walker_publish_put(&w, REF_END);
        rsx_fifo_status st; unsigned pumps = 0, total = 0;
        do {
            rsx_fifo_pump_report rep;
            st = rsx_fifo_walker_pump(&w, budget, NULL, &rep);
            CHECK(rep.words_processed <= budget);
            total += rep.words_processed;
            CHECK(st == RSX_FIFO_BUDGET_EXHAUSTED || st == RSX_FIFO_REACHED_PUT);
            CHECK(++pumps < 1000);
        } while (st != RSX_FIFO_REACHED_PUT);
        check_ref(&r);
        CHECK(total == PATH_N - 1);
    }
    for (cancel_every = 2; cancel_every <= 6; ++cancel_every) {
        rsx_fifo_walker w = {0}; io_port p; rec r;
        walker_new(&w, &p, &r, 0x100);
        rsx_fifo_walker_publish_put(&w, REF_END);
        rsx_fifo_cancel c = {NULL, cancel_fn};
        cancel_polls = 0;
        rsx_fifo_status st; unsigned pumps = 0, cancels = 0;
        do {
            st = rsx_fifo_walker_pump(&w, 1000, &c, NULL);
            CHECK(st == RSX_FIFO_CANCELLED || st == RSX_FIFO_REACHED_PUT);
            if (st == RSX_FIFO_CANCELLED) ++cancels;
            CHECK(++pumps < 1000);
        } while (st != RSX_FIFO_REACHED_PUT);
        check_ref(&r);
        CHECK(cancels > 0);
    }
    /* PUT published at every path position (including inside a packet and
     * inside the called region), then at the end. */
    for (size_t k = 0; k < PATH_N; ++k) {
        for (uint32_t budget = 1; budget <= 2; ++budget) {
            rsx_fifo_walker w = {0}; io_port p; rec r;
            walker_new(&w, &p, &r, 0x100);
            rsx_fifo_walker_publish_put(&w, k_path[k]);
            rsx_fifo_status st;
            do st = rsx_fifo_walker_pump(&w, budget, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
            CHECK(st == (k_payload[k] ? RSX_FIFO_NEED_INPUT : RSX_FIFO_REACHED_PUT));
            CHECK(w.get == k_path[k]);
            CHECK(w.packet.active == k_payload[k]);
            unsigned before = r.n;
            st = rsx_fifo_walker_pump(&w, 7, NULL, NULL);   /* nothing new published */
            CHECK(st == (k_payload[k] ? RSX_FIFO_NEED_INPUT : RSX_FIFO_REACHED_PUT) && r.n == before);
            rsx_fifo_walker_publish_put(&w, REF_END);
            do st = rsx_fifo_walker_pump(&w, budget, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
            CHECK(st == RSX_FIFO_REACHED_PUT);
            check_ref(&r);
        }
    }
}

/* Payload-as-header guard: payload words that are valid control words. */
static void test_payload_never_header(void) {
    wr(0x400, INC(0, 0x1F00, 4));
    wr(0x404, JMP(0x404)); wr(0x408, RET); wr(0x40C, CALL(0x00100000u)); wr(0x410, 0xFEAD0001u);
    wr(0x414, 0);
    for (uint32_t split = 0x400; split <= 0x414; split += 4) {
        for (uint32_t budget = 1; budget <= 3; ++budget) {
            rsx_fifo_walker w = {0}; io_port p; rec r;
            walker_new(&w, &p, &r, 0x400);
            rsx_fifo_walker_publish_put(&w, split);
            rsx_fifo_status st;
            do st = rsx_fifo_walker_pump(&w, budget, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
            CHECK(!rsx_fifo_status_is_terminal(st));
            rsx_fifo_walker_publish_put(&w, 0x418);
            do st = rsx_fifo_walker_pump(&w, budget, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
            CHECK(st == RSX_FIFO_REACHED_PUT);
            CHECK(r.n == 4);
            CHECK(r.t[0].m == 0x1F00 && r.t[0].d == JMP(0x404));
            CHECK(r.t[1].m == 0x1F04 && r.t[1].d == RET);
            CHECK(r.t[2].m == 0x1F08 && r.t[2].d == CALL(0x00100000u));
            CHECK(r.t[3].m == 0x1F0C && r.t[3].d == 0xFEAD0001u);
            CHECK(!w.in_call);
        }
    }
}

static void test_branches(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r; rsx_fifo_status st;
    /* Self-park, then patched park is followed. */
    wr(0x600, INC(0, 0x0310, 1)); wr(0x604, 0x10203040u); wr(0x608, JMP(0x608));
    walker_new(&w, &p, &r, 0x600);
    rsx_fifo_walker_publish_put(&w, 0x700);   /* producer's put beyond the park */
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_PARKED);
    CHECK(w.get == 0x608 && r.n == 1);
    unsigned reads = p.reads;
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_PARKED);
    CHECK(w.get == 0x608 && r.n == 1 && p.reads == reads + 1);
    wr(0x608, INC(0, 0x0314, 1)); wr(0x60C, 0x50607080u); wr(0x610, JMP(0x700));
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.n == 2 && r.t[1].m == 0x0314 && r.t[1].d == 0x50607080u && w.get == 0x700);
    /* JUMP whose target equals put stops there; JUMP backwards (ring wrap). */
    wr(0x800, JMP(0x200)); wr(0x200, INC(0, 0x0320, 1)); wr(0x204, 7); wr(0x208, JMP(0x900));
    walker_new(&w, &p, &r, 0x800);
    rsx_fifo_walker_publish_put(&w, 0x900);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.n == 1 && r.t[0].off == 0x204 && w.get == 0x900);
    /* Unmapped JUMP / CALL targets: terminal, cursor at the branch word. */
    const uint32_t bad_targets[] = {0x00F00000u, 0x00200000u /* uncommitted page */};
    for (int c = 0; c < 2; ++c) for (int t = 0; t < 2; ++t) {
        wr(0xA00, INC(0, 0x0310, 1)); wr(0xA04, 1);
        wr(0xA08, c ? CALL(bad_targets[t]) : JMP(bad_targets[t]));
        walker_new(&w, &p, &r, 0xA00);
        rsx_fifo_walker_publish_put(&w, 0xA10);
        st = rsx_fifo_walker_pump(&w, 100, NULL, NULL);
        CHECK(st == RSX_FIFO_UNMAPPED_BRANCH);
        const rsx_fifo_failure *f = rsx_fifo_walker_failure(&w);
        CHECK(f->status == st && f->offset == 0xA08 && f->detail == bad_targets[t]);
        CHECK(f->io_offset == bad_targets[t] && f->io_result == RSX_FIFO_IO_UNMAPPED && f->effect == RSX_FIFO_EFFECT_NONE);
        CHECK(f->word == (c ? CALL(bad_targets[t]) : JMP(bad_targets[t])));
        CHECK(w.get == 0xA08 && r.n == 1 && !w.in_call);
        reads = p.reads;
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == st && p.reads == reads && r.n == 1);
        CHECK(rsx_fifo_walker_publish_put(&w, 0xB00) == st);
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == st && p.reads == reads);
    }
    /* Branch target whose guest EA is outside the address space -> read fault. */
    wr(0xA20, JMP(0x00300000u));
    walker_new(&w, &p, &r, 0xA20); rsx_fifo_walker_publish_put(&w, 0xA30);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_BRANCH_READ_FAULT);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0xA20 && rsx_fifo_walker_failure(&w)->detail == 0x00300000u);
    CHECK(rsx_fifo_walker_failure(&w)->io_offset == 0x00300000u && rsx_fifo_walker_failure(&w)->io_result == RSX_FIFO_IO_FAULT);
    CHECK(rsx_fifo_walker_failure(&w)->effect == RSX_FIFO_EFFECT_NONE && w.get == 0xA20);
    /* Nested CALL refused. */
    wr(0xC00, CALL(0x00100200u)); wr(0x00100200u, INC(0, 0x0310, 1)); wr(0x00100204u, 5);
    wr(0x00100208u, CALL(0x00100300u)); wr(0x00100300u, RET);
    walker_new(&w, &p, &r, 0xC00); rsx_fifo_walker_publish_put(&w, 0xC04);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_CALL_DEPTH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x00100208u && w.get == 0x00100208u && w.in_call && r.n == 1);
    CHECK(w.return_offset == 0xC04);
    /* RET without CALL. */
    wr(0xC80, RET);
    walker_new(&w, &p, &r, 0xC80); rsx_fifo_walker_publish_put(&w, 0xC84);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_RET_WITHOUT_CALL);
    CHECK(w.get == 0xC80 && rsx_fifo_walker_failure(&w)->word == RET);
    /* PUT inside the called region beyond numeric PUT ordering: stop there,
     * call state retained, then resume and return. */
    wr(0xD00, CALL(0x00100400u)); wr(0xD04, INC(0, 0x0330, 1)); wr(0xD08, 9);
    wr(0x00100400u, INC(3, 0x0340, 2)); wr(0x00100404u, 1); wr(0x00100408u, 2); wr(0x0010040Cu, RET);
    walker_new(&w, &p, &r, 0xD00); rsx_fifo_walker_publish_put(&w, 0x00100408u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_NEED_INPUT);
    CHECK(w.in_call && w.packet.active && w.packet.remaining == 1 && w.packet.next_method == 0x0344);
    CHECK(w.packet.header_offset == 0x00100400u && w.packet.subchannel == 3 && w.packet.increment == 1 && w.packet.index == 1);
    rsx_fifo_walker_publish_put(&w, 0xD0C);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.n == 3 && r.t[1].m == 0x0344 && r.t[1].s == 3 && r.t[2].m == 0x0330 && !w.in_call);
}

/* Branch whose target equals the published PUT: the target is not executed before
 * a new PUT, so it is not probed (it may lie one past readable memory). */
static unsigned reads_of(const io_port *p, uint32_t off) {
    unsigned k = 0;
    for (unsigned i = 0; i < p->reads && i < 64; ++i) k += p->log[i] == off;
    return k;
}
static void test_branch_target_equals_put(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r;
    /* JUMP to put == one past the readable end of segment A. */
    wr(0xFFF0, INC(0, 0x0310, 1)); wr(0xFFF4, 0x31u); wr(0xFFF8, JMP(0x10000u));
    walker_new(&w, &p, &r, 0xFFF0); CHECK(rsx_fifo_walker_publish_put(&w, 0x10000u) == RSX_FIFO_REACHED_PUT);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(w.get == 0x10000u && r.n == 1 && reads_of(&p, 0x10000u) == 0 && !w.terminal);
    /* PUT then moves on: the target is fetched normally; unreadable -> fetch provenance. */
    CHECK(rsx_fifo_walker_publish_put(&w, 0x10010u) == RSX_FIFO_REACHED_PUT);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_FETCH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x10000u && rsx_fifo_walker_failure(&w)->io_offset == 0x10000u);
    /* Backward JUMP (numerically below get) to put: equality stop, no probe. */
    wr(0x800, JMP(0x200));
    walker_new(&w, &p, &r, 0x800); rsx_fifo_walker_publish_put(&w, 0x200);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT && w.get == 0x200 && reads_of(&p, 0x200) == 0);
    /* JUMP at the last IO word to put: needs no +4, allowed. */
    wr(0xFFFFFFFCu, JMP(0x10000u));
    walker_new(&w, &p, &r, 0xFFFFFFFCu); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT && w.get == 0x10000u && p.reads == 1);
    /* CALL to put == one past readable: call state committed, survives the PUT update. */
    wr(0x900, CALL(0x10000u)); wr(0x904, INC(0, 0x0320, 1)); wr(0x908, 0x32u);
    walker_new(&w, &p, &r, 0x900); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(w.get == 0x10000u && w.in_call && w.return_offset == 0x904 && p.reads == 1);
    /* CALL to a mapped put: stop unprobed, then the producer writes the subroutine and
     * publishes the post-return PUT; the call returns correctly. */
    wr(0x00100500u, 0xFFFFFFFFu);   /* garbage until published */
    wr(0xA00, CALL(0x00100500u)); wr(0xA04, INC(0, 0x0330, 1)); wr(0xA08, 0x33u);
    walker_new(&w, &p, &r, 0xA00); rsx_fifo_walker_publish_put(&w, 0x00100500u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT && reads_of(&p, 0x00100500u) == 0);
    wr(0x00100500u, INC(5, 0x0340, 1)); wr(0x00100504u, 0x34u); wr(0x00100508u, RET);
    rsx_fifo_walker_publish_put(&w, 0xA0C);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.n == 2 && r.t[0].s == 5 && r.t[0].d == 0x34u && r.t[1].m == 0x0330 && !w.in_call && w.get == 0xA0C);
    /* RET to put == one past readable (CALL at the last readable word of A). */
    wr(0xFFFC, CALL(0x00100600u)); wr(0x00100600u, INC(0, 0x0350, 1)); wr(0x00100604u, 0x35u); wr(0x00100608u, RET);
    walker_new(&w, &p, &r, 0xFFFC); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(w.get == 0x10000u && !w.in_call && r.n == 1 && reads_of(&p, 0x10000u) == 0);
    /* Malformed operands are still refused when the masked target equals put. */
    const uint32_t bad[] = {0x20000001u | 0x10000u, 0x20000003u | 0x10000u, 0x80000002u | 0x10000u,
                            0x40000002u | 0x10000u, 0x20000002u | 0x10000u};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        wr(0xB00, bad[i]);
        walker_new(&w, &p, &r, 0xB00); rsx_fifo_walker_publish_put(&w, 0x10000u);
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_INVALID_HEADER);
        CHECK(w.get == 0xB00 && !w.in_call && reads_of(&p, 0x10000u) == 0);
    }
    /* CALL at the last IO word: return offset would wrap -> overflow even if target == put. */
    wr(0xFFFFFFFCu, CALL(0x10000u));
    walker_new(&w, &p, &r, 0xFFFFFFFCu); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_OFFSET_OVERFLOW);
    CHECK(w.get == 0xFFFFFFFCu && !w.in_call && p.reads == 1);
    /* Nested CALL to put is still CALL_DEPTH. */
    wr(0xC00, CALL(0x00100700u)); wr(0x00100700u, CALL(0x10000u));
    walker_new(&w, &p, &r, 0xC00); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_CALL_DEPTH && w.get == 0x00100700u);
    /* Target != put stays probed (numerically beyond put too). */
    wr(0xD00, JMP(0x10004u));
    walker_new(&w, &p, &r, 0xD00); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_BRANCH);
}

static void test_invalid_and_memory(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r;
    struct { uint32_t word; rsx_fifo_status st; } cases[] = {
        {0x00000001u, RSX_FIFO_INVALID_HEADER}, {0x00000003u, RSX_FIFO_INVALID_HEADER},
        {0x00010000u | INC(0, 0x100, 1), RSX_FIFO_INVALID_HEADER},
        {0x80000000u | INC(0, 0x100, 1), RSX_FIFO_INVALID_HEADER},
        {0x60000000u, RSX_FIFO_INVALID_HEADER}, {0x20000001u, RSX_FIFO_INVALID_HEADER},
        {0x40000002u, RSX_FIFO_INVALID_HEADER}, {0xE0000000u, RSX_FIFO_INVALID_HEADER},
        {INC(0, 0x1FFC, 2), RSX_FIFO_INVALID_HEADER},      /* increment would carry into subch */
        {INC(0, 0x1F00, 0x7FF), RSX_FIFO_INVALID_HEADER},
        {0xFEAD0001u, RSX_FIFO_UNSUPPORTED_CONTROL}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        wr(0xE00, INC(0, 0x0310, 1)); wr(0xE04, 0x1234u); wr(0xE08, cases[i].word);
        walker_new(&w, &p, &r, 0xE00); rsx_fifo_walker_publish_put(&w, 0xE10);
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == cases[i].st);
        const rsx_fifo_failure *f = rsx_fifo_walker_failure(&w);
        CHECK(f->offset == 0xE08 && f->word == cases[i].word && w.get == 0xE08 && r.n == 1);
    }
    /* NI at the top method is valid (no increment). */
    wr(0xE00, NI(0, 0x1FFC, 3)); wr(0xE04, 1); wr(0xE08, 2); wr(0xE0C, 3);
    walker_new(&w, &p, &r, 0xE00); rsx_fifo_walker_publish_put(&w, 0xE10);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT && r.n == 3 && r.t[2].m == 0x1FFC);
    /* Header word itself unmapped (get on an unmapped IO offset). */
    walker_new(&w, &p, &r, 0x00F00000u); rsx_fifo_walker_publish_put(&w, 0x00F00010u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_FETCH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x00F00000u && !w.packet.active);
    CHECK(rsx_fifo_walker_failure(&w)->io_offset == 0x00F00000u && rsx_fifo_walker_failure(&w)->io_result == RSX_FIFO_IO_UNMAPPED);
    /* Payload runs off the end of a mapped segment: prefix kept, packet state reported. */
    wr(0xFFF8, INC(0, 0x0400, 3)); wr(0xFFFC, 0xAAAA0001u);
    walker_new(&w, &p, &r, 0xFFF8); rsx_fifo_walker_publish_put(&w, 0x10008u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_FETCH);
    const rsx_fifo_failure *f = rsx_fifo_walker_failure(&w);
    CHECK(f->offset == 0x10000u && r.n == 1 && r.t[0].d == 0xAAAA0001u);
    CHECK(f->packet.active && f->packet.remaining == 2 && f->packet.index == 1 && f->packet.next_method == 0x0404);
    CHECK(f->packet.header_offset == 0xFFF8 && f->packet.header == INC(0, 0x0400, 3));
    /* Real decommit of the guest page between pumps (memory contract UNMAPPED). */
    wr(0x00100800u, INC(0, 0x0500, 2)); wr(0x00100804u, 1);
    walker_new(&w, &p, &r, 0x00100800u); rsx_fifo_walker_publish_put(&w, 0x00100808u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_NEED_INPUT && r.n == 1);
    CHECK(g_mem.decommit(g_mem.context, 0x20000u, 0x1000u) == PS3_GUEST_MEMORY_OK);
    rsx_fifo_walker_publish_put(&w, 0x0010080Cu);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_FETCH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x00100808u && r.n == 1);
    CHECK(g_mem.commit(g_mem.context, 0x20000u, 0x1000u) == PS3_GUEST_MEMORY_OK);
    /* Read fault (EA outside host space). */
    walker_new(&w, &p, &r, 0x00300000u); rsx_fifo_walker_publish_put(&w, 0x00300008u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_READ_FAULT);
    CHECK(rsx_fifo_walker_failure(&w)->io_offset == 0x00300000u && rsx_fifo_walker_failure(&w)->io_result == RSX_FIFO_IO_FAULT);
    /* Offset overflow: payload word at 0xFFFFFFFC cannot commit. */
    wr(0xFFFFFFF8u, INC(0, 0x0600, 1)); wr(0xFFFFFFFCu, 0x77u);
    walker_new(&w, &p, &r, 0xFFFFFFF8u); rsx_fifo_walker_publish_put(&w, 0);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_OFFSET_OVERFLOW);
    CHECK(r.n == 0 && w.get == 0xFFFFFFFCu && w.packet.active);
}

/* Sink refusal and memory failure at every phase of the reference stream. */
static void test_failures_each_phase(void) {
    write_ref_stream();
    for (unsigned i = 1; i <= REF_N; ++i) {
        rsx_fifo_walker w = {0}; io_port p; rec r;
        walker_new(&w, &p, &r, 0x100);
        r.fail_at = i; r.fail_code = 0x5A + (int)i;
        /* Effect of the refused word is reported by the sink, never inferred from the cursor:
         * mode 0 leaves the preset (UNKNOWN), 1 NONE, 2 applied-then-failed PARTIAL, 3 out-of-range. */
        const int mode = (int)(i % 4u);
        r.fail_effect = mode == 0 ? -1 : mode == 1 ? RSX_FIFO_EFFECT_NONE : mode == 2 ? RSX_FIFO_EFFECT_PARTIAL : 77;
        r.apply_then_fail = mode == 2;
        rsx_fifo_walker_publish_put(&w, REF_END);
        rsx_fifo_status st;
        do st = rsx_fifo_walker_pump(&w, 3, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
        CHECK(st == RSX_FIFO_SINK_FAILED);
        const unsigned applied = i - 1 + (mode == 2);   /* mode 2: the failed word's effect happened */
        CHECK(r.n == applied);
        for (unsigned k = 0; k < r.n; ++k) CHECK(r.t[k].off == k_ref[k].off && r.t[k].d == k_ref[k].d);
        const rsx_fifo_failure *f = rsx_fifo_walker_failure(&w);
        CHECK(f->effect == (mode == 1 ? RSX_FIFO_EFFECT_NONE : mode == 2 ? RSX_FIFO_EFFECT_PARTIAL : RSX_FIFO_EFFECT_UNKNOWN));
        CHECK(f->io_result == 0);
        CHECK(f->offset == k_ref[i - 1].off && f->word == k_ref[i - 1].d && f->detail == (uint32_t)(0x5A + i));
        CHECK(w.get == k_ref[i - 1].off && f->packet.active && w.packet.next_method == k_ref[i - 1].m);
        CHECK(w.methods_committed == i - 1);
        unsigned reads = p.reads;
        r.fail_at = 0;   /* even if the sink would now accept, the walker stays failed */
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_SINK_FAILED);
        CHECK(rsx_fifo_walker_publish_put(&w, REF_END) == RSX_FIFO_SINK_FAILED);
        CHECK(r.n == applied && p.reads == reads);
    }
    /* Injected port failure at every read (fetches and branch probes). */
    rsx_fifo_walker w0 = {0}; io_port p0; rec r0;
    walker_new(&w0, &p0, &r0, 0x100); rsx_fifo_walker_publish_put(&w0, REF_END);
    CHECK(rsx_fifo_walker_pump(&w0, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    const unsigned total_reads = p0.reads;
    CHECK(total_reads == (PATH_N - 1) + 4);   /* + CALL, RET (r3, F5), JUMP, JUMP probes */
    CHECK(total_reads <= 64);
    unsigned probes = 0;
    for (unsigned n = 1; n < total_reads; ++n) probes += p0.log[n - 1] == p0.log[n];
    CHECK(probes == 4);
    CHECK(reads_of(&p0, 0x124) == 2);   /* RET return target: probe, then fetch */
    for (unsigned n = 1; n <= total_reads; ++n) for (int kind = 0; kind < 3; ++kind) {
        rsx_fifo_walker w = {0}; io_port p; rec r;
        walker_new(&w, &p, &r, 0x100);
        /* kind 2: out-of-range reader value -> treated as FAULT, raw value preserved. */
        p.fail_at = n; p.fail_as = kind == 2 ? (rsx_fifo_read_result)9 : kind ? RSX_FIFO_IO_FAULT : RSX_FIFO_IO_UNMAPPED;
        rsx_fifo_walker_publish_put(&w, REF_END);
        rsx_fifo_status st;
        do st = rsx_fifo_walker_pump(&w, 2, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
        CHECK(rsx_fifo_status_is_terminal(st));
        /* A probe read is the one immediately followed by a fetch of the same offset. */
        const int probe = n < total_reads && p0.log[n - 1] == p0.log[n];
        const rsx_fifo_failure *f = rsx_fifo_walker_failure(&w);
        if (kind) CHECK(st == (probe ? RSX_FIFO_BRANCH_READ_FAULT : RSX_FIFO_READ_FAULT));
        else CHECK(st == (probe ? RSX_FIFO_UNMAPPED_BRANCH : RSX_FIFO_UNMAPPED_FETCH));
        CHECK(f->io_offset == p0.log[n - 1] && f->io_result == (uint32_t)p.fail_as && f->effect == RSX_FIFO_EFFECT_NONE);
        if (!probe) CHECK(f->offset == p0.log[n - 1] && w.get == f->offset);
        /* RET probe failure is attributed to the RET word; nothing committed. */
        if (probe && p0.log[n - 1] == 0x124)
            CHECK(f->offset == 0x0010000Cu && f->word == RET && f->detail == 0x124 && w.get == 0x0010000Cu &&
                  w.in_call && w.return_offset == 0x124);
        CHECK(r.n <= REF_N);
        for (unsigned k = 0; k < r.n; ++k) CHECK(r.t[k].off == k_ref[k].off);
        unsigned seen = r.n, reads = p.reads;
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == st && r.n == seen && p.reads == reads);
    }
}

static void test_reentrancy(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r;
    write_ref_stream();
    walker_new(&w, &p, &r, 0x100);
    r.reenter = &w;
    rsx_fifo_walker_publish_put(&w, REF_END);
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.reenter_pump == RSX_FIFO_REENTRANT && r.reenter_put == RSX_FIFO_REENTRANT);
    CHECK(w.put == REF_END);
    check_ref(&r);
    /* F3: the re-entrant pump's report carries the returned status, not REACHED_PUT. */
    CHECK(r.reenter_rep.status == RSX_FIFO_REENTRANT && r.reenter_rep.words_processed == 0);
}

/* F1: the public init called from inside the walker's own sink is refused while busy;
 * cursor, pending packet, call state, put and latch are preserved; reset after the pump
 * and construction on fresh zero storage remain valid. */
static void test_reinit_while_busy(void) {
    write_ref_stream();
    rsx_fifo_walker w = {0}; io_port p; rec r;
    walker_new(&w, &p, &r, 0x100);
    r.reinit = &w;
    rsx_fifo_walker_publish_put(&w, REF_END);
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.reinit_calls == REF_N && r.reinit_ok == RSX_FIFO_REENTRANT && r.reinit_bad == RSX_FIFO_REENTRANT);
    check_ref(&r);
    CHECK(w.get == REF_END && w.put == REF_END && !w.terminal && !w.in_call && !w.packet.active);
    CHECK(w.reader.read_word == io_read && w.reader.user == &p && w.methods_committed == REF_N);
    /* Inside a packet and inside the called region: pending packet and call state survive. */
    walker_new(&w, &p, &r, 0x100);
    r.reinit = &w;
    rsx_fifo_walker_publish_put(&w, 0x00100008u);   /* stop after the first payload word of the call */
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_NEED_INPUT);
    CHECK(r.reinit_ok == RSX_FIFO_REENTRANT && r.reinit_bad == RSX_FIFO_REENTRANT);
    CHECK(w.get == 0x00100008u && w.in_call && w.return_offset == 0x124 && w.packet.active);
    CHECK(w.packet.remaining == 1 && w.packet.next_method == 0x0B84 && w.packet.subchannel == 7 && w.packet.index == 1);
    rsx_fifo_walker_publish_put(&w, REF_END);
    CHECK(rsx_fifo_walker_pump(&w, 1, NULL, NULL) == RSX_FIFO_BUDGET_EXHAUSTED);   /* resumes the pending word */
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    check_ref(&r);
    /* A refused re-init does not clear a latch: sink refuses after re-init attempt. */
    walker_new(&w, &p, &r, 0x100);
    r.reinit = &w; r.fail_at = 2; r.fail_code = 0x77;
    rsx_fifo_walker_publish_put(&w, REF_END);
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_SINK_FAILED);
    CHECK(r.reinit_ok == RSX_FIFO_REENTRANT && w.terminal && w.get == 0x108 && rsx_fifo_walker_failure(&w)->detail == 0x77);
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_SINK_FAILED && r.n == 1);
    /* Not busy: init is the reset (clears the latch), as before. */
    r.reinit = NULL; r.fail_at = 0;
    CHECK(rsx_fifo_walker_init(&w, (rsx_fifo_reader){&p, io_read}, (rsx_fifo_sink){&r, rec_method}, 0x100) == RSX_FIFO_REACHED_PUT);
    CHECK(!w.terminal && w.get == 0x100 && w.put == 0x100 && rsx_fifo_walker_failure(&w)->status == RSX_FIFO_REACHED_PUT);
    r.n = 0; rsx_fifo_walker_publish_put(&w, REF_END);
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    check_ref(&r);
    /* Fresh zero storage constructs normally. */
    rsx_fifo_walker fresh = {0}; io_port fp; rec fr;
    walker_new(&fresh, &fp, &fr, 0x100); rsx_fifo_walker_publish_put(&fresh, REF_END);
    CHECK(rsx_fifo_walker_pump(&fresh, 1000, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    check_ref(&fr);
}

/* F3: early-return statuses are mirrored in the report; budget 0 order. */
static void test_report_on_misuse(void) {
    rsx_fifo_pump_report rep;
    memset(&rep, 0xA5, sizeof rep);
    CHECK(rsx_fifo_walker_pump(NULL, 5, NULL, &rep) == RSX_FIFO_INVALID_ARGUMENT);
    CHECK(rep.status == RSX_FIFO_INVALID_ARGUMENT && rep.words_processed == 0 && rep.get == 0 && rep.put == 0);
    rsx_fifo_walker z = {0};
    memset(&rep, 0xA5, sizeof rep);
    CHECK(rsx_fifo_walker_pump(&z, 5, NULL, &rep) == RSX_FIFO_INVALID_ARGUMENT && rep.status == RSX_FIFO_INVALID_ARGUMENT);
    /* budget 0: get == put -> REACHED_PUT / NEED_INPUT; terminal -> latched; else BUDGET_EXHAUSTED. */
    write_ref_stream();
    rsx_fifo_walker w = {0}; io_port p; rec r;
    walker_new(&w, &p, &r, 0x100);
    CHECK(rsx_fifo_walker_pump(&w, 0, NULL, &rep) == RSX_FIFO_REACHED_PUT && rep.status == RSX_FIFO_REACHED_PUT);
    rsx_fifo_walker_publish_put(&w, 0x104);
    CHECK(rsx_fifo_walker_pump(&w, 1, NULL, NULL) == RSX_FIFO_NEED_INPUT);
    CHECK(rsx_fifo_walker_pump(&w, 0, NULL, &rep) == RSX_FIFO_NEED_INPUT && rep.status == RSX_FIFO_NEED_INPUT);
    rsx_fifo_walker_publish_put(&w, REF_END);
    unsigned reads = p.reads;
    CHECK(rsx_fifo_walker_pump(&w, 0, NULL, &rep) == RSX_FIFO_BUDGET_EXHAUSTED && p.reads == reads && r.n == 0);
    r.fail_at = 1; r.fail_code = 3;
    CHECK(rsx_fifo_walker_pump(&w, 1000, NULL, NULL) == RSX_FIFO_SINK_FAILED);
    CHECK(rsx_fifo_walker_pump(&w, 0, NULL, &rep) == RSX_FIFO_SINK_FAILED && rep.status == RSX_FIFO_SINK_FAILED);
}

/* F5: RET validates its return target like JUMP/CALL (target == PUT stays exempt). */
static void test_ret_target_probe(void) {
    rsx_fifo_walker w = {0}; io_port p; rec r; const rsx_fifo_failure *f;
    /* CALL in the last mapped word of A; the subroutine returns to 0x10000 (unmapped). */
    wr(0xFFFC, CALL(0x00100A00u)); wr(0x00100A00u, INC(0, 0x0350, 1)); wr(0x00100A04u, 0x35u); wr(0x00100A08u, RET);
    walker_new(&w, &p, &r, 0xFFFC); rsx_fifo_walker_publish_put(&w, 0x10010u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_BRANCH);
    f = rsx_fifo_walker_failure(&w);
    CHECK(f->offset == 0x00100A08u && f->word == RET && f->detail == 0x10000u && f->io_offset == 0x10000u);
    CHECK(f->io_result == RSX_FIFO_IO_UNMAPPED && f->effect == RSX_FIFO_EFFECT_NONE && !f->packet.active);
    CHECK(w.get == 0x00100A08u && w.in_call && w.return_offset == 0x10000u && r.n == 1 && reads_of(&p, 0x10000u) == 1);
    unsigned reads = p.reads;
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_BRANCH && p.reads == reads);
    /* Late PUT (1): stop on the RET word first, then publish beyond -> RET probes and fails there. */
    walker_new(&w, &p, &r, 0xFFFC); rsx_fifo_walker_publish_put(&w, 0x00100A08u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT && w.in_call);
    rsx_fifo_walker_publish_put(&w, 0x10010u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_BRANCH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x00100A08u && rsx_fifo_walker_failure(&w)->io_offset == 0x10000u);
    /* Late PUT (2): put == return target -> RET commits unprobed; a later PUT fetches it
     * normally, so the failure then has fetch provenance at the target. */
    walker_new(&w, &p, &r, 0xFFFC); rsx_fifo_walker_publish_put(&w, 0x10000u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(w.get == 0x10000u && !w.in_call && reads_of(&p, 0x10000u) == 0);
    rsx_fifo_walker_publish_put(&w, 0x10010u);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_UNMAPPED_FETCH);
    CHECK(rsx_fifo_walker_failure(&w)->offset == 0x10000u && rsx_fifo_walker_failure(&w)->io_offset == 0x10000u);
    /* FAULT and out-of-range reader values on the RET probe: BRANCH_READ_FAULT, raw value kept. */
    wr(0xA40, CALL(0x00100B00u)); wr(0xA44, INC(0, 0x0360, 1)); wr(0xA48, 0x36u);
    wr(0x00100B00u, INC(0, 0x0370, 1)); wr(0x00100B04u, 0x37u); wr(0x00100B08u, RET);
    for (int kind = 0; kind < 2; ++kind) {
        walker_new(&w, &p, &r, 0xA40); rsx_fifo_walker_publish_put(&w, 0xA4C);
        /* reads: fetch CALL, probe sub, fetch hdr, fetch payload, fetch RET, probe 0xA44 */
        p.fail_at = 6; p.fail_as = kind ? (rsx_fifo_read_result)9 : RSX_FIFO_IO_FAULT;
        CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_BRANCH_READ_FAULT);
        f = rsx_fifo_walker_failure(&w);
        CHECK(p.log[5] == 0xA44 && f->offset == 0x00100B08u && f->word == RET && f->io_offset == 0xA44);
        CHECK(f->io_result == (uint32_t)p.fail_as && w.get == 0x00100B08u && w.in_call && r.n == 1);
    }
    /* Mapped return target: probed, then fetched; stream completes. */
    walker_new(&w, &p, &r, 0xA40); rsx_fifo_walker_publish_put(&w, 0xA4C);
    CHECK(rsx_fifo_walker_pump(&w, 100, NULL, NULL) == RSX_FIFO_REACHED_PUT);
    CHECK(r.n == 2 && r.t[1].m == 0x0360 && reads_of(&p, 0xA44) == 2 && !w.in_call);
}

typedef struct owner { rsx_fifo_walker w; io_port p; rec r; uint32_t start, end; int ok; } owner;
static void *owner_thread(void *arg) {
    owner *o = arg;
    for (int iter = 0; iter < 200; ++iter) {
        walker_new(&o->w, &o->p, &o->r, o->start);
        rsx_fifo_walker_publish_put(&o->w, o->end);
        rsx_fifo_status st;
        do st = rsx_fifo_walker_pump(&o->w, 1, NULL, NULL); while (st == RSX_FIFO_BUDGET_EXHAUSTED);
        if (st != RSX_FIFO_REACHED_PUT || o->r.n != 16) return NULL;
        for (unsigned k = 0; k < 16; ++k)
            if (o->r.t[k].d != o->start * 16u + k || o->r.t[k].s != (o->start == 0x2000 ? 4u : 6u)) return NULL;
    }
    o->ok = 1;
    return NULL;
}
static void test_two_owners_concurrent(void) {
    static owner a, b;
    a.start = 0x2000; b.start = 0x3000;
    for (int o = 0; o < 2; ++o) {
        uint32_t s = o ? 0x3000 : 0x2000, sub = o ? 6 : 4;
        wr(s, NI(sub, 0x0310, 16));
        for (unsigned k = 0; k < 16; ++k) wr(s + 4 + 4 * k, s * 16u + k);
        (o ? &b : &a)->end = s + 4 + 64;
    }
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, owner_thread, &a) == 0);
    CHECK(pthread_create(&tb, NULL, owner_thread, &b) == 0);
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    CHECK(a.ok && b.ok);
    CHECK(same_tuples(&a.r, &a.r) && !same_tuples(&a.r, &b.r));
}

int main(void) {
    g_space = elf_test_host_space_create(1u << 20);
    CHECK(g_space);
    g_mem = elf_test_host_space_port(g_space);
    CHECK(g_mem.commit(g_mem.context, 0x10000u, 0x30000u) == PS3_GUEST_MEMORY_OK);   /* A, B, C; D stays uncommitted */
    test_arguments();
    test_tuples_endian_increment();
    test_splits();
    test_payload_never_header();
    test_branches();
    test_branch_target_equals_put();
    test_invalid_and_memory();
    test_failures_each_phase();
    test_reentrancy();
    test_reinit_while_busy();
    test_report_on_misuse();
    test_ret_target_probe();
    test_two_owners_concurrent();
    elf_test_host_space_destroy(g_space);
    puts("test_rsx_fifo_walker: PASS (host CPU semantics only)");
    return 0;
}
