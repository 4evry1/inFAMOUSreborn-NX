#ifndef NATIVE_STARTUP_EVENT_QUEUE_H
#define NATIVE_STARTUP_EVENT_QUEUE_H
/* Guest ABI adapter for lv2 syscall 128 (sys_event_queue_create) over the owned ps3_lv2_objects domain.
 * Header-only on purpose (no build-list change). It knows only guest memory, the lv2 registry and the
 * pinned guest ABI: it never touches PPU registers, diagnostics, tracing or platform code, and holds no
 * state of its own. The startup bridge sequences it as
 *     prepare()  - validates every pointer/read; NO effects
 *     (bridge reserves the trace record and polls cancellation/deadline: both still pre-effect)
 *     commit()   - creates the queue, stores the big-endian id, rolls back if the store fails
 * Guest ABI (sys_event.cpp sys_event_queue_create): r3 = u32* equeue_id, r4 = sys_event_queue_attribute_t*
 * {be u32 protocol, be s32 type, char name[8]}, r5 = u64 ipc_key, r6 = s32 size. A faulting guest pointer has
 * no CELL error in the reference, so it is a native STOP (BadPointer), never an invented EFAULT. A register
 * with bits above 32 is rejected under NATIVE admission policy (like the other startup syscalls); this is not
 * claimed to be reference-equivalent behaviour. Attribute name bytes are indeterminate guest stack: carried
 * to the domain verbatim, never validated or required to be NUL-terminated. Domain
 * validation (size/protocol/type -> EINVAL, exhaustion -> EAGAIN) is returned verbatim as a guest CELL code;
 * a nonzero ipc_key is legal lv2 that the domain does not own and is a native STOP (Unsupported). */
#include "../domain/lv2_objects.h"
#include <cstddef>
#include <cstdint>
namespace startup_event_queue {
constexpr uint32_t ATTRIBUTE_BYTES = 16, OUTPUT_BYTES = 4;
enum class Prepare { Ready, NoService, BadPointer, ProviderFailure };
enum class Commit {
    Created,          // id stored; queue owned by the registry
    GuestError,       // domain CELL code in `cell`; no output written, nothing allocated
    Unsupported,      // domain refused a legal input it does not own (ipc key); nothing allocated
    DomainFailure,    // unexpected domain code (invariant stop); nothing allocated
    StoreFailed,      // output store failed after allocation; the queue was rolled back (id never reused)
    RetainedOwnership // store failed AND the domain refused the rollback: queue stays owned (queue_id)
};
struct Request { uint32_t output = 0; ps3_lv2_queue_attr attr{}; uint64_t ipc_key = 0; int32_t size = 0; };
struct Outcome { Commit status = Commit::DomainFailure; uint32_t cell = 0, queue_id = 0; };
inline uint32_t be32(const unsigned char *b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
inline bool caller_fault(ps3_guest_memory_status s) { return s == PS3_GUEST_MEMORY_INVALID_RANGE || s == PS3_GUEST_MEMORY_UNMAPPED; }
/* `writable(address, length)` is the caller's permission oracle for the output range (RAM write scopes). */
template <class Writable>
Prepare prepare(ps3_lv2_objects *lv2, const ps3_guest_memory &memory, Writable writable,
    uint64_t output, uint64_t attr, uint64_t ipc_key, uint64_t size, Request &request)
{
    if (!lv2 || !lv2->initialized) return Prepare::NoService;
    if (!output || !attr || output > UINT32_MAX || attr > UINT32_MAX) return Prepare::BadPointer;
    if (OUTPUT_BYTES > UINT64_C(0x100000000) - output || ATTRIBUTE_BYTES > UINT64_C(0x100000000) - attr) return Prepare::BadPointer;
    if (!writable(uint32_t(output), OUTPUT_BYTES)) return Prepare::BadPointer;
    unsigned char probe[OUTPUT_BYTES], raw[ATTRIBUTE_BYTES];
    ps3_guest_memory_status status = memory.read(memory.context, uint32_t(output), probe, sizeof probe);
    if (status != PS3_GUEST_MEMORY_OK) return caller_fault(status) ? Prepare::BadPointer : Prepare::ProviderFailure;
    status = memory.read(memory.context, uint32_t(attr), raw, sizeof raw);
    if (status != PS3_GUEST_MEMORY_OK) return caller_fault(status) ? Prepare::BadPointer : Prepare::ProviderFailure;
    request.output = uint32_t(output);
    request.attr.protocol = be32(raw);
    request.attr.type = be32(raw + 4);   // s32 on the wire; a negative value is rejected by the domain's type check
    request.attr.name = uint64_t(be32(raw + 8)) << 32 | be32(raw + 12);   // carried, never validated or interpreted
    request.ipc_key = ipc_key;
    request.size = int32_t(uint32_t(size));   // PPU ABI: s32 argument = low 32 bits of the register
    return Prepare::Ready;
}
inline Outcome commit(ps3_lv2_objects *lv2, const ps3_guest_memory &memory, const Request &request)
{
    Outcome out;
    uint32_t id = 0;
    const uint32_t rc = ps3_lv2_queue_create(lv2, &id, &request.attr, request.ipc_key, request.size);
    if (rc == PS3_LV2_EINVAL || rc == PS3_LV2_EAGAIN || rc == 0x80010014u /* EEXIST: IPC key taken */) { out.status = Commit::GuestError; out.cell = rc; return out; }
    if (rc == PS3_LV2_UNSUPPORTED) { out.status = Commit::Unsupported; return out; }
    if (rc != PS3_LV2_OK || !id) { out.status = Commit::DomainFailure; return out; }
    out.queue_id = id;
    const unsigned char encoded[OUTPUT_BYTES] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    if (memory.write(memory.context, request.output, encoded, sizeof encoded) == PS3_GUEST_MEMORY_OK) {
        out.status = Commit::Created;
        return out;
    }
    // Real rollback through the owning domain; if it refuses, ownership is retained and reported, never leaked.
    out.status = ps3_lv2_queue_destroy(lv2, id) == PS3_LV2_OK ? Commit::StoreFailed : Commit::RetainedOwnership;
    return out;
}
}
#endif
