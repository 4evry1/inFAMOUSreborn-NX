#ifndef NATIVE_STARTUP_EXECUTION_H
#define NATIVE_STARTUP_EXECUTION_H
#ifndef PS3_NATIVE_CONTEXT_MEMORY
#error "Common startup owner requires the explicit context-memory variant"
#endif
#if defined(PS3_NATIVE_STARTUP_SPU_INITIALIZE) && !defined(PS3_NATIVE_STARTUP_LWMUTEX)
#error "SPU initializer requires the STARTUP_LWMUTEX syscall bridge"
#endif
#if defined(PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES) && !defined(PS3_NATIVE_STARTUP_LWMUTEX)
#error "SPURS attributes require the STARTUP_LWMUTEX HLE bridge"
#endif
#if defined(PS3_NATIVE_STARTUP_SPURS_INITIALIZE) && \
    (!defined(PS3_NATIVE_STARTUP_LWMUTEX) || !defined(PS3_NATIVE_STARTUP_LWCOND) || !defined(PS3_NATIVE_STARTUP_SPU_INITIALIZE) || !defined(PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES))
#error "SPURS initialize requires STARTUP_LWMUTEX, STARTUP_LWCOND (sync registries), STARTUP_SPU_INITIALIZE (shared SPU policy) and STARTUP_SPURS_ATTRIBUTES"
#endif
#if defined(PS3_NATIVE_STARTUP_THREAD_PRIORITY) && !defined(PS3_NATIVE_STARTUP_LWMUTEX)
#error "Thread priority query requires the STARTUP_LWMUTEX syscall bridge"
#endif
#if defined(PS3_NATIVE_STARTUP_CURRENT_TIME) && (!defined(PS3_NATIVE_STARTUP_LWMUTEX) || !defined(PS3_NATIVE_STARTUP_MEMORY) || !defined(PS3_NATIVE_STARTUP_CLOCK) || !defined(PS3_NATIVE_STARTUP_TIMEBASE))
#error "Current-time variant requires STARTUP_LWMUTEX, STARTUP_MEMORY, STARTUP_CLOCK and STARTUP_TIMEBASE"
#endif
#if defined(PS3_NATIVE_STARTUP_TIMEZONE) && \
    (!defined(PS3_NATIVE_STARTUP_LWMUTEX) || !defined(PS3_NATIVE_STARTUP_MEMORY) || \
     !defined(PS3_NATIVE_STARTUP_CLOCK) || !defined(PS3_NATIVE_STARTUP_TIMEBASE))
#error "Timezone variant requires STARTUP_LWMUTEX, STARTUP_MEMORY, STARTUP_CLOCK and STARTUP_TIMEBASE"
#endif
#if defined(PS3_NATIVE_STARTUP_TIMEBASE_COUNTER) && \
    (!defined(PS3_NATIVE_STARTUP_LWMUTEX) || !defined(PS3_NATIVE_STARTUP_MEMORY) || \
     !defined(PS3_NATIVE_STARTUP_CLOCK) || !defined(PS3_NATIVE_STARTUP_TIMEBASE))
#error "Timebase-counter variant requires STARTUP_LWMUTEX, STARTUP_MEMORY, STARTUP_CLOCK and STARTUP_TIMEBASE"
#endif
#if defined(PS3_NATIVE_STARTUP_LWCOND) && \
    (!defined(PS3_NATIVE_STARTUP_LWMUTEX) || !defined(PS3_NATIVE_STARTUP_MEMORY) || \
     !defined(PS3_NATIVE_STARTUP_CLOCK) || !defined(PS3_NATIVE_STARTUP_TIMEBASE))
#error "Lwcond variant requires STARTUP_LWMUTEX, STARTUP_MEMORY, STARTUP_CLOCK and STARTUP_TIMEBASE"
#endif
#include "ppu_memory_context.h"
#include "../domain/lv2_spu.h"
#include "execution_services.h"
#include "compiled_dispatch.h"
#include "calendar_clock.h"
#include "../../../ps3recomp/runtime/sync/lwcond.h"
#include "../../../ps3recomp/runtime/sync/guard.h"
#include "../../../ps3recomp/runtime/process/exitspawn_callbacks.h"
#include "../diagnostics/module_admission.h"
#include "../../../ps3recomp/runtime/sysutil/callback_registry.h"
#include "../domain/spu_resource_policy.h"
#include "../domain/boot_content.h"
#include "../domain/system_parameters.h"
#include "../domain/system_profile.h"
#include "../domain/presentation_topology.h"
#include "../domain/presentation_mode.h"
#include "../domain/gcm_init_service.h"
#include "../domain/gamedata_service.h"
/* GCM-init (PS3_NATIVE_GCM_INIT) and game data (PS3_NATIVE_GAMEDATA) are compile-time features in
 * startup_execution.cpp only. Every GCM and game-data type, stop value and StartupControl/
 * StartupExecution/StartupPpuCreateRequest member below stays unconditional so both builds have
 * exactly the same struct layout and a feature cannot change any other adapter's ABI. */
#include "../domain/disc_change_callbacks.h"
#include "../domain/spurs_attributes.h"
/* Batch i1 (decision 22) appends three accepted domain units to this adapter. Every port, observation and
 * StartupControl/StartupExecution member below is UNCONDITIONAL, exactly as for gamedata and the TTY ring: only the
 * dispatch bodies and the ps3_hle_call router lines live behind PS3_NATIVE_STARTUP_PAD / PS3_NATIVE_STARTUP_FS.
 * spurs_lfqueue_attach.h is included in startup_execution.cpp's existing PS3_NATIVE_STARTUP_SPURS_INITIALIZE block
 * instead, the same way spurs_lfqueue.h is, so the feature-off build carries none of those builders.
 * These three headers are platform-free C11; the two domain ones are the same units their own verifiers compile. */
#include "../domain/pad_service.h"
#include "../domain/audio_service.h"
#include "../domain/lv2_timer.h"
#include "../domain/rsx_live_fifo.h"
#include "../domain/fs_service.h"
#include "fs_storage_posix.h"
/* SPURS taskset setup block: the two pure struct-init byte-image builders. Unconditional for the
 * same reason as gamedata_service.h -- the builders are platform-free and the adapter's use of them
 * is a .cpp-only compile-time feature. */
#include "../domain/spurs_taskset_init.h"
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
#include "../domain/spurs_instance.h"
#include "../domain/spurs_workload_registration.h"
#include "../domain/spurs_idle_request.h"
#include "../domain/spurs_exception_handler.h"
#include "../domain/spurs_ready_count.h"
#include "../domain/spurs_workload_info.h"
#include "../domain/lv2_objects.h"
#include "../../../ps3recomp/runtime/memory/user_memory.h"
#endif
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
class DiscoveryBudgetBinding;
namespace infamous::diagnostics { class StartupObservation; }

/* Root (2026-11-12): sampling profile of the guest PPU threads. Each thread publishes what it is in (guest code, an
 * lv2 syscall, an import) in one word; a sampler thread reads the words and counts. Diagnostic only. */
extern std::atomic<uint32_t> g_movie_pool_calls, g_movie_pool_zero, g_movie_pool_waits, g_movie_pool_wait_timeouts, g_movie_pool_last_player;
void startup_activity_sample();
void startup_activity_print();
void startup_activity_print_calls(unsigned seconds);   // cumulative call counts of the busiest threads, for deltas between marks
/* Root (2026-11-12, Decision 83): what cellSaveDataListAutoLoad did, for the final log. */
struct StartupSaveDataRecord {
    uint32_t calls = 0, returns = 0, version = 0, func_fixed = 0, func_stat = 0, result = 0;
    uint32_t fixed_called = 0, stat_called = 0;
    int32_t fixed_result = 0, stat_result = 0;
    uint32_t list_load_calls = 0, list_load_result = 0; int32_t list_load_verdict = 0;
    char dir_name[40]{};
    /* Decision 129: the two save imports (cellSaveDataListSave2, cellSaveDataAutoSave2) and the last save's result. */
    uint32_t list_save_calls = 0, auto_save_calls = 0, save_result = 0;
};
extern StartupSaveDataRecord g_savedata_auto_load;
struct StartupSpuCallRecord { uint32_t number, a0, a1, a2, result; };
inline constexpr uint32_t kStartupSpuCallLogCapacity = 96;
extern StartupSpuCallRecord g_startup_spu_call_log[kStartupSpuCallLogCapacity];
extern std::atomic<uint32_t> g_startup_spu_call_count;
enum class StartupStop { None, Returned, UnknownImport, UnsupportedExternal,
    InvalidMemory, Cancelled, Deadline, TraceCapacity, MissingServices, ProviderFailure, UnsupportedSyscall, MutexProviderFailure, MemoryProviderFailure, ClockProviderFailure, DispatchFailure,
    GeneratedBudget, HleSyscallBudget, IndirectBudget, BudgetClockFailure, BudgetInvalidInput,
    ObservationCapacity, ConditionProviderFailure, ExitspawnProviderFailure,
    DiagnosticModuleAdmissionFailure, SysutilCallbackFailure, SpuPolicyFailure,
    SpursKernelFence, SpursInitializeStop, SpursAttachStop, SpursInfoStop, SpursWorkloadStop, SpursIdleStop, SpursExceptionStop, SpursHelperFence, SpursReadyStop, SpursTaskCreateStop, TaskwaitStop, SpursWorkloadInfoStop, BootDiscStop, SystemParameterStop, SystemStringStop, DiscChangeStop, VideoOutputCountStop, VideoDeviceInfoStop, VideoAvailabilityStop, VideoResolutionStop, VideoStateStop, GcmInitStop, GcmQueryStop, VideoConfigureStop, GuestThreadExit,
    // Decision 33 / B2b: phase C1's boundary. APPENDED -- every earlier value keeps its number.
    SpuThreadGroupStart };
enum class StartupVideoOutputCountFailure { None, UnconfiguredTopology, InvalidTopology, UnsupportedSelector };
struct StartupVideoOutputCountObservation {
    uint64_t raw_video_out=0;
    uint32_t video_out=0;
    PresentationTopology topology=PresentationTopology::Unconfigured;
    PresentationInventoryStatus inventory_status=PresentationInventoryStatus::Unconfigured;
    StartupVideoOutputCountFailure failure=StartupVideoOutputCountFailure::None;
    int32_t count=0;
    bool inventory_checked=false,toc_written=false,has_result=false;
    size_t publications=0;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
enum class StartupVideoBridgeFailure { None, UnsupportedTopology, UnsupportedMode, WrongLr, WrongSelector, WrongIndex, WrongOutput, InvalidExtent, UnknownResolution, NullOutput, InvalidPointer };
struct StartupVideoDeviceInfoObservation {
    uint64_t raw_video_out=0, raw_index=0, raw_output=0;
    uint32_t video_out=0, index=0, output=0;
    PresentationTopology topology=PresentationTopology::Unconfigured;
    NativePresentationMode mode=NativePresentationMode::Unconfigured;
    StartupVideoBridgeFailure failure=StartupVideoBridgeFailure::None;
    size_t bytes_written=0, publications=0;
    bool bridge_checked=false, toc_written=false, has_result=false;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
struct StartupVideoAvailabilityObservation {
    uint64_t raw_selector=0, raw_resolution_id=0, raw_aspect=0, raw_option=0;
    uint32_t selector=0, resolution_id=0, aspect=0, option=0;
    PresentationTopology topology=PresentationTopology::Unconfigured;
    NativePresentationMode mode=NativePresentationMode::Unconfigured;
    StartupVideoBridgeFailure failure=StartupVideoBridgeFailure::None;
    int32_t available=0;
    size_t publications=0;
    bool toc_written=false, has_result=false;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
struct StartupVideoResolutionObservation {
    uint64_t raw_resolution_id=0, raw_output=0;
    uint32_t resolution_id=0, output=0, result=0;
    StartupVideoBridgeFailure failure=StartupVideoBridgeFailure::None;
    uint32_t width=0, height=0;
    size_t writes=0, publications=0;
    bool matched=false, height_written=false, width_written=false, toc_written=false, has_result=false;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
// Consumed-view specialization of cellVideoOutGetState: the two bound callers
// func_00115870 (caller 1) and func_003AA898 (caller 2), only for their logical
// 720p target. The written bytes are a declared native policy at the
// authenticated displayMode offsets, not the SDK halfword order.
enum class StartupVideoStateFailure { None, UnsupportedTopology, UnsupportedMode, WrongLr, WrongSelector, WrongIndex, WrongOutput, InvalidExtent };
struct StartupVideoStateObservation {
    uint64_t raw_selector=0, raw_index=0, raw_output=0;
    uint32_t selector=0, index=0, output=0;
    PresentationTopology topology=PresentationTopology::Unconfigured;
    NativePresentationMode mode=NativePresentationMode::Unconfigured;
    StartupVideoStateFailure failure=StartupVideoStateFailure::None;
    size_t bytes_written=0, publications=0;
    bool bridge_checked=false, toc_written=false, has_result=false;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
    uint64_t lr=0;            // LR observed at entry
    uint32_t caller=0;        // 0 none, 1 func_00115870, 2 func_003AA898
};
// Consumed view of cellVideoOutConfigure bound to func_003AA898 at LR 0x003AAB44:
// accepts exactly the game's traced 720p logical target record and makes no
// physical display claim and no guest store beyond the TOC return.
enum class StartupVideoConfigureFailure { None, UnsupportedTopology, UnsupportedMode, WrongLr, WrongVideoOut, HighBits, InvalidExtent, UnsupportedConfiguration, UnsupportedOption };
struct StartupVideoConfigureObservation {
    uint64_t lr=0, raw_r3=0, raw_r4=0, raw_r5=0, raw_r6=0;
    uint8_t record[16]{};
    StartupVideoConfigureFailure failure=StartupVideoConfigureFailure::None;
    size_t publications=0;
    bool toc_written=false, has_result=false;
};
struct StartupDiscChangeObservation {
    uint64_t raw_eject=0, raw_insert=0;
    ps3_disc_change_result registration{};
    bool registration_attempted=false, toc_written=false, has_result=false;
    size_t publications=0;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
enum class StartupSystemStringFailure { None, InvalidProfile, UnimplementedParameter };
struct StartupSystemStringObservation {
    uint64_t raw_id=0, raw_output=0, raw_size=0;
    uint32_t id=0, output=0, size=0, result=0;
    SystemProfileSource source=SystemProfileSource::Unconfigured;
    SystemProfileStatus profile_status=SystemProfileStatus::Unconfigured;
    StartupSystemStringFailure failure=StartupSystemStringFailure::None;
    size_t text_length=0, output_write_attempts=0, output_written_bytes=0, publications=0;
    bool profile_checked=false, text_checked=false, toc_written=false, has_result=false;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
// The validated process-parameter SDK, shared by independent system services.
/* Decision 33 / B2a: the two services the lv2 sync arms need beyond the lv2 object registry.
 * `sleep_microseconds` performs a real sleep, returning false when it was cancelled; it must not hold
 * any lv2 guard. `cancel` is the SAME cooperative token the queue receive uses, shared so a blocked
 * semaphore or event-flag waiter stops on the same discovery deadline/cancellation as everything else. */
struct StartupLv2SleepPort {
    void *context = nullptr;
    bool (*sleep_microseconds)(void *context, uint64_t microseconds, bool cancelled) = nullptr;
};
struct StartupProcessSdk {
    uint32_t raw = 0;
    bool valid = false;
};
enum class StartupSystemParameterFailure { None, MissingLanguage, UnsupportedLanguage, MissingProfile, InvalidProfile, UnimplementedParameter };
struct StartupSystemParameterObservation {
    uint64_t raw_id=0, raw_output=0;
    uint32_t id=0, effective_id=0, output=0, value=0, result=0;
    StartupProcessSdk process_sdk{};
    SystemParameters settings{};
    StartupSystemParameterFailure failure=StartupSystemParameterFailure::None;
    bool settings_checked=false, legacy_projection=false, output_written=false, toc_written=false, has_result=false;
    size_t output_write_attempts=0, publications=0;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
};
enum class StartupBootDiscFailure { None, MissingMetadata, InvalidMetadata };
struct StartupBootDiscObservation {
    uint64_t raw_output = 0;
    uint32_t output = 0, result = 0;
    BootContentMetadata metadata{};
    bool metadata_checked = false, has_result = false, toc_written = false;
    size_t output_write_attempts = 0, output_writes = 0, output_written_bytes = 0, publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupBootDiscFailure failure = StartupBootDiscFailure::None;
};
enum class StartupEntry { Game, TlsStub, UnsupportedExternal, MutexCreateStub, MutexLockStub, MutexTryStub, MutexUnlockStub, MutexDestroyStub, MemorySyscall, ClockStub, MemorySc32, IndirectCall };
enum class SysutilCallbackArgument { None, Slot, Descriptor, Userdata };
#ifdef PS3_NATIVE_THREAD_ID_QUERY
enum { STARTUP_SPAN_CAPACITY = 12, STARTUP_TRACE_CAPACITY = 4096, STARTUP_IMPORT_CAPACITY = 64 };
#else
enum { STARTUP_SPAN_CAPACITY = 12, STARTUP_TRACE_CAPACITY = 2048, STARTUP_IMPORT_CAPACITY = 32 };
#endif
struct StartupSpan { uint32_t address; uint64_t length; bool writable; };
using StartupAccess = PpuMemoryTraceEvent;
constexpr size_t STARTUP_DISPATCH_CAPACITY = 32;
struct StartupDispatchRecord {
    uint64_t captured_ctr = 0;
    uint32_t depth_before = 0, depth_after = 0;
    ppu_context before{};
    ps3_compiled_dispatch_result result{};
    bool core_returned = false;
};
struct StartupSpursAttributeObservation {
    uint64_t arguments[7]{};
    uint32_t nid = 0, result = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    bool has_result = false, memory_checked = false, write_attempted = false;
    bool last_write_is_toc = false;
    size_t successful_writes = 0, publications = 0, attribute_bytes = 0;
    uint32_t last_write_address = 0, last_attribute_address = 0;
    size_t last_write_length = 0, last_attribute_length = 0;
    unsigned char last_attribute_bytes[PS3_SPURS_ATTRIBUTE_SIZE]{};
};
/* Diagnostic observation of the LAST _cellSpursWorkloadAttributeInitialize (NID 0xefeb2679) call (single PPU writer; read after
 * join). Reset at call entry; independent of every spurs_attribute*, spurs_info_* and spurs_attach_* field. arguments are
 * r3..r10 exactly as received (full 64 bits); the ninth argument is the BE64 stack slot at (SP+0x70)&~3 whose low 32 bits
 * are max_contention. Every effect flag is set at the actual successful operation, BEFORE its trace publication, and is
 * never rolled back: toc_written can be true with has_result false when the TOC publication failed. image_written_bytes
 * is 512 only after the coherent write returned OK (a failed write claims no bytes). memory_status is the status of the
 * most recent coherent operation. publications counts successful HLE publications (reads, image words, TOC). */
struct StartupSpursWorkloadAttributeObservation {
    uint64_t arguments[8]{};
    uint64_t sp = 0, stack_raw = 0;
    uint32_t stack_slot = 0, max_contention = 0, priority_address = 0;
    unsigned char priority[8]{};
    uint32_t result = 0;
    bool has_result = false;
    bool stack_read = false, priority_read = false, image_write_attempted = false, toc_written = false;
    size_t image_written_bytes = 0, publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    unsigned char image[PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE]{};
};
/* Same discipline for cellSpursWorkloadAttributeSetName (NID 0x4a5eab63). The name arguments are opaque numbers: they are
 * recorded and stored, never dereferenced. patch is class then instance (BE32 each) for the 8 bytes at attribute+0x28. */
struct StartupSpursWorkloadNameObservation {
    uint64_t arguments[3]{};
    uint32_t attribute = 0, name_class = 0, name_instance = 0, patch_address = 0;
    unsigned char patch[PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE]{};
    uint32_t result = 0;
    bool has_result = false;
    bool class_write_attempted = false, class_written = false;
    bool instance_write_attempted = false, instance_written = false, toc_written = false;
    size_t written_bytes = 0, publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
};
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
struct StartupSpursRegistrationObservation {
    uint64_t arguments[3]{};
    uint32_t attribute = 0, revision = 0;
    ps3_spurs_registration_request request{};
    ps3_spurs_registration_result domain_result{};
    uint32_t result = 0;
    bool has_domain_result = false, has_result = false, toc_written = false, trace_failure = false;
    size_t publications = 0, attribute_reads = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupStop latched_stop = StartupStop::None;
};
struct StartupSpursIdleObservation {
    uint64_t arguments[3]{};
    ps3_spurs_idle_request request{};
    ps3_spurs_idle_result domain_result{};
    uint32_t result = 0;
    bool has_domain_result = false, has_result = false, toc_written = false, trace_failure = false;
    size_t publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupStop latched_stop = StartupStop::None;
};
struct StartupSpursReadyObservation {
    uint64_t arguments[3]{};
    ps3_spurs_ready_count_request request{};
    ps3_spurs_ready_count_result domain_result{};
    uint32_t result = 0;
    bool has_domain_result = false, has_result = false, toc_written = false, trace_failure = false;
    size_t publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupStop latched_stop = StartupStop::None;
};
struct StartupSpursWorkloadInfoObservation {
    uint64_t arguments[3]{};
    ps3_spurs_workload_info_request request{};
    ps3_spurs_workload_info_result domain_result{};
    uint32_t result = 0;
    bool has_domain_result = false, has_result = false, toc_written = false, trace_failure = false;
    size_t publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupStop latched_stop = StartupStop::None;
};
struct StartupSpursExceptionObservation {
    uint64_t arguments[4]{};
    uint32_t nid = 0;
    ps3_spurs_exception_request request{};
    ps3_spurs_exception_result domain_result{};
    uint32_t result = 0;
    bool has_domain_result = false, has_result = false, toc_written = false, trace_failure = false;
    size_t publications = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    StartupStop latched_stop = StartupStop::None;
};
/* Quota ledger around the REAL aa6269a8 initialize. The shared user-memory quota also carries every earlier
 * guest allocation (the real startup holds 0x500000 before SPURS runs), so "quota used" is the TOTAL, never the
 * SPURS charge. The SPURS charge is the measured DELTA between a sample taken immediately before and one taken
 * immediately after ps3_spurs_runtime_initialize on the same allocator; both statuses are kept. */
constexpr int STARTUP_QUOTA_UNOBSERVED = -1;
constexpr uint64_t STARTUP_SPURS_FIRST_INSTANCE_CHARGE = 0x14C000;   // 5 x 256 KiB LS + 0x8000 + 0x4000 (one 5-SPU instance)
struct StartupQuotaSample {
    int status = STARTUP_QUOTA_UNOBSERVED;   // ps3_user_memory_status of the query; UNOBSERVED = never sampled
    uint64_t quota = 0, used = 0, available = 0;
    size_t allocations = 0;
};
struct StartupSpursQuotaReport {
    StartupQuotaSample before{}, after{};
    bool measured = false;     // both samples OK, same quota, used+available==quota in each
    bool charged = false;      // measured and the quota only grew: delta_charge is defined and equals the available drop
    uint64_t prior_used = 0;   // before.used: allocations that existed before SPURS ran (guest or otherwise)
    uint64_t delta_charge = 0; // after.used - before.used: the charge of this initialize
    uint64_t total_used = 0;   // after.used: prior_used + delta_charge
};
/* Pure calculation shared by the dispatch adapter and every test; touches no allocator. */
StartupSpursQuotaReport startup_spurs_quota_report(const StartupQuotaSample &before, const StartupQuotaSample &after);
/* True only for a fully measured, conserved report whose delta charge is exactly expected_delta. */
bool startup_spurs_quota_charge_is(const StartupSpursQuotaReport &report, uint64_t expected_delta);
StartupQuotaSample startup_quota_sample(ps3_user_memory *memory);   // nullptr -> status UNOBSERVED
/* Immutable PROCESS SDK configuration for B9BC6207 (cellSpursAttachLv2EventQueue). `raw` is the validated process-parameters
 * sdk_version field exactly as the loader plan decoded it (never CellSpurs +0xDA4, never an attribute, never a constant);
 * `valid` is true only when that plan really carried process parameters. The composition root sets it once, before the
 * worker starts, and nothing writes it afterwards. The reference get_sdk_version() reads this value as s32: raw -1 maps to
 * 0x485000 (the loader rejects UINT32_MAX today, the mapping is kept faithful), any other negative value, or a missing
 * configuration, stops the attach BEFORE any effect. */
constexpr uint32_t STARTUP_SPURS_ATTACH_MAPPED_UNSET_SDK = 0x485000;
/* Bounded strong-CAS retry policy of the DA8 OR. Caller policy (not a domain default): 64 attempts is far above any observed
 * contention (the only concurrent DA8 writers are the owned SPURS kernel workers) yet finite; exhausting it stops and rolls back. */
constexpr uint32_t STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS = 64;
/* True and the effective s32 process SDK (always 0..INT32_MAX) only for a valid configuration that is not a non-(-1) negative. */
bool startup_spurs_attach_effective_sdk(const StartupProcessSdk &sdk, uint32_t *effective);
/* DIAGNOSTIC observation of the two SPURS synchronization create callbacks (lwmutex_create, lwcond_create) at the startup
 * adapter boundary. It records what the REAL registries were asked and answered; it never influences admission, return codes,
 * guest memory, tracing or stops. Contract:
 *  - one record per callback entry, in call order, written by the single PPU thread that runs the callback (read after join);
 *    destroy callbacks never write here, so a failed create survives the unwind that follows it;
 *  - each registry is sampled SEPARATELY while holding only that registry's own guard (lock, copy scalars, unlock): no guard is
 *    nested, nothing waits, allocates or publishes under it, and both samples are released before the real create runs.
 *    The two samples are NOT atomic with each other nor with the real call: under concurrent mutation they can disagree with what
 *    the create saw, so the snapshot is evidence of occupancy just before the call, never a certain reason for the status;
 *  - counting follows the registries' own slot-selection rule for diagnostics only: an object is ACTIVE when state != 0; an
 *    inactive object is REUSABLE unless its generation reached UINT64_MAX (EXHAUSTED_GENERATION). The queue-id namespace is
 *    available while next_queue_id is nonzero and below PS3_LWMUTEX_RESERVED. A call with no reusable slot, an unavailable
 *    queue-id namespace or (condition create only) full binding credits is what the registries map to RESOURCE_EXHAUSTED;
 *  - bounded: STARTUP_SPURS_SYNC_CREATE_CAPACITY records (two instances x lwmutex + lwcond). A call beyond that is counted in
 *    spurs_sync_create_calls / spurs_sync_create_dropped, sets spurs_sync_create_overflow, and only its FAILURE is still kept in
 *    spurs_sync_last_failed. Overflow never stops the guest or changes a return code. No allocation, no dynamic log. */
enum class StartupSpursSyncOperation : uint32_t { None = 0, LwmutexCreate = 1, LwcondCreate = 2 };
constexpr size_t STARTUP_SPURS_SYNC_CREATE_CAPACITY = 4;
struct StartupSpursSyncRegistrySnapshot {
    bool taken = false;                         // false: registry absent/uninitialized, nothing below is meaningful
    size_t object_capacity = 0, active_objects = 0, reusable_objects = 0, exhausted_generation_objects = 0;
    bool overlap_evaluated = false;             // overlap is evaluated only for the registry the operation targets
    size_t overlapping_objects = 0;             // nonfree objects overlapping the requested guest span
    uint32_t next_queue_id = 0;
    bool queue_id_available = false;            // next_queue_id nonzero and below PS3_LWMUTEX_RESERVED
    size_t condition_binding_count = 0, condition_binding_capacity = 0;   // lwmutex registry only
};
struct StartupSpursSyncCreateRecord {
    StartupSpursSyncOperation operation = StartupSpursSyncOperation::None;
    uint32_t sequence = 0;                      // 1-based call number among every create callback entry
    uint32_t address = 0;                       // lwmutex or lwcond guest address as requested
    uint32_t mutex_address = 0;                 // lwcond create only
    uint32_t protocol = 0, recursive = 0;       // lwmutex create only
    bool real_call_performed = false;           // false: the trace reservation refused before the registry was called
    int native_status = -1;                     // ps3_lwmutex_status / ps3_lwcond_status of the real call (-1: not called)
    int memory_status = -1;                     // ps3_guest_memory_status carried by the real result (-1: not called)
    int nested_mutex_status = -1;               // lwcond create: ps3_lwmutex_status carried by the lwcond result (-1 otherwise)
    uint32_t mapped_rc = 0;                     // the lv2 rc the callback returned to SPURS
    size_t publication_count = 0;               // lwcond create: guest publications the registry reported
    StartupSpursSyncRegistrySnapshot mutexes{}, conditions{};   // pre-call snapshots (see contract)
};
#endif
/* Generic immutable borrowed guest sys_event_queue_receive (sc130) port. No concrete LV2/service type is named here.
 * Contract:
 *  - borrowed by StartupExecution::event_receive through confirmed host join; immutable while the worker runs;
 *    null (or receive == nullptr) stops sc130 as MissingServices.
 *  - receive() is SYNCHRONOUS and MAY genuinely block on the exiting host thread. It owns its own waiter, cancel token and
 *    activity publication. It must never throw, free its context, join, or re-enter startup execution, and the dispatcher
 *    holds no registry lock or guest-memory pin across it. The result is copied by value.
 *  - queue_id is the low 32 bits of r3; timeout_us is r5 (0 = wait forever). r4 is a dummy buffer pointer the syscall
 *    never validates, reads or writes.
 *  - Completed: `result` is the CELL return placed (sign-extended through int32) in r3; ONLY result == 0 publishes
 *    source/data1/data2/data3 into r4..r7, a nonzero completed result preserves r4..r7.
 *  - Cancelled -> StartupStop::Cancelled, Unsupported -> UnsupportedSyscall, ProviderFailure -> ProviderFailure; each goes
 *    through the actual stop()/terminal notification and no guest instruction after the syscall runs. */
enum class StartupEventReceiveOutcome { Completed, Cancelled, Unsupported, ProviderFailure };
struct StartupEventReceiveResult {
    StartupEventReceiveOutcome outcome = StartupEventReceiveOutcome::ProviderFailure;
    uint32_t result = 0;
    uint64_t source = 0, data1 = 0, data2 = 0, data3 = 0;
};
struct StartupEventReceivePort {
    void *context = nullptr;
    StartupEventReceiveResult (*receive)(void *context, uint32_t queue_id, uint64_t timeout_us) = nullptr;
};
/* Composition-owned game-data service port, borrowed through confirmed join. The storage is the
 * injected domain port; `state` is the last check record and the value cellGameDataGetSizeKB reads. */
struct StartupGameDataPort {
    gamedata_storage_port storage{};
    gamedata_check *state = nullptr;       // last check, for cellGameDataGetSizeKB
};
/* cellPadInit (0x1CF98800) / cellPadSetPortSetting (0x578E3C98) observations.
 * UNCONDITIONAL (no #ifdef): only the two dispatch bodies and the
 * router lines live behind PS3_NATIVE_STARTUP_PAD, so the layout is identical with the flag on and off.
 * `state` is INSTANCE-OWNED and survives across calls: it holds the firmware's module-static initialized flag, which is the
 * only reason a second cellPadInit answers 0x80121103. A per-call copy would hide that (INTEGRATION.md §4.2).
 * `result` is the guest-visible r3 as a signed 32-bit value, which is what the firmware's `extsw` produces. */
struct StartupPadInitObservation {
    uint64_t raw_r3=0;               // the count argument exactly as the guest passed it
    uint32_t count=0;                // r3 truncated: the value the range check saw
    int32_t result=0;                // 0, or a negative 0x8012xxxx code
    pad_status status=PAD_SERVICE_OK;
    pad_state state{};               // instance-owned across calls (the firmware's module flag)
    bool argument_checked=false, initialized=false, second_call=false;
};
struct StartupPadPortSettingObservation {
    uint64_t raw_r3=0, raw_r4=0;     // port_no and setting before truncation
    uint32_t port_no=0, setting=0;
    int32_t result=0;                // the guest-visible r3; the game ignores it (pad-b3 CONTRACT §4)
    pad_status status=PAD_SERVICE_OK;
    pad_setting_result detail{};     // applied, field_a, field_b, record_offset, syscall_*
    bool applied=false, syscall_replayed=false;
};
/* The PROCESS's pad module state, borrowed by every guest thread's StartupExecution. The firmware keeps ONE module-static
 * state (the initialized flag, max_pads, the per-port reader records), so a second thread's cellPadInit must see the first
 * one's init; and all five pad arms must mutate the SAME records that the first thread's port setting wrote.
 *
 *  - NULL `pad_process` keeps the previous behaviour exactly: each execution's StartupControl::pad_init.state is the state
 *    and no controller source is consulted (the service falls back to the assumed pad count).
 *  - `port` is the controller source. A port whose three function pointers are all NULL is passed to the service as NULL,
 *    so "no controller source" stays one representation.
 *  - `lock` is held for the duration of ONE pad call on every arm, because a pad_state update is not atomic: two guest
 *    threads polling GetInfo2/GetData every frame would otherwise interleave inside the reader records. It guards no guest
 *    access and no wait, and it is always released before the arm returns.
 *  - The four counters are the readers' statistics; the two cellPadInit/cellPadSetPortSetting observables keep their place in
 *    StartupControl, so their existing prints and tests are unchanged. */
struct StartupPadProcess {
    pad_state state{};
    pad_port port{};                      // all-NULL = no controller source
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint64_t info2_calls = 0, data_calls = 0, data_changed = 0, act_calls = 0;
};
/* Root (2026-11-11): the process-wide audio service for the 13 cellAudioOut and cellAudio imports of
 * native/domain/audio_service.h. One per process, borrowed by every guest thread's execution. NULL keeps
 * those imports unserved (UnknownImport), as before. The lock is held for one import or one tick. Silent:
 * the port buffers are guest memory the game writes; nothing reads them yet. */
struct StartupAudioProcess {
    audio_service service{};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint64_t ticks = 0, index_stores = 0, store_failures = 0;
};
/* Advances the block clock of every running port and stores each changed read index (big-endian u64 at
 * layout.index_base + 8 * port) into guest memory. Safe from a host thread: a refused store is counted,
 * never a stop. */
void startup_audio_tick(StartupAudioProcess &process, const ps3_guest_memory &memory, uint64_t now_us);
/* Root (2026-11-11): the process-wide lv2 timer table (syscalls 70..76, native/domain/lv2_timer.h). One per
 * process, borrowed by every guest thread. `objects` is the process's lv2 object registry: an expired timer
 * is delivered with ps3_lv2_queue_send. NULL keeps 70..76 UnsupportedSyscall, as before. */
struct StartupTimerProcess {
    lv2_timer_table table{};
    ps3_lv2_objects *objects = nullptr;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint64_t fires = 0, send_failures = 0, last_send_code = 0;
};
/* Root (2026-11-11): the live RSX command-buffer consumer (native/domain/rsx_live_fifo.h) and the GCM imports
 * that feed it. One per process, borrowed by every guest thread. The composition's tick thread pumps it; it
 * stays idle until cellGcmInit has run (`gcm` reports INITIALIZED). No renderer is attached yet: methods are
 * counted, references / labels / reports / flips are answered. NULL keeps those imports unserved. */
struct StartupRsxProcess {
    rsx_live_fifo fifo{};
    rsx_live_layout layout{};              // control and label addresses, set by the composition
    const gcm_init_service *gcm = nullptr; // the IO mapping and the INITIALIZED state are read from here
    int (*gcm_ready)(const gcm_init_service *) = nullptr;   // set by the composition (gcm_init_service_is_initialized): keeps this unit free of a link dependency
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> lock_waiters{0};   // threads inside Guard's acquire (Root 2026-11-15: the pump yields to them between chunks)
    bool configured = false;
    uint32_t handlers[5]{};                // guest OPDs: flip, vblank, user, graphics, queue (recorded, never called yet)
    // Optional observers, called on the pumping thread with the lock held (the renderer or a capture goes here).
    void *hook_user = nullptr;
    void (*method_hook)(void *user, uint32_t subchannel, uint32_t method, uint32_t data) = nullptr;
    void (*flip_hook)(void *user, uint32_t buffer_id) = nullptr;
    uint32_t debug_level = 0;
    uint64_t pumps = 0, flip_commands = 0, flip_immediate = 0, handler_sets = 0, address_queries = 0;
    uint32_t last_status = 0;
};
/* One consumer step from the composition's tick thread. */
void startup_rsx_pump(StartupRsxProcess &process, const ps3_guest_memory &memory, uint32_t word_budget);
/* Root (2026-11-11): the message dialog (cellMsgDialogOpen2 / Close / Abort) and the sysutil callback poll that
 * delivers its result. There is no dialog on screen yet: the text goes to the log, a dialog WITH buttons is
 * answered at once (YES / OK), a dialog without buttons stays open until the game closes it. The result
 * callback runs on the thread that calls cellSysutilCheckCallback, as on the console. Process-wide. */
struct StartupDialogProcess {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    bool open = false, result_pending = false;
    uint32_t type = 0, callback = 0, userdata = 0;
    int32_t button = 0;
    uint64_t opens = 0, closes = 0, aborts = 0, callbacks = 0, check_calls = 0, busy = 0;
    uint32_t last_type = 0;
    char last_text[160]{};
};
/* Root (2026-11-11): process-wide lv2 mutexes (syscalls 100..104). Each mutex is one registry semaphore
 * (initial 1, max 1); this table adds what a mutex has and a semaphore lacks: the owning guest thread and
 * the recursion count. The guest's mutex id IS the semaphore id. NULL keeps 100..104 unsupported. */
struct StartupMutexProcess {
    enum : uint32_t { capacity = 384 };
    struct Entry { uint32_t id = 0, owner = 0, count = 0; bool recursive = false, in_use = false; };
    Entry entries[capacity]{};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint64_t creates = 0, destroys = 0, locks = 0, contended = 0, unlocks = 0, errors = 0;
    uint64_t bink_handoffs = 0, bink_relock_failures = 0;   // Decision 62
    uint64_t bink_calls = 0;
    uint32_t bink_first_cond = 0, bink_first_lr = 0, bink_first_thread = 0, bink_first_ids[4] = {}, bink_first_owners[4] = {};
    uint32_t live = 0, peak = 0;
};
extern "C" { extern uint32_t g_lv2_mutex_stop_site, g_lv2_mutex_stop_id, g_lv2_mutex_stop_thread; }
extern "C" void startup_print_missing_services(void);
/* Sends one event for every timer that has expired at `now_us`. Called from the composition's tick thread. */
void startup_timer_fire(StartupTimerProcess &process, uint64_t now_us);
/* File system (six `sys_fs` imports: cellFsStat/Opendir/Readdir/Closedir/Rmdir/Unlink,
 * decision 24). UNCONDITIONAL, like the pad pair: only the dispatch lives
 * behind PS3_NATIVE_STARTUP_FS.
 * `mount` indexes fs_canonical_guest_mount(), so a captured call names WHICH mount the purge targeted -- the open question
 * B2-S5 exists to answer. `rc` is the guest code the import returns (0 or a negative 0x8001xxxx).
 * STARTUP_FS_CAPTURE_CAPACITY bounds the retained calls; the summary counters are never truncated. */
constexpr size_t STARTUP_FS_CAPTURE_CAPACITY = 32;
constexpr size_t STARTUP_FS_CAPTURE_PATH = 256;   // bytes retained per guest path, including the NUL
constexpr size_t STARTUP_FS_PROCESS_LOG_CAPACITY = 1024;   // root 2026-11-12: was 128; the game makes more calls than that before its first screen
constexpr size_t STARTUP_FS_PROCESS_LOG_PATH = 160;   // bytes retained per guest path, including the NUL
struct StartupFsProcessCall {
    uint32_t sc = 0;            // lv2 number of the operation (801, 805, 808, 813, 814)
    uint32_t thread_id = 0;     // cpu.thread_id of the caller (0 when unknown)
    int32_t rc = 0;
    bool import_form = false, truncated = false;
    char path[STARTUP_FS_PROCESS_LOG_PATH]{};   // escaped exactly like StartupFsCallRecord::path
};
struct StartupFsCallRecord {
    uint32_t sc=0;                    // the lv2 syscall number (801, 805..808, 813, 814)
    uint32_t mount=0xFFFFFFFFu;       // fs_canonical_guest_mount() index, 0xFFFFFFFF when unresolved
    int32_t rc=0;
    bool resolved=false, truncated=false;
    char path[STARTUP_FS_CAPTURE_PATH]{};   // NUL-terminated, escaped, at most 255 characters
};
struct StartupFsObservation {
    fs_counters counters{};           // the domain's own counters, copied after every call
    StartupFsCallRecord capture[STARTUP_FS_CAPTURE_CAPACITY]{};
    size_t captured=0;                // min(calls, capacity)
    size_t dropped=0;                 // calls - captured
    uint64_t calls=0, returns=0;
    int32_t last_rc=0;
    bool active=false;                // the composition composed the root and the five mounts
    bool root_ready=false;            // fs_storage_posix_init succeeded
    fs_storage_status last_storage_status=FS_STORAGE_IO_ERROR;
};
/* The mount table the composition installs (decision 24). The production root token lives in the composition
 * (native/tests/test_startup_indirect.cpp / the console composition), never here, so the host tests can point the same
 * code path at a scratch directory.
 * `storage` and `service` are composition-owned storage: fs_service is about 42 KiB, which must not sit on a stack. */
struct StartupFsContext {
    fs_storage_posix storage{};
    fs_service service{};
    ps3_instance_guard guard{};
    StartupFsProcessCall process_log[STARTUP_FS_PROCESS_LOG_CAPACITY]{};
    size_t process_logged = 0;      // min(calls, capacity)
    uint64_t process_dropped = 0;   // path-bearing calls after the log filled
    bool active=false;
};
/* Native lv2 syscall 403 (sys_tty_write) console sink. UNCONDITIONAL, like the game-data types: only the dispatch code in
 * startup_execution.cpp is flag-guarded (PS3_NATIVE_STARTUP_TTY), so every translation unit agrees on the layout. The ring is
 * composition-owned and borrowed (a null ring stops MissingServices); the dispatch copies at most STARTUP_TTY_RING_BYTES
 * escaped bytes and only counts the excess in `dropped`, so its capacity is a hard bound, never a truncation hazard. */
constexpr size_t STARTUP_TTY_RING_BYTES = 4096;
struct StartupTtyRing {
    char bytes[STARTUP_TTY_RING_BYTES]{};
    size_t used = 0;      // escaped bytes stored, always <= STARTUP_TTY_RING_BYTES
    size_t dropped = 0;   // escaped bytes that did not fit; never a guest-visible error
    // C1b (2026-11-11): the ring is the PROCESS's console, and child PPU threads write to it too (a game worker thread
    // calls sc403). Held only around the byte-append loop: no guest access, no wait and no stop happens under it.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
};
/* Native lv2 syscall 44 (sys_ppu_thread_join) outcome and result. UNCONDITIONAL, like the game-data types: only the dispatch
 * code in startup_execution.cpp is flag-guarded, so every translation unit agrees on the layout of StartupControl and the
 * create port keeps one definition. */
enum class StartupPpuJoinStatus { Joined, NotFound, NotExited, AlreadyJoined, ProviderFailure };
struct StartupPpuJoinResult {
    StartupPpuJoinStatus status = StartupPpuJoinStatus::ProviderFailure;
    uint64_t exit_value = 0;   // Joined only (the provider's guest exit value, written as a BE u64 to the guest output pointer)
};
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
/* Generic immutable borrowed guest sys_ppu_thread_create (sysPrxForUser NID 0x24a1ea07) port. No concrete child owner is named.
 * The bridge decodes and validates the whole call BEFORE any provider effect, then runs exactly:
 *   prepare(request, primary TLS template copy) -> [bridge writes BE u64 id + saved TOC, closes its trace batch] -> start(id).
 *  - borrowed by StartupExecution::ppu_create through confirmed join; immutable while the worker runs (configure before the
 *    parent starts); null port/prepare/start stops MissingServices before any effect.
 *  - Both callbacks run on the calling host thread, never throw, never re-enter startup execution, and never free their context.
 *    start() MAY block until the child reaches its first observable state; at that point the bridge holds no trace
 *    reservation, no guest-memory pin and no tracked guard. The request/TLS pointers are valid only during prepare().
 *  - prepare() allocates/reserves provider-owned child resources and may initialize their private stack/TLS bytes.
 *    It never writes the caller's thread-id output or saved TOC and never starts the child.
 *      Prepared      -> thread_id (nonzero) names the provider-owned child; the BRIDGE writes the BE u64 id, then calls start.
 *      GuestError    -> a COMPLETED guest-visible error (`cell` nonzero): nothing was reserved, no id is written, start is not
 *                       called, r3 = sign-extended cell and r2 is preserved. Only a provider with a pinned reference for that
 *                       error may return it; the bridge never invents one.
 *      Unsupported   -> StartupStop::UnsupportedSyscall;  ProviderFailure (or any out-of-range value) -> ProviderFailure.
 *  - start() -> Started: the child has reached the state accepted by its composition; the first-prefix diagnostic requires
 *    a real Blocked state. The parent returns r3 = 0 with r2 preserved. Any other result is a
 *    terminal stop of the PARENT with no r3: Terminal (diagnostic: the child stopped; the game helper cannot return) and
 *    ProviderFailure -> StartupStop::ProviderFailure (StartupControl::ppu_create_start records which), Unsupported ->
 *    UnsupportedSyscall. Stopping NEVER rolls back, joins or frees: the provider keeps ownership of the prepared child until
 *    its composition root tears it down after the parent has joined.
 *  - prepare() runs while the bridge's output trace reservation is open, so it must be bounded and non-blocking. Private
 *    stack/TLS initialization uses coherent memory directly and must not publish into the parent's recorder or batch.
 *    That reservation is closed before start(). The bridge polls cancellation/deadline before prepare,
 *    after prepare (before the id write), and after the id/TOC publication (before start) and after start (before r3). */
enum class StartupPpuCreatePrepareStatus { Prepared, GuestError, Unsupported, ProviderFailure };
enum class StartupPpuCreateStartStatus { Started, Terminal, Unsupported, ProviderFailure };
constexpr size_t STARTUP_PPU_CREATE_NAME_SCAN = 128;   // bytes scanned for the NUL; the name is at most 127 characters
struct StartupPpuCreateRequest {
    ps3_disc_change_registry *disc_change_callbacks=nullptr;
    const StartupGameDataPort *gamedata=nullptr;
    SystemParameters system_parameters{};
    SystemProfile system_profile{};
    PresentationTopology presentation_topology=PresentationTopology::Unconfigured;
    NativePresentationMode presentation_mode=NativePresentationMode::Unconfigured;
    StartupProcessSdk process_sdk{};
    BootContentMetadata boot_content{};
    uint32_t opd_address = 0, entry = 0, toc = 0;   // OPD{BEu32 entry, BEu32 TOC} decoded from guest memory
    uint64_t argument = 0;                          // r5, full width
    int32_t priority = 0;                           // r6 as s32, validated 0..3071
    uint32_t stack_size = 0;                        // r7 low 32 bits, nonzero
    uint64_t flags = 0;                             // r8, exactly 1 (joinable) is the only supported value
    uint32_t output = 0;                            // guest address of the BE u64 thread id the bridge will write
    uint32_t name_address = 0;                      // r9, nonnull
    uint32_t name_length = 0;                       // characters before the NUL (<= 127)
    char name[STARTUP_PPU_CREATE_NAME_SCAN]{};      // NUL terminated copy
    uint32_t creator_thread_id = 0;
    int32_t creator_priority = 0;                   // StartupExecution::guest_priority of the creating thread
};
struct StartupPpuCreatePrepareResult {
    StartupPpuCreatePrepareStatus status = StartupPpuCreatePrepareStatus::ProviderFailure;
    uint32_t thread_id = 0;   // Prepared only
    uint32_t cell = 0;        // GuestError only
};
struct StartupPpuCreatePort {
    void *context = nullptr;
    StartupPpuCreatePrepareResult (*prepare)(void *context, const StartupPpuCreateRequest *request,
        const Ps3TlsTemplate *primary_tls_template) = nullptr;
    StartupPpuCreateStartStatus (*start)(void *context, uint32_t thread_id) = nullptr;
    /* lv2 syscall 44 (sys_ppu_thread_join) on the same provider. Appended, so every existing aggregate initializer keeps its
     * meaning; a null slot means the bridge stops MissingServices before any effect.
     *  - joined: thread_id is the exact id prepare() handed the bridge (the value the guest read back). On Joined the bridge
     *    writes the exit value as a BE u64 to the caller's output pointer and returns r3 = 0. NotFound/NotExited/AlreadyJoined
     *    are terminal UnsupportedSyscall stops: a real PS3 would block (NotExited) or answer ESRCH/EINVAL, and this diagnostic
     *    runtime does neither until a later decision, so no guest errno is invented. ProviderFailure stops ProviderFailure.
     *  - Contract shared with prepare/start: runs on the calling host thread, never throws, never re-enters startup execution,
     *    never frees the context, and never cancels or reaps. The composition root stays the only release path. */
    StartupPpuJoinResult (*join)(void *context, uint32_t thread_id) = nullptr;
};
#endif
struct StartupControl;
struct StartupExecution;
struct HleTraceBatch;
/* Native _cellGcmInitBody (NID 0x15BAE46B) observation. Reset at call entry. publication counts the
 * events this call published; has_result only after the TOC and the context EA were committed. */
enum class StartupGcmInitFailure { None, MissingService, WrongLr, HighBits, InvalidMemory, ServiceRefused, ServiceFailed };
struct StartupGcmInitObservation {
    uint64_t raw_r3=0, raw_r4=0, raw_r5=0, raw_r6=0;
    gcm_init_request request{};
    gcm_init_result result{};
    StartupGcmInitFailure failure=StartupGcmInitFailure::None;
    size_t publications=0;
    bool toc_written=false, has_result=false;
};
/* Guest port of the native GCM-init service. The composition owns both this bridge and the service
 * storage; the bridge only stores the active trace batch for the duration of one dispatch. */
struct StartupGcmGuestBridge {
    StartupExecution *execution = nullptr;
    HleTraceBatch *batch = nullptr;
    size_t events = 0;
    bool active = false;
};
/* Native post-init cellGcmSys queries: GetConfiguration, GetLabelAddress,
 * GetOffsetTable, GetControlRegister. Reset at call entry; the counters keep the
 * cumulative picture and gcm_query the last call only. */
enum class StartupGcmQueryFailure { None, MissingService, NotInitialized, HighBits, InvalidMemory, ServiceFailed };
struct StartupGcmQueryObservation {
    uint32_t nid=0;
    uint64_t lr=0, raw_r3=0;
    /* Raw argument registers r4..r10, recorded for the tile and zcull routes (unused elsewhere). */
    uint64_t raw_r4=0, raw_r5=0, raw_r6=0, raw_r7=0, raw_r8=0, raw_r9=0, raw_r10=0;
    /* Parameter-save words read from SP+0x70/0x78/0x80/0x88 for cellGcmBindZcull. */
    uint32_t raw_stack9=0, raw_stack10=0, raw_stack11=0, raw_stack12=0;
    uint32_t result=0;
    gcm_init_status status=GCM_INIT_OK;
    StartupGcmQueryFailure failure=StartupGcmQueryFailure::None;
    size_t publications=0;
    bool toc_written=false, has_result=false;
};
/* Optional per-execution terminal notification port (execution-lifetime owned by the StartupControl).
 * Contract (real lifetime constraints, not conventions):
 *  - fn/context are injected before the worker starts and are immutable while it runs; absent (fn == nullptr)
 *    for ordinary primary executions, whose stop behavior is unchanged. No global observer or locator.
 *  - stop() finishes the trace batch, records the exact reason and terminal CPU, clears the thread-local
 *    dispatch state, invokes fn exactly once ON THE EXITING HOST THREAD, then always calls threadExit().
 *    The observer therefore runs after the terminal record is complete and guest code never resumes.
 *  - The observer may synchronously publish a service EXITED state / wake. It must not release resources,
 *    join or free the current thread, re-enter guest execution, throw, or block: the thread is still alive
 *    (and is joined only by its owner afterwards), and the context must outlive that join.
 *  - There is no unwind mechanism; the control and context stay owned by the creator until after join. */
typedef void (*StartupTerminalNotifyFn)(void *context, StartupControl *control);
struct StartupTerminalNotification {
    StartupTerminalNotifyFn fn = nullptr;
    void *context = nullptr;
};
/* Last cellGameDataCheckCreate2 call (single PPU writer; read after join). Reset at entry. Raw
 * argument registers keep the exact r3..r7 values; the records' guest addresses are the frame
 * locations handed to funcStat. dispatch_status is the compiled-dispatch result, if one ran. */
struct StartupGameDataObservation {
    uint64_t raw_r3=0, raw_r4=0, raw_r5=0, raw_r6=0, raw_r7=0;
    uint32_t version=0, dir_name=0, err_dialog=0, func_stat=0, container=0;
    uint32_t opd=0, entry=0, toc=0, frame=0;
    uint32_t cb_result=0, stat_get=0, stat_set=0;
    int32_t result=0, err_need_size_kb=0;
    uint32_t set_param=0;
    gamedata_action action{};
    gamedata_status status=GAMEDATA_OK;
    gamedata_check check{};
    ps3_compiled_dispatch_status dispatch_status=PS3_COMPILED_DISPATCH_OK;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
    size_t publications=0;
    bool argument_checked=false, callback_invoked=false, dispatch_returned=false;
    bool toc_written=false, has_result=false;
};
struct StartupGameDataSizeObservation {
    uint64_t raw_size=0;
    uint32_t size=0;
    int32_t value=0;
    ps3_guest_memory_status memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
    size_t publications=0;
    bool has_result=false, written=false, toc_written=false;
};
struct StartupControl {
    size_t video_output_count_calls=0,video_output_count_returns=0;
    StartupVideoOutputCountObservation video_output_count{};
    size_t video_device_info_calls=0,video_device_info_returns=0;
    StartupVideoDeviceInfoObservation video_device_info{};
    size_t video_availability_calls=0,video_availability_returns=0;
    StartupVideoAvailabilityObservation video_availability{};
    size_t video_resolution_calls=0,video_resolution_returns=0;
    StartupVideoResolutionObservation video_resolution{};
    size_t video_state_calls=0,video_state_returns=0;
    StartupVideoStateObservation video_state{};
    size_t video_configure_calls=0, video_configure_returns=0;
    StartupVideoConfigureObservation video_configure{};
    bool video_logical_target_configured=false;
    size_t gcm_init_calls=0, gcm_init_returns=0;
    StartupGcmInitObservation gcm_init{};
    size_t gcm_config_calls=0, gcm_config_returns=0;
    size_t gcm_label_calls=0, gcm_label_returns=0;
    size_t gcm_offset_table_calls=0, gcm_offset_table_returns=0;
    size_t gcm_control_calls=0, gcm_control_returns=0;
    size_t gcm_flip_mode_calls=0, gcm_flip_mode_returns=0;
    size_t gcm_tile_info_calls=0, gcm_tile_info_returns=0;
    size_t gcm_bind_tile_calls=0, gcm_bind_tile_returns=0;
    size_t gcm_bind_zcull_calls=0, gcm_bind_zcull_returns=0;
    size_t gcm_unbind_zcull_calls=0, gcm_unbind_zcull_returns=0;
    size_t gcm_unbind_tile_calls=0, gcm_unbind_tile_returns=0;
    size_t gcm_display_buffer_calls=0, gcm_display_buffer_returns=0;
    StartupGcmQueryObservation gcm_query{};
    size_t gamedata_check_calls=0, gamedata_check_returns=0;
    StartupGameDataObservation gamedata_check{};
    size_t gamedata_size_calls=0, gamedata_size_returns=0;
    StartupGameDataSizeObservation gamedata_size{};
    size_t guest_thread_exit_calls=0;
    uint64_t guest_exit_value=0;
    bool has_guest_exit=false;
    size_t disc_change_calls=0, disc_change_returns=0;
    StartupDiscChangeObservation disc_change{};
    size_t system_string_calls=0, system_string_returns=0;
    StartupSystemStringObservation system_string{};
    size_t system_parameter_calls=0, system_parameter_returns=0;
    StartupSystemParameterObservation system_parameter{};
    size_t boot_disc_calls = 0, boot_disc_returns = 0;
    StartupBootDiscObservation boot_disc{};
    std::atomic<bool> cancelled{false};
    uint64_t deadline_tick = 0;
    StartupAccess accesses[STARTUP_TRACE_CAPACITY]{};
    PpuMemoryTraceJournal journal{accesses, STARTUP_TRACE_CAPACITY, 0};
    uint32_t imports[STARTUP_IMPORT_CAPACITY]{};
    size_t import_count = 0;
    uint64_t import_overflow = 0;
    size_t tls_returns = 0;
    // Committed configuration for child TLS; published only after successful primary initialization.
    Ps3TlsTemplate initialized_tls_template{};
    bool initialized_tls_template_valid = false;
    size_t mutex_returns = 0;
    // Decision 33 (LLE libsre): lifted firmware calls routed into Sony's own code, the TOC-restore
    // failures that would mean the caller's r2 was not put back, and the libc imports the lifted
    // code reaches through ps3_hle_call. Unconditional so no layout depends on PS3_NATIVE_LLE_SRE.
    size_t lle_sre_routed = 0;
    size_t lle_sre_toc_restores = 0;
    size_t lle_sre_toc_failures = 0;
    size_t lle_sre_libc_returns = 0;
    // Decision 33 / B2a lv2 sync + object syscalls. Appended scalars only.
    size_t lv2_sync_calls = 0, lv2_sync_returns = 0;
    uint32_t lv2_sync_number = 0, lv2_sync_argument = 0, lv2_sync_result = 0;
    uint64_t lv2_sync_blocked = 0;
    uint32_t lv2_spu_run_start_code = 0;   // root (C1b): ps3_spu_run_start result for the last 173
    // Decision 33 / B2b: SPU images and thread groups. Appended scalars only.
    size_t lv2_spu_calls = 0, lv2_spu_returns = 0, lv2_spu_image_segments = 0;
    uint64_t lv2_spu_number = 0, lv2_spu_argument = 0;
    uint32_t lv2_spu_last_entry = 0, lv2_spu_segments_address = 0;
    uint32_t lv2_spu_last_group = 0, lv2_spu_last_num = 0, lv2_spu_last_priority = 0;
    uint32_t lv2_spu_thread_ids[8]{}, lv2_spu_entries[8]{};
    uint64_t lv2_spu_args1[8]{}, lv2_spu_ls_hashes[8]{};
    bool lv2_spu_has_start = false;
    ps3_lv2_spu_start_snapshot lv2_spu_start{};
    uint32_t lle_sre_last_nid = 0;
    uint32_t lle_sre_last_code = 0;
    uint32_t syscall_number = 0;
    size_t memory_returns = 0;
    uint64_t clock_sample = 0;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    uint64_t first_clock_sample = 0;
#endif
    ps3_clock_status clock_status = PS3_CLOCK_ARGUMENT;
    size_t clock_calls = 0, clock_returns = 0;
    size_t calendar_calls = 0;
    ps3_calendar_clock_status calendar_status = PS3_CALENDAR_CLOCK_INVALID_ARGUMENT;
    ps3_calendar_time calendar_sample{};
    size_t timezone_calls = 0;
    int32_t timezone_minutes = 0, summertime_minutes = 0;
    size_t timebase_attempts = 0;
    ps3_clock_status timebase_status = PS3_CLOCK_ARGUMENT;
    uint64_t timebase_last_microseconds = 0, timebase_last_ticks = 0;
    uint64_t timebase_equal_samples = 0;
    bool timebase_has_sample = false;
    size_t condition_calls = 0, condition_returns = 0;
    uint32_t condition_nid = 0, condition_address = 0;
    ps3_lwcond_result condition_result{};
    size_t condition_publications = 0, condition_selected = 0;
    size_t exitspawn_calls = 0, exitspawn_returns = 0;
    ps3_exitspawn_kind exitspawn_kind = PS3_EXITSPAWN_ATEXITSPAWN;
    uint64_t exitspawn_argument = 0;
    ps3_exitspawn_result exitspawn_result{};
    bool exitspawn_pending_trampoline_seen = false;
    size_t diagnostic_module_calls = 0, diagnostic_module_returns = 0;
    uint64_t diagnostic_module_argument = 0;
    ps3_diagnostic_module_result diagnostic_module_result{};
    bool diagnostic_module_has_result = false;
    bool diagnostic_module_pending_trampoline_seen = false;
    // Appended, unconditional: cellSysmoduleInitialize (NID 0x63ff6ff9) accounting.
    size_t sysmodule_initialize_calls = 0, sysmodule_initialize_returns = 0;
    uint32_t sysmodule_initialize_last_result = 0;
    bool sysmodule_initialize_second_call = false;
    size_t sysutil_callback_calls = 0, sysutil_callback_returns = 0;
    uint64_t sysutil_callback_slot = 0, sysutil_callback_descriptor = 0, sysutil_callback_userdata = 0;
    ps3_sysutil_callback_result sysutil_callback_result{};
    bool sysutil_callback_has_result = false, sysutil_callback_pending_trampoline_seen = false;
    SysutilCallbackArgument sysutil_callback_invalid_argument = SysutilCallbackArgument::None;
    size_t spu_initialize_calls = 0, spu_initialize_returns = 0;
    uint64_t spu_initialize_usable = 0, spu_initialize_raw = 0;
    ps3_spu_policy_result spu_initialize_result{};
    bool spu_initialize_has_result = false;
    size_t thread_priority_calls = 0, thread_priority_returns = 0;
    uint64_t thread_priority_id = 0, thread_priority_output = 0;
    uint64_t thread_priority_result = 0;
    int32_t thread_priority_value = 0;
    ps3_guest_memory_status thread_priority_memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    bool thread_priority_has_value = false, thread_priority_has_result = false;
    bool thread_priority_memory_checked = false, thread_priority_write_attempted = false;
    bool thread_priority_published = false;
    size_t spurs_attribute_calls = 0, spurs_attribute_returns = 0;
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // aa6269a8 observations (single PPU thread writer; read after confirmed join).
    size_t spurs_initialize_calls = 0, spurs_initialize_returns = 0, spurs_initialize_writes = 0;
    size_t spurs_registration_calls = 0, spurs_registration_returns = 0;
    StartupSpursRegistrationObservation spurs_registration{};
    size_t spurs_idle_calls = 0, spurs_idle_returns = 0;
    StartupSpursIdleObservation spurs_idle{};
    size_t spurs_exception_calls = 0, spurs_exception_returns = 0;
    StartupSpursExceptionObservation spurs_exception{};
    size_t spurs_ready_calls = 0, spurs_ready_returns = 0;
    StartupSpursReadyObservation spurs_ready{};
    size_t spurs_workload_info_calls = 0, spurs_workload_info_returns = 0;
    StartupSpursWorkloadInfoObservation spurs_workload_info{};
    uint64_t spurs_initialize_arguments[7]{};
    ps3_spurs_result spurs_initialize_result{};
    bool spurs_initialize_has_result = false, spurs_write_trace_failure = false;
    // Owned kernel/helper terminal fence, latched by a pre-effect poll.
    ps3_spurs_stop_info spurs_fence{};
    bool spurs_fence_latched = false;
    // Measured before/after quota ledger of the LAST aa6269a8 call (single PPU writer; read after join).
    StartupSpursQuotaReport spurs_quota{};
#endif
    StartupSpursAttributeObservation spurs_attribute{};
    // Workload attribute initializer (0xefeb2679) and SetName (0x4a5eab63): separate from the instance attribute above.
    StartupSpursWorkloadAttributeObservation spurs_workload_attribute{};
    StartupSpursWorkloadNameObservation spurs_workload_name{};
    size_t spurs_workload_attribute_calls = 0, spurs_workload_attribute_returns = 0;
    size_t spurs_workload_name_calls = 0, spurs_workload_name_returns = 0;
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // Guest sc128 observation (single PPU writer; read after join). retained_id is nonzero only when a
    // failed output publication could not roll the created queue back: ownership stays with the registry.
    size_t event_queue_calls = 0;
    uint32_t event_queue_created_id = 0, event_queue_retained_id = 0;
    // Every queue this guest created (including a retained one): the shared registry owns the object, this
    // record is the guest-side ownership ledger that normal shutdown releases explicitly (or retains: BUSY).
    uint32_t event_queue_owned_ids[PS3_LV2_QUEUE_CAP]{};
    size_t event_queue_owned_count = 0;
    // Guest sc134 (sys_event_port_create) observation and ownership ledger (single PPU writer; read after join). Every port this
    // guest created is recorded IMMEDIATELY after the domain create, before any cancellation poll or id publication, and stays
    // recorded through any later failure (retained_id is nonzero only when a failed id store could not be rolled back). Normal
    // shutdown destroys these BEFORE the guest queues; an entry the domain refuses to release is kept and retried alone.
    size_t event_port_create_calls = 0, event_port_create_returns = 0;
    uint32_t event_port_created_id = 0, event_port_retained_id = 0, event_port_last_type = 0;
    uint64_t event_port_last_name = 0;
    uint32_t event_port_owned_ids[PS3_LV2_PORT_CAP]{};
    size_t event_port_owned_count = 0;
    // Guest sc136 (sys_event_port_connect_local) observation. Register-only: never any trace/guest-memory effect.
    size_t event_port_connect_calls = 0, event_port_connect_returns = 0;
    uint32_t event_port_connect_port = 0, event_port_connect_queue = 0, event_port_connect_cell = 0;
    // Guest sc130 observation (single PPU writer; read after join). Register-only: never any trace/guest-memory effect.
    size_t event_receive_calls = 0, event_receive_returns = 0;
    uint32_t event_receive_queue = 0;
    uint64_t event_receive_timeout = 0;
    // Guest B9BC6207 (cellSpursAttachLv2EventQueue) observation (single PPU writer; read after join). Bounded scalars only.
    // spurs_attach_arguments holds r3..r8 as received; the callback counters count PERFORMED memory effects through the
    // attach memory port; cas_committed counts CAS calls that exchanged. stop_latched is the reason a callback refused
    // (cancel/deadline/budget/fence) and callback_error that any callback returned nonzero or latched a trace failure.
    size_t spurs_attach_calls = 0, spurs_attach_returns = 0;
    uint64_t spurs_attach_arguments[6]{};
    bool spurs_attach_has_sdk = false, spurs_attach_has_result = false, spurs_attach_callback_error = false;
    bool spurs_attach_trace_failure = false;
    uint32_t spurs_attach_sdk = 0;   // the effective process SDK actually passed to the domain
    uint32_t spurs_attach_reads = 0, spurs_attach_writes = 0, spurs_attach_cas_calls = 0, spurs_attach_cas_committed = 0;
    uint32_t spurs_attach_invalid_address = 0;
    StartupStop spurs_attach_stop_latched = StartupStop::None;
    ps3_spurs_result spurs_attach_result{};
    ps3_spurs_attach_info spurs_attach_info{};
    // Guest 1F402F8F (cellSpursGetInfo) observation (single PPU writer; read after join). Bounded scalars only; every field except
    // calls/returns describes the LAST call (reset at call entry). reads/writes count memory effects the adapter ACTUALLY performed
    // through the info memory port (never the attach counters); spurs_info_report.reads/writes are the domain's own count and must
    // agree. arguments hold r3/r4 as received (full 64-bit; only the low 32 bits are used). stop_latched is the first callback refusal
    // (cancel/deadline/budget/fence/invalid memory), callback_error that any callback refused or latched a trace failure.
    // The report's meet/miss deadline zeros are reference-compatibility output, NOT telemetry.
    size_t spurs_info_calls = 0, spurs_info_returns = 0;
    uint64_t spurs_info_arguments[2]{};
    bool spurs_info_has_result = false, spurs_info_callback_error = false, spurs_info_trace_failure = false;
    uint32_t spurs_info_reads = 0, spurs_info_writes = 0, spurs_info_invalid_address = 0;
    StartupStop spurs_info_stop_latched = StartupStop::None;
    ps3_spurs_result spurs_info_result{};
    ps3_spurs_info_report spurs_info_report{};
    // Guest NID 0x24a1ea07 observation (single PPU writer; read after join). Bounded scalars only, no dynamic log.
    // calls counts every entry; prepared/id_written/started/returns count confirmed milestones in call order.
    size_t ppu_create_calls = 0, ppu_create_prepared = 0, ppu_create_id_written = 0, ppu_create_started = 0, ppu_create_returns = 0;
    size_t ppu_create_guest_errors = 0, ppu_create_terminal = 0;
    uint32_t ppu_create_last_entry = 0, ppu_create_last_toc = 0, ppu_create_last_output = 0, ppu_create_last_thread_id = 0;
    uint32_t ppu_create_last_stack = 0, ppu_create_last_cell = 0;
    int32_t ppu_create_last_priority = 0;
    uint64_t ppu_create_last_argument = 0, ppu_create_last_flags = 0;
    bool ppu_create_has_prepare = false, ppu_create_has_start = false;
    StartupPpuCreatePrepareStatus ppu_create_prepare = StartupPpuCreatePrepareStatus::ProviderFailure;
    StartupPpuCreateStartStatus ppu_create_start = StartupPpuCreateStartStatus::ProviderFailure;
    // SPURS synchronization create diagnostics (single PPU writer; read after join). See StartupSpursSyncCreateRecord.
    // calls counts every create callback entry; retained = min(calls, capacity); dropped = calls - retained. A failure is
    // always copied to last_failed (even when also retained), so the most recent failed create survives overflow and unwind.
    size_t spurs_sync_create_calls = 0, spurs_sync_create_retained = 0, spurs_sync_create_dropped = 0;
    bool spurs_sync_create_overflow = false, spurs_sync_has_last_failed = false;
    StartupSpursSyncCreateRecord spurs_sync_create[STARTUP_SPURS_SYNC_CREATE_CAPACITY]{};
    StartupSpursSyncCreateRecord spurs_sync_last_failed{};
#endif
    // lv2 syscall 44 (sys_ppu_thread_join) diagnostics. UNCONDITIONAL (no #ifdef): the bridge lives in the SPURS-initialize
    // flag region but the layout must not depend on it. Single PPU writer; read after join. calls counts every entry, returns
    // counts the only successful one (r3 = 0 after the BE u64 exit value was published).
    size_t ppu_join_calls = 0, ppu_join_returns = 0;
    uint64_t ppu_join_last_thread = 0, ppu_join_last_exit_value = 0;
    StartupPpuJoinStatus ppu_join_last_status = StartupPpuJoinStatus::ProviderFailure;
    bool ppu_join_has_result = false;
    // lv2 syscall 403 (sys_tty_write) diagnostics. UNCONDITIONAL (no #ifdef): the dispatch lives behind
    // PS3_NATIVE_STARTUP_TTY but the layout must not depend on it. Single PPU writer; read after join. calls counts every
    // entry; returns counts the confirmed r3 = 0; bytes counts guest bytes accepted (before escaping); the last fields
    // describe the LAST call only (reset at entry). Appended, never flag-conditional.
    size_t tty_write_calls = 0, tty_write_returns = 0, tty_bytes = 0;
    uint32_t tty_last_channel = 0, tty_last_len = 0;
    // SPURS taskset setup block, the two pure struct-init entries.
    // UNCONDITIONAL (no #ifdef): both dispatches live in the PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // region of ps3_hle_call, but the layout must not depend on it -- the verifiers build this header with the
    // flag off as well. Single PPU writer; read after join. `_calls` counts every entry to the NID, `_returns`
    // counts the calls that produced a firmware result (success 0 or a refusal); a call that stopped with an
    // InvalidMemory/TraceCapacity stop advanced `_calls` and not `_returns`. `_last` holds the last r3 published
    // (0 on success, the sign-extended 0x804109xx code on refusal), reset to 0xFFFFFFFF at each entry so a call
    // that never returned is distinguishable. `attr_writes`/`evflag_writes` count the struct bytes the adapter
    // actually wrote into guest memory (512 and 128 on success, 0 on refusal). Appended, never flag-conditional.
    size_t taskset_attr_init_calls = 0, taskset_attr_init_returns = 0;
    uint32_t taskset_attr_init_last = 0xFFFFFFFFu;
    size_t taskset_attr_init_writes = 0;
    size_t event_flag_init_calls = 0, event_flag_init_returns = 0;
    uint32_t event_flag_init_last = 0xFFFFFFFFu;
    size_t event_flag_init_writes = 0;
    // The next two entries of the same taskset setup block (g11, extended by g12,
    // corrected by g12b and completed by g12c): the attribute name setter
    // 0x652B70E2 (leaf: one 4-byte store of the name POINTER at attr+0x1C, no guest read) and the event-flag attach to an
    // lv2 event queue 0x87630976 (four argument gates, then the firmware continuation: create `_spuEvF`, connect it in
    // DYNAMIC port mode, write F+0x7C and F+0x0C, return 0).
    // Same conventions as the pair above, plus `name_writes` for the setter's 4 bytes. The attach entry now runs the
    // firmware continuation, so it also records what the continuation produced. Appended,
    // never flag-conditional.
    size_t taskset_name_calls = 0, taskset_name_returns = 0;
    uint32_t taskset_name_last = 0xFFFFFFFFu;
    size_t taskset_name_writes = 0;
    size_t event_flag_attach_calls = 0, event_flag_attach_returns = 0;
    uint32_t event_flag_attach_last = 0xFFFFFFFFu;
    // g12c: the continuation's own observations, reset per call. `queues_created` counts a successful sc 128, `queue` is
    // the id it produced, `queues_destroyed` counts the firmware's sc 129 unwind (0xB310), `port` is the SPU port the
    // kernel chose (0xFF = none), `writes` counts the guest bytes the adapter itself wrote (the helper's port byte is
    // written by the attach domain and counted in spurs_attach_writes). The domain result itself is published in
    // spurs_attach_result / spurs_attach_has_result, which the attach dispatch already owns. Scalars only, so this block
    // stays layout-identical whether or not PS3_NATIVE_STARTUP_SPURS_INITIALIZE is defined. Appended, never
    // flag-conditional.
    size_t event_flag_attach_queues_created = 0, event_flag_attach_queues_destroyed = 0;
    uint32_t event_flag_attach_queue = 0;
    uint32_t event_flag_attach_port = 0xFF;
    size_t event_flag_attach_writes = 0;
    // The same block's creation entry (corrected by g13b):
    // cellSpursCreateTasksetWithAttribute, NID 0xC10931CB. The workload registration carries pm
    // 0x00023680, size 0x00001E40 and data = the taskset EA (firmware 0x14B5C/0x14B64/0x14B88).
    // UNCONDITIONAL (no #ifdef): the dispatch lives in
    // the PS3_NATIVE_STARTUP_SPURS_INITIALIZE region of ps3_hle_call but the layout must not depend on it.
    // Single PPU writer; read after join. `_calls` counts every entry, `_returns` the calls that produced a
    // firmware r3; `_last` holds that r3 (0 on success, the sign-extended 0x804109xx on a refusal), reset to
    // 0xFFFFFFFF at entry so a call that never returned is distinguishable. `_wid` is the workload id the
    // native registration claimed (0xFFFFFFFF when none was claimed). `_writes` counts the guest bytes the
    // adapter itself wrote (6400 for the taskset image, plus 4, 1 and the 8-byte event-flag word on the
    // publishing arm). `_registered` counts the calls that reached those three stores, i.e. the calls on
    // which the firmware's 0xDC5C runs.
    size_t taskset_create_calls = 0, taskset_create_returns = 0;
    uint32_t taskset_create_last = 0xFFFFFFFFu;
    uint32_t taskset_create_wid = 0xFFFFFFFFu;
    size_t taskset_create_writes = 0, taskset_create_registered = 0;
    // The domain's own observations, reproduced here as scalars rather than as domain structs so that an
    // include of this header never depends on a domain type. `registration_sdk` is the code the registration
    // produced (0 when the domain was not reached), `registration_stop` its ps3_spurs_registration_stop value,
    // `registration_effects` its PS3_SPURS_REGISTRATION_* bit mask and `registration_wid` the wid it claimed
    // (0xFFFFFFFF when none). Appended, never flag-conditional.
    bool taskset_create_has_registration = false;
    bool taskset_create_trace_failure = false;
    StartupStop taskset_create_latched_stop = StartupStop::None;
    uint32_t taskset_create_registration_sdk = 0;
    uint32_t taskset_create_registration_stop = 0;
    uint32_t taskset_create_registration_effects = 0;
    uint32_t taskset_create_registration_wid = 0xFFFFFFFFu;
    // _cellSpursLFQueueInitialize (NID 0x011EE38B), the entry the game reaches right after the taskset
    // block. Same shape as the taskset-creation counters
    // above: `_calls` counts every entry, `_returns` the calls that produced a firmware r3, `_last` that r3
    // (0 on success, the sign-extended 0x804109xx on a refusal), reset to 0xFFFFFFFF at entry so a call that
    // never returned is distinguishable. `_writes` counts the guest bytes this adapter wrote (2 * 30 for the
    // auxiliary runs, 128 for the line, 2 for the state word). `_shape` is 1 for the CellSpursLFQueue shape
    // and 0 for the CellSyncLFQueue one (0xFF when no shape was selected). `_owner` is the raw r3 the game
    // passed (the low bit is its taskset tag) and `_version` the value read at taskset+0x24 or spurs+0x74.
    // Appended, never flag-conditional.
    size_t lfq_calls = 0, lfq_returns = 0;
    uint32_t lfq_last = 0xFFFFFFFFu;
    size_t lfq_writes = 0;
    unsigned lfq_shape = 0xFFu;
    uint32_t lfq_owner = 0xFFFFFFFFu;
    uint32_t lfq_version = 0xFFFFFFFFu;
    // cellSpursLFQueueAttachLv2EventQueue (NID 0x1656D49F), the entry func_00424774 reaches at LR 0x0042483C right after
    // _cellSpursLFQueueInitialize returns. Same shape as
    // the predecessor's block above: `_calls` counts every entry, `_returns` the calls that produced an r3, `_last` that r3
    // (0 on success, the sign-extended 0x804109xx on a refusal), reset to 0xFFFFFFFF at entry. `_owner` is the line's raw
    // +0x74 word, `_group` the id it resolved to, `_version` the line's +0x24 word, `_shape` 1 for the SPURS shape and 0 for
    // the CellSync one, `_iterations` the firmware's trip count, `_port` the SPU port the connect bound and `_writes` the
    // guest bytes this adapter wrote (2 + 4 for the SPURS publication span).
    // OBSERVABLE CONSTRAINT: on the game's own arguments `_last` must be 0 (a non-zero r3 sends the caller to
    // func_00452290, a print-then-`tw 31` fatal assert). Appended, never flag-conditional.
    size_t lfqa_calls = 0, lfqa_returns = 0;
    uint32_t lfqa_last = 0xFFFFFFFFu;
    uint32_t lfqa_owner = 0, lfqa_group = 0, lfqa_version = 0;
    uint32_t lfqa_shape = 0xFFu, lfqa_iterations = 0;
    uint32_t lfqa_queues_created = 0, lfqa_queues_destroyed = 0;
    uint32_t lfqa_port = 0xFFu;
    size_t lfqa_writes = 0;
    // cellSpursCreateTask (NID 0xBEB600AC), the entry func_00424714 reaches at LR
    // 0x00424744. Same shape as the predecessors:
    // `_calls` counts every entry, `_returns` the calls that produced an r3, `_last` that r3 (0 on
    // success, the sign-extended 0x804109xx on a refusal), reset to 0xFFFFFFFF at entry. `_id` is the
    // task id the call published (0xFFFFFFFF when none), `_slot` the taskset slot it claimed (0xFF when
    // none) and `_writes` the guest bytes this adapter wrote (4 record fields of 8, the 4-byte taskId,
    // the 4-byte allocation mask and the 4-byte ready mask = 44 on the game's path). `_ready_raised` /
    // `_signal_written` say whether 0xA750's mask raise and 0x85D0's signal byte committed, `_ready_address`
    // is the mask word (spurs+0x70 for wid <= 15, +0x78 otherwise) and `_mask_bit` the bit 0xA750 set.
    // Appended, never flag-conditional.
    size_t task_calls = 0, task_returns = 0;
    uint32_t task_last = 0xFFFFFFFFu;
    uint32_t task_id = 0xFFFFFFFFu;
    size_t task_writes = 0;
    unsigned task_slot = 0xFFu;
    unsigned task_ready_raised = 0, task_signal_written = 0;
    uint32_t task_ready_address = 0;
    uint32_t task_mask_bit = 0;
    // g17c fix 4: the caller's *taskId is written only at 0x125D0, after the signal returned 0.
    // `_id_written` records whether that store happened; a refused signal leaves it 0 and the
    // caller's word untouched. Appended, never flag-conditional.
    unsigned task_id_written = 0;
    // cellPadInit / cellPadSetPortSetting. The observation structs hold the
    // instance-owned pad_state, which is the firmware's module flag; `pad_init_requests` counts the calls whose result was
    // NON-ZERO, because that is the only evidence the game took its silent early return. Appended, never
    // flag-conditional.
    size_t pad_init_calls = 0, pad_init_returns = 0, pad_init_requests = 0;
    StartupPadInitObservation pad_init{};
    size_t pad_set_port_setting_calls = 0, pad_set_port_setting_returns = 0;
    StartupPadPortSettingObservation pad_set_port_setting{};
    // The six `sys_fs` imports. Appended, never flag-conditional.
    StartupFsObservation fs{};
    ps3_user_memory_service_result memory_result{};
    uint32_t mutex_nid = 0;
    uint32_t observed_lock_address = 0, observed_lock_owner = 0;
    size_t observed_lock_acquisitions = 0;
    ps3_lwmutex_result mutex_failure{};
    StartupStop stop = StartupStop::None;
    uint32_t external_id = 0;
    uint64_t invalid_address = 0;
    ppu_context before_tls{}, after_tls{}, terminal{};
    ppu_context *cpu = nullptr;
    StartupEntry entry = StartupEntry::Game;
    StartupTerminalNotification terminal_notification{};   // see contract above; immutable while running
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    StartupDispatchRecord dispatches[STARTUP_DISPATCH_CAPACITY]{};
    size_t dispatch_count = 0;
    ps3_compiled_dispatch_stop_reason dispatch_stop_latched = PS3_COMPILED_DISPATCH_CONTINUE;
#endif
    uint64_t dispatch_overflow = 0;
};
struct StartupFixedTimezone {
    const int32_t timezone_minutes;
    const int32_t summertime_minutes;
};
/* Every CPU passed to generated code belongs to this nested standard-layout
 * owner. The immutable RAM permission scopes and coherent domain are shared
 * with HLE services. Admission owns a reservation record until joined release.
 * Legacy standalone CPU contexts and original VM objects are incompatible. */
enum class StartupTaskwaitFailure { None, MissingProvider, ProviderFailure, UnbalancedEnd, DepthOverflow };
struct StartupTaskwaitPort {
    void *context = nullptr;
    bool (*yield_once)(void *) noexcept = nullptr;
};
// Single execution writer; inspect only after its confirmed join. A terminal stop preserves depth.
struct StartupTaskwaitObservation {
    size_t begins = 0, spins = 0, ends = 0, pauses_attempted = 0, pauses_completed = 0;
    uint32_t depth = 0, max_depth = 0;
    StartupTaskwaitFailure failure = StartupTaskwaitFailure::None;
};
struct StartupExecution {
    PpuMemoryExecution ppu;
    BootContentMetadata boot_content{};
    SystemParameters system_parameters{};
    SystemProfile system_profile{};
    PresentationTopology presentation_topology=PresentationTopology::Unconfigured;
    NativePresentationMode presentation_mode=NativePresentationMode::Unconfigured;
    StartupProcessSdk process_sdk{};
    StartupControl *control;
    int32_t guest_priority = 0;
    bool has_guest_priority = false;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    // Root (2026-11-12, Decision 89): the depth limit of nested indirect calls. 8 was a probe-era bound tied to the
    // observation recorder's 8 pending entries; hardware run rv20 reached depth 8 in ordinary game code after the
    // loading screen (stop=14, ctr=0x49bca0, lr=0x2695ec). The LLE profile runs the game, so the limit there is
    // the host stack's business; calls deeper than STARTUP_OBSERVED_DISPATCH_DEPTH are dispatched unobserved.
#ifdef PS3_NATIVE_LLE_PROFILE
    ps3_compiled_dispatch_state dispatch{0, 4096};
#else
    ps3_compiled_dispatch_state dispatch{0, 8};
#endif
    ps3_compiled_dispatch_stop_port dispatch_stop{};
#endif
    infamous::diagnostics::StartupObservation *observation = nullptr;
    // Immutable borrowed capability/context through confirmed worker join.
    const ps3_calendar_clock *calendar_clock = nullptr;
    // Borrowed immutable policy through join, never an SDK failure fallback.
    const StartupFixedTimezone *fixed_timezone = nullptr;
    // Borrowed condition registry and its complete collaborator graph through
    // join and any retained cleanup; no ownership transfer at admission.
    ps3_lwcond_registry *conditions = nullptr;
    // Opaque process registration state, borrowed through join/retained stop.
    ps3_exitspawn_registry *exitspawn_callbacks = nullptr;
    // Opt-in assumed admissions only; never a real module/provider capability.
    ps3_diagnostic_module_state *diagnostic_modules = nullptr;
    // Borrowed registration only; no callback/event dispatch capability.
    ps3_sysutil_callback_registry *sysutil_callbacks = nullptr;
    // One mutable process registry and its guard are borrowed through every PPU join.
    ps3_disc_change_registry *disc_change_callbacks = nullptr;
    // Owned resource authority and guard remain borrowed through confirmed join.
    ps3_spu_resource_policy *spu_resources = nullptr;
    // Native GCM-init service (composition-owned storage, borrowed here) and its guest-port bridge.
    gcm_init_service *gcm_init = nullptr;
    StartupGcmGuestBridge gcm_init_bridge{};
    // Composition-owned storage/state, borrowed through confirmed join; null stops MissingServices.
    const StartupGameDataPort *gamedata = nullptr;
    // Composition-owned TTY ring (sys_tty_write, lv2 403), borrowed through confirmed join; a null ring stops MissingServices.
    StartupTtyRing *tty = nullptr;
    // Composition-owned file-system composition (storage adapter + mount table + service), borrowed through confirmed join;
    // null or inactive stops MissingServices for the six `sys_fs` imports. Its storage is never on a thread stack.
    StartupFsContext *fs = nullptr;
    // Set by the child owner only; a primary-thread exit stays unsupported.
    bool child_thread_exit_allowed = false;
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // Borrowed zero-filled runtime (domain/lv2/policy/ppu/threads/LS/sync ports) through confirmed join;
    // native_startup_release_after_join shuts every owned service down BEFORE guest memory is released.
    ps3_spurs_runtime *spurs = nullptr;
    // The shared allocator whose quota the SPURS charges join (set by startup_spurs_compose only; borrowed).
    ps3_user_memory *spurs_quota_memory = nullptr;
    // Guest sc128 (sys_event_queue_create) target: the SAME owned lv2 registry the SPURS services use (set by
    // startup_spurs_compose only; borrowed). Null or uninitialised => the syscall stops MissingServices.
    ps3_lv2_objects *event_queues = nullptr;
    // Guest sc130 (sys_event_queue_receive) port: borrowed, immutable through confirmed join (see contract above).
    // Null or receive == nullptr => the syscall stops MissingServices.
    const StartupEventReceivePort *event_receive = nullptr;
    // Decision 33 / B2a: sleep + cancellation for the lv2 sync arms. Borrowed through join; null keeps
    // the previous behaviour exactly (the affected syscalls then stop MissingServices).
    const StartupLv2SleepPort *lv2_sleep = nullptr;
    ps3_notify_cancel *lv2_sync_cancel = nullptr;
    // Decision 33 / C1b: where a blocking lv2 wait (semaphore, event flag, SPU group join) publishes "blocked" (1 while
    // it sleeps, 0 when it resumes). A child PPU points this at its ps3_ppu_service::activity word, so the creator's
    // prefix wait in ps3_ppu_service_start ends when the child blocks in ANY real lv2 wait, not only in the queue
    // receive. Null (the primary, and every pre-C1b composition) keeps StartupControl::lv2_sync_blocked. The wake uses
    // event_queues->port, which the composition shares with the PPU thread registry.
    uint64_t *lv2_blocked_word = nullptr;
    // Decision 33 / D1: the process-wide pad module state (borrowed; null keeps the per-control state and no controller
    // source). Set once, before any guest thread runs; every pad arm goes through it when it is set.
    StartupPadProcess *pad_process = nullptr;
    // Root (2026-11-11): the process-wide audio service (borrowed; null = the audio imports stay unserved).
    StartupAudioProcess *audio_process = nullptr;
    // Root (2026-11-11): the process-wide lv2 timers (borrowed; null = syscalls 70..76 stay unsupported).
    StartupTimerProcess *timer_process = nullptr;
    // Root (2026-11-11): the process-wide lv2 mutex table (borrowed; null = syscalls 100..104 stay unsupported).
    StartupMutexProcess *mutex_process = nullptr;
    // Root (2026-11-11): the live RSX consumer (borrowed; null = flip and handler imports stay unserved).
    StartupRsxProcess *rsx_process = nullptr;
    // Root (2026-11-11): the message dialog state (borrowed; null = the dialog imports stay unserved).
    StartupDialogProcess *dialog_process = nullptr;
    // The process's PPU thread registry (borrowed; null in compositions without child threads). Used by
    // sys_process_is_stack to recognise a child thread's guest stack, which lives in user memory here.
    ps3_ppu_threads *ppu_threads = nullptr;
    // Decision 33 / B2b: the SPU image and thread-group registries. Borrowed through join; null keeps
    // the previous behaviour exactly (the SPU syscalls and imports then stop MissingServices).
    ps3_lv2_spu_images *spu_images = nullptr;
    ps3_lv2_spu_groups *spu_groups = nullptr;
    // A 4-byte guest word this adapter may use to receive a user-memory allocation's published EA. The
    // composition owns it (it must be inside a declared RAM range). 0 = the import stops MissingServices.

    // Guest sys_ppu_thread_create (NID 0x24a1ea07) port: borrowed, immutable through confirmed join (see contract above).
    // Null or a missing callback => the import stops MissingServices before any effect.
    const StartupPpuCreatePort *ppu_create = nullptr;
    // Composition copies the port; its context remains borrowed through this execution's join.
    StartupTaskwaitPort taskwait{};
    StartupTaskwaitObservation taskwait_observation{};
#endif
};
static_assert(std::is_standard_layout<ppu_context>::value, "CPU must have standard layout");
static_assert(std::is_standard_layout<StartupExecution>::value && offsetof(StartupExecution, ppu) == 0 && offsetof(PpuMemoryExecution, cpu) == 0,
              "CPU must be pointer-interconvertible with its explicit execution owner");
static_assert(sizeof(ppu_context) == 1104 && offsetof(ppu_context, cr) == 1024 &&
              offsetof(ppu_context, lr) == 1032, "Original generated CPU ABI must remain unchanged");
static_assert(std::atomic<bool>::is_always_lock_free, "Cancellation must not hide a service lock");

bool native_startup_admit(StartupExecution &, PpuMemoryBinding &, const Ps3PpuExecutionServices &);
#ifdef PS3_NATIVE_FAST_SYNC
/* ppu-speed-20261113 item 1: a host view of guest memory (pointer to [address, address + bytes) when every page is committed, else
 * null) for the lwmutex import arm's memory touches; see startup_execution.cpp. Call once, before any guest thread runs. */
void native_startup_enable_fast_sync(unsigned char *(*pointer)(void *context, uint32_t address, uint32_t bytes), void *context);
#endif
/* Recorder and event storage are exclusively borrowed through worker join and
 * readback. Selection is immutable while admitted; failed admission preserves
 * caller-owned credit. This route does not select the legacy journal. */
bool native_startup_admit_recorded(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder);
/* Budget/policy/clock and recorder storage remain stable through confirmed
 * join. Admission reads no clock and never resets the caller's fixed epoch or
 * deadline; a new run needs a freshly constructed policy/binding pair. */
bool native_startup_admit_budgeted(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder, DiscoveryBudgetBinding &budget);
/* All selected storage is caller-owned and exclusively borrowed through
 * confirmed join. No observer may be read or reclaimed after a failed join. */
bool native_startup_admit_discovery(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder, DiscoveryBudgetBinding &budget,
    infamous::diagnostics::StartupObservation &observation);
/* With PS3_NATIVE_STARTUP_SPURS_INITIALIZE this first cancels, wakes and joins every SPURS service
 * and worker; if any join fails it returns PS3_GUEST_MEMORY_BUSY and releases NOTHING (retryable). */
ps3_guest_memory_status native_startup_release_after_join(StartupExecution &);
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
/* Real lwmutex/lwcond registry adapters (execution.conditions) for ps3_spurs_runtime_config::sync. */
extern "C" ps3_spurs_sync_port native_startup_spurs_sync_port(StartupExecution *execution);
#endif
extern "C" void native_startup_worker(void *execution);
/* Validates and creates the composition-owned GCM-init service; on failure nothing is attached and
 * the composition reports the status. The layout's regions must already be writable guest memory. */
gcm_init_status startup_gcm_init_attach(StartupExecution &execution, gcm_init_service *storage,
                                        const gcm_init_layout &layout);
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
extern "C" void ps3_taskwait_begin(void);
extern "C" void ps3_taskwait_spin(void);
extern "C" void ps3_taskwait_end(void);
#endif
#endif
