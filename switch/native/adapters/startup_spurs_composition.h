#ifndef NATIVE_STARTUP_SPURS_COMPOSITION_H
#define NATIVE_STARTUP_SPURS_COMPOSITION_H
#include "startup_execution.h"
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
/* N12 application composition of the native SPURS runtime (aa6269a8). This is the ONE place that
 * constructs the stable, caller-owned object graph the bridge needs and injects it as
 * StartupExecution::spurs. It is OS-neutral: the shared notify port, the guards and the service
 * thread ports are injected by the composition (Horizon on target, pthread in host tests), so the
 * same wiring code is what the host bridge test exercises.
 *
 * Preconditions (all checked, nothing assumed): execution is ADMITTED (its cpu.thread_id is the real
 * main PPU thread, its guest priority is the creator priority), execution.conditions carries the
 * real lwmutex/lwcond registries (sync adapters), execution.spu_resources is the caller's policy
 * (configured only by the guest's own sys_spu_initialize path, never here), `policy` is exactly that
 * policy and is already constructed with a guard of its own, `memory` is the user-memory allocator
 * that serves the guest's own allocation syscalls (SPU/service charges share its quota).
 *
 * Effects, in order: the ONE notify port is attached to the atomic domain; charge records are attached
 * to `memory`; lv2 objects and PPU service registry are constructed (distinct guards, same port); the
 * REAL main thread id is adopted into the service registry (L5); the policy is attached to memory, lv2
 * and the same port; the runtime is constructed; ONLY THEN execution.spurs is set. Any failure returns
 * a non-Ok status with execution.spurs still null (the bridge then stops MissingServices); partially
 * attached owners stay attached, so a failed composition is fatal for the process and the caller
 * must keep all storage alive. Storage lifetime: zero-filled, stable, outlives confirmed joins of
 * every service thread (native_startup_release_after_join first, then everything else). */
enum { PS3_STARTUP_SPURS_CHARGE_RECORDS = 16 };
struct StartupSpursComposition {
    ps3_lv2_objects lv2{};
    ps3_ppu_threads ppu{};
    ps3_user_memory_charge_record charges[PS3_STARTUP_SPURS_CHARGE_RECORDS]{};
    ps3_spurs_runtime runtime{};
};
struct StartupSpursPorts {
    ps3_notify_port notify{};                 // THE shared port: domain, lv2, ppu, policy and runtime
    ps3_instance_guard lv2_guard{}, ppu_guard{};   // distinct from each other, the policy's and the domain's
    ps3_service_thread_port threads[PS3_SPURS_MAX_INSTANCES][3]{};   // helper, handler, kernel worker
    unsigned char *local_stores[PS3_SPURS_LS_SLOTS]{};               // PS3_SPURS_K1_LS_BYTES each, caller-owned
};
enum class StartupSpursComposeStatus {
    Ok = 0, InvalidArgument, NotAdmitted, MissingConditions, PolicyMismatch, GuardsNotDistinct, AlreadyComposed,
    DomainWaitPort, ChargeRecords, Lv2Init, PpuInit, MainIdentity, PolicyAttach, RuntimeInit
};
StartupSpursComposeStatus startup_spurs_compose(StartupExecution &execution, StartupSpursComposition &composition,
    ps3_atomic_domain &domain, ps3_user_memory &memory, ps3_spu_resource_policy &policy, const StartupSpursPorts &ports);
const char *startup_spurs_compose_status_name(StartupSpursComposeStatus);
#endif
#endif
