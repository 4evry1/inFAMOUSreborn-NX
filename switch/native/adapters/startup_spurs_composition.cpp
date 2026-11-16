#include "startup_spurs_composition.h"
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
const char *startup_spurs_compose_status_name(StartupSpursComposeStatus status)
{
    switch (status) {
    case StartupSpursComposeStatus::Ok: return "ok";
    case StartupSpursComposeStatus::InvalidArgument: return "invalid-argument";
    case StartupSpursComposeStatus::NotAdmitted: return "execution-not-admitted";
    case StartupSpursComposeStatus::MissingConditions: return "missing-sync-registries";
    case StartupSpursComposeStatus::PolicyMismatch: return "policy-not-the-execution-policy";
    case StartupSpursComposeStatus::GuardsNotDistinct: return "guards-not-distinct";
    case StartupSpursComposeStatus::AlreadyComposed: return "already-composed";
    case StartupSpursComposeStatus::DomainWaitPort: return "domain-wait-port";
    case StartupSpursComposeStatus::ChargeRecords: return "charge-records";
    case StartupSpursComposeStatus::Lv2Init: return "lv2-init";
    case StartupSpursComposeStatus::PpuInit: return "ppu-init";
    case StartupSpursComposeStatus::MainIdentity: return "main-identity";
    case StartupSpursComposeStatus::PolicyAttach: return "policy-attach";
    case StartupSpursComposeStatus::RuntimeInit: return "runtime-init";
    }
    return "unknown";
}
static bool guard_complete(const ps3_instance_guard &guard) { return guard.context && guard.lock && guard.unlock; }
StartupSpursComposeStatus startup_spurs_compose(StartupExecution &execution, StartupSpursComposition &composition,
    ps3_atomic_domain &domain, ps3_user_memory &memory, ps3_spu_resource_policy &policy, const StartupSpursPorts &ports)
{
    using Status = StartupSpursComposeStatus;
    if (execution.spurs || composition.runtime.initialized || composition.lv2.initialized || composition.ppu.initialized)
        return Status::AlreadyComposed;
    if (!ports.notify.wait || !ports.notify.wake_all || !guard_complete(ports.lv2_guard) || !guard_complete(ports.ppu_guard) ||
        !guard_complete(policy.guard) || !domain.initialized || !guard_complete(domain.guard))
        return Status::InvalidArgument;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned role = 0; role < 3; ++role)
            if (!ports.threads[i][role].start || !ports.threads[i][role].join) return Status::InvalidArgument;
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) if (!ports.local_stores[i]) return Status::InvalidArgument;
    // Admitted identity: the real main thread id and the creator priority come from the admitted execution.
    auto &cpu = execution.ppu.cpu;
    if (!execution.ppu.admitted || !execution.control || !execution.has_guest_priority || !cpu.thread_id ||
        cpu.thread_id > UINT32_MAX || execution.guest_priority <= 0 || execution.guest_priority > 3071)
        return Status::NotAdmitted;
    if (!execution.conditions || !execution.conditions->mutexes) return Status::MissingConditions;
    if (execution.spu_resources != &policy) return Status::PolicyMismatch;
    // Lock order policy -> lv2 -> port: every nesting level needs its own guard.
    const void *const contexts[4] = {ports.lv2_guard.context, ports.ppu_guard.context, policy.guard.context, domain.guard.context};
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = a + 1; b < 4; ++b)
            if (contexts[a] == contexts[b]) return Status::GuardsNotDistinct;
    // ---- effects ----
    if (ps3_atomic_domain_attach_wait_port(&domain, ports.notify) != PS3_GUEST_MEMORY_OK) return Status::DomainWaitPort;
    if (ps3_user_memory_attach_charges(&memory, composition.charges, PS3_STARTUP_SPURS_CHARGE_RECORDS) != PS3_USER_MEMORY_OK)
        return Status::ChargeRecords;
    if (ps3_lv2_objects_init(&composition.lv2, ports.lv2_guard, ports.notify) != PS3_LV2_OK) return Status::Lv2Init;
    if (ps3_ppu_threads_init(&composition.ppu, ports.ppu_guard, ports.notify, &memory) != PS3_PPU_OK) return Status::PpuInit;
    if (ps3_ppu_threads_adopt_main(&composition.ppu, uint32_t(cpu.thread_id)) != PS3_PPU_OK ||
        ps3_ppu_threads_main_id(&composition.ppu) != uint32_t(cpu.thread_id))
        return Status::MainIdentity;
    if (!ps3_spu_resource_policy_attach(&policy, &memory, &composition.lv2, ports.notify)) return Status::PolicyAttach;
    ps3_spurs_runtime_config config{};
    config.domain = &domain;
    config.notify = ports.notify;
    config.lv2 = &composition.lv2;
    config.policy = &policy;
    config.ppu = &composition.ppu;
    config.sync = native_startup_spurs_sync_port(&execution);
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned role = 0; role < 3; ++role) config.threads[i][role] = ports.threads[i][role];
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) config.local_stores[i] = ports.local_stores[i];
    // Native scheduling policy [R-PRIO], not an OS guarantee: services run at 249/ppuPriority and every
    // create is fenced by the real blocked-word handshake, whatever the host scheduler does.
    config.creator_priority = uint32_t(execution.guest_priority);
    if (ps3_spurs_runtime_init(&composition.runtime, &config) != 0) return Status::RuntimeInit;
    execution.spurs_quota_memory = &memory;   // the allocator the charges join; read-only observation port
    execution.event_queues = &composition.lv2;   // guest sc128 creates in the SAME registry the SPURS services own
    execution.spurs = &composition.runtime;   // only after every owner is constructed and attached
    return Status::Ok;
}
#endif
