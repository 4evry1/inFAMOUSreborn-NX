// Real dispatcher, coherent memory and owned runtime; no policy execution is substituted here.
#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t REGISTER_NID = 0xc0158d8b;
constexpr uint32_t POLICY_NULL = 0x80410811, POLICY_ALIGN = 0x80410810, POLICY_INVAL = 0x80410802;

void workload_image(Runtime &r, uint32_t revision, uint32_t module, uint32_t minimum) {
    unsigned char image[56]{};
    const auto put = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) image[offset + i] = uint8_t(value >> (24 - 8 * i));
    };
    put(0, revision); put(4, 0x220002); put(8, module); put(12, 0x2d80);
    put(20, 0x404e9300);
    for (unsigned i = 0; i < 5; ++i) image[24 + i] = 2;
    put(32, minimum); put(36, 5); put(40, 0x5ecef8); put(44, 0x915c80);
    r.put(ATTR, image, sizeof image);
}

void call_registration(Runtime &r, uint64_t spurs, uint64_t wid, uint64_t attribute) {
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
    cpu.gpr[3] = spurs; cpu.gpr[4] = wid; cpu.gpr[5] = attribute;
    r.run(REGISTER_NID, false);
}

void expect_policy_error(uint64_t spurs, uint64_t wid, uint64_t attr,
                         uint32_t revision, uint32_t module, uint32_t minimum, uint32_t expected) {
    Runtime r;
    workload_image(r, revision, module, minimum);
    r.put32(PORT_EA, 0xfeedface);
    const size_t before = r.published();
    call_registration(r, spurs, wid, attr);
    CHECK(r.control->stop == StartupStop::None);
    CHECK(r.execution.ppu.cpu.gpr[3] == rc_reg(expected));
    CHECK(r.execution.ppu.cpu.gpr[2] == TOC && r.get(TOC_SLOT, 8) == TOC);
    CHECK(r.get(PORT_EA, 4) == 0xfeedface);
    const auto events = r.since(before);
    CHECK(!events.empty() && is_event(events.back(), true, TOC_SLOT, 8, TOC));
    for (const auto &event : events)
        CHECK(event.operation != PpuMemoryTraceOperation::Write || event.address == TOC_SLOT);
}
struct RegistrationServices {
    ps3_pthread_guard guards[PS3_SPURS_MAX_INSTANCES]{};
    std::atomic<unsigned> unexpected_execution{0};
    RegistrationServices() { for (auto &guard : guards) CHECK(!ps3_pthread_guard_init(&guard)); }
    ~RegistrationServices() { for (auto &guard : guards) ps3_pthread_guard_destroy(&guard); }
    static ps3_spurs_policy_execution_result refuse_begin(void *context, const ps3_spurs_policy_execution_request *) {
        ++static_cast<RegistrationServices *>(context)->unexpected_execution;
        ps3_spurs_policy_execution_result result{}; result.status = PS3_SPURS_POLICY_UNSUPPORTED; return result;
    }
    static ps3_spurs_policy_execution_result refuse_step(void *context, uint32_t, uint64_t) { return refuse_begin(context, nullptr); }
    static ps3_spurs_policy_execution_status refuse_lifecycle(void *, uint32_t) { return PS3_SPURS_POLICY_UNSUPPORTED; }
    void install(Runtime &runtime) {
        ps3_instance_guard ports[PS3_SPURS_MAX_INSTANCES];
        ps3_spurs_policy_execution_port execution[PS3_SPURS_MAX_INSTANCES];
        for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) {
            ports[i] = ps3_pthread_guard_port(&guards[i]);
            execution[i] = {this, refuse_begin, refuse_step, refuse_step, refuse_lifecycle, refuse_lifecycle};
        }
        CHECK(!ps3_spurs_runtime_set_workload_services(&runtime.comp->runtime, ports, execution));
    }
};
void real_registration_without_ready_jobs() {
    RegistrationServices services;
    {
        Runtime r; services.install(r); r.boot(); workload_image(r, 1, 0x69c880, 1);
        r.put32(PORT_EA, 0xfeedface);
        const size_t from = r.published();
        const uint64_t enabled_before = r.get(INSTANCE1 + 0xb0, 4);
        call_registration(r, INSTANCE1, PORT_EA, ATTR);
        const auto &observation = r.control->spurs_registration;
        CHECK(r.control->stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == 0);
        CHECK(r.get(PORT_EA, 4) == 0 && r.get(TOC_SLOT, 8) == TOC);
        CHECK(observation.has_domain_result && observation.has_result && observation.domain_result.sdk_result == 0);
        CHECK(observation.domain_result.effects & PS3_SPURS_REGISTRATION_RUNNABLE_PUBLISHED);
        CHECK(r.get(INSTANCE1 + 0xb0, 4) == (enabled_before | 0x80000000u) && r.get(INSTANCE1 + 0xb4, 4) == 0x80000000u);
        CHECK(r.get(INSTANCE1 + 0x80, 1) == 2 && r.get(INSTANCE1, 1) == 0);
        CHECK(r.get(INSTANCE1 + 0xb00, 8) == 0x69c880 && r.get(INSTANCE1 + 0xb08, 8) == 0x404e9300);
        CHECK(observation.publications == r.since(from).size());
        CHECK(r.control->spurs_registration_calls == 1 && r.control->spurs_registration_returns == 1);
        CHECK(services.unexpected_execution.load() == 0);
    }
    CHECK(services.unexpected_execution.load() == 0);
}
void observer_distinguishes_proposals_from_effects() {
    Runtime r;
    SpursAttachMemory context{&r.execution};
    const unsigned char proposed[8]{0, 0, 0, 0, 0, 0, 0, 9};
    const size_t before = r.published();
    // Synthetic observer inputs test transformation only, not scheduler or CAS execution.
    const ps3_spurs_registration_event failed_cas{PS3_SPURS_REGISTRATION_COMPARE_EXCHANGE, PORT_EA, 8, proposed, 7, 9, 0};
    CHECK(spurs_registration_observe(&context, &failed_cas));
    auto events = r.since(before);
    CHECK(events.size() == 1 && is_event(events[0], false, PORT_EA, 8, 7));
    const ps3_spurs_registration_event failed_sc{PS3_SPURS_REGISTRATION_CONDITIONAL_LINE, PORT_EA, 8, proposed, 0, 0, 0};
    CHECK(spurs_registration_observe(&context, &failed_sc));
    CHECK(r.since(before).size() == 1 && r.control->spurs_registration.publications == 1);
    r.leave_trace_capacity(0);
    CHECK(!spurs_registration_observe(&context, &failed_cas));
    CHECK(context.trace_failure && r.control->spurs_registration.publications == 1);
}
void committed_effect_survives_trace_refusal() {
    RegistrationServices services;
    Runtime r; services.install(r); r.boot(); workload_image(r, 1, 0x69c880, 1);
    r.put32(PORT_EA, 0xfeedface);
    // Leave room for the decoded attribute and a small real domain prefix, not the entire registration.
    r.leave_trace_capacity(20);
    call_registration(r, INSTANCE1, PORT_EA, ATTR);
    const auto &observation = r.control->spurs_registration;
    CHECK(r.control->stop == StartupStop::TraceCapacity);
    CHECK(observation.has_domain_result && !observation.has_result && observation.trace_failure);
    CHECK(observation.domain_result.effects & PS3_SPURS_REGISTRATION_SLOT_CLAIMED);
    CHECK(observation.domain_result.writes > 0 && observation.domain_result.stop == PS3_SPURS_REGISTRATION_OBSERVER);
    CHECK(r.instance().state == PS3_SPURS_INSTANCE_ACTIVE && r.control->spurs_registration_returns == 0);
    CHECK(services.unexpected_execution.load() == 0);
}
}

int main() {
    expect_policy_error(0, 0, 0, 2, 3, 0, POLICY_NULL);
    expect_policy_error(0, 0, ATTR + 1, 2, 3, 0, POLICY_ALIGN);
    expect_policy_error(0, 0, ATTR, 2, 3, 0, POLICY_INVAL);
    expect_policy_error(INSTANCE1 + 1, PORT_EA, ATTR, 1, 0, 1, POLICY_NULL);
    expect_policy_error(INSTANCE1 + 1, PORT_EA, ATTR, 1, 0x69c880, 1, POLICY_ALIGN);
    expect_policy_error(INSTANCE1, PORT_EA, ATTR, 1, 0x69c880, 0, POLICY_INVAL);
    expect_policy_error(UINT64_C(0xabcd000000000000), PORT_EA,
                        UINT64_C(0x1234000000000000) | ATTR, 1, 0x69c880, 1, POLICY_NULL);
    {
        Runtime r; r.boot();
        workload_image(r, 1, 0x69c880, 1);
        r.put32(PORT_EA, 0xfeedface);
        const uint64_t saved_toc = r.get(TOC_SLOT, 8);
        call_registration(r, INSTANCE1, PORT_EA, ATTR);
        CHECK(r.control->stop == StartupStop::SpursWorkloadStop);
        CHECK(r.control->spurs_registration.has_domain_result);
        CHECK(r.control->spurs_registration.domain_result.stop == PS3_SPURS_REGISTRATION_NO_EXECUTION_SERVICES);
        CHECK(r.control->spurs_registration.domain_result.effects == 0 && !r.control->spurs_registration.has_result);
        CHECK(r.control->spurs_registration_returns == 0 && r.get(TOC_SLOT, 8) == saved_toc);
        CHECK(r.execution.ppu.cpu.gpr[3] == INSTANCE1 && r.get(PORT_EA, 4) == 0xfeedface);
    }
    real_registration_without_ready_jobs();
    committed_effect_survives_trace_refusal();
    observer_distinguishes_proposals_from_effects();
    std::printf("SPURS_REGISTRATION_HLE_FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}
