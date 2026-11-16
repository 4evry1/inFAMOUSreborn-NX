/* HOST integration test for the child composition glue (adapters/startup_child_composition.{h,cpp}). NOT game execution.
 * Real collaborators (all reused from test_startup_child_ppu.cpp's rig): the actual startup_execution.cpp sys_ppu_thread_create
 * bridge (ps3_hle_call -> dispatch_ppu_thread_create), the real StartupChildPpu owner, native_startup_worker/IndirectCall through
 * the real compiled-code lookup port, the atomic domain + coherent binding, ps3_user_memory, ps3_lv2 queues, ps3_ppu_threads and
 * real pthread service threads. The guest "parent" is a host thread that seeds the registers the import stub would and issues one
 * real HLE call; compiled bodies are LABEL fixtures (they never claim the game ABI). Expected values derive from the contract in
 * the two headers, not from captured output. */
#define main inherited_child_main
#include "test_startup_child_ppu.cpp"
#undef main
#include "../adapters/startup_child_composition.h"

namespace {
constexpr unsigned NID_CREATE = 0x24a1ea07;
constexpr uint32_t OUTA = SLOT_PAGE + 0x100, OPDA = SLOT_PAGE + 0x200, NAMEA = SLOT_PAGE + 0x300, PSTACK = SLOT_PAGE + 0x400,
    PTOC_SLOT = PSTACK + 0x28, CHILD_TOC = 0x0098F3C0;
constexpr uint64_t OUT_SENTINEL = UINT64_C(0xCAFEF00DCAFEF00D), TOC_SENTINEL = UINT64_C(0x5555555555555555);
const char NAME_TEXT[] = "Job Manager Event Handler";

Obs *g_comp_obs = nullptr;
void adopt(ppu_context *cpu) { Obs *o = g_comp_obs; o->thread_id = uint32_t(cpu->thread_id); g_obs[0] = o; }
void comp_receive(ppu_context *cpu) { adopt(cpu); body_receive(cpu); }
void comp_return(ppu_context *cpu) { adopt(cpu); body_return(cpu); }
void comp_gated(ppu_context *cpu) { adopt(cpu); body_gated(cpu); }
const CodeEntry comp_entries[] = {{ENTRY_RECEIVE, comp_receive}, {ENTRY_RETURN, comp_return}, {ENTRY_GATED, comp_gated}};
ps3_compiled_code_status comp_find(const void *, uint32_t address, ps3_compiled_function *target)
{
    for (const CodeEntry &e : comp_entries) if (e.address == address) { *target = e.function; return PS3_COMPILED_CODE_OK; }
    return PS3_COMPILED_CODE_NOT_FOUND;
}
void poll_sleep(void *context) { static_cast<std::atomic<unsigned> *>(context)->fetch_add(1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
void poll_open_gate(void *context) { static_cast<Obs *>(context)->gate_open.store(true); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

StartupChildCompositionPolicy first_child_policy()
{
    StartupChildCompositionPolicy p;
    p.kind = 1; p.priority = PRIORITY; p.creator_priority = CREATOR; p.stack_size = uint32_t(G64); p.tls_capacity = G64; p.return_lr = LR; p.stack_entry_slack = 0x70;
    return p;
}

struct CompRig {
    Rig r;
    std::unique_ptr<StartupChildComposition> comp{new StartupChildComposition};
    ChildRig *c = nullptr;
    Obs obs;
    uint32_t q = 0;
    std::atomic<unsigned> polls{0};
    bool parent_returned = false;
    explicit CompRig(bool configure = true, bool with_queue = true) {
        g_comp_obs = &obs; clear_obs();
        r.lookup.find = comp_find;
        if (with_queue) q = r.queue_create();
        c = &r.new_child(q);
        obs.queue = q;
        r.primary.has_guest_priority = true; r.primary.guest_priority = int32_t(CREATOR);
        r.primary_control->initialized_tls_template = {SRC, 4, 0x200}; r.primary_control->initialized_tls_template_valid = true;
        put32(OPDA, ENTRY_RECEIVE); put32(OPDA + 4, CHILD_TOC);
        r.put(NAMEA, NAME_TEXT, sizeof NAME_TEXT);
        put64(OUTA, OUT_SENTINEL); put64(PTOC_SLOT, TOC_SENTINEL);
        if (configure) { CHECK(comp->configure(ports(), first_child_policy()) == StartupChildCompositionConfigure::Ok); r.primary.ppu_create = comp->create_port(); }
    }
    ~CompRig() { clear_obs(); g_comp_obs = nullptr; }
    StartupChildPpuPorts ports() { StartupChildPpuPorts p = r.ports(*c); p.dispatch_stop = {}; return p; }
    void put32(uint32_t a, uint32_t v) { unsigned char b[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; r.put(a, b, 4); }
    void put64(uint32_t a, uint64_t v) { unsigned char b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i)); r.put(a, b, 8); }
    uint64_t get64(uint32_t a) { unsigned char b[8]; r.get(a, b, 8); return be64(b); }
    void seed(uint64_t priority = PRIORITY, uint64_t stack = G64) {
        auto &cpu = r.primary.ppu.cpu;
        for (int i = 3; i < 32; ++i) if (i != 13) cpu.gpr[i] = UINT64_C(0x1111000000000000) + uint64_t(i);
        cpu.gpr[1] = PSTACK; cpu.gpr[2] = TOC; cpu.gpr[3] = OUTA; cpu.gpr[4] = OPDA; cpu.gpr[5] = OUTA; cpu.gpr[6] = priority; cpu.gpr[7] = stack;
        cpu.gpr[8] = 1; cpu.gpr[9] = NAMEA;
    }
    // The parent: ONE real HLE call on its own host thread (the thread-local `active` execution is per thread).
    void parent_body() {
        auto &cpu = r.primary.ppu.cpu;
        r.primary_control->stop = StartupStop::None;
        active = r.primary_control.get(); r.primary_control->cpu = &cpu; g_trampoline_fn = [](void *) {};
        try { ps3_hle_call(NID_CREATE, &cpu); parent_returned = true; active = nullptr; } catch (const Terminal &) { parent_returned = false; }
        g_trampoline_fn = nullptr;
    }
    std::thread spawn_parent() { return std::thread([this] { parent_body(); }); }
    bool run_parent() { std::thread t = spawn_parent(); t.join(); return parent_returned; }
    StartupChildCompositionTeardownPolicy td(unsigned polls = 2000, unsigned retries = 3) {
        StartupChildCompositionTeardownPolicy p; p.context = &polls_counter; p.poll = poll_sleep; p.max_reap_polls = polls; p.max_stage_retries = retries; return p;
    }
    std::atomic<unsigned> polls_counter{0};
    StartupPpuCreateRequest hand_request() {
        StartupPpuCreateRequest q2;
        q2.opd_address = OPDA; q2.entry = ENTRY_RECEIVE; q2.toc = CHILD_TOC; q2.argument = OUTA; q2.priority = int32_t(PRIORITY); q2.stack_size = uint32_t(G64); q2.flags = 1;
        q2.output = OUTA; q2.name_address = NAMEA; q2.name_length = uint32_t(sizeof NAME_TEXT - 1); std::memcpy(q2.name, NAME_TEXT, sizeof NAME_TEXT);
        q2.creator_thread_id = 1; q2.creator_priority = int32_t(CREATOR);
        return q2;
    }
    Ps3TlsTemplate tls_template() { return {SRC, 4, 0x200}; }
    // All teardown outcomes that retain nothing: baseline checks.
    void expect_released_baseline() { CHECK(comp->released()); check_released_baseline(r); }
};

void case_boot_content_inheritance() {
    CompRig g; g.seed();
    const BootContentMetadata expected=boot_content_fixture(BootContentOrigin::DiscPackage);
    g.r.primary.boot_content=expected;
    g.r.primary_control->boot_disc_calls=17; g.r.primary_control->boot_disc_returns=16;
    g.r.primary_control->boot_disc.has_result=true; g.r.primary_control->boot_disc.output_writes=9;
    g.r.primary_control->boot_disc.metadata_checked=true;
    CHECK(g.run_parent());
    CHECK(g.obs.execution_control!=g.r.primary_control.get());
    CHECK(same_boot_content(g.obs.boot_content_before,expected));
    CHECK(!g.obs.boot_disc_calls && !g.obs.boot_disc_returns && g.obs.boot_disc_observation_fresh);
    g.r.primary.boot_content=boot_content_fixture(BootContentOrigin::InstalledPackage);
    g.r.primary.boot_content.title_id[0]='X'; g.r.primary.boot_content.parental_level=99;
    g.r.send(g.q,1,2,3);
    const auto *child=g.comp->child(0); CHECK(child);
    arm(10000);
    while (child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(same_boot_content(g.obs.boot_content_after,expected));
    CHECK(g.r.primary.boot_content.origin==BootContentOrigin::InstalledPackage && g.r.primary.boot_content.parental_level==99);
    CHECK(g.r.primary_control->boot_disc_calls==17 && g.r.primary_control->boot_disc_returns==16);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}

void case_system_parameters_inheritance() {
    CompRig g; g.seed(); const auto expected=system_parameters_fixture();
    g.r.primary.system_parameters=expected;
    auto &parent=*g.r.primary_control;
    parent.system_parameter_calls=23; parent.system_parameter_returns=22;
    parent.system_parameter.has_result=true; parent.system_parameter.settings_checked=true;
    parent.system_parameter.settings=expected; parent.system_parameter.raw_id=0x111;
    parent.system_parameter.output_written=true; parent.system_parameter.output_write_attempts=1;
    CHECK(g.run_parent());
    CHECK(g.obs.execution_control!=g.r.primary_control.get());
    CHECK(same_system_parameters(g.obs.system_parameters_before,expected));
    CHECK(!g.obs.system_parameter_calls && !g.obs.system_parameter_returns && g.obs.system_parameter_observation_fresh);
    g.r.primary.system_parameters={};
    g.r.send(g.q,1,2,3);
    const auto *child=g.comp->child(0); CHECK(child); arm(10000);
    while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(same_system_parameters(g.obs.system_parameters_after,expected));
    CHECK(g.r.primary.system_parameters.profile==SystemParameterProfile::Unavailable);
    CHECK(parent.system_parameter_calls==23 && parent.system_parameter_returns==22);
    CHECK(parent.system_parameter.raw_id==0x111 && parent.system_parameter.output_written);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}
void case_system_sdk_inheritance() {
    for(auto language:{SystemLanguage::EnglishGB,SystemLanguage::PortugueseBR}) for(bool valid:{true,false}) {
        CompRig g; g.seed(); g.obs.query_language=true;
        g.r.primary.system_parameters=system_parameters_fixture();
        g.r.primary.system_parameters.language.language=language;
        g.r.primary.process_sdk={0x30ffff,valid};
        CHECK(g.run_parent());
        const uint32_t expected=valid?(language==SystemLanguage::EnglishGB?1:7):uint32_t(language);
        CHECK(g.obs.language_effective_before==(valid?0x2101u:0x111u));
        CHECK(g.obs.language_value_before==expected);
        CHECK(g.obs.sdk_before.raw==0x30ffff && g.obs.sdk_before.valid==valid);
        CHECK(g.obs.system_parameter_observation_fresh && !g.obs.system_parameter_calls && !g.obs.system_parameter_returns);
        g.r.primary.process_sdk={0x450000,true};
        g.r.send(g.q,1,2,3);
        const auto *child=g.comp->child(0); CHECK(child); arm(10000);
        while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(g.obs.language_effective_after==(valid?0x2101u:0x111u));
        CHECK(g.obs.language_value_after==expected);
        CHECK(g.obs.sdk_after.raw==0x30ffff && g.obs.sdk_after.valid==valid);
        CHECK(!g.r.primary_control->system_parameter_calls && !g.r.primary_control->system_parameter_returns);
        CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
        g.expect_released_baseline();
    }
}
void case_system_profile_inheritance() {
    CompRig g; g.seed(); const auto expected=system_profile_fixture();
    g.r.primary.system_profile=expected;
    auto &parent=*g.r.primary_control;
    parent.system_string_calls=29; parent.system_string_returns=28;
    parent.system_string.has_result=true; parent.system_string.profile_checked=true;
    parent.system_string.source=expected.source; parent.system_string.raw_id=0x131;
    parent.system_string.output_written_bytes=17; parent.system_string.output_write_attempts=18;
    CHECK(g.run_parent());
    CHECK(g.obs.execution_control!=g.r.primary_control.get());
    CHECK(same_system_profile(g.obs.system_profile_before,expected));
    CHECK(!g.obs.system_string_calls && !g.obs.system_string_returns && g.obs.system_string_observation_fresh);
    g.r.primary.system_profile={};
    g.r.send(g.q,1,2,3);
    const auto *child=g.comp->child(0); CHECK(child); arm(10000);
    while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(same_system_profile(g.obs.system_profile_after,expected));
    CHECK(g.r.primary.system_profile.source==SystemProfileSource::Unconfigured);
    CHECK(parent.system_string_calls==29 && parent.system_string_returns==28);
    CHECK(parent.system_string.raw_id==0x131 && parent.system_string.output_written_bytes==17);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}
void case_presentation_topology_inheritance() {
    CompRig g;g.seed();g.obs.query_video_count=true;
    g.r.primary.presentation_topology=PresentationTopology::SingleNativeTarget;
    auto &parent=*g.r.primary_control;parent.video_output_count_calls=37;parent.video_output_count_returns=36;
    parent.video_output_count.raw_video_out=99;parent.video_output_count.inventory_checked=true;
    parent.video_output_count.has_result=true;parent.video_output_count.toc_written=true;parent.video_output_count.publications=17;
    CHECK(g.run_parent());
    CHECK(g.obs.execution_control!=g.r.primary_control.get());
    CHECK(g.obs.topology_before==PresentationTopology::SingleNativeTarget && g.obs.video_observation_fresh);
    CHECK(!g.obs.video_calls_before && !g.obs.video_returns_before);
    g.r.primary.presentation_topology=PresentationTopology::Unconfigured;
    g.r.send(g.q,1,2,3);const auto *child=g.comp->child(0);CHECK(child);arm(10000);
    while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g.obs.topology_after==PresentationTopology::SingleNativeTarget && g.obs.resumed.load());
    CHECK(g.obs.video_count_before==1 && g.obs.video_count_after==1);
    CHECK(g.obs.video_calls_after==2 && g.obs.video_returns_after==2);
    CHECK(parent.video_output_count_calls==37 && parent.video_output_count_returns==36);
    CHECK(parent.video_output_count.raw_video_out==99 && parent.video_output_count.publications==17);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}
void case_disc_change_process_registry() {
    DiscRegistryFixture shared,alternate;const ps3_disc_change_pair initial{1,3},other{5,7};
    shared.replace(initial);alternate.replace(other);
    CompRig g;g.seed();g.obs.use_disc_hle=true;g.r.primary.disc_change_callbacks=&shared.registry;
    auto &parent=*g.r.primary_control;parent.disc_change_calls=31;parent.disc_change_returns=30;
    parent.disc_change.raw_eject=0x123;parent.disc_change.raw_insert=0x456;
    parent.disc_change.registration_attempted=true;parent.disc_change.registration.committed=1;
    parent.disc_change.toc_written=true;parent.disc_change.has_result=true;parent.disc_change.publications=1;
    CHECK(g.run_parent());
    CHECK(g.obs.disc_registry_before==&shared.registry && same_disc_pair(g.obs.disc_pair_before,initial));
    CHECK(g.obs.disc_observation_fresh && !g.obs.disc_calls_before && !g.obs.disc_returns_before);
    CHECK(same_disc_pair(g.obs.disc_registration_before.registration.previous,initial));
    CHECK(same_disc_pair(shared.snapshot(),DISC_ENTRY));
    shared.replace(DISC_PARENT);g.r.primary.disc_change_callbacks=&alternate.registry;
    g.r.send(g.q,1,2,3);const auto *child=g.comp->child(0);CHECK(child);arm(10000);
    while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g.obs.disc_registry_after==&shared.registry && same_disc_pair(g.obs.disc_pair_after,DISC_PARENT));
    CHECK(same_disc_pair(g.obs.disc_registration_after.registration.previous,DISC_PARENT));
    CHECK(g.obs.disc_calls_after==2 && g.obs.disc_returns_after==2);
    CHECK(same_disc_pair(shared.snapshot(),DISC_RESUME) && same_disc_pair(alternate.snapshot(),other));
    CHECK(parent.disc_change_calls==31 && parent.disc_change_returns==30 && parent.disc_change.raw_eject==0x123);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}
// ============ B. configure: explicit validated policy/ports; port only after configure ============
void case_configure()
{
    CHECK(!std::strcmp(startup_child_teardown_name(StartupChildCompositionTeardown::Released), "released"));
    CHECK(!std::strcmp(startup_child_teardown_name(StartupChildCompositionTeardown::StillRunning), "still-running"));
    CHECK(!std::strcmp(startup_child_teardown_name(StartupChildCompositionTeardown::NeverPrepared), "never-prepared"));
    CompRig g(false);
    CHECK(g.comp->create_port() == nullptr);
    const StartupChildPpuPorts good = g.ports();
    const StartupChildCompositionPolicy gp = first_child_policy();
    struct PortRow { const char *name; void (*mutate)(StartupChildPpuPorts &); };
    const PortRow port_rows[] = {
        {"threads", [](StartupChildPpuPorts &p) { p.threads = nullptr; }}, {"queues", [](StartupChildPpuPorts &p) { p.queues = nullptr; }},
        {"memory", [](StartupChildPpuPorts &p) { p.memory = nullptr; }}, {"thread start", [](StartupChildPpuPorts &p) { p.thread.start = nullptr; }},
        {"thread join", [](StartupChildPpuPorts &p) { p.thread.join = nullptr; }}, {"code", [](StartupChildPpuPorts &p) { p.code = nullptr; }},
        {"clock", [](StartupChildPpuPorts &p) { p.clock = nullptr; }}, {"recorder", [](StartupChildPpuPorts &p) { p.recorder = nullptr; }},
        {"budget", [](StartupChildPpuPorts &p) { p.budget = nullptr; }}, {"dispatch limit", [](StartupChildPpuPorts &p) { p.dispatch_limit = 0; }},
        {"caller dispatch stop must be empty", [](StartupChildPpuPorts &p) { static StopCounter sc; p.dispatch_stop = {&sc, stop_requested}; }}};
    for (const PortRow &row : port_rows) {
        StartupChildPpuPorts p = good; row.mutate(p);
        if (g.comp->configure(p, gp) != StartupChildCompositionConfigure::InvalidArgument) std::printf("  port row '%s'\n", row.name);
        CHECK(g.comp->configure(p, gp) == StartupChildCompositionConfigure::InvalidArgument && g.comp->create_port() == nullptr);
    }
    struct PolRow { const char *name; void (*mutate)(StartupChildCompositionPolicy &); };
    const PolRow pol_rows[] = {
        {"priority 3072", [](StartupChildCompositionPolicy &p) { p.priority = 3072; }}, {"creator 3072", [](StartupChildCompositionPolicy &p) { p.creator_priority = 3072; }},
        {"stack 0", [](StartupChildCompositionPolicy &p) { p.stack_size = 0; }}, {"tls 0", [](StartupChildCompositionPolicy &p) { p.tls_capacity = 0; }},
        {"return lr 0", [](StartupChildCompositionPolicy &p) { p.return_lr = 0; }}, {"slack 0", [](StartupChildCompositionPolicy &p) { p.stack_entry_slack = 0; }}};
    for (const PolRow &row : pol_rows) {
        StartupChildCompositionPolicy p = gp; row.mutate(p);
        if (g.comp->configure(good, p) != StartupChildCompositionConfigure::InvalidArgument) std::printf("  policy row '%s'\n", row.name);
        CHECK(g.comp->configure(good, p) == StartupChildCompositionConfigure::InvalidArgument && g.comp->create_port() == nullptr);
    }
    CHECK(g.comp->configure(good, gp) == StartupChildCompositionConfigure::Ok);
    const StartupPpuCreatePort *port = g.comp->create_port();
    CHECK(port && port->context && port->prepare && port->start);
    CHECK(g.comp->configure(good, gp) == StartupChildCompositionConfigure::AlreadyConfigured && g.comp->create_port() == port);
    CHECK(g.comp->report().configured && g.comp->child().status().phase == StartupChildPpuPhase::Empty);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->released());   // nothing prepared
    check_released_baseline(g.r); g.r.check_primary_untouched();
}

// ============ C. the whole path through the real bridge: parent create -> glue -> real child -> actual BLOCKED -> teardown ============
void case_integration_blocked()
{
    CompRig g; g.seed();
    CHECK(g.run_parent());
    auto &cpu = g.r.primary.ppu.cpu;
    CHECK(cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && g.r.primary_control->stop == StartupStop::None);
    const StartupControl &k = *g.r.primary_control;
    StartupChildPpuStatus s = g.comp->child().status();
    const uint32_t id = g.comp->child().id();
    CHECK(id != 0 && k.ppu_create_calls == 1 && k.ppu_create_prepared == 1 && k.ppu_create_id_written == 1 && k.ppu_create_started == 1 && k.ppu_create_returns == 1);
    CHECK(k.ppu_create_last_thread_id == id && k.ppu_create_last_entry == ENTRY_RECEIVE && k.ppu_create_start == StartupPpuCreateStartStatus::Started);
    CHECK(g.get64(OUTA) == id && g.get64(PTOC_SLOT) == TOC);   // the BRIDGE wrote the BE id and the saved TOC
    // request mapping observed from inside the real compiled body (through the real lookup + IndirectCall)
    CHECK(g.obs.entered.load() && g.obs.prefix.load() && !g.obs.resumed.load());
    CHECK(g.obs.r1 == uint64_t(s.storage.stack_address) + G64 - 0x70 && g.obs.r2 == CHILD_TOC && g.obs.r3 == OUTA && g.obs.r4 == 0);
    CHECK(g.obs.lr == LR && g.obs.ctr == ENTRY_RECEIVE && g.obs.r13 == s.tls.thread_pointer && g.obs.cpu_thread_id == id);
    CHECK(g.obs.id_read == PS3_GUEST_MEMORY_OK && be64(g.obs.id_bytes) == id);   // id was in guest memory BEFORE the child ran
    // storage + TLS (copied from the primary snapshot) on the shared allocator
    CHECK(s.storage.allocation_bytes == 0x20000 && s.storage.stack_region_bytes == G64 && s.storage.tls_capacity_bytes == G64);
    CHECK(g.r.used() == 0x20000 && g.r.allocations() == 1);
    unsigned char image[4]; g.r.get(s.tls.block, image, 4);
    CHECK(image[0] == 0x11 && image[1] == 0x22 && image[2] == 0x33 && image[3] == 0x44);
    // ACTUAL blocked state, with the real queue and entry PC
    CHECK(ps3_ppu_service_is_blocked(&g.r.ppu, s.ref));
    CHECK(s.phase == StartupChildPpuPhase::Started && s.receive_calls == 1 && s.last_queue == g.q && s.last_pc == ENTRY_RECEIVE && s.terminal_calls == 0);
    const StartupChildCompositionReport &pre = g.comp->report();
    CHECK(pre.configured && pre.prepare_calls == 1 && pre.start_calls == 1 && pre.prepare_attempted && pre.start_attempted && !pre.start_skipped_by_cancel_latch);
    CHECK(pre.prepare.status == StartupChildPpuPrepareStatus::Ok && pre.mapped_prepare == StartupPpuCreatePrepareStatus::Prepared && pre.prepared_id == id);
    CHECK(pre.start.status == StartupChildPpuStartStatus::Blocked && pre.observed_blocked && pre.mapped_start == StartupPpuCreateStartStatus::Started);
    CHECK(!pre.teardown_ran && g.c->thread.start_calls.load() == 1 && g.c->thread.join_calls.load() == 0);
    // teardown AFTER the parent join: latches asserted, diagnostics captured BEFORE cancel, bounded reap (real exit gate), one host join
    const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
    CHECK(rep.teardown == StartupChildCompositionTeardown::Released && g.comp->released() && rep.teardown_ran && rep.latches_asserted_by_teardown);
    CHECK(!rep.stop_requested_by_watchdog);
    CHECK(rep.before.phase == StartupChildPpuPhase::Started && rep.before.last_queue == g.q && rep.before.last_pc == ENTRY_RECEIVE && rep.before.receive_calls == 1);
    CHECK(rep.before.terminal_calls == 0 && !rep.before.cancel_requested && rep.before.consumed_events == 0);
    CHECK(rep.reap.status == StartupChildPpuReapStatus::Released && rep.reap_calls >= 1);
    CHECK(rep.after.terminal_calls == 1 && rep.after.stop == StartupStop::Cancelled && rep.after.cancel_requested && rep.after.consumed_events == 0);
    CHECK(rep.after.fault == StartupChildPpuReceiveFault::Cancelled && !g.obs.resumed.load());   // honest cancel terminal: no event invented
    CHECK(rep.after.phase == StartupChildPpuPhase::Released && !rep.after.has_resources && rep.after.exit_code == uint32_t(StartupStop::Cancelled));
    CHECK(g.c->thread.join_calls.load() == 1 && g.c->thread.join_ok.load() == 1);
    g.expect_released_baseline();
    CHECK(g.r.primary_control->stop == StartupStop::None);   // the primary was never touched by the child
}

// ============ D. guest request policy (real bridge): mismatch -> UnsupportedSyscall, child stays Empty, nothing effected ============
void case_policy_mismatch()
{
    struct Row { const char *name; void (*mutate)(CompRig &); };
    const Row rows[] = {
        {"priority", [](CompRig &g) { g.r.primary.ppu.cpu.gpr[6] = 100; }},
        {"stack", [](CompRig &g) { g.r.primary.ppu.cpu.gpr[7] = 0x8000; }},
        {"creator priority", [](CompRig &g) { g.r.primary.guest_priority = 251; }}};
    for (const Row &row : rows) {
        CompRig g; g.seed(); row.mutate(g);
        const bool returned = g.run_parent();
        if (returned || g.r.primary_control->stop != StartupStop::UnsupportedSyscall) std::printf("  row '%s' returned=%d stop=%d\n", row.name, int(returned), int(g.r.primary_control->stop));
        CHECK(!returned && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
        const StartupChildCompositionReport &rep = g.comp->report();
        CHECK(rep.prepare_calls == 1 && !rep.prepare_attempted && rep.start_calls == 0 && !rep.start_attempted);
        CHECK(rep.mapped_prepare == StartupPpuCreatePrepareStatus::Unsupported);
        CHECK(g.comp->child().status().phase == StartupChildPpuPhase::Empty && !g.comp->child().status().has_resources);
        CHECK(g.get64(OUTA) == OUT_SENTINEL && g.get64(PTOC_SLOT) == TOC_SENTINEL);   // the bridge wrote nothing
        CHECK(g.c->thread.start_calls.load() == 0 && g.r.used() == 0 && g.r.allocations() == 0);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared);
        g.expect_released_baseline();
    }
    {   // unknown entry: the bridge's lookup rejects BEFORE the glue sees prepare
        CompRig g; g.seed(); g.put32(OPDA, ENTRY_RECEIVE + 4);
        CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::DispatchFailure && g.comp->report().prepare_calls == 0);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared);
        g.expect_released_baseline();
    }
    {   // ONE child only: the second create is Unsupported, the first child is untouched and still really blocked
        CompRig g; g.seed();
        CHECK(g.run_parent());
        const uint32_t id = g.comp->child().id();
        g.seed(); g.put64(OUTA, OUT_SENTINEL);
        CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
        const StartupChildCompositionReport &rep = g.comp->report();
        CHECK(rep.prepare_calls == 2 && rep.start_calls == 1 && g.comp->child().id() == id && g.get64(OUTA) == OUT_SENTINEL);
        CHECK(ps3_ppu_service_is_blocked(&g.r.ppu, g.comp->child().status().ref) && g.r.used() == 0x20000 && g.c->thread.start_calls.load() == 1);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
        g.expect_released_baseline();
    }
}

// ============ E. latches: request_stop is async-only; the owner skips start; prepare refuses after stop ============
void case_stop_latches()
{
    {   // stop BETWEEN prepare and start: start is skipped (no host start), the reservation is abandoned by teardown
        CompRig g; const StartupPpuCreatePort *port = g.comp->create_port();
        const StartupPpuCreateRequest req = g.hand_request(); const Ps3TlsTemplate tls = g.tls_template();
        const StartupPpuCreatePrepareResult p = port->prepare(port->context, &req, &tls);
        CHECK(p.status == StartupPpuCreatePrepareStatus::Prepared && p.thread_id != 0 && g.r.used() == 0x20000);
        g.comp->request_stop();
        CHECK(port->start(port->context, p.thread_id) == StartupPpuCreateStartStatus::ProviderFailure);
        const StartupChildCompositionReport &rep = g.comp->report();
        CHECK(rep.start_skipped_by_cancel_latch && !rep.start_attempted && rep.start_calls == 1 && rep.stop_requested_by_watchdog);
        CHECK(g.c->thread.start_calls.load() == 0 && g.comp->child().status().phase == StartupChildPpuPhase::Reserved && g.r.used() == 0x20000);   // retained until teardown
        CHECK(!g.comp->released() || rep.teardown == StartupChildCompositionTeardown::NeverPrepared);
        const StartupChildCompositionReport &done = g.comp->teardown(g.td());
        CHECK(done.teardown == StartupChildCompositionTeardown::Released && done.latches_asserted_by_teardown && g.c->thread.start_calls.load() == 0 && g.c->thread.join_calls.load() == 0);
        g.expect_released_baseline();
    }
    {   // stop BEFORE prepare: nothing is reserved at all
        CompRig g; g.comp->request_stop();
        const StartupPpuCreatePort *port = g.comp->create_port();
        const StartupPpuCreateRequest req = g.hand_request(); const Ps3TlsTemplate tls = g.tls_template();
        CHECK(port->prepare(port->context, &req, &tls).status == StartupPpuCreatePrepareStatus::ProviderFailure);
        CHECK(!g.comp->report().prepare_attempted && g.comp->child().status().phase == StartupChildPpuPhase::Empty && g.r.used() == 0);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared);
        g.expect_released_baseline();
    }
    {   // start with a foreign id / without prepare never reaches the child; null tls snapshot is refused
        CompRig g; const StartupPpuCreatePort *port = g.comp->create_port();
        CHECK(port->start(port->context, 7) == StartupPpuCreateStartStatus::ProviderFailure && !g.comp->report().start_attempted);
        const StartupPpuCreateRequest req = g.hand_request();
        CHECK(port->prepare(port->context, &req, nullptr).status == StartupPpuCreatePrepareStatus::ProviderFailure && !g.comp->report().prepare_attempted);
        const Ps3TlsTemplate tls = g.tls_template();
        const StartupPpuCreatePrepareResult p = port->prepare(port->context, &req, &tls);
        CHECK(p.status == StartupPpuCreatePrepareStatus::Prepared);
        CHECK(port->start(port->context, p.thread_id + 1) == StartupPpuCreateStartStatus::ProviderFailure && !g.comp->report().start_attempted);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
        g.expect_released_baseline();
    }
}

// ============ F. terminal BEFORE block: fail closed, resources retained until teardown ============
void case_terminal_before_block()
{
    CompRig g(true, false); g.seed(); g.put32(OPDA, ENTRY_RETURN);
    CHECK(!g.run_parent());
    const StartupControl &k = *g.r.primary_control;
    CHECK(k.stop == StartupStop::ProviderFailure && k.ppu_create_start == StartupPpuCreateStartStatus::Terminal && k.ppu_create_returns == 0);
    CHECK(g.get64(OUTA) == g.comp->child().id());   // the id was written before start; stopping never rolls it back
    const StartupChildCompositionReport &pre = g.comp->report();
    CHECK(pre.start_attempted && pre.start.status == StartupChildPpuStartStatus::Returned && pre.start.stop == StartupStop::Returned);
    CHECK(!pre.observed_blocked && pre.mapped_start == StartupPpuCreateStartStatus::Terminal);
    CHECK(g.r.used() == 0x20000 && g.comp->child().status().has_resources && !g.comp->released());   // retained, not freed behind the caller's back
    const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
    CHECK(rep.teardown == StartupChildCompositionTeardown::Released && rep.before.terminal_calls >= 1 && rep.after.stop == StartupStop::Returned);
    CHECK(g.c->thread.join_calls.load() == 1);
    g.expect_released_baseline();
}

// ============ G. bounded teardown: StillRunning is polled finitely (never a join); *Retained is retried finitely ============
void case_teardown_bounds()
{
    {   // running compiled code ignores cancel until the gate opens: first teardown gives up after EXACTLY max polls, second finishes
        CompRig g; g.seed(); g.put32(OPDA, ENTRY_GATED);
        CHECK(g.run_parent());
        g.r.send(g.q, 0x61, 0x62, 0x63);
        arm(10000); while (!g.obs.gate_reached.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const StartupChildCompositionReport &a = g.comp->teardown(g.td(2, 0));
        CHECK(a.teardown == StartupChildCompositionTeardown::StillRunning && !g.comp->released() && a.reap.status == StartupChildPpuReapStatus::StillRunning);
        CHECK(a.reap_calls == 3 && a.still_running_polls == 2 && g.polls_counter.load() == 2 && g.c->thread.join_calls.load() == 0);
        CHECK(a.before.phase == StartupChildPpuPhase::Started && a.after.has_resources && g.r.used() == 0x20000);   // everything retained
        StartupChildCompositionTeardownPolicy open = g.td(2000, 0); open.context = &g.obs; open.poll = poll_open_gate;
        const StartupChildCompositionReport &b = g.comp->teardown(open);
        CHECK(b.teardown == StartupChildCompositionTeardown::Released && b.reap_calls > 3 && !g.obs.gate_timeout.load() && !g.obs.second_returned.load());
        CHECK(b.after.stop == StartupStop::Cancelled && b.after.consumed_events == 1 && b.after.first_consumed.data1 == 0x61);
        CHECK(b.before.cancel_requested == false && g.c->thread.join_calls.load() == 1);   // `before` is the FIRST capture, kept across re-calls
        g.expect_released_baseline();
    }
    {   // join failure once: no retries -> retained (not released); then retried -> released
        CompRig g; g.seed(); CHECK(g.run_parent());
        g.c->thread.fail_join = 1;
        const StartupChildCompositionReport &a = g.comp->teardown(g.td(2000, 0));
        CHECK(a.teardown == StartupChildCompositionTeardown::ServiceRetained && !g.comp->released() && a.after.has_resources && g.r.used() == 0x20000);
        CHECK(g.comp->teardown(g.td(2000, 1)).teardown == StartupChildCompositionTeardown::Released);
        CHECK(g.c->thread.join_calls.load() == 2 && g.c->thread.join_ok.load() == 1);
        g.expect_released_baseline();
    }
    {   // decommit failure once, retried inside ONE teardown call: exactly one stage retry, one host join
        CompRig g; g.seed(); CHECK(g.run_parent());
        g.r.spy.fail_decommit.store(1);
        const StartupChildCompositionReport &a = g.comp->teardown(g.td(2000, 2));
        CHECK(a.teardown == StartupChildCompositionTeardown::Released && a.stage_retries == 1 && g.c->thread.join_calls.load() == 1);
        g.expect_released_baseline();
    }
}

// ============ H. watchdog latches while the parent runs (TSan lane): every interleaving still tears down cleanly ============
void case_watchdog_race()
{
    unsigned blocked = 0, stopped = 0;
    for (unsigned i = 0; i < 10; ++i) {
        CompRig g; g.seed();
        std::thread parent = g.spawn_parent();
        std::this_thread::sleep_for(std::chrono::microseconds(150 * i));
        g.comp->request_stop();   // ONLY the two prepublished atomics; never an owner method
        parent.join();
        const StartupChildCompositionReport &pre = g.comp->report();
        CHECK(pre.stop_requested_by_watchdog);
        CHECK(g.parent_returned == (pre.mapped_start == StartupPpuCreateStartStatus::Started));
        if (g.parent_returned) { CHECK(pre.observed_blocked); ++blocked; } else ++stopped;
        const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
        // stop BEFORE prepare leaves nothing reserved (NeverPrepared); every other interleaving must end Released
        CHECK((rep.teardown == StartupChildCompositionTeardown::Released || rep.teardown == StartupChildCompositionTeardown::NeverPrepared) && rep.latches_asserted_by_teardown);
        g.expect_released_baseline();
        CHECK(g.c->thread.join_calls.load() == (g.c->thread.start_calls.load() ? 1u : 0u));
    }
    std::printf("watchdog interleavings: blocked=%u stopped=%u\n", blocked, stopped);
}

// ============ I. G-ALLOC: production-shaped allocator (plain ps3_atomic_domain_memory, NO spy) end to end through the glue ============
// This proves the production allocator shape (owners[8], pins[128], plain domain memory) supports the full child lifecycle. It is NOT a
// discriminator for 'backing.commit alone' (a mutant that bypasses the domain also passes single-threaded: commit/decommit differ only in
// pin-BUSY exclusion and range invalidation). The coherence fact is a source fact: ps3_atomic_domain_memory().commit == ps3_atomic_commit.
struct ProdRig {
    ElfTestHostSpace *space = elf_test_host_space_create(HOST_SIZE);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    ps3_pthread_guard g_dom, g_mem, g_lv2, g_ppu;
    ps3_pthread_wait_port port;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[8]{};      // production sizes (test_startup_indirect.cpp)
    ps3_atomic_pin_record pins[128]{};
    ps3_user_memory memory{};
    ps3_user_memory_slot slots[64]{};
    ps3_lv2_objects lv2{};
    ps3_ppu_threads ppu{};
    PpuMemoryRamRange ranges[2]{{SLOT_PAGE, 0x1000, true}, {WINDOW, WINDOW_END - WINDOW, true}};
    std::unique_ptr<PpuMemoryBinding> binding;
    ps3_clock clock{nullptr, clock_now};
    ps3_compiled_code_lookup lookup{nullptr, comp_find};
    std::unique_ptr<ChildRig> child{new ChildRig};
    std::unique_ptr<StartupChildComposition> comp{new StartupChildComposition};
    ProdRig() {
        CHECK(space && backing.commit(backing.context, 0, HOST_SIZE) == PS3_GUEST_MEMORY_OK);
        CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) &&
              !ps3_pthread_guard_init(&g_ppu) && !ps3_pthread_wait_port_init(&port));
        CHECK(ps3_atomic_domain_init(&domain, backing, ps3_pthread_guard_port(&g_dom), owners, 8, pins, 128) == PS3_GUEST_MEMORY_OK);
        const ps3_guest_memory coherent = ps3_atomic_domain_memory(&domain);   // EXACTLY what the production allocator is configured with
        // (its commit/decommit ARE ps3_atomic_commit/decommit: atomic_domain.c port_commit/port_decommit; see ROOT note in HANDOFF)
        const ps3_user_memory_config mc{WINDOW, WINDOW_END, QUOTA, coherent, ps3_pthread_guard_port(&g_mem)};
        CHECK(ps3_user_memory_init(&memory, &mc, slots, 64) == PS3_USER_MEMORY_OK);
        CHECK(ps3_lv2_objects_init(&lv2, ps3_pthread_guard_port(&g_lv2), ps3_pthread_wait_port_port(&port)) == PS3_LV2_OK);
        CHECK(ps3_ppu_threads_init(&ppu, ps3_pthread_guard_port(&g_ppu), ps3_pthread_wait_port_port(&port), &memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_adopt_main(&ppu, 1) == PS3_PPU_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges, 2));
        CHECK(ps3_atomic_commit(&domain, SLOT_PAGE, 0x1000) == PS3_GUEST_MEMORY_OK);
        const unsigned char src[4]{0x11, 0x22, 0x33, 0x44};
        CHECK(binding->coherent_memory.write(binding->coherent_memory.context, SRC, src, 4) == PS3_GUEST_MEMORY_OK);
        CHECK(child->recorder.configure(child->events.data(), child->events.size(), PpuMemoryTraceRetention::Strict, 0));
        child->budget_clock = ps3_clock{&child->budget_time, budget_clock_now};
        child->budget.reset(new DiscoveryBudgetBinding(child->budget_policy, child->budget_clock));
        child->thread.real = {};
    }
    ~ProdRig() {
        binding.reset();
        CHECK(ps3_user_memory_cleanup(&memory).status == PS3_USER_MEMORY_OK);
        ps3_pthread_wait_port_destroy(&port);
        ps3_pthread_guard_destroy(&g_dom); ps3_pthread_guard_destroy(&g_mem); ps3_pthread_guard_destroy(&g_lv2); ps3_pthread_guard_destroy(&g_ppu);
        elf_test_host_space_destroy(space);
    }
};
void case_alloc_production_shape()
{
    ProdRig g; ChildRig &c = *g.child;
    g_comp_obs = &c.obs; clear_obs(); c.obs.queue = 0;
    StartupChildPpuPorts p;
    p.threads = &g.ppu; p.queues = &g.lv2; p.memory = g.binding.get(); p.thread = {&c.thread, thread_start, thread_join};
    p.code = &g.lookup; p.clock = &g.clock; p.recorder = &c.recorder; p.budget = c.budget.get(); p.dispatch_limit = 8;
    CHECK(g.comp->configure(p, first_child_policy()) == StartupChildCompositionConfigure::Ok);
    const StartupPpuCreatePort *port = g.comp->create_port();
    StartupPpuCreateRequest req; req.opd_address = OPDA; req.entry = ENTRY_RETURN; req.toc = CHILD_TOC; req.argument = OUTA; req.priority = int32_t(PRIORITY);
    req.stack_size = uint32_t(G64); req.flags = 1; req.output = OUTA; req.creator_thread_id = 1; req.creator_priority = int32_t(CREATOR);
    const Ps3TlsTemplate tls{SRC, 4, 0x200};
    const StartupPpuCreatePrepareResult prepared = port->prepare(port->context, &req, &tls);
    // prepare writes the stack/TLS THROUGH the coherent binding: it can only succeed if the allocator commit is domain-visible
    CHECK(prepared.status == StartupPpuCreatePrepareStatus::Prepared);
    const StartupChildPpuStatus s = g.comp->child().status();
    CHECK(s.storage.allocation_bytes == 0x20000);
    unsigned char image[4]{}; CHECK(g.binding->coherent_memory.read(g.binding->coherent_memory.context, s.tls.block, image, 4) == PS3_GUEST_MEMORY_OK);
    CHECK(image[0] == 0x11 && image[3] == 0x44);
    unsigned char probe[16]{}; CHECK(g.binding->coherent_memory.read(g.binding->coherent_memory.context, s.storage.stack_address, probe, 16) == PS3_GUEST_MEMORY_OK);
    const uint32_t stack_address = s.storage.stack_address, allocation = s.storage.allocation_address;
    const unsigned char be[8]{0, 0, 0, 0, uint8_t(prepared.thread_id >> 24), uint8_t(prepared.thread_id >> 16), uint8_t(prepared.thread_id >> 8), uint8_t(prepared.thread_id)};
    CHECK(g.binding->coherent_memory.write(g.binding->coherent_memory.context, OUTA, be, 8) == PS3_GUEST_MEMORY_OK);
    CHECK(port->start(port->context, prepared.thread_id) == StartupPpuCreateStartStatus::Terminal);   // ENTRY_RETURN returns without a receive
    StartupChildCompositionTeardownPolicy t; std::atomic<unsigned> polls{0}; t.context = &polls; t.poll = poll_sleep; t.max_reap_polls = 2000; t.max_stage_retries = 2;
    CHECK(g.comp->teardown(t).teardown == StartupChildCompositionTeardown::Released);
    // release went through the SAME domain: the freed range is no longer visible, the quota is back, no allocation remains
    CHECK(g.binding->coherent_memory.read(g.binding->coherent_memory.context, stack_address, probe, 16) != PS3_GUEST_MEMORY_OK);
    ps3_user_memory_info info; CHECK(ps3_user_memory_query(&g.memory, &info) == PS3_USER_MEMORY_OK && info.used_bytes == 0 && info.allocation_count == 0);
    CHECK(allocation == stack_address);   // storage layout: allocation base is the stack base
    CHECK(ps3_ppu_threads_live(&g.ppu) == 0);
    clear_obs(); g_comp_obs = nullptr;
}

// ====================================================================================================================================
// TWO-SLOT composition (bounded 2 actual children). Same real collaborators as above; no fake event, readiness or errno anywhere.
// Both compiled fixtures are LABEL bodies that adopt their observation record from the guest ARGUMENT (== the output address), so
// two helpers run the SAME entry/TOC exactly like the target and are told apart only by what the guest passed.
// HOST fixture evidence only: it says nothing about the original game, the Switch target or hardware execution.
// ====================================================================================================================================
constexpr uint32_t OUTB = SLOT_PAGE + 0x140, OUTC = SLOT_PAGE + 0x180, OPDB = SLOT_PAGE + 0x240,
    OUTD = SLOT_PAGE + 0x1C0, OPDC = SLOT_PAGE + 0x2C0;   // third slot (three-slot fixture)
constexpr uint64_t ARG_REFUSED = UINT64_C(0x00000000007D0228);   // deliberately != its output address (refusal diagnostics)
Obs *g_comp2_obs[2] = {nullptr, nullptr};
void adopt2(ppu_context *cpu)
{
    // Slot 2's argument (OUTD) is only used by the three-slot fixture, which registers its own third-slot body; a blocking slot-2
    // fixture reuses this one and adopts slot 1's record so that a real receive can be driven there.
    const unsigned k = cpu->gpr[3] == OUTB || cpu->gpr[3] == OUTD ? 1 : 0;
    if (cpu->gpr[3] != OUTA && cpu->gpr[3] != OUTB && cpu->gpr[3] != OUTD) { std::printf("CHILD_PPU_FAIL unexpected child argument\n"); std::fflush(stdout); _exit(1); }
    Obs *o = g_comp2_obs[k]; o->thread_id = uint32_t(cpu->thread_id); g_obs[k] = o;
}
void comp2_receive(ppu_context *cpu) { adopt2(cpu); body_receive(cpu); }
void comp2_return(ppu_context *cpu) { adopt2(cpu); body_return(cpu); }
void comp2_gated(ppu_context *cpu) { adopt2(cpu); body_gated(cpu); }
// Stop-latch probe: real block + real wake, then compiled code that waits at a gate and finally performs a REAL indirect call (the
// production dispatch boundary that polls the slot's dispatch-stop port). Continue -> second_returned; latched -> the child stops Cancelled.
constexpr uint32_t ENTRY_NESTED = 0x00500400, ENTRY_STOP_PROBE = 0x00500500;
// decision 14: the two process-wide syscalls a child may now reach. Distinct addresses from every other fixture in this file.
constexpr uint32_t ENTRY_PROC_TZ = 0x00500A00, ENTRY_PROC_ALLOC = 0x00500B00;
constexpr int32_t PROC_TZ_MINUTES = -60, PROC_TZ_SUMMER_MINUTES = 30;
void comp2_proc_tz(ppu_context *cpu)
{
    adopt2(cpu);
    Obs *o = obs_for(cpu->thread_id);
    o->entered.store(true);
#ifdef PS3_NATIVE_STARTUP_TIMEZONE
    const uint32_t tz_out = uint32_t(cpu->gpr[1] + 0x80), sum_out = uint32_t(cpu->gpr[1] + 0x84);
    cpu->gpr[11] = 144; cpu->gpr[3] = tz_out; cpu->gpr[4] = sum_out;
    lv2_syscall(cpu);
    o->r3 = cpu->gpr[3];
#endif
    cpu->gpr[3] = 0; ps3_hle_call(0xaff080a4, cpu);   // the run-to-exit slot's real end (never returns)
}
void comp2_proc_alloc(ppu_context *cpu)
{
    adopt2(cpu);
    Obs *o = obs_for(cpu->thread_id);
    o->entered.store(true);
    const uint32_t out = uint32_t(cpu->gpr[1] + 0x80);
    cpu->gpr[11] = 348; cpu->gpr[3] = 0x10000; cpu->gpr[4] = 0x200; cpu->gpr[5] = out;
    lv2_syscall(cpu);
    o->r3 = cpu->gpr[3];
    cpu->gpr[3] = 0; ps3_hle_call(0xaff080a4, cpu);   // the run-to-exit slot's real end (never returns)
}
void comp2_nested(ppu_context *) {}
void comp2_stop_probe(ppu_context *cpu)
{
    adopt2(cpu); body_receive(cpu);
    Obs *o = obs_for(cpu->thread_id);
    o->gate_reached.store(true);
    for (int i = 0; i < 5000 && !o->gate_open.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!o->gate_open.load()) o->gate_timeout.store(true);
    cpu->ctr = ENTRY_NESTED; ps3_indirect_call(cpu);
    o->second_returned.store(true);
}
const CodeEntry comp2_entries[] = {{ENTRY_RECEIVE, comp2_receive}, {ENTRY_RETURN, comp2_return}, {ENTRY_GATED, comp2_gated},
    {ENTRY_NESTED, comp2_nested}, {ENTRY_STOP_PROBE, comp2_stop_probe},
    {ENTRY_PROC_TZ, comp2_proc_tz}, {ENTRY_PROC_ALLOC, comp2_proc_alloc}};
ps3_compiled_code_status comp2_find(const void *, uint32_t address, ps3_compiled_function *target)
{
    for (const CodeEntry &e : comp2_entries) if (e.address == address) { *target = e.function; return PS3_COMPILED_CODE_OK; }
    return PS3_COMPILED_CODE_NOT_FOUND;
}

// Wraps one real ThreadRig as the slot's service-thread port and records, AT THE REAL JOIN BOUNDARY, which children the owner had already
// cancelled. A composition that reaps slot 0 before cancelling slot 1 shows slot 1 un-cancelled here. Called on the owner thread only.
struct JoinProbe {
    ThreadRig *thread = nullptr;
    StartupChildComposition *comp = nullptr;
    unsigned cancelled_mask_at_first_join = 0;
    bool joined = false;
};
int probe_start(void *c, void (*entry)(void *), void *argument) { return thread_start(static_cast<JoinProbe *>(c)->thread, entry, argument); }
int probe_join(void *c)
{
    JoinProbe *p = static_cast<JoinProbe *>(c);
    if (!p->joined) {
        p->joined = true;
        for (unsigned i = 0; i < 2; ++i) if (p->comp->child(i)->status().cancel_requested) p->cancelled_mask_at_first_join |= 1u << i;
    }
    return thread_join(p->thread);
}

struct Comp2Opt {
    uint64_t quota = QUOTA;
    bool configure = true, slot1_recorder = true;
    uint32_t slot1_priority = PRIORITY;
};
struct Comp2Rig {
    Rig r;
    std::unique_ptr<StartupChildComposition> comp{new StartupChildComposition};
    ChildRig *c[2]{};
    JoinProbe probe[2];
    StartupChildSlotBinding bind[2];
    uint32_t q[2]{};
    const uint32_t out[2]{OUTA, OUTB};
    bool parent_returned = false;
    std::atomic<unsigned> polls_counter{0};
    explicit Comp2Rig(const Comp2Opt &opt = Comp2Opt()) : r(opt.quota) {
        clear_obs();
        r.lookup.find = comp2_find;
        q[0] = r.queue_create(); q[1] = r.queue_create();
        c[0] = &r.new_child(q[0]); c[1] = &r.new_child(q[1], opt.slot1_recorder);
        g_comp2_obs[0] = &c[0]->obs; g_comp2_obs[1] = &c[1]->obs;
        r.primary.has_guest_priority = true; r.primary.guest_priority = int32_t(CREATOR);
        r.primary_control->initialized_tls_template = {SRC, 4, 0x200}; r.primary_control->initialized_tls_template_valid = true;
        put32(OPDA, ENTRY_RECEIVE); put32(OPDA + 4, CHILD_TOC); put32(OPDB, ENTRY_RECEIVE); put32(OPDB + 4, CHILD_TOC);
        r.put(NAMEA, NAME_TEXT, sizeof NAME_TEXT);
        put64(OUTA, OUT_SENTINEL); put64(OUTB, OUT_SENTINEL); put64(OUTC, OUT_SENTINEL); put64(PTOC_SLOT, TOC_SENTINEL);
        for (unsigned k = 0; k < 2; ++k) { probe[k].thread = &c[k]->thread; probe[k].comp = comp.get(); }
        if (opt.configure) {
            CHECK(comp->configure(shared(), bindings(opt.slot1_priority), 2) == StartupChildCompositionConfigure::Ok);
            bind[0] = StartupChildSlotBinding(); bind[1] = StartupChildSlotBinding();   // the composition COPIED them: the caller array is dead now
            r.primary.ppu_create = comp->create_port();
        }
    }
    ~Comp2Rig() { clear_obs(); g_comp2_obs[0] = g_comp2_obs[1] = nullptr; }
    StartupChildPpuPorts shared() { StartupChildPpuPorts p = r.ports(*c[0]); p.thread = {}; p.recorder = nullptr; p.budget = nullptr; p.dispatch_stop = {}; return p; }
    StartupChildCompositionPolicy policy(uint32_t priority = PRIORITY) { StartupChildCompositionPolicy p = first_child_policy(); p.priority = priority; return p; }
    const StartupChildSlotBinding *bindings(uint32_t slot1_priority = PRIORITY) {
        for (unsigned k = 0; k < 2; ++k) bind[k] = {{&probe[k], probe_start, probe_join}, &c[k]->recorder, c[k]->budget.get(), policy(k ? slot1_priority : PRIORITY)};
        return bind;
    }
    void put32(uint32_t a, uint32_t v) { unsigned char b[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; r.put(a, b, 4); }
    void put64(uint32_t a, uint64_t v) { unsigned char b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i)); r.put(a, b, 8); }
    uint64_t get64(uint32_t a) { unsigned char b[8]; r.get(a, b, 8); return be64(b); }
    // Registers the import stub would seed for one sys_ppu_thread_create call (r3 = id output, r4 = OPD, r5 = argument).
    void seed_raw(uint32_t output, uint32_t opd, uint64_t argument, uint64_t priority = PRIORITY) {
        auto &cpu = r.primary.ppu.cpu;
        for (int i = 3; i < 32; ++i) if (i != 13) cpu.gpr[i] = UINT64_C(0x1111000000000000) + uint64_t(i);
        cpu.gpr[1] = PSTACK; cpu.gpr[2] = TOC; cpu.gpr[3] = output; cpu.gpr[4] = opd; cpu.gpr[5] = argument; cpu.gpr[6] = priority; cpu.gpr[7] = G64;
        cpu.gpr[8] = 1; cpu.gpr[9] = NAMEA;
    }
    void seed(unsigned k, uint64_t priority = PRIORITY) { seed_raw(out[k], k ? OPDB : OPDA, out[k], priority); }
    void parent_call() {   // ONE real HLE call on the calling (parent) host thread
        auto &cpu = r.primary.ppu.cpu;
        r.primary_control->stop = StartupStop::None;
        active = r.primary_control.get(); r.primary_control->cpu = &cpu; g_trampoline_fn = [](void *) {};
        try { ps3_hle_call(NID_CREATE, &cpu); parent_returned = true; active = nullptr; } catch (const Terminal &) { parent_returned = false; }
        g_trampoline_fn = nullptr;
    }
    bool run_parent() { std::thread t([this] { parent_call(); }); t.join(); return parent_returned; }
    bool create(unsigned k, uint64_t priority = PRIORITY) { seed(k, priority); return run_parent(); }
    StartupChildCompositionTeardownPolicy td(unsigned polls = 2000, unsigned retries = 3) {
        StartupChildCompositionTeardownPolicy p; p.context = &polls_counter; p.poll = poll_sleep; p.max_reap_polls = polls; p.max_stage_retries = retries; return p;
    }
    StartupChildPpuStatus st(unsigned k) { return comp->child(k)->status(); }
    uint32_t id(unsigned k) { return comp->child(k)->id(); }
    bool blocked(unsigned k) { return ps3_ppu_service_is_blocked(&r.ppu, st(k).ref); }
    bool exited(unsigned k) {
        const StartupChildPpuStatus s = st(k);
        return s.terminal_calls >= 1 && __atomic_load_n(&r.ppu.services[s.ref.slot].activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_EXITED;
    }
    void wait_exited(unsigned k) { arm(10000); while (!exited(k)) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    StartupPpuCreateRequest hand_request(unsigned k, int32_t priority = int32_t(PRIORITY)) {
        StartupPpuCreateRequest q2;
        q2.opd_address = k ? OPDB : OPDA; q2.entry = ENTRY_RECEIVE; q2.toc = CHILD_TOC; q2.argument = out[k]; q2.priority = priority; q2.stack_size = uint32_t(G64); q2.flags = 1;
        q2.output = out[k]; q2.name_address = NAMEA; q2.name_length = uint32_t(sizeof NAME_TEXT - 1); std::memcpy(q2.name, NAME_TEXT, sizeof NAME_TEXT);
        q2.creator_thread_id = 1; q2.creator_priority = int32_t(CREATOR);
        return q2;
    }
    // Hand-driven bridge sequence for slot k's request: prepare, then the BRIDGE's id write. start() is left to the caller.
    StartupPpuCreatePrepareResult hand_prepare(unsigned k, int32_t priority = int32_t(PRIORITY)) {
        const StartupPpuCreatePort *port = comp->create_port(); const StartupPpuCreateRequest req = hand_request(k, priority); const Ps3TlsTemplate tls{SRC, 4, 0x200};
        const StartupPpuCreatePrepareResult p = port->prepare(port->context, &req, &tls);
        if (p.status == StartupPpuCreatePrepareStatus::Prepared) put64(out[k], p.thread_id);
        return p;
    }
    StartupPpuCreateStartStatus hand_start(uint32_t thread_id) { const StartupPpuCreatePort *port = comp->create_port(); return port->start(port->context, thread_id); }
    void expect_released_baseline() { CHECK(comp->released()); check_released_baseline(r); }
};
// Every slot's cancel was issued before any slot's first reap (sequence numbers of the composition's own teardown).
bool cancels_before_reaps(Comp2Rig &g)
{
    unsigned max_cancel = 0, min_reap = ~0u;
    for (unsigned k = 0; k < 2; ++k) {
        const StartupChildCompositionReport &x = g.comp->report(k);
        if (x.cancel_sequence > max_cancel) max_cancel = x.cancel_sequence;
        if (x.first_reap_sequence && x.first_reap_sequence < min_reap) min_reap = x.first_reap_sequence;
    }
    return max_cancel != 0 && min_reap != ~0u && max_cancel < min_reap;
}
bool disjoint(const StartupChildPpuStatus &a, const StartupChildPpuStatus &b)
{
    return uint64_t(a.storage.allocation_address) + a.storage.allocation_bytes <= b.storage.allocation_address ||
           uint64_t(b.storage.allocation_address) + b.storage.allocation_bytes <= a.storage.allocation_address;
}

// ============ J. configure(shared, bindings, count): bounded 1..3, explicit per-slot bindings, atomic refusal ============
void case_configure2()
{
    Comp2Rig g((Comp2Opt{QUOTA, false, true, PRIORITY}));
    CHECK(g.comp->create_port() == nullptr && g.comp->slot_count() == 0 && STARTUP_CHILD_COMPOSITION_MAX_SLOTS == 3);
    const StartupChildPpuPorts good = g.shared();
    const StartupChildSlotBinding *gb = g.bindings();
    StartupChildSlotBinding b3[3]{gb[0], gb[1], gb[1]};
    StartupChildSlotBinding b4[4]{gb[0], gb[1], gb[1], gb[1]};
    auto refused = [&](const char *name, const StartupChildPpuPorts &p, const StartupChildSlotBinding *b, unsigned n) {
        const StartupChildCompositionConfigure rc = g.comp->configure(p, b, n);
        if (rc != StartupChildCompositionConfigure::InvalidArgument) std::printf("  refusal row '%s' returned %d\n", name, int(rc));
        CHECK(rc == StartupChildCompositionConfigure::InvalidArgument && g.comp->create_port() == nullptr && g.comp->slot_count() == 0);
        CHECK(!g.comp->report(0).configured && !g.comp->report(1).configured);   // nothing was half-installed
    };
    refused("count 0", good, gb, 0); refused("count 3 with a aliased sibling", good, b3, 3); refused("count 4 (above MAX_SLOTS)", good, b4, 4);
    refused("count 3 with 2 valid", good, b3, 3); refused("null bindings", good, nullptr, 1);
    struct PortRow { const char *name; void (*mutate)(StartupChildPpuPorts &); };
    const PortRow shared_rows[] = {
        {"threads", [](StartupChildPpuPorts &p) { p.threads = nullptr; }}, {"queues", [](StartupChildPpuPorts &p) { p.queues = nullptr; }},
        {"memory", [](StartupChildPpuPorts &p) { p.memory = nullptr; }}, {"code", [](StartupChildPpuPorts &p) { p.code = nullptr; }},
        {"clock", [](StartupChildPpuPorts &p) { p.clock = nullptr; }}, {"dispatch limit", [](StartupChildPpuPorts &p) { p.dispatch_limit = 0; }},
        {"shared thread must be empty", [](StartupChildPpuPorts &p) { static int ctx; p.thread = {&ctx, thread_start, thread_join}; }},
        {"shared recorder must be null", [](StartupChildPpuPorts &p) { static PpuMemoryTraceRecorder rec; p.recorder = &rec; }},
        {"shared budget must be null", [](StartupChildPpuPorts &p) { p.budget = reinterpret_cast<DiscoveryBudgetBinding *>(&p); }},
        {"shared dispatch stop must be empty", [](StartupChildPpuPorts &p) { static StopCounter sc; p.dispatch_stop = {&sc, stop_requested}; }}};
    for (const PortRow &row : shared_rows) { StartupChildPpuPorts p = good; row.mutate(p); refused(row.name, p, gb, 2); }
    struct BindRow { const char *name; unsigned slot; void (*mutate)(StartupChildSlotBinding &, const StartupChildSlotBinding &other); };
    const BindRow rows[] = {
        {"thread start", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.thread.start = nullptr; }},
        {"thread join", 0, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.thread.join = nullptr; }},
        {"thread context null (count 2)", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.thread.context = nullptr; }},
        {"recorder null", 0, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.recorder = nullptr; }},
        {"budget null", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.budget = nullptr; }},
        {"alias thread context", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &o) { b.thread.context = o.thread.context; }},
        {"alias recorder", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &o) { b.recorder = o.recorder; }},
        {"alias budget", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &o) { b.budget = o.budget; }},
        {"policy priority 3072", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.priority = 3072; }},
        {"policy creator 3072", 0, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.creator_priority = 3072; }},
        {"policy stack 0", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.stack_size = 0; }},
        {"policy tls 0", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.tls_capacity = 0; }},
        {"policy return lr 0", 0, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.return_lr = 0; }},
        {"policy slack 0", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.stack_entry_slack = 0; }},
        {"run_to_exit without an exit bound", 0, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.run_to_exit = true; b.policy.max_exit_polls = 0; }},   // an unbounded wait is refused
        {"run_to_exit unbounded on slot 1", 1, [](StartupChildSlotBinding &b, const StartupChildSlotBinding &) { b.policy.run_to_exit = true; }}};
    for (const BindRow &row : rows) {
        StartupChildSlotBinding b[2]{gb[0], gb[1]};
        row.mutate(b[row.slot], b[1 - row.slot]);
        refused(row.name, good, b, 2);
    }
    // count 1 through the new overload: a null thread CONTEXT is only refused when it could alias a sibling
    { StartupChildSlotBinding one = gb[0]; one.thread.context = nullptr; std::unique_ptr<StartupChildComposition> k(new StartupChildComposition);
      CHECK(k->configure(good, &one, 1) == StartupChildCompositionConfigure::Ok && k->slot_count() == 1 && !k->report(1).configured);
      CHECK(k->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && k->released()); }
    // a refusal installed nothing, so a good configure after all of them is accepted; then it is final
    CHECK(g.comp->configure(good, gb, 2) == StartupChildCompositionConfigure::Ok);
    const StartupPpuCreatePort *port = g.comp->create_port();
    CHECK(port && port->context && port->prepare && port->start && g.comp->slot_count() == 2);
    CHECK(g.comp->configure(good, gb, 2) == StartupChildCompositionConfigure::AlreadyConfigured && g.comp->create_port() == port);
    StartupChildPpuPorts legacy_ports = good; legacy_ports.thread = gb[0].thread; legacy_ports.recorder = gb[0].recorder; legacy_ports.budget = gb[0].budget;
    CHECK(g.comp->configure(legacy_ports, gb[0].policy) == StartupChildCompositionConfigure::AlreadyConfigured && g.comp->slot_count() == 2);
    CHECK(g.comp->report(0).configured && g.comp->report(1).configured && g.comp->report().configured);
    CHECK(!g.comp->report(2).configured && !g.comp->report(99).configured && g.comp->child(3) == nullptr && g.comp->child(2) != nullptr && g.comp->child(1) != nullptr && g.comp->child(0) == &g.comp->child());
    CHECK(g.comp->child(0)->status().phase == StartupChildPpuPhase::Empty && g.comp->child(1)->status().phase == StartupChildPpuPhase::Empty);
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.slot_count == 2 && t.prepare_calls == 0 && t.start_calls == 0 && t.refused_no_slot == 0 && t.policy_mismatch == 0 && t.last_refusal.reason == StartupChildRefusalReason::None);
    // the legacy overload is the same machinery with count 1 (existing contract: dispatch_stop must be empty, the policy is slot 0's)
    Comp2Rig h((Comp2Opt{QUOTA, false, true, PRIORITY}));
    StartupChildPpuPorts lp = h.shared(); lp.thread = {&h.probe[0], probe_start, probe_join}; lp.recorder = &h.c[0]->recorder; lp.budget = h.c[0]->budget.get();
    CHECK(h.comp->configure(lp, h.policy()) == StartupChildCompositionConfigure::Ok && h.comp->slot_count() == 1 && h.comp->create_port());
    CHECK(h.comp->report(0).configured && !h.comp->report(1).configured && h.comp->child(1) != nullptr);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->released());
    CHECK(g.comp->report(1).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->report(1).latches_asserted_by_teardown);
    CHECK(h.comp->teardown(h.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && h.comp->released());
    check_released_baseline(g.r); g.r.check_primary_untouched(); check_released_baseline(h.r);
}

// ============ K. two real helpers coexist: distinct ids/outputs/CPU/TLS/stack/owners/recorders/budgets; the first stays blocked ============
void case_coexist_blocked()
{
    Comp2Rig g;
    CHECK(g.create(0));
    CHECK(g.blocked(0) && g.st(0).receive_calls == 1);
    CHECK(g.create(1));
    CHECK(g.blocked(0) && g.blocked(1) && g.st(0).receive_calls == 1 && g.st(1).receive_calls == 1);   // the first is STILL really blocked after the second started
    const StartupChildPpuStatus s0 = g.st(0), s1 = g.st(1);
    const uint32_t id0 = g.id(0), id1 = g.id(1);
    auto &cpu = g.r.primary.ppu.cpu; const StartupControl &k = *g.r.primary_control;
    CHECK(cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && k.stop == StartupStop::None);
    CHECK(id0 != 0 && id1 != 0 && id0 != id1);
    CHECK(k.ppu_create_calls == 2 && k.ppu_create_prepared == 2 && k.ppu_create_id_written == 2 && k.ppu_create_started == 2 && k.ppu_create_returns == 2);
    CHECK(g.get64(OUTA) == id0 && g.get64(OUTB) == id1 && g.get64(PTOC_SLOT) == TOC);   // the BRIDGE wrote two distinct BE ids to two outputs
    // distinct service records / CPU contexts / storage / stacks / TLS / atomic memory owners
    CHECK(s0.phase == StartupChildPpuPhase::Started && s1.phase == StartupChildPpuPhase::Started && s0.admitted && s1.admitted && s0.service_owned && s1.service_owned);
    CHECK(s0.ref.slot != s1.ref.slot && s0.ref.id != s1.ref.id && disjoint(s0, s1));
    CHECK(s0.storage.stack_address != s1.storage.stack_address && s0.tls.block != s1.tls.block && s0.tls.thread_pointer != s1.tls.thread_pointer);
    CHECK(g.r.used() == 0x40000 && g.r.allocations() == 2 && ps3_ppu_threads_live(&g.r.ppu) == 2);
    for (unsigned i = 0; i < 2; ++i) {
        const Obs &o = g.c[i]->obs; const StartupChildPpuStatus &s = i ? s1 : s0;
        CHECK(o.entered.load() && o.prefix.load() && !o.resumed.load());
        CHECK(o.r1 == uint64_t(s.storage.stack_address) + G64 - 0x70 && o.r2 == CHILD_TOC && o.r3 == g.out[i] && o.r4 == 0);
        CHECK(o.lr == LR && o.ctr == ENTRY_RECEIVE && o.r13 == s.tls.thread_pointer && o.cpu_thread_id == g.id(i));
        CHECK(o.id_read == PS3_GUEST_MEMORY_OK && be64(o.id_bytes) == g.id(i));   // each id was in ITS guest output before ITS child ran
        CHECK(s.last_queue == g.q[i] && s.last_pc == ENTRY_RECEIVE && s.terminal_calls == 0 && s.consumed_events == 0);
        CHECK(g.c[i]->thread.start_calls.load() == 1 && g.c[i]->thread.join_calls.load() == 0);
    }
    // TLS bytes come from the template per slot; a write into child 2's TLS leaves child 1's untouched
    unsigned char t0[4], t1[4]; g.r.get(s0.tls.block, t0, 4); g.r.get(s1.tls.block, t1, 4);
    CHECK(t0[0] == 0x11 && t0[3] == 0x44 && t1[0] == 0x11 && t1[3] == 0x44);
    const unsigned char poke[4]{0x7A, 0x7B, 0x7C, 0x7D}; g.r.put(s1.tls.block, poke, 4);
    g.r.get(s0.tls.block, t0, 4); g.r.get(s1.tls.block, t1, 4);
    CHECK(t0[0] == 0x11 && t0[1] == 0x22 && t0[2] == 0x33 && t0[3] == 0x44 && t1[0] == 0x7A && t1[3] == 0x7D);
    // reports: slot 0 stays the legacy detail, slot 1 has its own, totals are separate (no fabricated aggregate fields)
    const StartupChildCompositionReport &a = g.comp->report(0), &b = g.comp->report(1);
    CHECK(a.prepared_id == id0 && b.prepared_id == id1 && a.observed_blocked && b.observed_blocked && a.prepare_attempted && b.prepare_attempted);
    CHECK(a.mapped_start == StartupPpuCreateStartStatus::Started && b.mapped_start == StartupPpuCreateStartStatus::Started && a.prepare_calls == 1 && b.prepare_calls == 1);
    CHECK(&g.comp->report() == &a && g.comp->report().prepared_id == id0 && g.comp->report().prepare_calls == 1 && g.comp->report().start_calls == 1);
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.slot_count == 2 && t.prepare_calls == 2 && t.start_calls == 2 && t.refused_no_slot == 0 && t.policy_mismatch == 0 && t.unmatched_start == 0 && t.refused_stopped == 0 && t.refused_invalid == 0);
    // teardown after the join: BOTH cancelled before ANY reap, both released, no event invented
    const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
    CHECK(&rep == &a && a.teardown == StartupChildCompositionTeardown::Released && b.teardown == StartupChildCompositionTeardown::Released && g.comp->released());
    CHECK(cancels_before_reaps(g) && a.latches_asserted_by_teardown && b.latches_asserted_by_teardown);
    CHECK(g.probe[0].cancelled_mask_at_first_join == 3 && g.probe[1].cancelled_mask_at_first_join == 3);   // independent proof at the real join boundary
    for (unsigned i = 0; i < 2; ++i) {
        const StartupChildCompositionReport &x = g.comp->report(i);
        CHECK(x.before.phase == StartupChildPpuPhase::Started && !x.before.cancel_requested && x.before.consumed_events == 0 && x.before.receive_calls == 1);
        CHECK(x.after.stop == StartupStop::Cancelled && x.after.fault == StartupChildPpuReceiveFault::Cancelled && x.after.consumed_events == 0 && !g.c[i]->obs.resumed.load());
        CHECK(x.after.phase == StartupChildPpuPhase::Released && !x.after.has_resources && x.reap.status == StartupChildPpuReapStatus::Released);
        CHECK(g.c[i]->thread.join_calls.load() == 1 && g.c[i]->thread.join_ok.load() == 1);
        CHECK(g.c[i]->recorder.report().configured);   // fixture bodies publish no traced memory events: recorder routing is NOT content-proven here (see HANDOFF)
    }
    g.expect_released_baseline();
    CHECK(g.r.primary_control->stop == StartupStop::None);
}

// ============ L. third request: refused with zero effects; actual policy mismatch is distinguished from "no free slot" ============
void case_third_refusal()
{
    Comp2Rig g;
    CHECK(g.create(0) && g.create(1));
    const uint32_t id0 = g.id(0), id1 = g.id(1);
    g.seed_raw(OUTC, OPDA, ARG_REFUSED);
    CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
    CHECK(g.get64(OUTC) == OUT_SENTINEL && g.get64(OUTA) == id0 && g.get64(OUTB) == id1);   // no id written; both ids intact
    CHECK(g.r.used() == 0x40000 && g.r.allocations() == 2 && ps3_ppu_threads_live(&g.r.ppu) == 2);   // registry/allocator/owners unchanged
    CHECK(g.id(0) == id0 && g.id(1) == id1 && g.blocked(0) && g.blocked(1) && g.st(0).receive_calls == 1 && g.st(1).receive_calls == 1);
    CHECK(g.c[0]->thread.start_calls.load() == 1 && g.c[1]->thread.start_calls.load() == 1);
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.prepare_calls == 3 && t.start_calls == 2 && t.refused_no_slot == 1 && t.policy_mismatch == 0);
    CHECK(t.last_refusal.reason == StartupChildRefusalReason::NoSlot && t.last_refusal.free_slots == 0);
    CHECK(t.last_refusal.priority == int32_t(PRIORITY) && t.last_refusal.creator_priority == int32_t(CREATOR) && t.last_refusal.stack_size == uint32_t(G64));
    CHECK(t.last_refusal.argument == ARG_REFUSED && t.last_refusal.output == OUTC && t.last_refusal.entry == ENTRY_RECEIVE);
    CHECK(g.comp->report(0).prepare_calls == 1 && g.comp->report(1).prepare_calls == 1 && g.comp->report(0).start_calls == 1);   // a refusal belongs to no slot
    // a request that ALSO mismatches every policy is still "no slot": the cause is occupancy, and the diagnostics say so
    g.seed_raw(OUTC, OPDA, ARG_REFUSED, 100);
    CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
    CHECK(t.refused_no_slot == 2 && t.policy_mismatch == 0 && t.last_refusal.priority == 100 && t.last_refusal.reason == StartupChildRefusalReason::NoSlot);
    CHECK(g.blocked(0) && g.blocked(1) && g.r.used() == 0x40000);
    const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
    CHECK(rep.teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
    g.expect_released_baseline();
}
void case_policy_vs_slot()
{
    Comp2Opt o; o.slot1_priority = 200;   // slot 0 serves priority 249, slot 1 serves priority 200
    Comp2Rig g(o);
    // both free, nothing matches: a POLICY mismatch (not occupancy)
    CHECK(!g.create(0, 100) && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.policy_mismatch == 1 && t.refused_no_slot == 0 && t.last_refusal.reason == StartupChildRefusalReason::PolicyMismatch && t.last_refusal.free_slots == 2);
    CHECK(t.last_refusal.priority == 100 && t.last_refusal.output == OUTA && t.last_refusal.argument == OUTA);
    CHECK(g.get64(OUTA) == OUT_SENTINEL && g.r.used() == 0 && g.r.allocations() == 0 && g.c[0]->thread.start_calls.load() == 0 && g.c[1]->thread.start_calls.load() == 0);
    CHECK(!g.comp->report(0).prepare_attempted && !g.comp->report(1).prepare_attempted && g.comp->child(0)->status().phase == StartupChildPpuPhase::Empty);
    // priority 200 matches only slot 1: the LOWEST ELIGIBLE free slot is chosen, slot 0 stays free
    CHECK(g.create(1, 200));
    CHECK(!g.comp->report(0).prepare_attempted && g.comp->report(1).prepare_attempted && g.comp->report(1).observed_blocked && g.blocked(1));
    CHECK(g.comp->child(0)->status().phase == StartupChildPpuPhase::Empty && g.get64(OUTB) == g.id(1));
    // slot 1 is taken and slot 0 is free but ineligible: still a policy mismatch, NOT "no slot"
    g.seed_raw(OUTC, OPDA, ARG_REFUSED, 200);
    CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
    CHECK(t.policy_mismatch == 2 && t.refused_no_slot == 0 && t.last_refusal.reason == StartupChildRefusalReason::PolicyMismatch && t.last_refusal.free_slots == 1);
    CHECK(t.last_refusal.priority == 200 && t.last_refusal.argument == ARG_REFUSED && t.last_refusal.output == OUTC && g.get64(OUTC) == OUT_SENTINEL);
    // priority 249 takes slot 0; now both are taken and ANY request (even a matching one) is "no slot"
    CHECK(g.create(0, PRIORITY) && g.blocked(0) && g.blocked(1) && g.id(0) != g.id(1));
    g.seed_raw(OUTC, OPDA, ARG_REFUSED, PRIORITY);
    CHECK(!g.run_parent() && t.refused_no_slot == 1 && t.policy_mismatch == 2 && t.last_refusal.reason == StartupChildRefusalReason::NoSlot && t.last_refusal.free_slots == 0);
    CHECK(g.r.used() == 0x40000 && g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}

// ============ M. start(id) routes by the exact prepared id; zero/unknown/repeated ids touch no child ============
void case_start_routing()
{
    Comp2Rig g;
    const StartupPpuCreatePrepareResult p0 = g.hand_prepare(0), p1 = g.hand_prepare(1);
    CHECK(p0.status == StartupPpuCreatePrepareStatus::Prepared && p1.status == StartupPpuCreatePrepareStatus::Prepared && p0.thread_id && p1.thread_id && p0.thread_id != p1.thread_id);
    CHECK(g.r.used() == 0x40000 && g.st(0).phase == StartupChildPpuPhase::Reserved && g.st(1).phase == StartupChildPpuPhase::Reserved);
    const uint32_t bogus[] = {0, p0.thread_id + p1.thread_id + 7, 0xFFFFFFFFu, 1};   // zero, unknown, unknown, the PRIMARY's id
    for (uint32_t id : bogus) CHECK(g.hand_start(id) == StartupPpuCreateStartStatus::ProviderFailure);
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.start_calls == 4 && t.unmatched_start == 4 && g.comp->report(0).start_calls == 0 && g.comp->report(1).start_calls == 0);
    CHECK(!g.comp->report(0).start_attempted && !g.comp->report(1).start_attempted && g.c[0]->thread.start_calls.load() == 0 && g.c[1]->thread.start_calls.load() == 0);
    CHECK(g.st(0).phase == StartupChildPpuPhase::Reserved && g.st(1).phase == StartupChildPpuPhase::Reserved && !g.st(0).host_started && !g.st(1).host_started);
    // route the SECOND prepared id first: slot 0 is untouched
    CHECK(g.hand_start(p1.thread_id) == StartupPpuCreateStartStatus::Started);
    CHECK(g.comp->report(1).start_attempted && g.comp->report(1).observed_blocked && g.blocked(1) && !g.comp->report(0).start_attempted);
    CHECK(g.st(0).phase == StartupChildPpuPhase::Reserved && g.c[0]->thread.start_calls.load() == 0 && g.c[1]->thread.start_calls.load() == 1);
    // repeated / foreign ids after the start change nothing
    CHECK(g.hand_start(p1.thread_id) == StartupPpuCreateStartStatus::ProviderFailure && g.hand_start(0) == StartupPpuCreateStartStatus::ProviderFailure);
    CHECK(g.c[1]->thread.start_calls.load() == 1 && g.blocked(1) && g.st(1).receive_calls == 1 && g.comp->report(1).start_calls == 2 && g.st(0).phase == StartupChildPpuPhase::Reserved);
    CHECK(g.comp->report(1).observed_blocked && g.comp->report(1).start.status == StartupChildPpuStartStatus::Blocked && g.comp->report(1).mapped_start == StartupPpuCreateStartStatus::ProviderFailure);   // the first start's evidence is not overwritten
    CHECK(g.hand_start(p0.thread_id) == StartupPpuCreateStartStatus::Started && g.blocked(0) && g.blocked(1));
    CHECK(g.hand_start(p0.thread_id) == StartupPpuCreateStartStatus::ProviderFailure && g.c[0]->thread.start_calls.load() == 1 && t.unmatched_start == 5);
    CHECK(g.comp->report(0).observed_blocked && g.comp->report(0).start.status == StartupChildPpuStartStatus::Blocked && g.comp->report(0).start_calls == 2);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
    CHECK(cancels_before_reaps(g));
    g.expect_released_baseline();
}

// ============ N. independent waits: no fake event, no new mutable child API: a REAL send wakes one helper, a REAL per-slot budget stops one ============
void case_independent_waits()
{
    for (unsigned first = 0; first < 2; ++first) {   // both orders
        Comp2Rig g; const unsigned other = 1 - first;
        CHECK(g.create(0) && g.create(1) && g.blocked(0) && g.blocked(1));
        g.r.send(g.q[first], 0x41, 0x42, 0x43);          // a real event port -> THAT helper's queue only
        g.wait_exited(first);
        const StartupChildPpuStatus w = g.st(first), o = g.st(other);
        CHECK(w.terminal_calls == 1 && w.stop == StartupStop::Returned && !w.cancel_requested && w.fault == StartupChildPpuReceiveFault::None);
        CHECK(w.consumed_events == 1 && w.first_consumed.data1 == 0x41 && w.first_consumed.data2 == 0x42 && w.first_consumed.data3 == 0x43 && g.c[first]->obs.resumed.load());
        CHECK(g.blocked(other) && o.phase == StartupChildPpuPhase::Started && o.receive_calls == 1 && o.consumed_events == 0 && o.terminal_calls == 0 && !g.c[other]->obs.resumed.load());
        const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
        (void)rep;
        CHECK(g.comp->released() && g.comp->report(first).teardown == StartupChildCompositionTeardown::Released && g.comp->report(other).teardown == StartupChildCompositionTeardown::Released);
        CHECK(g.comp->report(first).after.stop == StartupStop::Returned && g.comp->report(other).after.stop == StartupStop::Cancelled);   // the woken one was NOT cancelled
        CHECK(g.comp->report(other).after.fault == StartupChildPpuReceiveFault::Cancelled && g.comp->report(other).after.consumed_events == 0 && !g.c[other]->obs.resumed.load());
        g.expect_released_baseline();
    }
    {   // per-slot budget seam (existing injected clock): exhausting slot 1's budget stops ONLY slot 1; slot 0 later resumes on its own budget
        Comp2Rig g;
        CHECK(g.create(0) && g.create(1));
        g.c[1]->budget_time.store(UINT64_C(1) << 50);
        g.r.send(g.q[1], 0x51, 0x52, 0x53);
        g.wait_exited(1);
        const StartupChildPpuStatus s1 = g.st(1);
        CHECK(s1.stop != StartupStop::Returned && s1.stop != StartupStop::None && !s1.cancel_requested && s1.consumed_events == 1 && s1.first_consumed.data1 == 0x51 && !g.c[1]->obs.resumed.load());
        CHECK(g.blocked(0) && g.st(0).receive_calls == 1 && g.st(0).consumed_events == 0 && g.st(0).terminal_calls == 0);   // slot 0 unaffected by slot 1's budget
        g.r.send(g.q[0], 0x61, 0x62, 0x63);
        g.wait_exited(0);
        CHECK(g.st(0).stop == StartupStop::Returned && g.st(0).consumed_events == 1 && g.c[0]->obs.resumed.load());
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
        g.expect_released_baseline();
    }
}

// ============ O. failures of the SECOND child: real admission/storage failure, retention, the first child stays valid ============
void case_partial_failure()
{
    {   // owner/admission failure (slot 1's recorder was never configured): reservation + TLS stay owned, parent stops, slot 0 valid
        Comp2Opt o; o.slot1_recorder = false; Comp2Rig g(o);
        CHECK(g.create(0));
        const uint32_t id0 = g.id(0);
        CHECK(!g.create(1) && g.r.primary_control->stop == StartupStop::ProviderFailure);
        const StartupChildCompositionReport &b = g.comp->report(1);
        CHECK(b.prepare_attempted && b.prepare.status == StartupChildPpuPrepareStatus::AdmissionFailed && b.prepare.owns_resources && b.mapped_prepare == StartupPpuCreatePrepareStatus::ProviderFailure);
        CHECK(!b.start_attempted && g.get64(OUTB) == OUT_SENTINEL && g.c[1]->thread.start_calls.load() == 0);   // no id written, never started
        CHECK(g.st(1).has_resources && !g.st(1).admitted && g.st(1).phase == StartupChildPpuPhase::Reserved && g.r.used() == 0x40000);
        CHECK(g.id(0) == id0 && g.blocked(0) && g.st(0).receive_calls == 1 && g.get64(OUTA) == id0);   // the first child is still valid
        CHECK(g.create(1) == false && g.comp->totals().refused_no_slot == 1 && g.comp->report(1).prepare_calls == 1);   // a failed slot is NEVER reused
        const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
        CHECK(rep.teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
        CHECK(g.c[0]->thread.join_calls.load() == 1 && g.c[1]->thread.join_calls.load() == 0 && cancels_before_reaps(g));
        g.expect_released_baseline();
    }
    {   // reserve failure (allocator quota fits exactly one child): zero effects for slot 1, the slot is consumed, the first child valid
        Comp2Opt o; o.quota = 0x30000; Comp2Rig g(o);
        CHECK(g.create(0) && g.r.used() == 0x20000);
        CHECK(!g.create(1) && g.r.primary_control->stop == StartupStop::ProviderFailure);
        const StartupChildCompositionReport &b = g.comp->report(1);
        CHECK(b.prepare_attempted && b.prepare.status == StartupChildPpuPrepareStatus::ReserveFailed && b.prepare.native_rc == PS3_PPU_ENOMEM && !b.prepare.owns_resources);
        CHECK(!g.st(1).has_resources && g.st(1).phase == StartupChildPpuPhase::Empty && g.r.used() == 0x20000 && g.r.allocations() == 1 && ps3_ppu_threads_live(&g.r.ppu) == 1);
        CHECK(g.get64(OUTB) == OUT_SENTINEL && g.blocked(0) && g.get64(OUTA) == g.id(0));
        CHECK(!g.create(1) && g.comp->totals().refused_no_slot == 1);
        const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
        CHECK(rep.teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->released());
        g.expect_released_baseline();
    }
    {   // cancel latch after BOTH prepares and before either start: no host start at all, both abandoned by teardown
        Comp2Rig g;
        const StartupPpuCreatePrepareResult p0 = g.hand_prepare(0), p1 = g.hand_prepare(1);
        CHECK(p0.status == StartupPpuCreatePrepareStatus::Prepared && p1.status == StartupPpuCreatePrepareStatus::Prepared);
        g.comp->request_stop();
        CHECK(g.hand_start(p0.thread_id) == StartupPpuCreateStartStatus::ProviderFailure && g.hand_start(p1.thread_id) == StartupPpuCreateStartStatus::ProviderFailure);
        CHECK(g.comp->report(0).start_skipped_by_cancel_latch && g.comp->report(1).start_skipped_by_cancel_latch && !g.comp->report(0).start_attempted && !g.comp->report(1).start_attempted);
        CHECK(g.c[0]->thread.start_calls.load() == 0 && g.c[1]->thread.start_calls.load() == 0 && g.st(0).phase == StartupChildPpuPhase::Reserved && g.st(1).phase == StartupChildPpuPhase::Reserved);
        const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
        CHECK(rep.teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released && g.c[0]->thread.join_calls.load() == 0 && g.c[1]->thread.join_calls.load() == 0);
        g.expect_released_baseline();
    }
    {   // slot 0 really started and blocked; slot 1 prepared; stop; slot 1 never starts; teardown releases both (1 join / 0 joins)
        Comp2Rig g;
        CHECK(g.create(0));
        const StartupPpuCreatePrepareResult p1 = g.hand_prepare(1);
        CHECK(p1.status == StartupPpuCreatePrepareStatus::Prepared);
        g.comp->request_stop();
        CHECK(g.hand_start(p1.thread_id) == StartupPpuCreateStartStatus::ProviderFailure && g.comp->report(1).start_skipped_by_cancel_latch && g.c[1]->thread.start_calls.load() == 0);
        CHECK(g.blocked(0) && g.st(1).phase == StartupChildPpuPhase::Reserved);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
        CHECK(g.c[0]->thread.join_calls.load() == 1 && g.c[1]->thread.join_calls.load() == 0 && cancels_before_reaps(g));
        g.expect_released_baseline();
    }
}

// ============ P. cleanup retention: one slot retained / still running, the sibling still releases, nothing is joined twice ============
void case_cleanup_retention()
{
    for (unsigned bad = 0; bad < 2; ++bad) {   // the retained slot is slot 0, then slot 1
        Comp2Rig g; const unsigned good = 1 - bad;
        CHECK(g.create(0) && g.create(1));
        g.c[bad]->thread.fail_join = 1;
        g.comp->teardown(g.td(2000, 0));
        const StartupChildCompositionReport &rb = g.comp->report(bad), &rg = g.comp->report(good);
        CHECK(rb.teardown == StartupChildCompositionTeardown::ServiceRetained && rg.teardown == StartupChildCompositionTeardown::Released && !g.comp->released());
        CHECK(g.c[bad]->thread.join_calls.load() == 1 && g.c[bad]->thread.join_ok.load() == 0 && g.c[good]->thread.join_calls.load() == 1 && g.c[good]->thread.join_ok.load() == 1);
        CHECK(g.st(bad).has_resources && !g.st(good).has_resources && g.r.used() == 0x20000 && g.r.allocations() == 1 && ps3_ppu_threads_live(&g.r.ppu) == 1);   // the shared graph is still borrowed
        CHECK(cancels_before_reaps(g) && g.probe[0].cancelled_mask_at_first_join == 3 && g.probe[1].cancelled_mask_at_first_join == 3);
        const unsigned good_cancel = rg.cancel_sequence, good_reaps = rg.reap_calls, good_polls = g.polls_counter.load();
        const unsigned bad_cancel = rb.cancel_sequence, bad_first_reap = rb.first_reap_sequence;   // the retained slot's FIRST cancel / first reap
        CHECK(bad_cancel != 0 && bad_first_reap != 0 && bad_cancel < bad_first_reap);
        // no reuse: a later create is refused by the latch, whatever the slot state
        const StartupPpuCreatePort *port = g.comp->create_port(); const StartupPpuCreateRequest req = g.hand_request(0); const Ps3TlsTemplate tls{SRC, 4, 0x200};
        CHECK(port->prepare(port->context, &req, &tls).status == StartupPpuCreatePrepareStatus::ProviderFailure && g.comp->totals().refused_stopped == 1);
        // repeated teardown touches ONLY the retained slot
        g.comp->teardown(g.td(2000, 1));
        CHECK(rb.teardown == StartupChildCompositionTeardown::Released && g.comp->released());
        // a retry re-issues the (legacy) cancel but must NOT renumber the report: first cancel and first reap keep their original sequence
        CHECK(rb.cancel_sequence == bad_cancel && rb.first_reap_sequence == bad_first_reap && cancels_before_reaps(g));
        CHECK(g.c[bad]->thread.join_calls.load() == 2 && g.c[bad]->thread.join_ok.load() == 1);   // retried exactly once, then joined
        CHECK(g.c[good]->thread.join_calls.load() == 1 && rg.cancel_sequence == good_cancel && rg.reap_calls == good_reaps && g.polls_counter.load() == good_polls);   // never touched again
        g.expect_released_baseline();
    }
    {   // StillRunning (bounded) on slot 0 while slot 1 releases; then the gate opens and a second teardown finishes ONLY slot 0
        Comp2Rig g; g.put32(OPDA, ENTRY_GATED);
        CHECK(g.create(0) && g.create(1));
        g.r.send(g.q[0], 0x61, 0x62, 0x63);
        arm(10000); while (!g.c[0]->obs.gate_reached.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g.comp->teardown(g.td(2, 0));
        const StartupChildCompositionReport &a = g.comp->report(0), &b = g.comp->report(1);
        CHECK(a.teardown == StartupChildCompositionTeardown::StillRunning && b.teardown == StartupChildCompositionTeardown::Released && !g.comp->released());
        CHECK(a.reap_calls == 3 && a.still_running_polls == 2 && g.polls_counter.load() == 2 && g.c[0]->thread.join_calls.load() == 0 && g.c[1]->thread.join_calls.load() == 1);
        CHECK(g.st(0).has_resources && !g.st(1).has_resources && g.r.used() == 0x20000 && cancels_before_reaps(g));
        const unsigned b_reaps = b.reap_calls;
        const unsigned a_cancel = a.cancel_sequence, a_first_reap = a.first_reap_sequence;   // slot 0's FIRST cancel / first reap
        CHECK(a_cancel != 0 && a_first_reap != 0 && a_cancel < a_first_reap);
        StartupChildCompositionTeardownPolicy open = g.td(2000, 0); open.context = &g.c[0]->obs; open.poll = poll_open_gate;
        g.comp->teardown(open);
        CHECK(a.teardown == StartupChildCompositionTeardown::Released && g.comp->released() && !g.c[0]->obs.gate_timeout.load() && !g.c[0]->obs.second_returned.load());
        // the retry on the still-running slot keeps its first cancel sequence, so every cancel still precedes every first reap
        CHECK(a.cancel_sequence == a_cancel && a.first_reap_sequence == a_first_reap && cancels_before_reaps(g));
        CHECK(g.c[0]->thread.join_calls.load() == 1 && g.c[1]->thread.join_calls.load() == 1 && b.reap_calls == b_reaps);
        CHECK(a.after.stop == StartupStop::Cancelled && a.after.consumed_events == 1 && a.before.cancel_requested == false);
        g.expect_released_baseline();
    }
}

// ============ Q. request_stop reaches both slots through atomics only ============
void case_stop_both()
{
    for (unsigned latched = 0; latched < 2; ++latched) {   // control first: with NO stop the same fixture's indirect calls simply continue
        Comp2Rig g; g.put32(OPDA, ENTRY_STOP_PROBE); g.put32(OPDB, ENTRY_STOP_PROBE);
        CHECK(g.create(0) && g.create(1));
        g.r.send(g.q[0], 0x71, 0x72, 0x73); g.r.send(g.q[1], 0x74, 0x75, 0x76);
        arm(10000);
        while (!g.c[0]->obs.gate_reached.load() || !g.c[1]->obs.gate_reached.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(!g.exited(0) && !g.exited(1) && !g.st(0).cancel_requested && !g.st(1).cancel_requested);   // both RUN compiled code; nobody cancelled
        if (latched) g.comp->request_stop();                  // atomics only: this thread never touches an owner method
        g.c[0]->obs.gate_open.store(true); g.c[1]->obs.gate_open.store(true);
        g.wait_exited(0); g.wait_exited(1);
        for (unsigned i = 0; i < 2; ++i) {
            const StartupChildPpuStatus s = g.st(i);
            CHECK(!s.cancel_requested && s.consumed_events == 1 && !g.c[i]->obs.gate_timeout.load());   // no owner cancel in either mode
            if (latched) CHECK(s.stop == StartupStop::Cancelled && !g.c[i]->obs.second_returned.load());   // each slot's OWN latch stopped ITS child at the dispatch boundary
            else CHECK(s.stop == StartupStop::Returned && g.c[i]->obs.second_returned.load());
        }
        CHECK(g.comp->report().stop_requested_by_watchdog == (latched != 0) && g.comp->report(1).stop_requested_by_watchdog == (latched != 0));
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
        CHECK(g.comp->report(0).latches_asserted_by_teardown && g.comp->report(1).latches_asserted_by_teardown);
        g.expect_released_baseline();
    }
}
// ONE parent thread issues two creates (re-seeding itself) while a watchdog fires request_stop at every interleaving.
void case_watchdog_race2()
{
    unsigned both = 0, one = 0, none = 0;
    for (unsigned i = 0; i < 10; ++i) {
        Comp2Rig g;
        std::atomic<unsigned> returned{0};
        std::thread parent([&] {
            for (unsigned k = 0; k < 2; ++k) {
                g.seed(k);   // the parent seeds its OWN registers: no other thread touches them
                g.parent_call();
                if (!g.parent_returned) return;
                returned.fetch_add(1);
            }
        });
        std::this_thread::sleep_for(std::chrono::microseconds(150 * i));
        g.comp->request_stop();   // ONLY fixed atomics; never an owner method, never a plain read
        parent.join();
        CHECK(g.comp->report().stop_requested_by_watchdog);
        const StartupChildCompositionTotals &t = g.comp->totals();
        CHECK(t.refused_no_slot == 0 && t.policy_mismatch == 0);
        for (unsigned k = 0; k < 2; ++k) if (g.comp->report(k).start_attempted) CHECK(g.comp->report(k).observed_blocked || g.comp->report(k).start.status != StartupChildPpuStartStatus::Blocked);
        if (returned.load() == 2) ++both; else if (returned.load() == 1) ++one; else ++none;
        g.comp->teardown(g.td());
        for (unsigned k = 0; k < 2; ++k) {
            const StartupChildCompositionReport &x = g.comp->report(k);
            CHECK((x.teardown == StartupChildCompositionTeardown::Released || x.teardown == StartupChildCompositionTeardown::NeverPrepared) && x.latches_asserted_by_teardown);
            CHECK(g.c[k]->thread.join_calls.load() == (g.c[k]->thread.start_calls.load() ? 1u : 0u));   // a join per host start, never more
        }
        g.expect_released_baseline();
    }
    std::printf("watchdog interleavings (two creates): both=%u one=%u none=%u\n", both, one, none);
}

// ============ R. the legacy one-slot form: the same machinery, slot 0 keeps its exact legacy report ============
void case_legacy_one_slot()
{
    CompRig g; g.seed();
    CHECK(g.comp->slot_count() == 1 && g.run_parent());
    g.seed(); g.put64(OUTA, OUT_SENTINEL);
    CHECK(!g.run_parent() && g.r.primary_control->stop == StartupStop::UnsupportedSyscall);
    const StartupChildCompositionReport &rep = g.comp->report();
    CHECK(rep.prepare_calls == 2 && rep.start_calls == 1 && &g.comp->report(0) == &rep && !g.comp->report(1).configured && g.comp->child(1) != nullptr);   // every call is slot 0's in a one-slot composition
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.slot_count == 1 && t.prepare_calls == 2 && t.start_calls == 1 && t.refused_no_slot == 1 && t.policy_mismatch == 0 && t.last_refusal.reason == StartupChildRefusalReason::NoSlot);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released && g.comp->released() && g.comp->report(1).teardown == StartupChildCompositionTeardown::NeverPrepared);
    g.expect_released_baseline();
}

void case_presentation_mode_inheritance() {
    CompRig g;g.seed();
    g.r.primary.presentation_mode=NativePresentationMode::Fixed720p;
    CHECK(g.run_parent());
    CHECK(g.obs.mode_before==NativePresentationMode::Fixed720p);
    g.r.primary.presentation_mode=NativePresentationMode::Unconfigured;
    g.r.send(g.q,1,2,3);const auto *child=g.comp->child(0);CHECK(child);arm(10000);
    while(child->status().terminal_calls==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g.obs.mode_after==NativePresentationMode::Fixed720p && g.obs.resumed.load());
    CHECK(g.r.primary.presentation_mode==NativePresentationMode::Unconfigured);
    CHECK(g.comp->teardown(g.td()).teardown==StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
}
// ====================================================================================================================================
// THREE-SLOT composition with a run_to_exit third slot (decision 10 A/B). The third slot models the game's third
// sys_ppu_thread_create: priority 2000, creator 250, stack 0x4000, running the game-data worker entry to its own
// sys_ppu_thread_exit. Same real collaborators; the run-to-exit wait is an INJECTED poll, never an unbounded wait.
// ====================================================================================================================================
// The game's third thread is priority 2000 / creator 250 / stack 0x4000. native/domain/ppu_threads.c's unfenced reserve refuses any
// reservation whose priority is not numerically LOWER than its creator's (PS3_PPU_UNSUPPORTED); the slot's run_to_exit flag routes
// prepare() through the FENCED reservation (ps3_ppu_service_reserve_guest_fenced), which admits the real pair. The fixture therefore
// uses the real {2000, 250, 0x4000} request, and case_run_to_exit_not_fenced below proves the same request is still refused when the
// slot does NOT carry run_to_exit.
// (test_startup_child_ppu.cpp is #included above and declares its own GD_PRIORITY/GD_CREATOR/GD_STACK in the same anonymous
// namespace, with these exact values, so this file reuses them instead of redeclaring them.)
static_assert(GD_PRIORITY == 2000 && GD_STACK == 0x4000, "the third child's request is fixed: {2000, 250, 0x4000}");
// Background is appended after Exited so every existing StartupChildPpuStartStatus value keeps its numeric identity. The
// composition's Background -> Started mapping itself is internal to startup_child_composition.cpp and is exercised only in an
// LLE build (this host verifier compiles WITHOUT PS3_NATIVE_LLE_SRE, where no request becomes background).
static_assert(static_cast<int>(StartupChildPpuStartStatus::Background) > static_cast<int>(StartupChildPpuStartStatus::Exited),
    "Background is appended after Exited: existing status values never change");
struct GdObs { uint32_t thread_id = 0; std::atomic<bool> entered{false}, import_returned{false}; const StartupExecution *execution = nullptr; };
GdObs *g_gd_obs = nullptr;
// The third worker: it observes its OWN execution's game-data port and allow_thread_exit, then really ends itself.
void gd_worker(ppu_context *cpu)
{
    GdObs *o = g_gd_obs;
    o->thread_id = uint32_t(cpu->thread_id);
    o->execution = reinterpret_cast<const StartupExecution *>(cpu);
    o->entered.store(true);
    cpu->gpr[3] = 0x00000007;
    ps3_hle_call(0xaff080a4, cpu);   // sys_ppu_thread_exit through the real dispatcher
    o->import_returned.store(true);
}
// A run-to-exit worker that stays in compiled code (NO receive) until the test opens its gate, then really exits. It proves the
// bounded exit wait: a child still Running when the bound is spent is a ProviderFailure, never Started.
constexpr uint32_t ENTRY_SLOW_EXIT = 0x00500800;
std::atomic<bool> g_slow_gate{false};
void gd_slow_worker(ppu_context *cpu)
{
    for (int i = 0; i < 5000 && !g_slow_gate.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gd_worker(cpu);
}
const CodeEntry gd_entries[] = {{ENTRY_RECEIVE, comp2_receive}, {ENTRY_RETURN, comp2_return}, {ENTRY_GATED, comp2_gated},
    {ENTRY_NESTED, comp2_nested}, {ENTRY_STOP_PROBE, comp2_stop_probe}, {ENTRY_EXIT, gd_worker}, {ENTRY_SLOW_EXIT, gd_slow_worker}};
ps3_compiled_code_status gd_find(const void *, uint32_t address, ps3_compiled_function *target)
{
    for (const CodeEntry &e : gd_entries) if (e.address == address) { *target = e.function; return PS3_COMPILED_CODE_OK; }
    return PS3_COMPILED_CODE_NOT_FOUND;
}
// Injected run-to-exit wait: advances by one host millisecond so the real child thread can reach its own exit.
void gd_exit_poll(void *context) { static_cast<std::atomic<unsigned> *>(context)->fetch_add(1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

struct Comp3Opt { bool third_run_to_exit = true; uint32_t third_entry = ENTRY_EXIT; unsigned third_polls = 2000; };
struct Comp3Rig {
    Rig r;
    std::unique_ptr<StartupChildComposition> comp{new StartupChildComposition};
    ChildRig *c[3]{};
    JoinProbe probe[3];
    StartupChildSlotBinding bind[3];
    uint32_t q[3]{};
    bool parent_returned = false;
    std::atomic<unsigned> exit_polls{0}, teardown_polls{0};
    gamedata_check state{};
    StartupGameDataPort gamedata{};
    GdObs gd;
    explicit Comp3Rig(const Comp3Opt &opt = Comp3Opt()) {
        clear_obs(); g_comp2_obs[0] = g_comp2_obs[1] = nullptr; g_gd_obs = &gd;
        r.lookup.find = gd_find;
        gamedata.state = &state;
        for (unsigned k = 0; k < 3; ++k) { q[k] = k < 2 ? r.queue_create() : 0; c[k] = &r.new_child(q[k]); probe[k].thread = &c[k]->thread; probe[k].comp = comp.get(); }
        g_comp2_obs[0] = &c[0]->obs; g_comp2_obs[1] = &c[1]->obs;
        r.primary.has_guest_priority = true; r.primary.guest_priority = int32_t(CREATOR);
        r.primary.gamedata = &gamedata;   // the create bridge copies execution.gamedata into the request
        r.primary_control->initialized_tls_template = {SRC, 4, 0x200}; r.primary_control->initialized_tls_template_valid = true;
        put32(OPDA, ENTRY_RECEIVE); put32(OPDA + 4, CHILD_TOC); put32(OPDB, ENTRY_RECEIVE); put32(OPDB + 4, CHILD_TOC);
        put32(OPDC, opt.third_entry); put32(OPDC + 4, CHILD_TOC);
        r.put(NAMEA, NAME_TEXT, sizeof NAME_TEXT);
        put64(OUTA, OUT_SENTINEL); put64(OUTB, OUT_SENTINEL); put64(OUTC, OUT_SENTINEL); put64(OUTD, OUT_SENTINEL); put64(PTOC_SLOT, TOC_SENTINEL);
        const StartupChildSlotBinding *b = bindings(opt);
        CHECK(comp->configure(shared(), b, 3) == StartupChildCompositionConfigure::Ok);
        for (unsigned k = 0; k < 3; ++k) bind[k] = StartupChildSlotBinding();   // the composition COPIED them
        r.primary.ppu_create = comp->create_port();
    }
    ~Comp3Rig() { clear_obs(); g_comp2_obs[0] = g_comp2_obs[1] = nullptr; g_gd_obs = nullptr; }
    StartupChildPpuPorts shared() { StartupChildPpuPorts p = r.ports(*c[0]); p.thread = {}; p.recorder = nullptr; p.budget = nullptr; p.dispatch_stop = {}; return p; }
    const StartupChildSlotBinding *bindings(const Comp3Opt &opt) {
        for (unsigned k = 0; k < 3; ++k) {
            StartupChildCompositionPolicy p = first_child_policy();
            if (k == 0) { p.priority = PRIORITY; p.stack_size = uint32_t(G64); p.tls_capacity = G64; }
            else if (k == 1) { p.priority = 201; p.stack_size = uint32_t(G64); p.tls_capacity = G64; }
            else {   // the game's third thread: priority 2000, creator 250, stack 0x4000
                p.priority = GD_PRIORITY; p.stack_size = GD_STACK; p.tls_capacity = G64;
                p.run_to_exit = opt.third_run_to_exit; p.max_exit_polls = opt.third_polls;
                p.exit_wait_context = &exit_polls; p.exit_wait_poll = gd_exit_poll;
            }
            bind[k] = {{&probe[k], probe_start, probe_join}, &c[k]->recorder, c[k]->budget.get(), p};
        }
        return bind;
    }
    void put32(uint32_t a, uint32_t v) { unsigned char b[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; r.put(a, b, 4); }
    void put64(uint32_t a, uint64_t v) { unsigned char b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i)); r.put(a, b, 8); }
    uint64_t get64(uint32_t a) { unsigned char b[8]; r.get(a, b, 8); return be64(b); }
    // ONE real sys_ppu_thread_create through the bridge, exactly as the guest stub seeds it.
    void seed_raw(uint32_t output, uint32_t opd, uint64_t argument, uint64_t priority, uint32_t stack) {
        auto &cpu = r.primary.ppu.cpu;
        for (int i = 3; i < 32; ++i) if (i != 13) cpu.gpr[i] = UINT64_C(0x1111000000000000) + uint64_t(i);
        cpu.gpr[1] = PSTACK; cpu.gpr[2] = TOC; cpu.gpr[3] = output; cpu.gpr[4] = opd; cpu.gpr[5] = argument; cpu.gpr[6] = priority; cpu.gpr[7] = stack;
        cpu.gpr[8] = 1; cpu.gpr[9] = NAMEA;
    }
    void parent_call() {
        auto &cpu = r.primary.ppu.cpu;
        r.primary_control->stop = StartupStop::None;
        active = r.primary_control.get(); r.primary_control->cpu = &cpu; g_trampoline_fn = [](void *) {};
        try { ps3_hle_call(NID_CREATE, &cpu); parent_returned = true; active = nullptr; } catch (const Terminal &) { parent_returned = false; }
        g_trampoline_fn = nullptr;
    }
    bool run_parent() { std::thread t([this] { parent_call(); }); t.join(); return parent_returned; }
    bool create_first() { seed_raw(OUTA, OPDA, OUTA, PRIORITY, uint32_t(G64)); return run_parent(); }
    bool create_second() { seed_raw(OUTB, OPDB, OUTB, 201, uint32_t(G64)); return run_parent(); }
    bool create_third() { seed_raw(OUTC, OPDC, OUTD, GD_PRIORITY, GD_STACK); return run_parent(); }
    StartupChildCompositionTeardownPolicy td(unsigned polls = 2000, unsigned retries = 3) {
        StartupChildCompositionTeardownPolicy p; p.context = &teardown_polls; p.poll = poll_sleep; p.max_reap_polls = polls; p.max_stage_retries = retries; return p;
    }
    StartupChildPpuStatus st(unsigned k) { return comp->child(k)->status(); }
    void expect_released_baseline() { CHECK(comp->released()); check_released_baseline(r); }
};

// ============ N. third slot: a run_to_exit child that really ends itself is Started with observed_exited; gamedata + flag reach it ============
void case_third_run_to_exit()
{
    Comp3Rig g;
    CHECK(g.comp->slot_count() == 3);
    CHECK(g.create_first() && g.create_second());
    CHECK(g.comp->report(0).observed_blocked && g.comp->report(1).observed_blocked);
    g.exit_polls.store(0);
    CHECK(g.create_third());   // the parent returns: Started, not a stop
    const StartupChildCompositionReport &r3 = g.comp->report(2);
    CHECK(r3.prepare_attempted && r3.prepared_id != 0 && r3.start_attempted);
    CHECK(r3.start.status == StartupChildPpuStartStatus::Exited && r3.final_start.status == StartupChildPpuStartStatus::Exited);
    CHECK(r3.observed_exited && !r3.observed_blocked && r3.mapped_start == StartupPpuCreateStartStatus::Started);
    CHECK(r3.final_start.stop == StartupStop::GuestThreadExit && r3.exit_polls <= 2000);
    CHECK(g.r.primary_control->stop == StartupStop::None && g.r.primary.ppu.cpu.gpr[3] == 0);   // the parent got its r3 = 0
    CHECK(g.get64(OUTC) == uint64_t(r3.prepared_id));   // the bridge wrote the third child's id before start()
    const StartupChildPpuStatus s3 = g.st(2);
    CHECK(s3.phase == StartupChildPpuPhase::Started && s3.stop == StartupStop::GuestThreadExit && s3.terminal_calls == 1);
    CHECK(s3.receive_calls == 0 && s3.consumed_events == 0);   // it never waited on a queue
    CHECK(g.gd.entered.load() && !g.gd.import_returned.load() && g.gd.thread_id == r3.prepared_id);
    // request->gamedata and run_to_exit reached the child's execution
    CHECK(g.gd.execution && g.gd.execution->gamedata == &g.gamedata && g.gd.execution->gamedata->state == &g.state);
    CHECK(g.gd.execution->child_thread_exit_allowed);
    CHECK(g.gd.execution->guest_priority == int32_t(GD_PRIORITY) && s3.storage.stack_logical_bytes == GD_STACK);
    CHECK(g.st(0).phase == StartupChildPpuPhase::Started && g.st(1).phase == StartupChildPpuPhase::Started);   // the other slots are untouched
    // storage: a 0x4000 stack rounds to one 0x10000 guest region, plus the 0x10000 TLS region
    CHECK(s3.storage.stack_logical_bytes == GD_STACK && s3.storage.allocation_bytes == 0x20000);
    CHECK(g.r.used() == 0x20000 * 3 && g.r.allocations() == 3);
    // teardown: the two blocked children cancel; the Exited child joins directly with no StillRunning
    const StartupChildCompositionReport &rep = g.comp->teardown(g.td());
    CHECK(&rep == &g.comp->report(0));
    CHECK(g.comp->report(0).teardown == StartupChildCompositionTeardown::Released && g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::Released && g.comp->report(2).still_running_polls == 0 && g.comp->released());
    CHECK(g.comp->report(2).before.phase == StartupChildPpuPhase::Started && g.comp->report(2).before.stop == StartupStop::GuestThreadExit);
    CHECK(g.comp->report(2).after.phase == StartupChildPpuPhase::Released && !g.comp->report(2).after.has_resources);
    CHECK(g.c[2]->thread.join_calls.load() == 1 && g.c[2]->thread.join_ok.load() == 1);
    CHECK(g.c[2]->thread.start_calls.load() == 1);
    g.expect_released_baseline();
}

// ============ O. run_to_exit children that never end themselves are ProviderFailures and stay retained ============
void case_third_run_to_exit_failures()
{
    {   // a run_to_exit child that BLOCKS in sc130 instead of exiting
        Comp3Opt o; o.third_entry = ENTRY_RECEIVE; Comp3Rig blocked(o);
        CHECK(blocked.create_first() && blocked.create_second());
        CHECK(!blocked.create_third());   // the parent stops ProviderFailure
        CHECK(blocked.r.primary_control->stop == StartupStop::ProviderFailure);
        const StartupChildCompositionReport &r3 = blocked.comp->report(2);
        CHECK(r3.start.status == StartupChildPpuStartStatus::Blocked && r3.final_start.status == StartupChildPpuStartStatus::Blocked);
        CHECK(!r3.observed_exited && r3.mapped_start == StartupPpuCreateStartStatus::ProviderFailure && r3.exit_polls == 0);
        CHECK(blocked.st(2).receive_calls == 1 && blocked.st(2).phase == StartupChildPpuPhase::Started);   // retained, not reaped
        const StartupChildCompositionTotals &t = blocked.comp->totals();
        CHECK(t.prepare_calls == 3 && t.start_calls == 3 && t.refused_no_slot == 0);
        CHECK(blocked.get64(OUTC) == uint64_t(r3.prepared_id));   // the id WAS written: the refusal is start's, not prepare's
        CHECK(blocked.comp->teardown(blocked.td()).teardown == StartupChildCompositionTeardown::Released);
        CHECK(blocked.comp->report(2).teardown == StartupChildCompositionTeardown::Released && blocked.comp->released());
        CHECK(blocked.st(2).phase == StartupChildPpuPhase::Released && blocked.st(2).stop == StartupStop::Cancelled);
        CHECK(blocked.c[2]->thread.start_calls.load() == 1 && blocked.c[2]->thread.join_calls.load() == 1);
        blocked.expect_released_baseline();
    }
    {   // a run_to_exit child that never blocks and never exits is NOT observable as Running: the real domain start (ppu_threads.c:202)
        // waits until the activity leaves RUNNING, so a worker in a long compiled loop makes start() return only after it exited. The
        // bounded wait therefore never has to fire for it, and nothing is invented: Exited is the honest answer.
        g_slow_gate.store(false);
        Comp3Opt o; o.third_entry = ENTRY_SLOW_EXIT; o.third_polls = 3;
        Comp3Rig g(o);
        CHECK(g.create_first() && g.create_second());
        CHECK(g.create_third());
        const StartupChildCompositionReport &r3 = g.comp->report(2);
        CHECK(r3.start.status == StartupChildPpuStartStatus::Exited && r3.final_start.status == StartupChildPpuStartStatus::Exited);
        CHECK(r3.observed_exited && r3.exit_polls == 0 && r3.mapped_start == StartupPpuCreateStartStatus::Started);
        CHECK(g.st(2).receive_calls == 0 && g.st(2).stop == StartupStop::GuestThreadExit);
        g_slow_gate.store(true);   // release the gate so nothing lingers
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
        CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
        g.expect_released_baseline();
    }
    {   // a run_to_exit child that BLOCKS in sc130 instead of exiting, bounded to a few polls
        Comp3Opt o; o.third_entry = ENTRY_GATED; o.third_polls = 3;
        Comp3Rig g(o);
        CHECK(g.create_first() && g.create_second());
        CHECK(!g.create_third());   // the parent stops ProviderFailure after the bounded wait
        CHECK(g.r.primary_control->stop == StartupStop::ProviderFailure);
        const StartupChildCompositionReport &r3 = g.comp->report(2);
        CHECK(r3.start.status == StartupChildPpuStartStatus::Blocked || r3.start.status == StartupChildPpuStartStatus::Running);
        CHECK(r3.final_start.status == StartupChildPpuStartStatus::Blocked || r3.final_start.status == StartupChildPpuStartStatus::Running);
        CHECK(!r3.observed_exited && r3.mapped_start == StartupPpuCreateStartStatus::ProviderFailure);
        if (r3.start.status == StartupChildPpuStartStatus::Running) CHECK(r3.exit_polls == 3 && g.exit_polls.load() == 3);   // the wait stopped exactly at the bound
        else CHECK(r3.exit_polls == 0 && g.exit_polls.load() == 0);   // it reached its real sc130 block before any poll was needed
        CHECK(g.st(2).phase == StartupChildPpuPhase::Started);   // still retained
        CHECK(g.c[2]->thread.start_calls.load() == 1);   // the host thread really started: the bound, not the start, ended the wait
        // the fixture really started running compiled code: release it, then it reaches its own real cancellation boundary
        g.c[2]->obs.gate_open.store(true);
        CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
        CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
        g.expect_released_baseline();
    }
}

// ============ P. run_to_exit with an unbounded (0) wait is refused at configure; count 4 is refused ============
void case_run_to_exit_configure_bounds()
{
    Comp3Rig g;   // a valid three-slot configure happened in the constructor
    CHECK(g.comp->slot_count() == 3 && g.comp->create_port());
    {   // count 4 is refused whatever the bindings look like
        Rig r; StartupChildComposition comp;
        ChildRig *c[4]{}; StartupChildSlotBinding b[4]{}; JoinProbe probe[4];
        for (unsigned k = 0; k < 4; ++k) { c[k] = &r.new_child(0); probe[k].thread = &c[k]->thread; probe[k].comp = &comp;
            b[k] = {{&probe[k], probe_start, probe_join}, &c[k]->recorder, c[k]->budget.get(), first_child_policy()};
            b[k].policy.priority = PRIORITY - k; }
        StartupChildPpuPorts shared = r.ports(*c[0]); shared.thread = {}; shared.recorder = nullptr; shared.budget = nullptr; shared.dispatch_stop = {};
        CHECK(comp.configure(shared, b, 4) == StartupChildCompositionConfigure::InvalidArgument && comp.create_port() == nullptr && comp.slot_count() == 0);
        clear_obs(); g_comp2_obs[0] = g_comp2_obs[1] = nullptr;
    }
    {   // run_to_exit with max_exit_polls == 0 never installs anything
        Rig r; StartupChildComposition comp;
        ChildRig &a = r.new_child(0); ChildRig &b2 = r.new_child(0); ChildRig &c3 = r.new_child(0);
        JoinProbe pa, pb, pc; pa.thread = &a.thread; pb.thread = &b2.thread; pc.thread = &c3.thread;
        pa.comp = pb.comp = pc.comp = &comp;
        StartupChildCompositionPolicy p0 = first_child_policy(), p1 = first_child_policy(), p2 = first_child_policy();
        p1.priority = 201; p2.priority = GD_PRIORITY; p2.stack_size = GD_STACK; p2.run_to_exit = true; p2.max_exit_polls = 0;
        StartupChildSlotBinding b[3] = {{{&pa, probe_start, probe_join}, &a.recorder, a.budget.get(), p0},
            {{&pb, probe_start, probe_join}, &b2.recorder, b2.budget.get(), p1}, {{&pc, probe_start, probe_join}, &c3.recorder, c3.budget.get(), p2}};
        StartupChildPpuPorts shared = r.ports(a); shared.thread = {}; shared.recorder = nullptr; shared.budget = nullptr; shared.dispatch_stop = {};
        CHECK(comp.configure(shared, b, 3) == StartupChildCompositionConfigure::InvalidArgument && comp.create_port() == nullptr && comp.slot_count() == 0);
        CHECK(!comp.report(0).configured && !comp.report(2).configured);   // nothing was half-installed
        b[2].policy.max_exit_polls = 1;   // one bounded poll is enough
        CHECK(comp.configure(shared, b, 3) == StartupChildCompositionConfigure::Ok && comp.slot_count() == 3);
        CHECK(comp.teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && comp.released());
        clear_obs(); g_comp2_obs[0] = g_comp2_obs[1] = nullptr;
    }
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->released());
    check_released_baseline(g.r);
}

// ============ Q. the real {2000, 250, 0x4000} request is refused at prepare when the slot does NOT carry run_to_exit ============
void case_run_to_exit_not_fenced()
{
    Comp3Opt o; o.third_run_to_exit = false;   // the slot does not opt into the fenced reservation
    Comp3Rig g(o);
    CHECK(g.comp->slot_count() == 3);
    CHECK(g.create_first() && g.create_second());
    CHECK(!g.create_third());   // prepare is refused: the parent never returns
    CHECK(g.r.primary_control->stop == StartupStop::ProviderFailure);
    const StartupChildCompositionReport &r3 = g.comp->report(2);
    CHECK(r3.prepare_attempted && r3.prepare.status == StartupChildPpuPrepareStatus::ReserveFailed);
    CHECK(r3.prepare.native_rc == PS3_PPU_UNSUPPORTED && !r3.prepare.owns_resources && r3.prepared_id == 0);
    CHECK(!r3.start_attempted && r3.mapped_prepare == StartupPpuCreatePrepareStatus::ProviderFailure);
    CHECK(g.get64(OUTC) == OUT_SENTINEL);   // no id was written: the refusal is prepare's
    CHECK(g.st(2).phase == StartupChildPpuPhase::Empty && !g.st(2).has_resources);
    CHECK(g.c[2]->thread.start_calls.load() == 0);   // nothing ran
    const StartupChildCompositionTotals &t = g.comp->totals();
    CHECK(t.prepare_calls == 3 && t.start_calls == 2 && t.refused_no_slot == 0 && t.policy_mismatch == 0);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::NeverPrepared && g.comp->released());   // it never reserved anything
    g.expect_released_baseline();
}

// ============ L. decision 14: the SHARED process_services / fixed_timezone fields are accepted by configure() and reach EVERY slot ============
// configure() copies `shared` verbatim into each slot's ports, so both process-wide capabilities reach slot 0 AND slot 1. The slots are
// run-to-exit (each child ends itself through the real sys_ppu_thread_exit), so both start() calls report Started for a real Exited.
void case_process_services_shared() {
    Comp2Rig g((Comp2Opt{QUOTA, false, true, PRIORITY}));   // configure deferred: this case configures with the two fields set
    StartupChildPpuPorts p = g.shared();
    p.process_services = &g.r.process_services; p.fixed_timezone = &g.r.process_timezone;
    StartupChildSlotBinding b[2] = {*g.bindings(), g.bindings()[1]};
    for (unsigned k = 0; k < 2; ++k) {
        b[k].policy.run_to_exit = true; b[k].policy.max_exit_polls = 2000;
        b[k].policy.exit_wait_poll = poll_sleep; b[k].policy.exit_wait_context = &g.polls_counter;
    }
    // configure() ACCEPTS both shared fields (null is also allowed; every legacy case above already covers that shape)
    CHECK(g.comp->configure(p, b, 2) == StartupChildCompositionConfigure::Ok);
    g.r.primary.ppu_create = g.comp->create_port();
    // Both slots' children run syscall 144 and then sys_ppu_thread_exit. The sc144 dispatch exists only with PS3_NATIVE_STARTUP_TIMEZONE;
    // with it the guest outputs are written from the process timezone, without it the syscall is refused after the capability gate. Either
    // way the child really ISSUED sc144 in EACH slot, which is what proves the borrowed ports reached every slot.
    for (unsigned k = 0; k < 2; ++k) {
        g.put32(k ? OPDB : OPDA, ENTRY_PROC_TZ);
        g.seed_raw(g.out[k], k ? OPDB : OPDA, g.out[k]);
        CHECK(g.run_parent() && g.r.primary_control->stop == StartupStop::None);
    }
    CHECK(g.comp->totals().prepare_calls == 2 && g.comp->totals().refused_no_slot == 0 && g.comp->totals().policy_mismatch == 0);
    for (unsigned k = 0; k < 2; ++k) {
        CHECK(g.comp->report(k).mapped_prepare == StartupPpuCreatePrepareStatus::Prepared);
        CHECK(g.comp->report(k).observed_exited && g.comp->report(k).mapped_start == StartupPpuCreateStartStatus::Started);
    }
    for (unsigned k = 0; k < 2; ++k) g.wait_exited(k);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
    g.expect_released_baseline();
#ifdef PS3_NATIVE_STARTUP_TIMEZONE
    for (unsigned k = 0; k < 2; ++k) {
        const StartupControl &ctl = g.comp->child(k)->control();
        CHECK(ctl.syscall_number == 144 && ctl.timezone_calls == 1);
        CHECK(ctl.timezone_minutes == PROC_TZ_MINUTES && ctl.summertime_minutes == PROC_TZ_SUMMER_MINUTES);
    }
#else
    // sc144 is compiled out of this build, so the fixture never issues it: no child claims a timezone it was not given.
    for (unsigned k = 0; k < 2; ++k) {
        const StartupControl &ctl = g.comp->child(k)->control();
        CHECK(ctl.timezone_calls == 0);
    }
#endif
    for (unsigned k = 0; k < 2; ++k) CHECK(g.c[k]->obs.entered.load());
}

void case_process_alloc_shared() {
    Comp2Rig g((Comp2Opt{QUOTA, false, true, PRIORITY}));   // configure deferred: this case configures with the two fields set
    StartupChildPpuPorts p = g.shared();
    p.process_services = &g.r.process_services; p.fixed_timezone = &g.r.process_timezone;
    StartupChildSlotBinding b[2] = {*g.bindings(), g.bindings()[1]};
    for (unsigned k = 0; k < 2; ++k) {
        b[k].policy.run_to_exit = true; b[k].policy.max_exit_polls = 2000;
        b[k].policy.exit_wait_poll = poll_sleep; b[k].policy.exit_wait_context = &g.polls_counter;
    }
    CHECK(g.comp->configure(p, b, 2) == StartupChildCompositionConfigure::Ok);
    g.r.primary.ppu_create = g.comp->create_port();
    // Both slots' children run syscall 348 (sys_memory_allocate): the PROCESS allocator really runs for each of them.
    for (unsigned k = 0; k < 2; ++k) {
        g.put32(k ? OPDB : OPDA, ENTRY_PROC_ALLOC);
        g.seed_raw(g.out[k], k ? OPDB : OPDA, g.out[k]);
        CHECK(g.run_parent() && g.r.primary_control->stop == StartupStop::None);
    }
    CHECK(g.r.process_alloc_calls.load() == 2);   // ONE real process allocation per child
    for (unsigned k = 0; k < 2; ++k) {
        CHECK(g.comp->report(k).observed_exited && g.comp->report(k).mapped_start == StartupPpuCreateStartStatus::Started);
    }
    for (unsigned k = 0; k < 2; ++k) g.wait_exited(k);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
    // Every CHILD resource is gone. The two process allocations remain charged to the PROCESS pool (they are the process's, not a
    // child's) until the process frees them: exactly the sharing decision 14 describes.
    CHECK(g.comp->released() && ps3_ppu_threads_live(&g.r.ppu) == 0 && g.r.used() == 2 * 0x10000);
    for (unsigned k = 0; k < 2; ++k) {
        const StartupControl &ctl = g.comp->child(k)->control();
        CHECK(ctl.syscall_number == 348 && ctl.memory_result.status == PS3_USER_MEMORY_SERVICE_OK);
        CHECK(g.c[k]->obs.entered.load() && g.c[k]->obs.r3 == 0);
    }
}

struct CompCase { const char *name; void (*run)(); };
// ============ R. lv2 syscall 44 (sys_ppu_thread_join), routed by the EXACT prepared id ============
// A run-to-exit child that really ended itself through sys_ppu_thread_exit is joined once with its exit value; a second join is
// AlreadyJoined; a blocked child is NotExited; an unknown id is NotFound; teardown() still releases every slot after a join.
void case_join_routing()
{
    Comp3Rig g;
    CHECK(g.create_first() && g.create_second());
    g.exit_polls.store(0);
    CHECK(g.create_third());
    const StartupChildCompositionReport &r3 = g.comp->report(2);
    CHECK(r3.observed_exited && r3.joined == false && r3.join_calls == 0);   // start() never joins by itself
    const uint32_t id2 = r3.prepared_id;
    CHECK(id2 != 0);
    // A run-to-exit child's own exit value (the third body exits with r3 = 0x00000007 through the real sys_ppu_thread_exit).
    const uint32_t exit_value = uint32_t(g.comp->child(2)->control().guest_exit_value);
    CHECK(g.comp->child(2)->control().has_guest_exit && exit_value == 0x00000007u);
    const StartupPpuCreatePort *port = g.comp->create_port();
    CHECK(port && port->join);
    // First join: Joined with the child's real exit value.
    StartupPpuJoinResult j0 = port->join(port->context, id2);
    CHECK(j0.status == StartupPpuJoinStatus::Joined && j0.exit_value == exit_value);
    CHECK(g.comp->report(2).joined && g.comp->report(2).join_calls == 1);
    CHECK(g.comp->totals().join_calls == 1 && g.comp->totals().join_unmatched == 0);
    // Second join of the same id: AlreadyJoined, nothing changes and nothing is written twice.
    StartupPpuJoinResult j1 = port->join(port->context, id2);
    CHECK(j1.status == StartupPpuJoinStatus::AlreadyJoined && j1.exit_value == 0);
    CHECK(g.comp->report(2).joined && g.comp->report(2).join_calls == 2 && g.comp->totals().join_calls == 2);
    // A blocked child is never joined.
    const uint32_t id0 = g.comp->report(0).prepared_id;
    CHECK(id0 != 0 && g.comp->report(0).start.status == StartupChildPpuStartStatus::Blocked && !g.comp->report(0).observed_exited);
    StartupPpuJoinResult jb = port->join(port->context, id0);
    CHECK(jb.status == StartupPpuJoinStatus::NotExited && jb.exit_value == 0);
    CHECK(!g.comp->report(0).joined && g.comp->report(0).join_calls == 1 && g.comp->totals().join_calls == 3);
    // An unknown / zero id touches no child.
    StartupPpuJoinResult ju = port->join(port->context, 0xDEADu);
    CHECK(ju.status == StartupPpuJoinStatus::NotFound);
    StartupPpuJoinResult jz = port->join(port->context, 0);
    CHECK(jz.status == StartupPpuJoinStatus::NotFound);
    CHECK(g.comp->totals().join_calls == 5 && g.comp->totals().join_unmatched == 2);
    CHECK(g.comp->report(0).join_calls == 1 && g.comp->report(1).join_calls == 0 && g.comp->report(2).join_calls == 2);
    // join() cancelled nothing and reaped nothing: every slot still holds its resources until teardown().
    CHECK(g.st(0).phase == StartupChildPpuPhase::Started && g.st(2).phase == StartupChildPpuPhase::Started);
    CHECK(!g.comp->released());
    CHECK(g.c[2]->thread.join_calls.load() == 0);   // the composition's own reap join is the only join of a host thread
    // Teardown still releases every slot after a join.
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(0).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(1).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
    CHECK(g.c[2]->thread.join_calls.load() == 1 && g.c[2]->thread.join_ok.load() == 1);
    g.expect_released_baseline();
}

// A join on a slot whose child end-ed without a recorded guest exit value would be a ProviderFailure, never an invented value.
// The composition's only realistic producer of that state is a child that returned through its start sentinel instead of exiting
// itself, so this case proves the composition refuses to join a child that never exited (NotExited) rather than guessing.
void case_join_never_invents()
{
    Comp3Opt o; o.third_entry = ENTRY_RETURN;   // a run_to_exit slot whose body RETURNS instead of exiting itself
    Comp3Rig g(o);
    CHECK(g.create_first() && g.create_second());
    CHECK(!g.create_third());   // start() reports Terminal (not Exited), so the parent stops ProviderFailure
    const StartupChildCompositionReport &r3 = g.comp->report(2);
    CHECK(!r3.observed_exited && r3.start_attempted);
    const StartupPpuCreatePort *port = g.comp->create_port();
    CHECK(port && port->join && r3.prepared_id != 0);
    const StartupPpuJoinResult j = port->join(port->context, r3.prepared_id);
    CHECK(j.status == StartupPpuJoinStatus::NotExited && j.exit_value == 0);   // never an invented exit value
    CHECK(g.comp->report(2).joined == false && g.comp->report(2).join_calls == 1);
    g.c[2]->obs.gate_open.store(true);
    CHECK(g.comp->teardown(g.td()).teardown == StartupChildCompositionTeardown::Released);
    CHECK(g.comp->report(2).teardown == StartupChildCompositionTeardown::Released && g.comp->released());
    g.expect_released_baseline();
}

const CompCase comp_cases[] = {
    {"process_services_shared",case_process_services_shared},
    {"process_alloc_shared",case_process_alloc_shared},
    {"disc_change_process_registry",case_disc_change_process_registry},
    {"system_profile_inheritance",case_system_profile_inheritance},
    {"presentation_topology_inheritance",case_presentation_topology_inheritance},
    {"presentation_mode_inheritance",case_presentation_mode_inheritance},
    {"system_sdk_inheritance",case_system_sdk_inheritance},
    {"system_parameters_inheritance",case_system_parameters_inheritance},
    {"boot_content_inheritance",case_boot_content_inheritance},
    {"configure", case_configure}, {"integration_blocked", case_integration_blocked},
    {"policy_mismatch", case_policy_mismatch}, {"stop_latches", case_stop_latches}, {"terminal_before_block", case_terminal_before_block},
    {"teardown_bounds", case_teardown_bounds}, {"watchdog_race", case_watchdog_race}, {"alloc_production_shape", case_alloc_production_shape},
    // two-slot composition (bounded 2 actual children)
    {"configure2", case_configure2}, {"coexist_blocked", case_coexist_blocked}, {"third_refusal", case_third_refusal}, {"policy_vs_slot", case_policy_vs_slot},
    {"start_routing", case_start_routing}, {"independent_waits", case_independent_waits}, {"partial_failure", case_partial_failure},
    {"cleanup_retention", case_cleanup_retention}, {"stop_both", case_stop_both}, {"watchdog_race2", case_watchdog_race2}, {"legacy_one_slot", case_legacy_one_slot},
    // three-slot composition with a run_to_exit third slot (decision 10 A/B)
    {"third_run_to_exit", case_third_run_to_exit}, {"third_run_to_exit_failures", case_third_run_to_exit_failures},
    {"run_to_exit_configure_bounds", case_run_to_exit_configure_bounds},
    {"run_to_exit_not_fenced", case_run_to_exit_not_fenced},
    // lv2 syscall 44 (sys_ppu_thread_join) routing
    {"join_routing", case_join_routing}, {"join_never_invents", case_join_never_invents}};
}

int run_isolated_comp(char **argv, size_t n)
{
    unsigned failed = 0;
    for (size_t i = 0; i < n; ++i) {
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) { execl("/proc/self/exe", argv[0], comp_cases[i].name, static_cast<char *>(nullptr)); _exit(127); }
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("ISOLATED %s %s (status=0x%x)\n", comp_cases[i].name, ok ? "PASS" : "FAIL", unsigned(status)); std::fflush(stdout);
        if (!ok) ++failed;
    }
    std::printf("ISOLATED_SUMMARY failed=%u of %zu\n", failed, n);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const size_t n = sizeof comp_cases / sizeof *comp_cases;
    if (argc > 1 && !strcmp(argv[1], "--isolated")) return run_isolated_comp(argv, n);
    std::thread(watchdog).detach();
    if (argc > 1 && !strcmp(argv[1], "--list")) { for (size_t i = 0; i < n; ++i) std::printf("%s\n", comp_cases[i].name); return 0; }
    for (size_t i = 0; i < n; ++i) {
        if (argc > 1 && strcmp(argv[1], comp_cases[i].name)) continue;
        arm(60000);
        std::printf("CASE %s\n", comp_cases[i].name); std::fflush(stdout);
        comp_cases[i].run();
        std::printf("PASS %s\n", comp_cases[i].name); std::fflush(stdout);
    }
    deadline_ms.store(0);
    std::printf("CHILD_COMPOSITION_ALL_PASS\n");
    return 0;
}
