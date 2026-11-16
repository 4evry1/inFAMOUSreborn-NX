/* Decision 33, phase B1a: Sony's real, lifted libsre.prx running inside the native runtime,
 * behind PS3_NATIVE_LLE_SRE.
 *
 * This is a HOST behaviour test, not game execution. It uses the real atomic domain, real coherent
 * guest memory, real lv2 objects, the real SPURS composition, the real ps3_hle_call and the real
 * lifted Sony code (built here from the user's own firmware by evidence/lle-sre-a1/lift_libsre.sh,
 * never copied into the tree).
 *
 * HLE/LLE equivalence is measured by building the SAME suite twice, from this one file:
 *   -DPS3_LLE_TEST_SELF_HLE  -- the suite runs with the flag absent, so ps3_hle_call serves the
 *                               cellSpurs NIDs from the native HLE arms (the accepted baseline);
 *   default                  -- the suite runs with PS3_NATIVE_LLE_SRE, so the same NIDs land in
 *                               the lifted Sony code before any HLE arm is reached.
 * The two binaries print measured result lines; verify_lle_sre.sh diffs them.
 */
#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main
/* The fixture suite keeps its own anonymous namespace open to the end of the file (its CHECK macro,
 * its `failures` counter and its Runtime are consulted by every lane), so this suite joins it rather
 * than opening a second one, and uses its own names. */
#include "../adapters/lle_sre.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>


#define LLE_CHECK(value) do { if (!(value)) { std::printf("LLE_SRE_FAIL line=%d expression=%s\n", __LINE__, #value); ++lle_failures; } } while (0)

constexpr uint32_t LLE_GAME_ONLY_CODE = 0x00102300;   // inside the game's own lifted range
void game_only_body(ppu_context *cpu);
constexpr uint32_t LLE_BASE = 0x30000000;
constexpr uint32_t LLE_TOC = 0x30039AB0;
constexpr uint32_t LLE_IMAGE_SIZE = 0x32340;
constexpr uint32_t LLE_EXTENT = 0x50000;
/* A NID outside every libsre library. 0x744680A2 is sysPrxForUser's _sys_initialize_tls, which the
 * game imports and this runtime serves from HLE; libsre does not re-export it. */
constexpr uint32_t FOREIGN_NID = 0x744680a2;
constexpr uint32_t FOREIGN_UNSERVED_NID = 0x11111111;   // exported by nothing
// The fixture's host space maps EA 1:1 and is at most 64 MiB, so the module window is declared at its
// real address inside a 64 MiB space: 0x30000000..0x3004FFFF is RAM, and so is the 0x7C0000..0x800000
// band the HLE arms and the public SPURS structures use.
// The caller's stack. The lifted body adjusts r1 down by its own frame size from whatever the caller
// passed and restores it on return, so the module needs no stack of its own: this is the frame the
// routed call writes into. The fixture puts its stack at 0xD00FF000, which its bounded host space
// reaches through a fold; here the same STACK-line geometry is placed inside the host space so the
// memory is real, ordinary RAM. LLE_STACK and LLE_TOC_SLOT are the values the calls actually use.
// The windows this suite declares: the module at its real base, the caller's RAM and the fixture's
// public SPURS structures. None overlaps another, and none overlaps 0x30000000..0x3004FFFF.
constexpr uint32_t LLE_STACK_BASE = 0xD00E0000;

constexpr uint32_t LLE_STACK_SPAN = 0x20000;
constexpr uint32_t LLE_STACK = 0xD00FF910;
constexpr uint32_t LLE_TOC_SLOT = LLE_STACK + 0x28;
constexpr uint32_t LLE_ATTR = 0xD00F0000;            // the attribute block, inside the caller's stack window
constexpr uint32_t LLE_RAM_BASE = 0x7CF000;
constexpr uint64_t LLE_RAM_SIZE = 0x3000;
constexpr size_t LLE_TRACE_CAPACITY = 1u << 21;   // Sony's real code touches far more memory per call than an HLE stub
constexpr uint32_t LLE_BLOCK = 0x7D1000;             // scratch inside the fixture's RAM window
constexpr uint32_t LLE_ATTR_SOURCE = 0xD00F0800;     // the SPURS attribute structure cellSpursInitialize takes
constexpr uint32_t LLE_INSTANCE = 0x7CF100;          // the CellSpurs structure it fills
constexpr uint32_t ATTRIBUTE_NID = 0x95180230;      // _cellSpursAttributeInitialize
constexpr uint32_t PREFIX_NID = 0x07529113;         // cellSpursAttributeSetNamePrefix
constexpr uint32_t TASKSET_ATTRIBUTE_NID = 0x16394a4e;  // _cellSpursTasksetAttributeInitialize
constexpr uint32_t TASKSET_NAME_NID = 0x652b70e2;   // cellSpursTasksetAttributeSetName
constexpr uint32_t INITIALIZE_NID = 0xacfc8dbc;     // cellSpursInitialize
constexpr uint32_t ATTRIBUTE_CODE = 0x300073C4;     // _cellSpursAttributeInitialize's lifted body
constexpr uint32_t PREFIX_CODE = 0x3000732C;        // cellSpursAttributeSetNamePrefix's lifted body
constexpr uint32_t ATTRIBUTE_SPAN = 0x200;          // 512-byte structure + its zero tail
constexpr uint32_t TASKSET_ATTRIBUTE_SPAN = 0x80;   // 128-byte structure
constexpr uint32_t TASKSET_NAME_SPAN = 0x20;        // 8-byte name field + guard bytes
constexpr uint64_t UNKNOWN_ARGUMENT = UINT64_C(0xDEADBEEF);
constexpr unsigned EXERCISED_EXPORTS = 12;          // how many exports case 2 calls through

int lle_failures;

#ifdef PS3_NATIVE_LLE_SRE
constexpr bool lle_route_enabled = true;
#else
constexpr bool lle_route_enabled = false;
#endif

/* Case 1 and case 3 need a whole guest space to themselves (the module takes 0x50000 out of the
 * RAM window, which the fixture's own 9 MiB space has no room for, and case 3 puts both routes side
 * by side). They get the raw pieces. */
/* The module lives at 0x30000000, which no bounded window around the fixture's low RAM can hold: the
 * guest space has to span it. This is a plain 4 GiB-capable byte+page-map adapter (two host arrays,
 * no fold), the same contract native/tests/guest_space_host.c implements at 64 MiB. Only the pages
 * the case actually touches are ever allocated, so the sparse 4 GiB address space costs little. */
struct LleHostSpace {
    std::vector<unsigned char *> pages{size_t(1) << 20};   // 4 GiB / 4 KiB page directories
    ~LleHostSpace()
    {
        for (unsigned char *page : pages) std::free(page);
    }
    static LleHostSpace *set(void *context) { return static_cast<LleHostSpace *>(context); }
    unsigned char *page(uint32_t address, bool create)
    {
        const size_t index = address / PS3_GUEST_PAGE_BYTES;
        if (!pages[index] && create) {
            pages[index] = static_cast<unsigned char *>(std::calloc(PS3_GUEST_PAGE_BYTES, 1));
        }
        return pages[index];
    }
    static bool span_ok(uint32_t address, uint64_t length)
    {
        return uint64_t(address) + length <= PS3_GUEST_ADDRESS_BYTES;
    }
    static ps3_guest_memory_status commit(void *context, uint32_t address, uint64_t length)
    {
        LleHostSpace &space = *set(context);
        if (!length || address % PS3_GUEST_PAGE_BYTES || length % PS3_GUEST_PAGE_BYTES || !span_ok(address, length))
            return PS3_GUEST_MEMORY_INVALID_RANGE;
        for (uint64_t offset = 0; offset < length; offset += PS3_GUEST_PAGE_BYTES)
            if (!space.page(uint32_t(address + offset), true)) return PS3_GUEST_MEMORY_NO_MEMORY;
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status all_or_nothing(void *context, uint32_t address, uint64_t length)
    {
        LleHostSpace &space = *set(context);
        if (!span_ok(address, length)) return PS3_GUEST_MEMORY_INVALID_RANGE;
        for (uint64_t offset = 0; offset < length; offset += PS3_GUEST_PAGE_BYTES)
            if (!space.page(uint32_t(address + offset), false)) return PS3_GUEST_MEMORY_UNMAPPED;
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status decommit(void *context, uint32_t address, uint64_t length)
    {
        if (!length || address % PS3_GUEST_PAGE_BYTES || length % PS3_GUEST_PAGE_BYTES || !span_ok(address, length))
            return PS3_GUEST_MEMORY_INVALID_RANGE;
        LleHostSpace &space = *set(context);
        for (uint64_t offset = 0; offset < length; offset += PS3_GUEST_PAGE_BYTES) {
            const size_t index = (address + offset) / PS3_GUEST_PAGE_BYTES;
            std::free(space.pages[index]);
            space.pages[index] = nullptr;
        }
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status read(void *context, uint32_t address, void *bytes, size_t length)
    {
        LleHostSpace &space = *set(context);
        if (!length) return PS3_GUEST_MEMORY_OK;
        if (all_or_nothing(context, address, length) != PS3_GUEST_MEMORY_OK) return PS3_GUEST_MEMORY_UNMAPPED;
        unsigned char *out = static_cast<unsigned char *>(bytes);
        for (size_t done = 0; done < length;) {
            const uint32_t here = uint32_t(address + done);
            const size_t in_page = PS3_GUEST_PAGE_BYTES - here % PS3_GUEST_PAGE_BYTES;
            const size_t step = length - done < in_page ? length - done : in_page;
            std::memcpy(out + done, space.page(here, false) + here % PS3_GUEST_PAGE_BYTES, step);
            done += step;
        }
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status write(void *context, uint32_t address, const void *bytes, size_t length)
    {
        LleHostSpace &space = *set(context);
        if (!length) return PS3_GUEST_MEMORY_OK;
        if (all_or_nothing(context, address, length) != PS3_GUEST_MEMORY_OK) return PS3_GUEST_MEMORY_UNMAPPED;
        const unsigned char *in = static_cast<const unsigned char *>(bytes);
        for (size_t done = 0; done < length;) {
            const uint32_t here = uint32_t(address + done);
            const size_t in_page = PS3_GUEST_PAGE_BYTES - here % PS3_GUEST_PAGE_BYTES;
            const size_t step = length - done < in_page ? length - done : in_page;
            std::memcpy(space.page(here, false) + here % PS3_GUEST_PAGE_BYTES, in + done, step);
            done += step;
        }
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status zero(void *context, uint32_t address, uint64_t length)
    {
        LleHostSpace &space = *set(context);
        if (!length) return PS3_GUEST_MEMORY_OK;
        if (all_or_nothing(context, address, length) != PS3_GUEST_MEMORY_OK) return PS3_GUEST_MEMORY_UNMAPPED;
        for (uint64_t done = 0; done < length;) {
            const uint32_t here = uint32_t(address + done);
            const size_t in_page = PS3_GUEST_PAGE_BYTES - here % PS3_GUEST_PAGE_BYTES;
            const size_t step = size_t(length - done < in_page ? length - done : in_page);
            std::memset(space.page(here, false) + here % PS3_GUEST_PAGE_BYTES, 0, step);
            done += step;
        }
        return PS3_GUEST_MEMORY_OK;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};

struct LleSpace {
    LleHostSpace space;
    ps3_guest_memory backing = space.port();
    // The module's own declaration comes from lle_sre_image_mapping(), exactly as a composition would
    // append it to its own RAM ranges.
    LleSreImageMapping module{};
    // The fixture's three fixed windows, all inside the 64 MiB host space the fixture uses for guest
    // memory (its stack lives high at 0xD00FF000, which is fine), plus the module's own window.
    LleSreImageMapping declared[4]{{LLE_RAM_BASE, LLE_RAM_SIZE, true}, {LLE_STACK_BASE, LLE_STACK_SPAN, true},
        {0x400000, 0x300000, true}, {PS3_LV2_SPU_ARENA_BASE, PS3_LV2_SPU_ARENA_SIZE, true}};
    /* The last two are filled in by the constructor: the module window, and the user-memory
     * allocator's window (0x400000..0x700000), which the SPU image import allocates its segs from. */
    PpuMemoryRamRange ranges[6]{{LLE_RAM_BASE, LLE_RAM_SIZE, true}, {LLE_STACK_BASE, LLE_STACK_SPAN, true},
        {}, {0x400000, 0x300000, true}, {}, {}};
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};
    ps3_atomic_pin_record pins[128]{};
    ps3_pthread_guard guard;
    Ps3PpuExecutionServices services{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(LLE_TRACE_CAPACITY);
    std::vector<PpuMemoryTraceEvent> journal = std::vector<PpuMemoryTraceEvent>(LLE_TRACE_CAPACITY);
    PpuMemoryTraceRecorder recorder;
    std::unique_ptr<PpuMemoryBinding> binding;
    LleSpace()
    {
        LLE_CHECK(lle_sre_image_mapping(declared, 4, &module));
        ranges[2] = PpuMemoryRamRange{module.base, module.length, module.writable};
        LLE_CHECK(ps3_pthread_guard_init(&guard) == 0);
        LLE_CHECK(ps3_atomic_domain_init(&domain, backing, ps3_pthread_guard_port(&guard), owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        ranges[4] = PpuMemoryRamRange{PS3_LV2_SPU_ARENA_BASE, PS3_LV2_SPU_ARENA_SIZE, true};
        binding.reset(new PpuMemoryBinding(domain, ranges, 5));
        const ps3_guest_memory_status c1 = ps3_atomic_commit(&domain, LLE_RAM_BASE, LLE_RAM_SIZE);
        const ps3_guest_memory_status c2 = ps3_atomic_commit(&domain, LLE_STACK_BASE, LLE_STACK_SPAN);
        const ps3_guest_memory_status c3 = ps3_atomic_commit(&domain, module.base, module.length);
        const ps3_guest_memory_status c4 = ps3_atomic_commit(&domain, PS3_LV2_SPU_ARENA_BASE, PS3_LV2_SPU_ARENA_SIZE);
        LLE_CHECK(c1 == PS3_GUEST_MEMORY_OK && c2 == PS3_GUEST_MEMORY_OK && c3 == PS3_GUEST_MEMORY_OK &&
                  c4 == PS3_GUEST_MEMORY_OK);
        services.memory = &binding->coherent_memory;
        LLE_CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Suffix, 0));
        static const LleSreMappingStatus admitted = lle_sre_mapping_admit();
        LLE_CHECK(admitted == LleSreMappingStatus::Mapped);
        LLE_CHECK(lle_sre_map_image(&binding->coherent_memory) == LleSreMappingStatus::Mapped);
    }
    ~LleSpace()
    {
        binding.reset();
        ps3_pthread_guard_destroy(&guard);
    }
    PpuMemoryBinding &binding_memory() const { return *binding; }
    bool admit_legacy(StartupExecution &execution)
    {
        execution.control->journal = PpuMemoryTraceJournal{journal.data(), journal.size(), 0};
        const bool admitted = native_startup_admit(execution, *binding, services);
        // Sony's own code makes far more indirect calls than one HLE stub: give the dispatch budget
        // depth room proportional to the module's 31 ps3_indirect_call sites.
        execution.dispatch.limit = 64;
        return admitted;
    }
    bool admit(StartupExecution &execution, PpuMemoryTraceRecorder &recorder_,
        DiscoveryBudgetBinding &budget_)
    {
        services.memory = &binding->coherent_memory;
        return native_startup_admit_budgeted(execution, *binding, services, recorder_, budget_);
    }
};

/* Every case after the mapping runs the REAL dispatcher over ONE LleSpace: the same atomic domain,
 * the same coherent memory, the same ps3_hle_call, the same HLE arms. What differs between the two
 * builds is only whether the route at the top of ps3_hle_call is compiled in. The fixture's own
 * Runtime is not reused because its host space is a bounded 9 MiB window and the module needs its
 * guest window at 0x30000000; the geometry, the RAM declarations and the CPU/TOC conventions below
 * are the fixture's (INSTANCE1/2, TOC, SDK). */
/* The user-memory service the SPU image import allocates through. File scope and one-at-a-time,
 * because ps3_lwmutex_* style callbacks receive only services.context. */
struct LleRuntime {
    LleSpace space;
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    ps3_user_memory memory{};
    ps3_user_memory_slot slots[4]{};
    ps3_user_memory_service *spu_allocation_service = nullptr;
    ps3_spu_resource_policy policy{};
    ConditionWait mutex_wait, cond_wait;
    ps3_lwmutex_registry mutexes{};
    ps3_lwmutex_object mutex_objects[4]{};
    ps3_lwmutex_waiter mutex_waiters[8]{};
    ps3_lwcond_registry conditions{};
    ps3_lwcond_object condition_objects[4]{};
    ps3_lwcond_waiter condition_waiters[4]{};
    uint32_t main_id = 1;
    ps3_pthread_guard g_dom, g_mem, g_lv2, g_ppu, g_pol, g_mutex, g_cond;
    ps3_pthread_wait_port port;
    std::unique_ptr<StartupSpursComposition> comp{new StartupSpursComposition};
    ps3_pthread_service_thread threads[PS3_SPURS_MAX_INSTANCES][3]{};
    std::vector<unsigned char> local_stores = std::vector<unsigned char>(size_t(PS3_SPURS_LS_SLOTS) * PS3_SPURS_K1_LS_BYTES);
    StartupSpursPorts ports;
    uint32_t attributes[2];
    static ps3_clock_status sample(void *, uint64_t *value) { *value = 1; return PS3_CLOCK_OK; }
    ps3_clock clock{nullptr, sample};
    infamous::discovery::DiscoveryBudget budget_policy{{1000000, 1000000, 1000000, UINT64_C(1000000000)}, 0};
    DiscoveryBudgetBinding budget{budget_policy, clock};
    bool composed = false;
    static ps3_compiled_code_status find_any(const void *, uint32_t, ps3_compiled_function *)
    {
        return PS3_COMPILED_CODE_NOT_FOUND;
    }
    static ps3_compiled_dispatch_stop_reason never_stop(void *) { return PS3_COMPILED_DISPATCH_CONTINUE; }
#ifdef PS3_NATIVE_LV2_SPU
    ps3_lv2_spu_images spu_images{};
    ps3_lv2_spu_groups spu_groups{};
    std::vector<unsigned char> spu_local_stores = std::vector<unsigned char>(
        size_t(PS3_LV2_SPU_GROUP_CAP) * PS3_LV2_SPU_THREADS_MAX * PS3_LV2_SPU_LS_BYTES);
    ps3_pthread_guard spu_image_guard, spu_group_guard;
    bool spu_ready = false;
#endif
#ifdef PS3_NATIVE_LV2_SYNC
    ps3_notify_cancel sync_cancel{0};
    static bool sleep_us(void *, uint64_t microseconds, bool cancelled)
    {
        if (cancelled) return false;
        const uint64_t capped = microseconds > 1000 ? 1000 : microseconds;   // the fixture never really waits long
        timespec wait{0, long(capped) * 1000};
        nanosleep(&wait, nullptr);
        return true;
    }
    StartupLv2SleepPort sleep_port{nullptr, sleep_us};
#endif
    const ps3_compiled_code_lookup lookup{this, find_any};
    const ps3_compiled_dispatch_stop_port dispatch_stop{this, never_stop};
    explicit LleRuntime(uint32_t attribute_block) : attributes{attribute_block, attribute_block + 0x400}
    {
        LLE_CHECK(space.recorder.configure(space.events.data(), space.events.size(),
            PpuMemoryTraceRetention::Suffix, 0));
        LLE_CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) &&
                  !ps3_pthread_guard_init(&g_ppu) && !ps3_pthread_guard_init(&g_pol) && !ps3_pthread_guard_init(&g_mutex) &&
                  !ps3_pthread_guard_init(&g_cond) && !ps3_pthread_wait_port_init(&port));
        LLE_CHECK(ps3_user_memory_init(&memory, &user_config, slots, 2) == PS3_USER_MEMORY_OK);
        LLE_CHECK(ps3_spu_resource_policy_init(&policy, ps3_pthread_guard_port(&g_pol)) == PS3_SPU_POLICY_OK);
        LLE_CHECK(ps3_lwmutex_init_with_conditions(&mutexes, &space.domain, ps3_pthread_guard_port(&g_mutex),
            mutex_wait.port(), mutex_objects, 4, mutex_waiters, 8, 4).status == PS3_LWMUTEX_OK);
        LLE_CHECK(ps3_lwcond_init(&conditions, &mutexes, ps3_pthread_guard_port(&g_cond), cond_wait.port(),
            {&main_id, main_exists}, condition_objects, 4, condition_waiters, 4).status == PS3_LWCOND_OK);
        execution.control = control.get(); execution.conditions = &conditions; execution.spu_resources = &policy;
        execution.guest_priority = 250; execution.has_guest_priority = true;
        execution.ppu.cpu.thread_id = 1; execution.ppu.cpu.gpr[1] = LLE_STACK; execution.ppu.cpu.gpr[2] = TOC;
        execution.ppu.cpu.lr = 0x2cfe70;
        execution.process_sdk = {SDK, true};
        execution.ppu.memory = &space.binding_memory();
        execution.ppu.services = &space.services;
        execution.dispatch = ps3_compiled_dispatch_state{0, 8};
        space.services.compiled_code = &lookup;
        execution.dispatch_stop = dispatch_stop;
        /* The legacy journal, not the recorder: Sony's own code reaches ps3_hle_call from inside a
         * call that already holds a trace batch, and the recorder admits exactly one open batch at a
         * time. The journal is a plain bounded array with no batch state, so nesting is simply more
         * entries -- which is what this suite wants to observe anyway. */
#ifdef PS3_NATIVE_LV2_SPU
        wire_spu();   // the SPU registries must exist before admission, for the lv2 arms too
#endif
        LLE_CHECK(space.admit_legacy(execution));
        for (unsigned index = 0; index < PS3_SPURS_LS_SLOTS; ++index)
            ports.local_stores[index] = &local_stores[size_t(index) * PS3_SPURS_K1_LS_BYTES];
        ports.notify = ps3_pthread_wait_port_port(&port);
        ports.lv2_guard = ps3_pthread_guard_port(&g_lv2); ports.ppu_guard = ps3_pthread_guard_port(&g_ppu);
        for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index) for (unsigned slot = 0; slot < 3; ++slot)
            ports.threads[index][slot] = ps3_pthread_service_thread_port(&threads[index][slot]);
        composed = startup_spurs_compose(execution, *comp, space.domain, memory, policy, ports) == StartupSpursComposeStatus::Ok;
        LLE_CHECK(composed && execution.spurs == &comp->runtime);
    }
    ~LleRuntime()
    {
        if (composed) LLE_CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        ps3_lwcond_report report{};
        ps3_lwcond_cleanup(&conditions, &report); ps3_lwmutex_cleanup(&mutexes);
        ps3_user_memory_cleanup(&memory);
        ps3_pthread_wait_port_destroy(&port);
        ps3_pthread_guard_destroy(&g_dom); ps3_pthread_guard_destroy(&g_mem); ps3_pthread_guard_destroy(&g_lv2);
        ps3_pthread_guard_destroy(&g_ppu); ps3_pthread_guard_destroy(&g_pol); ps3_pthread_guard_destroy(&g_mutex);
        ps3_pthread_guard_destroy(&g_cond);
    }
    ps3_user_memory_config user_config{WINDOW, WINDOW_END, QUOTA, ps3_atomic_domain_memory(&space.domain),
        ps3_pthread_guard_port(&g_mem)};
    ps3_guest_memory mem() const { return space.binding->coherent_memory; }
    uint64_t get(uint32_t address, size_t length)
    {
        unsigned char bytes[8]{};
        LLE_CHECK(length <= sizeof(bytes) && mem().read(mem().context, address, bytes, length) == PS3_GUEST_MEMORY_OK);
        uint64_t value = 0;
        for (size_t index = 0; index < length; ++index) value = value << 8 | bytes[index];
        return value;
    }
    ppu_context &cpu() { return execution.ppu.cpu; }
    /* Re-admit through the RECORDER path production uses, releasing the legacy one this runtime was
     * constructed with. The guest RAM stays committed and the module stays mapped. */
#ifdef PS3_NATIVE_LV2_SPU
    /* Both admission paths need the SPU registries: the constructor path (cases 5, 7, 11) and the
     * recorder re-admission (cases 10, 12). Idempotent. */
    void wire_spu()
    {
        if (!spu_ready) {
            spu_ready = ps3_pthread_guard_init(&spu_image_guard) == 0 && ps3_pthread_guard_init(&spu_group_guard) == 0 &&
                ps3_lv2_spu_images_init(&spu_images, ps3_pthread_guard_port(&spu_image_guard)) == PS3_LV2_SPU_OK &&
                ps3_lv2_spu_groups_init(&spu_groups, ps3_pthread_guard_port(&spu_group_guard),
                    spu_local_stores.data(), 6) == PS3_LV2_SPU_OK;
            LLE_CHECK(spu_ready);
        }
        execution.spu_images = &spu_images;
        execution.spu_groups = &spu_groups;
    }
#endif
    bool readmit_budgeted()
    {
        // The per-call bookkeeping arrays are per IMPORT CALL, and this admission starts a fresh run.
        control->import_count = 0;
        control->dispatch_count = 0;
        execution.ppu.admitted = false;
        execution.ppu.trace = nullptr;
        execution.ppu.recorder = nullptr;
        execution.ppu.budget = nullptr;
        execution.ppu.cpu = ppu_context{};
        execution.ppu.cpu.thread_id = 1;
        execution.ppu.cpu.gpr[1] = LLE_STACK;
        execution.ppu.cpu.gpr[2] = TOC;
        execution.ppu.cpu.lr = 0x2cfe70;
        execution.dispatch = ps3_compiled_dispatch_state{0, 64};
        execution.dispatch_stop = dispatch_stop;
#ifdef PS3_NATIVE_LV2_SYNC
        // B2a: the lv2 sync arms block on the shared cancel token and sleep through the port.
        execution.lv2_sync_cancel = &sync_cancel;
        execution.lv2_sleep = &sleep_port;
#endif
#ifdef PS3_NATIVE_LV2_SPU
        wire_spu();
#endif
        space.services.memory = &space.binding->coherent_memory;
        space.services.compiled_code = &lookup;
        if (!native_startup_admit_budgeted(execution, *space.binding, space.services,
                space.recorder, budget)) return false;
        execution.dispatch.limit = 64;
        return true;
    }
    void run(uint32_t nid_or_sc, bool syscall)
    {
        active = control.get(); control->cpu = &cpu();
        control->stop = StartupStop::None;
        try { if (syscall) { cpu().gpr[11] = nid_or_sc; lv2_syscall(&cpu()); } else ps3_hle_call(nid_or_sc, &cpu()); active = nullptr; }
        catch (const Terminal &) {}
    }
};

/* One routed-or-HLE call with the fixture's calling convention: r1 = LLE_STACK, r2 = TOC, the
 * TOC-restore slot primed, r3..r10 = arguments, extra registers filled with a recognisable
 * constant so a callee that reads one is visible. */
void call_import(LleRuntime &runtime, uint32_t nid, const uint64_t (&arguments)[8])
{
    auto &cpu = runtime.cpu();
    // The per-call bookkeeping arrays are the fixture's bounded records of ONE import call; a lane
    // that makes many calls must start each one with fresh credit, exactly as the fixture's own lanes do.
    runtime.control->import_count = 0;
    runtime.control->dispatch_count = 0;
    const unsigned char slot[8] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_TOC_SLOT, slot, 8) == PS3_GUEST_MEMORY_OK);
    cpu.gpr[1] = LLE_STACK;
    cpu.gpr[2] = TOC;
    for (unsigned index = 0; index < 8; ++index) cpu.gpr[3 + index] = arguments[index];
    runtime.run(nid, false);
}

std::vector<unsigned char> read_bytes(LleRuntime &runtime, uint32_t address, size_t length)
{
    std::vector<unsigned char> bytes(length);
    LLE_CHECK(runtime.mem().read(runtime.mem().context, address, bytes.data(), length) == PS3_GUEST_MEMORY_OK);
    return bytes;
}

void fill_bytes(LleRuntime &runtime, uint32_t address, unsigned char value, size_t length)
{
    std::vector<unsigned char> bytes(length, value);
    LLE_CHECK(runtime.mem().write(runtime.mem().context, address, bytes.data(), length) == PS3_GUEST_MEMORY_OK);
}

void print_bytes(const char *label, const std::vector<unsigned char> &bytes)
{
    std::printf("LLE_SRE_BYTES %s", label);
    for (size_t index = 0; index < bytes.size(); ++index) std::printf(" %02x", bytes[index]);
    std::printf("\n");
}

/* ---------- case 1: mapping ---------- */

void expect_image_bytes(LleRuntime &runtime, const char *when)
{
    const LibsreModuleDescription &module = libsre_module_description;
    const std::vector<unsigned char> mapped = read_bytes(runtime, LLE_BASE, module.image_size);
    bool equal = mapped.size() == size_t(module.image_size);
    for (uint32_t index = 0; equal && index < module.image_size; ++index)
        equal = mapped[index] == module.image[index];
    if (!equal) {
        for (uint32_t index = 0; index < module.image_size; ++index)
            if (mapped[index] != module.image[index]) {
                std::printf("LLE_SRE_IMAGE_DIFF %s offset=0x%05x image=%02x guest=%02x\n", when,
                    index, module.image[index], mapped[index]);
                break;
            }
    }
    LLE_CHECK(equal);
    bool zero_tail = true;
    for (uint32_t index = module.image_size; zero_tail && index < module.mapped_extent; ++index)
        zero_tail = runtime.get(LLE_BASE + index, 1) == 0;
    LLE_CHECK(zero_tail);
}

void case_mapping()
{
    LleRuntime runtime(LLE_ATTR);
    LLE_CHECK(lle_sre_base() == LLE_BASE);
    LLE_CHECK(lle_sre_toc() == LLE_TOC);
    LLE_CHECK(lle_sre_mapped_extent() == LLE_EXTENT);
    LLE_CHECK(libsre_module_description.image_size == LLE_IMAGE_SIZE);
    expect_image_bytes(runtime, "mapped");
    /* The composition's own declarations must reject the module when they would overlap it, and
     * accept it when they do not. */
    const LleSreImageMapping overlapping[2] = {{0x7CF000, 0x3000, true}, {0x2FFF0000, 0x20000, true}};
    LleSreImageMapping mapping{};
    LLE_CHECK(!lle_sre_image_mapping(overlapping, 2, &mapping));
    const LleSreImageMapping disjoint[2] = {{0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}};
    LLE_CHECK(lle_sre_image_mapping(disjoint, 2, &mapping));
    LLE_CHECK(mapping.base == LLE_BASE && mapping.length == LLE_EXTENT && mapping.writable);
    LLE_CHECK(lle_sre_image_mapping(nullptr, 0, &mapping) && mapping.length == LLE_EXTENT);
    LLE_CHECK(lle_sre_mapping_check() == LleSreMappingStatus::Mapped);
    /* Every one of the 267 exported OPDs is inside the mapped extent, and its code is a lifted body. */
    size_t opds_seen = 0;
    for (size_t index = 0; index < libsre_module_description.export_count; ++index) {
        const LibsreExportEntry &entry = libsre_module_description.exports[index];
        LLE_CHECK(entry.opd >= LLE_BASE && uint64_t(entry.opd) - LLE_BASE + 8 <= LLE_IMAGE_SIZE);
        const uint32_t code = uint32_t(runtime.get(entry.opd, 4));
        const uint32_t toc = uint32_t(runtime.get(entry.opd + 4, 4));
        LLE_CHECK(toc == LLE_TOC && code >= LLE_BASE && code < LLE_BASE + LLE_IMAGE_SIZE);
        ++opds_seen;
    }
    std::printf("LLE_SRE_MAPPING extent=0x%x image=%u opds=%zu\n", LLE_EXTENT, LLE_IMAGE_SIZE, opds_seen);
    LLE_CHECK(opds_seen == libsre_module_description.export_count);
}

/* ---------- case 2: routing ---------- */

/* The nine imports Sony's library takes from sysPrxForUser (exactly what the fixture path reaches). */
const uint32_t kLiftedImports[] = {0x68b9b011u, 0x6bf66ea7u, 0xfb5db080u, 0x2d36462bu,
    0x99c88692u, 0xd3039d4du, 0x052d29a6u, 0x996f7cf8u, 0x04e83d2cu};

void case_routing()
{
    std::printf("LLE_SRE_ROUTE_MODE %s\n", lle_route_enabled ? "lle" : "hle");
    LLE_CHECK(lle_sre_mapping_admitted());
    size_t resolvable = 0;
    for (size_t index = 0; index < libsre_module_description.export_count; ++index) {
        const LibsreExportEntry &entry = libsre_module_description.exports[index];
        uint32_t opd = 0;
        if (lle_sre_export_opd(entry.nid, &opd) && opd == entry.opd) ++resolvable;
    }
    LLE_CHECK(resolvable == libsre_module_description.export_count);
    LLE_CHECK(libsre_module_description.export_count == 267u);
    std::printf("LLE_SRE_EXPORTS resolved=%zu of %zu\n", resolvable,
        libsre_module_description.export_count);
    /* A NID no libsre library exports stays out of the route entirely, in both builds. */
    LLE_CHECK(!lle_sre_exports_nid(FOREIGN_NID));
    LLE_CHECK(!lle_sre_exports_nid(FOREIGN_UNSERVED_NID));
    LLE_CHECK(!lle_sre_exports_nid(0x00000000));
    LLE_CHECK(!lle_sre_exports_nid(0xFFFFFFFF));
    /* The module's export table is the same in both builds -- what the FLAG decides is whether the
     * dispatcher consults it. That difference is measured below, through the live ps3_hle_call. */
    LLE_CHECK(lle_sre_exports_nid(ATTRIBUTE_NID));
    LLE_CHECK(lle_sre_exports_nid(INITIALIZE_NID));

    /* A NID outside every libsre library still reaches its own arm: an unregistered NID stops at the
     * accepted UnknownImport in BOTH builds, so the route never swallows a foreign NID. */
    {
        LleRuntime runtime(LLE_ATTR);
        const uint64_t arguments[8] = {UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT, 0, 0, 0, 0, 0, 0};
        call_import(runtime, FOREIGN_UNSERVED_NID, arguments);
        LLE_CHECK(runtime.control->stop == StartupStop::UnknownImport);
        LLE_CHECK(runtime.control->lle_sre_routed == 0);
        std::printf("LLE_SRE_FOREIGN_NID stop=%d routed=%zu\n", int(runtime.control->stop),
            runtime.control->lle_sre_routed);
    }
    /* The cellSpurs NIDs themselves: with the flag on they never reach an HLE arm (the route counts
     * them and returns into Sony's code); with the flag off they reach the accepted HLE arms and stop
     * exactly where they stopped before this change. */
    for (size_t index = 0; index < sizeof(kLiftedImports) / sizeof(kLiftedImports[0]); ++index) {
        LleRuntime runtime(LLE_ATTR);
        const uint64_t arguments[8] = {UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT,
            UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT, UNKNOWN_ARGUMENT};
        call_import(runtime, kLiftedImports[index], arguments);
        if (lle_route_enabled) {
            /* The lifted import stub itself ran (ps3_hle_call re-entered from Sony's code), so the
             * libc arm served it and no HLE arm for this NID was reached. */
            LLE_CHECK(runtime.control->lle_sre_libc_returns == 1 || runtime.control->stop == StartupStop::InvalidMemory);
        } else {
            LLE_CHECK(runtime.control->lle_sre_libc_returns == 0);
            LLE_CHECK(runtime.control->stop == StartupStop::UnknownImport);
        }
        std::printf("LLE_SRE_IMPORT nid=0x%08x stop=%d libc=%zu\n", kLiftedImports[index],
            int(runtime.control->stop), runtime.control->lle_sre_libc_returns);
    }
    /* With the flag off, the four cellSpurs NIDs land on the accepted HLE arms: the attribute
     * writers succeed exactly as they always did. */
    if (!lle_route_enabled) {
        LleRuntime runtime(LLE_ATTR);
        const uint64_t arguments[8] = {runtime.attributes[0], 2, SDK, 5, 128, 249, 0, 0};
        call_import(runtime, ATTRIBUTE_NID, arguments);
        LLE_CHECK(runtime.control->stop == StartupStop::None && runtime.cpu().gpr[3] == 0);
        LLE_CHECK(runtime.control->spurs_attribute_returns == 1);
    }
}

/* ---------- case 3: equivalence with the accepted HLE ---------- */

void run_attribute_initialize(LleRuntime &runtime)
{
    const uint64_t arguments[8] = {runtime.attributes[0], 5, SDK, 128, 249, 0, 0, 0};
    call_import(runtime, ATTRIBUTE_NID, arguments);
    print_bytes("attribute_initialize", read_bytes(runtime, runtime.attributes[0], ATTRIBUTE_SPAN));
    std::printf("LLE_SRE_RESULT attribute_initialize stop=%d r3=0x%016llx\n",
        int(runtime.control->stop), (unsigned long long)runtime.cpu().gpr[3]);
}

void run_attribute_prefix(LleRuntime &runtime)
{
    fill_bytes(runtime, LLE_BLOCK, 0x5A, 0x20);
    const unsigned char name[6] = {'H', 'E', 'L', 'L', 'O', 0};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_BLOCK, name, sizeof(name)) == PS3_GUEST_MEMORY_OK);
    const uint64_t arguments[8] = {runtime.attributes[0], LLE_BLOCK, 5, 0, 0, 0, 0, 0};
    call_import(runtime, PREFIX_NID, arguments);
    print_bytes("attribute_prefix", read_bytes(runtime, runtime.attributes[0], 0x80));
    std::printf("LLE_SRE_RESULT attribute_prefix stop=%d r3=0x%016llx\n",
        int(runtime.control->stop), (unsigned long long)runtime.cpu().gpr[3]);
}

/* The one measured difference between the two routes, recorded rather than smoothed over:
 *   LLE (libsre_func_30014408, disassembly): r3 = 0x80410000 | 0x910 = 0x80410910 before it validates
 *     the structure, so 0x80410910 is Sony's own early-return code (CELL_SPURS_TASKSET_ERROR_ALIGN)
 *     and no guest byte is written after the stop.
 *   HLE (native/domain/spurs_taskset_init.c): the arm stops InvalidMemory before writing a result, so
 *     r3 still holds the caller's attribute pointer -- a pre-stop artefact, not an answer.
 * Sony's side is the authoritative one; the HLE arm is simply refusing a case it does not model. */
void run_taskset_attribute_initialize(LleRuntime &runtime)
{
    const uint64_t arguments[8] = {runtime.attributes[1], 5, 0x400, 3, 0x2000, 0, 0, 0};
    call_import(runtime, TASKSET_ATTRIBUTE_NID, arguments);
    print_bytes("taskset_attribute_initialize", read_bytes(runtime, runtime.attributes[1], TASKSET_ATTRIBUTE_SPAN));
    std::printf("LLE_SRE_RESULT taskset_attribute_initialize stop=%d\n", int(runtime.control->stop));
}

void run_taskset_attribute_name(LleRuntime &runtime)
{
    const unsigned char name[5] = {'n', 'a', 'm', 'e', 0};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_BLOCK, name, sizeof(name)) == PS3_GUEST_MEMORY_OK);
    fill_bytes(runtime, runtime.attributes[1], 0xC3, TASKSET_NAME_SPAN);
    const uint64_t arguments[8] = {runtime.attributes[1], LLE_BLOCK, 0, 0, 0, 0, 0, 0};
    call_import(runtime, TASKSET_NAME_NID, arguments);
    print_bytes("taskset_attribute_name", read_bytes(runtime, runtime.attributes[1], TASKSET_NAME_SPAN));
    std::printf("LLE_SRE_RESULT taskset_attribute_name stop=%d r3=0x%016llx\n",
        int(runtime.control->stop), (unsigned long long)runtime.cpu().gpr[3]);
}

void case_equivalence()
{
    { LleRuntime runtime(LLE_ATTR); run_attribute_initialize(runtime); }
    { LleRuntime runtime(LLE_ATTR); run_attribute_prefix(runtime); }
    { LleRuntime runtime(LLE_ATTR); run_taskset_attribute_initialize(runtime); }
    { LleRuntime runtime(LLE_ATTR); run_taskset_attribute_name(runtime); }
}

/* ---------- case 4: the nine libc imports ---------- */

/* Each of the nine, through ps3_hle_call with the exact C contract, on guest memory the fixture
 * owns. The expected value is the C function's, written out here, never read back from the
 * implementation. */
void case_libc()
{
    /* The nine libc services are part of the LLE route: with the flag off those NIDs reach the
     * accepted UnknownImport arms instead, which the equivalence build measures as such. */
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_LIBC_SKIPPED route=hle\n");
        return;
    }
    const uint32_t block = LLE_BLOCK;
    {
        LleRuntime runtime(LLE_ATTR);
        fill_bytes(runtime, block, 0x11, 0x40);
        const uint64_t arguments[8] = {block, 'Z', 16, 0, 0, 0, 0, 0};
        const size_t before = runtime.control->lle_sre_libc_returns;
        call_import(runtime, 0x68b9b011, arguments);   // _sys_memset
        const std::vector<unsigned char> bytes = read_bytes(runtime, block, 0x20);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block);
        LLE_CHECK(runtime.control->lle_sre_libc_returns == before + 1);
        for (unsigned index = 0; index < 16; ++index) LLE_CHECK(bytes[index] == 'Z');
        for (unsigned index = 16; index < 0x20; ++index) LLE_CHECK(bytes[index] == 0x11);
        std::printf("LLE_SRE_LIBC memset stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block + 0x100, source, 8) == PS3_GUEST_MEMORY_OK);
        fill_bytes(runtime, block, 0x00, 0x20);
        const uint64_t arguments[8] = {block, block + 0x100, 8, 0, 0, 0, 0, 0};
        call_import(runtime, 0x6bf66ea7, arguments);   // _sys_memcpy
        const std::vector<unsigned char> bytes = read_bytes(runtime, block, 0x10);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block);
        for (unsigned index = 0; index < 8; ++index) LLE_CHECK(bytes[index] == source[index]);
        for (unsigned index = 8; index < 0x10; ++index) LLE_CHECK(bytes[index] == 0);
        std::printf("LLE_SRE_LIBC memcpy stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char left[4] = {0x10, 0x20, 0x30, 0x40};
        const unsigned char right[4] = {0x10, 0x20, 0x31, 0x40};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, left, 4) == PS3_GUEST_MEMORY_OK);
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block + 0x100, right, 4) == PS3_GUEST_MEMORY_OK);
        const uint64_t differing[8] = {block, block + 0x100, 4, 0, 0, 0, 0, 0};
        call_import(runtime, 0xfb5db080, differing);   // _sys_memcmp
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(int64_t(runtime.cpu().gpr[3]) == int64_t(int(0x30) - int(0x31)));
        LLE_CHECK(runtime.cpu().gpr[3] == UINT64_C(0xFFFFFFFFFFFFFFFF));
        const uint64_t equal[8] = {block, block, 4, 0, 0, 0, 0, 0};
        call_import(runtime, 0xfb5db080, equal);
        LLE_CHECK(runtime.control->stop == StartupStop::None && runtime.cpu().gpr[3] == 0);
        std::printf("LLE_SRE_LIBC memcmp stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char text[6] = {'a', 'b', 'c', 'd', 'e', 0};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, text, 6) == PS3_GUEST_MEMORY_OK);
        const uint64_t arguments[8] = {block, 0, 0, 0, 0, 0, 0, 0};
        call_import(runtime, 0x2d36462b, arguments);   // _sys_strlen
        LLE_CHECK(runtime.control->stop == StartupStop::None && runtime.cpu().gpr[3] == 5);
        std::printf("LLE_SRE_LIBC strlen stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char text[6] = {'h', 'i', 0, 'X', 'X', 'X'};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, text, 6) == PS3_GUEST_MEMORY_OK);
        fill_bytes(runtime, block + 0x100, 0x77, 8);
        const uint64_t arguments[8] = {block + 0x100, block, 0, 0, 0, 0, 0, 0};
        call_import(runtime, 0x99c88692, arguments);   // _sys_strcpy
        const std::vector<unsigned char> bytes = read_bytes(runtime, block + 0x100, 8);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block + 0x100);
        LLE_CHECK(bytes[0] == 'h' && bytes[1] == 'i' && bytes[2] == 0 && bytes[3] == 0x77);
        std::printf("LLE_SRE_LIBC strcpy stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char text[3] = {'x', 0, 'y'};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, text, 3) == PS3_GUEST_MEMORY_OK);
        fill_bytes(runtime, block + 0x100, 0x77, 8);
        const uint64_t arguments[8] = {block + 0x100, block, 6, 0, 0, 0, 0, 0};
        call_import(runtime, 0xd3039d4d, arguments);   // _sys_strncpy
        const std::vector<unsigned char> bytes = read_bytes(runtime, block + 0x100, 8);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block + 0x100);
        LLE_CHECK(bytes[0] == 'x' && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0);
        LLE_CHECK(bytes[6] == 0x77 && bytes[7] == 0x77);
        std::printf("LLE_SRE_LIBC strncpy stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char destination[4] = {'a', 'b', 0, 0x00};
        const unsigned char tail[4] = {'c', 'd', 0, 0x00};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, destination, 4) == PS3_GUEST_MEMORY_OK);
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block + 0x100, tail, 4) == PS3_GUEST_MEMORY_OK);
        const uint64_t arguments[8] = {block, block + 0x100, 0, 0, 0, 0, 0, 0};
        call_import(runtime, 0x052d29a6, arguments);   // _sys_strcat
        const std::vector<unsigned char> bytes = read_bytes(runtime, block, 8);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block);
        LLE_CHECK(bytes[0] == 'a' && bytes[1] == 'b' && bytes[2] == 'c' && bytes[3] == 'd' && bytes[4] == 0);
        std::printf("LLE_SRE_LIBC strcat stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char destination[2] = {'a', 0};
        const unsigned char tail[4] = {'b', 'c', 'd', 0};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, destination, 2) == PS3_GUEST_MEMORY_OK);
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block + 0x100, tail, 4) == PS3_GUEST_MEMORY_OK);
        fill_bytes(runtime, block + 1, 0x00, 8);
        const uint64_t arguments[8] = {block, block + 0x100, 2, 0, 0, 0, 0, 0};
        call_import(runtime, 0x996f7cf8, arguments);   // _sys_strncat
        const std::vector<unsigned char> bytes = read_bytes(runtime, block, 8);
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.cpu().gpr[3] == block);
        LLE_CHECK(bytes[0] == 'a' && bytes[1] == 'b' && bytes[2] == 'c' && bytes[3] == 0);
        std::printf("LLE_SRE_LIBC strncat stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
    {
        LleRuntime runtime(LLE_ATTR);
        const unsigned char left[4] = {'a', 'b', 'c', 0};
        const unsigned char right[4] = {'a', 'b', 'd', 0};
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block, left, 4) == PS3_GUEST_MEMORY_OK);
        LLE_CHECK(runtime.mem().write(runtime.mem().context, block + 0x100, right, 4) == PS3_GUEST_MEMORY_OK);
        const uint64_t arguments[8] = {block, block + 0x100, 3, 0, 0, 0, 0, 0};
        call_import(runtime, 0x04e83d2c, arguments);   // _sys_strncmp
        LLE_CHECK(runtime.control->stop == StartupStop::None);
        LLE_CHECK(int64_t(runtime.cpu().gpr[3]) == int64_t(int('c') - int('d')));
        const uint64_t bounded[8] = {block, block + 0x100, 2, 0, 0, 0, 0, 0};
        call_import(runtime, 0x04e83d2c, bounded);
        LLE_CHECK(runtime.control->stop == StartupStop::None && runtime.cpu().gpr[3] == 0);
        std::printf("LLE_SRE_LIBC strncmp stop=%d r3=0x%016llx\n", int(runtime.control->stop),
            (unsigned long long)runtime.cpu().gpr[3]);
    }
}

/* ---------- case 5: cellSpursInitialize through LLE stops cleanly ---------- */

void case_cell_spurs_initialize()
{
    LleRuntime runtime(LLE_ATTR);
    /* The fixture's real attribute, as cellSpursGetInfo's cases build it. */
    const unsigned char image[20] = {0, 0, 0, 2, 0, 0x25, 0, 1, 0, 0, 0, 5, 0, 0, 0, 128, 0, 0, 0, 249};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_ATTR_SOURCE, image, sizeof(image)) == PS3_GUEST_MEMORY_OK);
    fill_bytes(runtime, runtime.attributes[0], 0x00, 0x200);
    const uint64_t arguments[8] = {LLE_INSTANCE, LLE_ATTR_SOURCE, 0, 0, 0, 0, 0, 0};
    call_import(runtime, INITIALIZE_NID, arguments);
    if (lle_route_enabled) {
        /* A real boundary -- a syscall nothing serves, an import nothing serves -- or, with B2a
         * serving syscall 14, Sony's code running to its own return and answering with its own
         * negative error code. Never a fabricated success. */
        std::printf("LLE_SRE_INITIALIZE_BOUNDARY stop=%d\n", int(runtime.control->stop));
        LLE_CHECK(runtime.control->stop == StartupStop::UnsupportedSyscall ||
                  runtime.control->stop == StartupStop::UnknownImport || runtime.control->stop == StartupStop::None);
        LLE_CHECK(runtime.control->lle_sre_routed == 1);
        LLE_CHECK(runtime.control->lle_sre_last_nid == INITIALIZE_NID);
        LLE_CHECK(runtime.cpu().gpr[3] != 0);
        std::printf("LLE_SRE_INITIALIZE stop=%d syscall=%u routed=%zu restored=%zu r3=0x%016llx invalid=0x%x\n",
            int(runtime.control->stop), unsigned(runtime.control->syscall_number),
            runtime.control->lle_sre_routed, runtime.control->lle_sre_toc_restores,
            (unsigned long long)runtime.cpu().gpr[3], unsigned(runtime.control->invalid_address));
    } else {
        /* With the flag off the same call reaches the accepted HLE behaviour. */
        std::printf("LLE_SRE_INITIALIZE stop=%d syscall=%u routed=%zu restored=%zu r3=0x%016llx\n",
            int(runtime.control->stop), unsigned(runtime.control->syscall_number),
            runtime.control->lle_sre_routed, runtime.control->lle_sre_toc_restores,
            (unsigned long long)runtime.cpu().gpr[3]);
    }
}

/* ---------- case 6: TOC discipline ---------- */

void case_toc_discipline()
{
    uint64_t toc = 0;
    LLE_CHECK(!lle_sre_callback_toc(&toc));   // no lifted frame is active before any call
    LleRuntime runtime(LLE_ATTR);
    const uint64_t arguments[8] = {runtime.attributes[0], 5, SDK, 128, 249, 0, 0, 0};
    call_import(runtime, ATTRIBUTE_NID, arguments);
    /* With the route on the caller's r2 came back as it went in; with it off the HLE arm restores it
     * the same way. Both are the same assertion because it is the same contract. */
    LLE_CHECK(runtime.cpu().gpr[2] == TOC);
    LLE_CHECK(runtime.cpu().gpr[1] == LLE_STACK);
    if (lle_route_enabled)
        LLE_CHECK(runtime.control->lle_sre_routed == 1 && runtime.control->lle_sre_toc_restores == 1 &&
                  runtime.control->lle_sre_toc_failures == 0);
    LLE_CHECK(!lle_sre_callback_toc(&toc));   // no lifted frame is active after the call returned
    /* The callback port itself: the composition-side query the lifted code uses before it jumps
     * into a guest function pointer. It answers with the owner's TOC only while the owner says it
     * is entered, and never leaves the output untouched on refusal. */
    struct Port {
        bool entered = false;
        uint64_t toc = UINT64_C(0x0123456789ABCDEF);
        static bool publish(void *context, uint64_t *toc) {
            Port &port = *static_cast<Port *>(context);
            if (!port.entered) return false;
            *toc = port.toc;
            return true;
        }
    };
    Port port;
    PpuMemoryGuestCallback callback{Port::publish, &port};
    runtime.execution.ppu.guest_callback = &callback;
    uint64_t published = UINT64_C(0x5555555555555555);
    LLE_CHECK(!ppu_memory_guest_callback_toc(&runtime.cpu(), &published));
    LLE_CHECK(published == UINT64_C(0x5555555555555555));
    port.entered = true;
    LLE_CHECK(ppu_memory_guest_callback_toc(&runtime.cpu(), &published));
    LLE_CHECK(published == port.toc);
    runtime.execution.ppu.guest_callback = nullptr;
    LLE_CHECK(!ppu_memory_guest_callback_toc(&runtime.cpu(), &published));
    std::printf("LLE_SRE_TOC restored=%zu failures=%zu\n",
        runtime.control->lle_sre_toc_restores, runtime.control->lle_sre_toc_failures);
}


/* Test-only link wraps, inert unless armed -- and this suite never arms them. They exist so the suite
 * links against the same `-Wl,--wrap=...` flags the fixture verifiers use, i.e. so the run here is the
 * same binary shape those runs are. Every call forwards to the real symbol. */
extern "C" ps3_guest_memory_status __real_ps3_atomic_read(ps3_atomic_domain *, uint64_t, void *, size_t);
extern "C" ps3_guest_memory_status __real_ps3_atomic_write(ps3_atomic_domain *, uint64_t, const void *, size_t);
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_read(ps3_atomic_domain *, uint64_t, void *, size_t);
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_write(ps3_atomic_domain *, uint64_t, const void *, size_t);
extern "C" bool __real__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *, size_t);
extern "C" bool __wrap__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *, size_t);
static_assert(std::is_same<decltype(&__wrap_ps3_atomic_read), decltype(&ps3_atomic_read)>::value &&
              std::is_same<decltype(&__real_ps3_atomic_read), decltype(&ps3_atomic_read)>::value &&
              std::is_same<decltype(&__wrap_ps3_atomic_write), decltype(&ps3_atomic_write)>::value &&
              std::is_same<decltype(&__real_ps3_atomic_write), decltype(&ps3_atomic_write)>::value,
              "wraps must match the wrapped signatures");
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_read(ps3_atomic_domain *domain, uint64_t address,
    void *bytes, size_t length)
{
    return __real_ps3_atomic_read(domain, address, bytes, length);
}
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_write(ps3_atomic_domain *domain, uint64_t address,
    const void *bytes, size_t length)
{
    return __real_ps3_atomic_write(domain, address, bytes, length);
}
extern "C" bool __wrap__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *recorder, size_t count)
{
    return __real__ZN22PpuMemoryTraceRecorder5beginEm(recorder, count);
}

/* ---------- case 7: the RECORDER path (production's, not the legacy journal) ----------
 *
 * Production admits with native_startup_admit_budgeted, whose recorder holds exactly one open batch.
 * A lifted body reaches ps3_hle_call (its own import stubs) and lv2_syscall from inside the routed
 * call; without the route closing the caller's batch first, every such nested entry refuses with
 * TraceCapacity. That was the B1a finding. This case runs Sony's code with the recorder attached and
 * demands the stop be the real one. */
void case_recorder_path()
{
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_RECORDER_SKIPPED route=hle\n");
        return;
    }
    LleRuntime runtime(LLE_ATTR);
    const bool readmitted = runtime.readmit_budgeted();
    std::printf("LLE_SRE_READMIT %d admitted=%d rec=%p\n", int(readmitted), int(runtime.execution.ppu.admitted),
        (void*)runtime.execution.ppu.recorder);
    LLE_CHECK(readmitted);
    LLE_CHECK(runtime.space.recorder.report().configured);
    LLE_CHECK(runtime.execution.ppu.recorder == &runtime.space.recorder);
    const unsigned char image[20] = {0, 0, 0, 2, 0, 0x25, 0, 1, 0, 0, 0, 5, 0, 0, 0, 128, 0, 0, 0, 249};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_ATTR_SOURCE, image, sizeof(image)) == PS3_GUEST_MEMORY_OK);
    const uint64_t arguments[8] = {LLE_INSTANCE, LLE_ATTR_SOURCE, 0, 0, 0, 0, 0, 0};
    call_import(runtime, INITIALIZE_NID, arguments);
    const StartupStop stop = runtime.control->stop;
    std::printf("LLE_SRE_RECORDER_PATH stop=%d syscall=%u last_nid=0x%08x\n", int(stop),
        unsigned(runtime.control->syscall_number), runtime.control->lle_sre_last_nid);
    /* The lifted body really ran: Sony's code was entered for this NID. */
    LLE_CHECK(runtime.control->lle_sre_routed == 1);
    LLE_CHECK(runtime.control->lle_sre_last_nid == INITIALIZE_NID);
    /* The recorder must never be the thing that refuses: that was the whole point of suspending the
     * caller's batch around Sony's code, and it holds. With B2a serving syscall 14 the lifted body
     * now runs to its own return and answers with Sony's own CELL_SPURS_CORE_ERROR_* code -- a real
     * answer from real code, not a stop and not a fabricated success. Without B2a it stops at a named
     * boundary instead. Either is acceptable here; TraceCapacity never is. */
    LLE_CHECK(stop != StartupStop::TraceCapacity);
    LLE_CHECK(stop == StartupStop::None || stop == StartupStop::MissingServices ||
              stop == StartupStop::UnsupportedSyscall || stop == StartupStop::UnknownImport);
    // Root (B2a): with syscall 14 = CELL_OK and the import-stub TOC save, the body runs past 14; the
    // recorder property (never TraceCapacity) is what this case proves, not which syscall comes last.
    if (stop == StartupStop::None) LLE_CHECK(uint32_t(runtime.cpu().gpr[3]) != 0x80410709u);
    LLE_CHECK(runtime.control->syscall_number >= 14);
    std::printf("LLE_SRE_RECORDER_BOUNDARY stop=%d syscall=%u\n", int(stop),
        unsigned(runtime.control->syscall_number));
}

/* ---------- case 8: a string that ends one byte before an unmapped page ----------
 *
 * A whole-chunk strlen reads past the terminator, so a string that ends exactly at the end of its
 * mapped region would fault on bytes it never needed. The read is byte-wise for that reason. */
void case_strlen_page_boundary()
{
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_STRLEN_SKIPPED route=hle\n");
        return;
    }
    LleRuntime runtime(LLE_ATTR);
    /* The last mapped byte of the caller's RAM window: the string is "ab", NUL at window_end - 1. */
    const uint32_t window_end = uint32_t(LLE_RAM_BASE + LLE_RAM_SIZE);
    const unsigned char text[2] = {'a', 'b'};
    const uint32_t start = window_end - 3;
    LLE_CHECK(runtime.mem().write(runtime.mem().context, start, text, 2) == PS3_GUEST_MEMORY_OK);
    const unsigned char terminator = 0;
    LLE_CHECK(runtime.mem().write(runtime.mem().context, window_end - 1, &terminator, 1) == PS3_GUEST_MEMORY_OK);
    LLE_CHECK(runtime.mem().read(runtime.mem().context, window_end, nullptr, 0) == PS3_GUEST_MEMORY_OK);
    const uint64_t arguments[8] = {start, 0, 0, 0, 0, 0, 0, 0};
    call_import(runtime, 0x2d36462b, arguments);   // _sys_strlen
    std::printf("LLE_SRE_STRLEN_BOUNDARY stop=%d r3=0x%016llx\n", int(runtime.control->stop),
        (unsigned long long)runtime.cpu().gpr[3]);
    LLE_CHECK(runtime.control->stop == StartupStop::None);
    LLE_CHECK(runtime.cpu().gpr[3] == 2);
}

/* ---------- case 9: the chained compiled-code lookup (finding 2) ---------- */

int recording_find_calls = 0;
ps3_compiled_code_status recording_find(const void *, uint32_t address, ps3_compiled_function *target)
{
    ++recording_find_calls;
    if (address != LLE_GAME_ONLY_CODE) return PS3_COMPILED_CODE_NOT_FOUND;
    *target = game_only_body;
    return PS3_COMPILED_CODE_OK;
}
void game_only_body(ppu_context *cpu) { cpu->gpr[3] = 0x1234; }

void case_chained_lookup()
{
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_CHAIN_SKIPPED route=hle\n");
        return;
    }
    const ps3_compiled_code_lookup chained =
        lle_sre_chained_compiled_code_lookup(ps3_compiled_code_lookup{nullptr, recording_find});
    ps3_compiled_function target = nullptr;
    /* A game address resolves through the GAME's table. */
    LLE_CHECK(chained.find(chained.context, LLE_GAME_ONLY_CODE, &target) == PS3_COMPILED_CODE_OK);
    LLE_CHECK(target == game_only_body);
    const int after_game = recording_find_calls;
    /* A lifted libsre address falls through to the MODULE's table. */
    target = nullptr;
    LLE_CHECK(chained.find(chained.context, ATTRIBUTE_CODE, &target) == PS3_COMPILED_CODE_OK);
    LLE_CHECK(target != nullptr && target != game_only_body);
    LLE_CHECK(recording_find_calls == after_game + 1);   // the game's table was asked, and declined
    /* Neither table knows an address no module maps. */
    target = nullptr;
    LLE_CHECK(chained.find(chained.context, 0x12345678u, &target) == PS3_COMPILED_CODE_NOT_FOUND);
    std::printf("LLE_SRE_CHAIN game=1 module=1 unknown=1\n");
}

/* ---------- case 11: B2a adapter ABI -- r3, big-endian output, EFAULT writes nothing ----------
 *
 * These go through the REAL lv2_syscall with the real lv2 object registry, so the registers, the byte
 * order and the error-to-r3 mapping are measured where the guest sees them. */
void case_b2a_adapter_abi()
{
#ifdef PS3_NATIVE_LV2_SYNC
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_B2A_ABI_SKIPPED route=hle\n");
        return;
    }
    LleRuntime runtime(LLE_ATTR);
    auto &cpu = runtime.cpu();
    auto syscall_now = [&](uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6, uint64_t a7) -> uint64_t {
        runtime.control->import_count = 0; runtime.control->dispatch_count = 0;
        cpu.gpr[1] = LLE_STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = a1; cpu.gpr[4] = a2; cpu.gpr[5] = a3; cpu.gpr[6] = a4;
        cpu.gpr[7] = a5; cpu.gpr[8] = a6; cpu.gpr[9] = a7; cpu.gpr[11] = number;
        runtime.run(uint32_t(number), true);
        return cpu.gpr[3];
    };
    const uint32_t attr = LLE_BLOCK;                 // sys_semaphore_attr: protocol FIFO, no ipc key
    const uint32_t out = LLE_BLOCK + 0x100;
    unsigned char attribute[24]{};
    attribute[3] = 1;                                 // protocol = PS3_SYNC_FIFO
    LLE_CHECK(runtime.mem().write(runtime.mem().context, attr, attribute, sizeof(attribute)) == PS3_GUEST_MEMORY_OK);
    const unsigned char ones[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, out, ones, sizeof(ones)) == PS3_GUEST_MEMORY_OK);

    /* sys_semaphore_create: r3 = 0, and the id is written BIG-ENDIAN into the guest word. */
    const uint64_t created = syscall_now(90, out, attr, 0, 2, 0, 0, 0);
    LLE_CHECK(created == 0);
    LLE_CHECK(runtime.control->stop == StartupStop::None);
    const uint64_t id_be = runtime.get(out, 4);
    LLE_CHECK(id_be != 0 && id_be != UINT64_C(0xAAAAAAAA));
    const uint32_t id = uint32_t(id_be);

    /* A NULL output pointer is an InvalidMemory stop and writes nothing (nothing to write to). */
    const uint64_t null_create = syscall_now(90, 0, attr, 0, 2, 0, 0, 0);
    LLE_CHECK(null_create == 0);   // unused: the run stopped before returning a value
    LLE_CHECK(runtime.control->stop == StartupStop::InvalidMemory);

    /* A bad EINVAL case goes to r3 sign-extended, not to a stop, and leaves the output word alone. */
    const unsigned char untouched[4] = {0x11, 0x22, 0x33, 0x44};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, out + 4, untouched, sizeof(untouched)) == PS3_GUEST_MEMORY_OK);
    LLE_CHECK(int32_t(uint32_t(syscall_now(90, out + 4, attr, 5, 2, 0, 0, 0))) == int32_t(0x80010002u));
    LLE_CHECK(runtime.get(out + 4, 4) == UINT64_C(0x11223344));

    /* destroy by the id the guest read back: r3 = 0. */
    LLE_CHECK(syscall_now(91, id, 0, 0, 0, 0, 0, 0) == 0);
    /* and an unknown id answers ESRCH sign-extended. */
    LLE_CHECK(int32_t(uint32_t(syscall_now(91, id, 0, 0, 0, 0, 0, 0))) == int32_t(0x80010005u));

    /* 14: success is CELL_OK (root fix: the caller tests r3 == 0); a bad flag word is the negative code. */
    LLE_CHECK(syscall_now(14, LLE_INSTANCE, 2, 0, 0, 0, 0, 0) == 0);
    LLE_CHECK(int32_t(uint32_t(syscall_now(14, LLE_INSTANCE, 0x40, 0, 0, 0, 0, 0))) == int32_t(0x80010002u));
    /* an unmapped address is EINVAL, never a fabricated success. */
    LLE_CHECK(int32_t(uint32_t(syscall_now(14, 0x2FFF0000u, 2, 0, 0, 0, 0, 0))) == int32_t(0x80010002u));

    /* 351: the four fields land big-endian, in order. */
    const uint32_t page = LLE_BLOCK + 0x200;
    LLE_CHECK(runtime.mem().write(runtime.mem().context, page, ones, 8) == PS3_GUEST_MEMORY_OK);
    LLE_CHECK(syscall_now(351, LLE_STACK, page, 0, 0, 0, 0, 0) == 0);
    LLE_CHECK(runtime.get(page, 8) == UINT64_C(0x40000));        // attribute
    LLE_CHECK(runtime.get(page + 8, 8) == UINT64_C(0x8));        // access_right: PPU_THR on the stack
    LLE_CHECK(runtime.get(page + 16, 4) == UINT64_C(0x1000));    // page_size: 4 KiB on the stack
    LLE_CHECK(runtime.get(page + 20, 4) == 0);
    /* a main-memory address is PPU with a large page. */
    LLE_CHECK(syscall_now(351, LLE_BLOCK, page, 0, 0, 0, 0, 0) == 0);
    LLE_CHECK(runtime.get(page + 8, 8) == UINT64_C(0xF));
    LLE_CHECK(runtime.get(page + 16, 4) == UINT64_C(0x100000));
    /* an unmapped address is EINVAL and leaves the structure exactly as it was. */
    LLE_CHECK(runtime.mem().write(runtime.mem().context, page, ones, 8) == PS3_GUEST_MEMORY_OK);
    LLE_CHECK(int32_t(uint32_t(syscall_now(351, 0x2FFF0000u, page, 0, 0, 0, 0, 0))) == int32_t(0x80010002u));
    LLE_CHECK(runtime.get(page, 8) == UINT64_C(0xAAAAAAAAAAAAAAAA));
    std::printf("LLE_SRE_B2A_ABI r3=ok bigendian=ok efast=untouched\n");
#else
    std::printf("LLE_SRE_B2A_ABI_SKIPPED no_lv2_sync\n");
#endif
}

/* ---------- case 12: no internal code ever reaches the guest ----------
 *
 * Every value >= 0xFFFF0000 is an outcome of this adapter layer, not a CELL error. The only correct
 * destinations are a named stop or (where lv2 has one) a documented CELL code. This drives a real
 * provider failure through the read_ls arm and requires a STOP with r3 untouched, never a
 * sign-extended 0xFFFF00xx in the guest's r3. */
void case_b2b_internal_code_mapping()
{
#ifdef PS3_NATIVE_LV2_SPU
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_B2B_INTERNAL_SKIPPED route=hle\n");
        return;
    }
    LleRuntime runtime(LLE_ATTR);
    auto &cpu = runtime.cpu();
    auto syscall_now = [&](uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) -> uint64_t {
        runtime.control->import_count = 0; runtime.control->dispatch_count = 0;
        cpu.gpr[1] = LLE_STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = a1; cpu.gpr[4] = a2; cpu.gpr[5] = a3; cpu.gpr[6] = a4;
        runtime.run(uint32_t(number), true);
        return cpu.gpr[3];
    };
    /* A group and one initialized thread, so read_ls reaches the domain. */
    const uint32_t group_out = LLE_BLOCK, thread_out = LLE_BLOCK + 0x10, ls_out = LLE_BLOCK + 0x20;
    const uint32_t attribute = LLE_BLOCK + 0x40;
    unsigned char zero_image[24]{};
    zero_image[3] = 1;   // protocol FIFO in the attr-shaped block the arm reads
    LLE_CHECK(runtime.mem().write(runtime.mem().context, attribute, zero_image, sizeof(zero_image)) == PS3_GUEST_MEMORY_OK);
    CHECK(syscall_now(170, group_out, 2, 100, attribute) == 0);
    const uint32_t group = uint32_t(runtime.get(group_out, 4));
    LLE_CHECK(group != 0);
    /* No image import has run, so thread_initialize answers ESRCH: a real CELL code, returned in r3. */
    /* A NULL image descriptor is a refused guest pointer: a named InvalidMemory stop. */
    (void)syscall_now(172, thread_out, group, 0, 0);
    LLE_CHECK(runtime.control->stop == StartupStop::InvalidMemory);
    /* An unknown segs EA is the guest's own ESRCH, in r3. */
    CHECK(syscall_now(170, group_out, 2, 100, attribute) == 0);
    const uint32_t group2 = uint32_t(runtime.get(group_out, 4));
    const uint32_t bad_image = LLE_BLOCK + 0x100;
    unsigned char image_desc[16]{};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, bad_image, image_desc, sizeof(image_desc)) == PS3_GUEST_MEMORY_OK);
    LLE_CHECK(int32_t(uint32_t(syscall_now(172, thread_out, group2, 0, bad_image))) == int32_t(0x80010005u));
    /* Drive a provider failure and require a STOP, not an r3. */
    ps3_lv2_spu_inject_provider_failure(&runtime.spu_groups, 1);
    const uint64_t r3_before = cpu.gpr[3];
    (void)syscall_now(182, 0x9D000000u, 0x100, ls_out, 4);
    ps3_lv2_spu_inject_provider_failure(&runtime.spu_groups, 0);
    std::printf("LLE_SRE_B2B_INTERNAL stop=%d r3=0x%016llx\n", int(runtime.control->stop),
        (unsigned long long)cpu.gpr[3]);
    LLE_CHECK(runtime.control->stop != StartupStop::None);      // never a fake success
    LLE_CHECK(runtime.control->stop == StartupStop::ProviderFailure);
    /* The leaked value never reached the guest: r3 is not an 0xFFFF00xx code. */
    LLE_CHECK((uint32_t(cpu.gpr[3]) < 0xFFFF0000u) || cpu.gpr[3] == r3_before);
#else
    std::printf("LLE_SRE_B2B_INTERNAL_SKIPPED no_lv2_spu\n");
#endif
}

/* ---------- case 10: B2a -- cellSpursInitialize gets PAST syscall 14 ----------
 *
 * With PS3_NATIVE_LV2_SYNC compiled in, the lv2 sync/object syscalls Sony's libsre makes are served
 * natively, so the recorder-path call must no longer terminate inside that set: it has to reach the
 * NEXT real boundary, which is an SPU-thread-group syscall (170..192, 248..253) or an import nothing
 * serves. A stop still inside the B2a set would be a bug in this phase, not an outcome. */
void case_b2a_next_boundary()
{
#ifdef PS3_NATIVE_LV2_SYNC
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_B2A_SKIPPED route=hle\n");
        return;
    }
    LleRuntime runtime(LLE_ATTR);
    LLE_CHECK(runtime.readmit_budgeted());
    const unsigned char image[20] = {0, 0, 0, 2, 0, 0x25, 0, 1, 0, 0, 0, 5, 0, 0, 0, 128, 0, 0, 0, 249};
    LLE_CHECK(runtime.mem().write(runtime.mem().context, LLE_ATTR_SOURCE, image, sizeof(image)) == PS3_GUEST_MEMORY_OK);
    /* Root fix (B2b): cellSpursInitialize(spurs, nSpu, spuPriority, ppuPriority, exitIfNoWork). The worker
     * passed an attribute pointer as nSpu, so syscall 170 (ppu_recomp_000.cpp:9565, r4 = nSpu) rightly
     * refused EINVAL. The game passes nSpu = 5 (Eden trace: sc 170 r4=0x5). */
    const uint64_t arguments[8] = {LLE_INSTANCE, 5, 128, 249, 0, 0, 0, 0};
    call_import(runtime, INITIALIZE_NID, arguments);
    const uint32_t stop = uint32_t(runtime.control->stop);
    const uint32_t sc = uint32_t(runtime.control->syscall_number);
    std::printf("LLE_SRE_B2A_NEXT stop=%u syscall=%u last_nid=0x%08x\n", stop, sc,
        runtime.control->lle_sre_last_nid);
    LLE_CHECK(runtime.control->lle_sre_routed == 1);
    LLE_CHECK(runtime.control->lle_sre_last_nid == INITIALIZE_NID);
    /* Two acceptable outcomes, both of which mean the call got PAST the B2a boundary:
     *  - it returned normally, answering with Sony's own negative CELL_SPURS code (r3 != 0), or
     *  - it stopped at a NAMED boundary OUTSIDE the B2a set (an SPU-thread-group syscall or an
     *    unserved import).
     * What it must never do is stop inside the set this phase serves. */
    /* Root fix (B2a Eden trace): returning CELL_SPURS_CORE_ERROR_STAT (0x80410709) is the syscall-14
     * failure path (ppu_recomp_000.cpp:9190-9193 requires r3 == 0), i.e. a bug in a served syscall, not
     * an outcome. The worker's "r3 != 0" accepted exactly that failure. */
    std::printf("LLE_SRE_B2A_NEXT_R3 r3=0x%016llx\n", (unsigned long long)runtime.cpu().gpr[3]);
    {   // root: name the import the lifted body reached last (the UnknownImport NID when stop=2)
        const size_t n = runtime.control->import_count;
        std::printf("LLE_SRE_B2A_LAST_IMPORTS count=%zu", n);
        for (size_t i = n > 6 ? n - 6 : 0; i < n && i < STARTUP_IMPORT_CAPACITY; ++i)
            std::printf(" 0x%08x", unsigned(runtime.control->imports[i]));
        std::printf("\n");
    }
#ifdef PS3_NATIVE_LV2_SPU
    /* B2b: Sony's code must now build its whole group and stop at phase C1's boundary. */
    std::printf("LLE_SRE_B2B_START stop=%u group=0x%08x num=%u entries=", stop,
        unsigned(runtime.control->lv2_spu_last_group), unsigned(runtime.control->lv2_spu_last_num));
    for (uint32_t index = 0; index < runtime.control->lv2_spu_last_num && index < 8; ++index)
        std::printf("%s0x%05x", index ? "," : "", unsigned(runtime.control->lv2_spu_entries[index]));
    std::printf(" images=%zu segments=%zu calls=%zu\n", size_t(runtime.control->lv2_spu_image_segments),
        size_t(runtime.control->lv2_spu_segments_address), runtime.control->lv2_spu_calls);
    /* Root (B2b): this fixture composes no child-PPU port, and in libsre sys_spu_thread_group_start (173)
     * is issued by a SPURS PPU thread that cellSpursInitialize creates. So the host call must build the
     * whole group (170, 5 x 172, 251, 134/136) and then stop MissingServices at sys_ppu_thread_create
     * (0x24A1EA07). The 173 SpuThreadGroupStart boundary itself is proven on Eden, where children run. */
    const size_t imports_seen = runtime.control->import_count;
    const uint32_t last_import = imports_seen && imports_seen <= STARTUP_IMPORT_CAPACITY
        ? runtime.control->imports[imports_seen - 1] : 0u;
    /* Root (B2b): this fixture also composes no lwmutex-create service, so after the group is built
     * (170 + 5 x 172, traced 2026-11-08) Sony's code stops MissingServices at sys_lwmutex_create
     * (0x2F85C0EF); with that service it would next stop at sys_ppu_thread_create (0x24A1EA07). Both are
     * named fixture gaps AFTER the B2b surface. The NRO composition serves both (Eden reaches 173). */
    std::printf("LLE_SRE_B2B_FIXTURE_STOP stop=%u last_import=0x%08x spu_calls=%zu\n", stop, last_import,
        runtime.control->lv2_spu_calls);
    LLE_CHECK(stop == uint32_t(StartupStop::MissingServices) || stop == uint32_t(StartupStop::SpuThreadGroupStart));
    if (stop == uint32_t(StartupStop::MissingServices))
        LLE_CHECK(last_import == 0x2F85C0EFu || last_import == 0x24A1EA07u);
    LLE_CHECK(runtime.control->lv2_spu_calls >= 7);
#endif
    if (stop == uint32_t(StartupStop::None))
        LLE_CHECK(uint32_t(runtime.cpu().gpr[3]) != 0x80410709u);
    else if (stop == uint32_t(StartupStop::UnsupportedSyscall))   // root: syscall_number is only the stop's subject for a syscall stop
        LLE_CHECK(!(sc == 14 || sc == 25 || sc == 351 || sc == 141 || (sc >= 82 && sc <= 94) ||
                    sc == 118 || sc == 128 || sc == 129 || sc == 130 || sc == 131 ||
                    sc == 134 || sc == 135 || sc == 136 || sc == 137 || sc == 138));
    LLE_CHECK(runtime.control->lv2_sync_returns >= 1);   // Sony's code really made a served syscall
#else
    std::printf("LLE_SRE_B2A_SKIPPED no_lv2_sync\n");
#endif
}

/* ---------- case 12: sys_process_is_stack and the sceNpTrophy family ----------
 *
 * Both are served by the native LLE process dispatcher; the fixture composes no PPU thread registry, so
 * sys_process_is_stack answers from the PS3 user-stack range alone. */
constexpr uint32_t PROCESS_IS_STACK_NID = 0x4f7172c9;
constexpr uint32_t TROPHY_INIT_NID = 0x39567781;
constexpr uint32_t TROPHY_REGISTER_CONTEXT_NID = 0x1197b52c;
constexpr uint32_t TROPHY_UNLOCK_TROPHY_NID = 0x8ceedd21;
constexpr uint64_t TROPHY_NOT_SUPPORTED = UINT64_C(0xFFFFFFFF80022903);
constexpr uint64_t TROPHY_NOT_INITIALIZED = UINT64_C(0xFFFFFFFF80022902);

void expect_import_result(uint32_t nid, uint32_t address, uint64_t expected, const char *label)
{
    LleRuntime runtime(LLE_ATTR);
    const uint64_t arguments[8] = {address, 0, 0, 0, 0, 0, 0, 0};
    call_import(runtime, nid, arguments);
    std::printf("LLE_SRE_IMPORT_RESULT %s nid=0x%08x arg=0x%08x stop=%d r3=0x%016llx\n", label, nid, address,
        int(runtime.control->stop), (unsigned long long)runtime.cpu().gpr[3]);
    LLE_CHECK(runtime.control->stop == StartupStop::None);
    LLE_CHECK(runtime.cpu().gpr[3] == expected);
}

void case_process_imports()
{
    if (!lle_route_enabled) {
        std::printf("LLE_SRE_PROCESS_IMPORTS_SKIPPED route=hle\n");
        return;
    }
    const struct { uint32_t address; uint64_t expected; } stack_cases[] = {
        {0xD0000000u, 1}, {0xDFFFFFFFu, 1}, {0xCFFFFFFFu, 0}, {0xE0000000u, 0}, {0x00010000u, 0}};
    for (size_t index = 0; index < sizeof stack_cases / sizeof stack_cases[0]; ++index)
        expect_import_result(PROCESS_IS_STACK_NID, stack_cases[index].address,
            stack_cases[index].expected, "sys_process_is_stack");
    expect_import_result(TROPHY_INIT_NID, 0, TROPHY_NOT_SUPPORTED, "sceNpTrophyInit");
    expect_import_result(TROPHY_REGISTER_CONTEXT_NID, 0, TROPHY_NOT_INITIALIZED,
        "sceNpTrophyRegisterContext");
    expect_import_result(TROPHY_UNLOCK_TROPHY_NID, 0, TROPHY_NOT_INITIALIZED, "sceNpTrophyUnlockTrophy");
}

int main()
{
    case_mapping();
    case_routing();
    case_equivalence();
    case_libc();
    case_cell_spurs_initialize();
    case_toc_discipline();
    case_recorder_path();
    case_strlen_page_boundary();
    case_chained_lookup();
    case_b2a_next_boundary();
    case_b2a_adapter_abi();
    case_b2b_internal_code_mapping();
    case_process_imports();
    std::printf("LLE_SRE_RESULT_TOTAL failures=%d route=%s\n", lle_failures,
        lle_route_enabled ? "lle" : "hle");
    return lle_failures ? 1 : 0;
}
