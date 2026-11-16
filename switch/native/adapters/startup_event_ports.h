#ifndef NATIVE_STARTUP_EVENT_PORTS_H
#define NATIVE_STARTUP_EVENT_PORTS_H
/* Guest ABI adapter for lv2 syscalls 134 (sys_event_port_create) and 136 (sys_event_port_connect_local) over the owned
 * ps3_lv2_objects domain. Header-only on purpose (no build-list change). It knows only guest memory, the lv2 registry and the
 * pinned guest ABI: it never touches PPU registers, diagnostics, tracing or platform code, and holds no state of its own.
 * Pinned reference: sys_event.cpp sys_event_port_create / sys_event_port_connect_local / sys_event_port_destroy.
 *
 * sc134  r3 = u32* eport_id, r4 = s32 port_type, r5 = u64 name.
 *   The bridge sequences it as
 *     prepare_create()  - validates the output pointer (canonical, 4-byte aligned, writable BE u32 range, readable); NO effects
 *     (bridge reserves the trace record, checks ledger room and polls cancellation/deadline/budget: all still pre-effect)
 *     create()          - the real domain create (full 64-bit name stored verbatim; duplicates are valid: the pinned code
 *                         performs no uniqueness lookup)
 *     (bridge records ownership IMMEDIATELY, before any poll or the id publication)
 *     publish()         - stores the big-endian id; a failed store is rolled back through the owning domain
 *   A faulting guest pointer has no CELL error in the reference, so it is a native STOP (BadPointer), never an invented EFAULT.
 *   A misaligned output is refused under NATIVE admission policy (fail closed); this is not claimed to be reference behaviour.
 *   r4 is an s32: only its low 32 bits are the type; the domain answers EINVAL for anything but LOCAL(1)/IPC(3). IPC is legal
 *   lv2 that this layer does not own: a native STOP (Unsupported), never a fake id. Domain EINVAL/EAGAIN are guest CELL codes.
 *
 * sc136  r3 = u32 eport_id, r4 = u32 equeue_id (low 32 bits each). Register-only.
 *   classify() is the diagnostic ownership restriction: the connect is performed only between a port and a queue THIS guest
 *   created (the ledgers). A resource that exists in the registry but is not guest-owned (SPURS-owned, native-owned) is Foreign:
 *   the bridge stops Unsupported BEFORE any effect. An absent resource yields the observed ESRCH without a later mutating call.
 *   connect() is the real domain connect only: it binds NO service filter, enqueues NO event and wakes NOBODY. */
#include "../domain/lv2_objects.h"
#include <cstddef>
#include <cstdint>
namespace startup_event_ports {
constexpr uint32_t OUTPUT_BYTES = 4;
enum class CreatePrepare { Ready, NoService, BadPointer, ProviderFailure };
enum class Create {
    Created,      // port allocated; id in port_id (the caller must record ownership before anything else)
    GuestError,   // domain CELL code in `cell`; nothing allocated, no id consumed
    Unsupported,  // legal lv2 input this layer does not own (IPC port); nothing allocated
    DomainFailure // unexpected domain code (invariant stop); nothing allocated
};
enum class Publish {
    Stored,            // id stored
    StoreFailed,       // store failed after allocation; the port was rolled back (id never reused)
    RetainedOwnership  // store failed AND the domain refused the rollback: the port stays owned
};
struct CreateRequest { uint32_t output = 0, type = 0; uint64_t name = 0; };
struct CreateOutcome { Create status = Create::DomainFailure; uint32_t cell = 0, port_id = 0; };
inline bool caller_fault(ps3_guest_memory_status s) { return s == PS3_GUEST_MEMORY_INVALID_RANGE || s == PS3_GUEST_MEMORY_UNMAPPED; }
/* `writable(address, length)` is the caller's permission oracle for the output range (RAM write scopes). */
template <class Writable>
CreatePrepare prepare_create(ps3_lv2_objects *lv2, const ps3_guest_memory &memory, Writable writable,
    uint64_t output, uint64_t type, uint64_t name, CreateRequest &request)
{
    if (!lv2 || !lv2->initialized) return CreatePrepare::NoService;
    if (!output || output > UINT32_MAX || (output & 3)) return CreatePrepare::BadPointer;
    if (OUTPUT_BYTES > UINT64_C(0x100000000) - output) return CreatePrepare::BadPointer;
    if (!writable(uint32_t(output), OUTPUT_BYTES)) return CreatePrepare::BadPointer;
    unsigned char probe[OUTPUT_BYTES];
    const ps3_guest_memory_status status = memory.read(memory.context, uint32_t(output), probe, sizeof probe);
    if (status != PS3_GUEST_MEMORY_OK) return caller_fault(status) ? CreatePrepare::BadPointer : CreatePrepare::ProviderFailure;
    request.output = uint32_t(output);
    request.type = uint32_t(type);   // PPU ABI: s32 argument = low 32 bits of the register; a negative value is EINVAL in the domain
    request.name = name;             // full 64 bits, carried verbatim
    return CreatePrepare::Ready;
}
inline CreateOutcome create(ps3_lv2_objects *lv2, const CreateRequest &request)
{
    CreateOutcome out;
    uint32_t id = 0;
    const uint32_t rc = ps3_lv2_port_create(lv2, &id, request.type, request.name);
    if (rc == PS3_LV2_EINVAL || rc == PS3_LV2_EAGAIN) { out.status = Create::GuestError; out.cell = rc; return out; }
    if (rc == PS3_LV2_UNSUPPORTED) { out.status = Create::Unsupported; return out; }
    if (rc != PS3_LV2_OK || !id) { out.status = Create::DomainFailure; return out; }
    out.status = Create::Created; out.port_id = id;
    return out;
}
inline Publish publish(ps3_lv2_objects *lv2, const ps3_guest_memory &memory, const CreateRequest &request, uint32_t id)
{
    const unsigned char encoded[OUTPUT_BYTES] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    if (memory.write(memory.context, request.output, encoded, sizeof encoded) == PS3_GUEST_MEMORY_OK) return Publish::Stored;
    // Real rollback through the owning domain; if it refuses, ownership is retained and reported, never leaked.
    return ps3_lv2_port_destroy(lv2, id) == PS3_LV2_OK ? Publish::StoreFailed : Publish::RetainedOwnership;
}

enum class Verdict { Proceed, Absent, Foreign };
enum class Connect { Connected, GuestError, DomainFailure };
struct ConnectOutcome { Connect status = Connect::DomainFailure; uint32_t cell = 0; };
inline bool owns(const uint32_t *ids, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; ++i) if (ids[i] == id) return true;
    return false;
}
/* Only ledger-owned ids may reach connect. Ids are never reused and the ledgers have one writer. An observed ESRCH remains
 * an error even if another creator subsequently allocates that id; no mutating call may act on the new foreign object. */
inline Verdict classify(ps3_lv2_objects *lv2, uint32_t port, uint32_t queue,
    const uint32_t *owned_ports, size_t port_count, const uint32_t *owned_queues, size_t queue_count)
{
    if (owns(owned_ports, port_count, port) && owns(owned_queues, queue_count, queue)) return Verdict::Proceed;
    ps3_lv2_port p{};
    ps3_lv2_queue q{};
    const uint32_t port_status = ps3_lv2_port_snapshot(lv2, port, &p);
    if (port_status == PS3_LV2_ESRCH) return Verdict::Absent;
    if (port_status != PS3_LV2_OK) return Verdict::Foreign;
    if (ps3_lv2_queue_snapshot(lv2, queue, &q) == PS3_LV2_ESRCH) return Verdict::Absent;
    return Verdict::Foreign;
}
inline ConnectOutcome connect(ps3_lv2_objects *lv2, uint32_t port, uint32_t queue)
{
    ConnectOutcome out;
    const uint32_t rc = ps3_lv2_port_connect_local(lv2, port, queue);
    if (rc == PS3_LV2_OK) { out.status = Connect::Connected; return out; }
    if (rc == PS3_LV2_ESRCH || rc == PS3_LV2_EINVAL || rc == PS3_LV2_EISCONN) { out.status = Connect::GuestError; out.cell = rc; return out; }
    return out;   // DomainFailure: an unexpected domain code is an invariant stop, never a guest error
}
}
#endif
