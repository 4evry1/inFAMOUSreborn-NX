#include "entry_checkpoint.h"
#include <switch.h>
#include <cstdlib>
#undef vm_read32
#undef vm_read64
#undef vm_write32
#undef vm_write64

namespace {
// Raw guest data and control-flow addresses only, never injected service callbacks.
thread_local EntryCapture *active_capture = nullptr;
[[noreturn]] void stop(EntryStopReason reason, uint32_t target = 0)
{
    if (!active_capture) std::abort();
    active_capture->reason = reason;
    active_capture->target = target;
    active_capture = nullptr;
    threadExit();
}
uint8_t *checked_address(uint64_t address, unsigned bytes, bool write)
{
    if (!active_capture) std::abort();
    bool permitted = false;
    if (address <= UINT32_MAX && bytes <= UINT64_C(0x100000000) - address) {
        for (const EntrySpan &span : active_capture->spans) {
            if (address >= span.address && address - span.address <= span.length &&
                bytes <= span.length - (address - span.address) && (!write || span.writable)) {
                permitted = true;
            }
        }
    }
    if (!permitted) {
        active_capture->invalid_address = address;
        stop(ENTRY_UNSUPPORTED_ACCESS);
    }
    const size_t trace_capacity = sizeof(active_capture->accesses) / sizeof(active_capture->accesses[0]);
    if (active_capture->access_count >= trace_capacity) stop(ENTRY_TRACE_OVERFLOW);
    return reinterpret_cast<uint8_t *>(active_capture->base + address);
}
uint64_t read_guest(uint64_t address, unsigned bytes)
{
    const uint8_t *source = checked_address(address, bytes, false);
    uint64_t value = 0;
    for (unsigned index = 0; index < bytes; ++index) value = (value << 8) | source[index];
    active_capture->accesses[active_capture->access_count++] = {address, value, bytes, false};
    return value;
}
void write_guest(uint64_t address, uint64_t value, unsigned bytes)
{
    uint8_t *destination = checked_address(address, bytes, true);
    for (unsigned index = 0; index < bytes; ++index) {
        destination[index] = uint8_t(value >> ((bytes - index - 1) * 8));
    }
    active_capture->accesses[active_capture->access_count++] = {address, value, bytes, true};
}
}
extern "C" {
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void *) = nullptr;
uint32_t vm_read32(uint64_t address) { return uint32_t(read_guest(address, 4)); }
uint64_t vm_read64(uint64_t address) { return read_guest(address, 8); }
void vm_write32(uint64_t address, uint32_t value) { write_guest(address, value, 4); }
void vm_write64(uint64_t address, uint64_t value) { write_guest(address, value, 8); }
void ps3_hle_call(unsigned int nid, ppu_context *context)
{
    if (!active_capture || context != &active_capture->context) std::abort();
    active_capture->nid = nid;
    g_trampoline_fn = nullptr;
    if (nid != 0x744680a2) stop(ENTRY_UNSUPPORTED_IMPORT);
    stop(ENTRY_IMPORT_DISPATCH);
}
void native_entry_unknown_import_worker(void *argument)
{
    active_capture = static_cast<EntryCapture *>(argument);
    g_trampoline_fn = nullptr;
    ps3_hle_call(0xffffffff, &active_capture->context);
    stop(ENTRY_RETURNED);
}
void native_entry_unreached_target_worker(void *argument)
{
    active_capture = static_cast<EntryCapture *>(argument);
    g_trampoline_fn = nullptr;
    func_00456420(&active_capture->context);
    stop(ENTRY_RETURNED);
}
void native_entry_checkpoint_worker(void *argument)
{
    active_capture = static_cast<EntryCapture *>(argument);
    g_trampoline_fn = nullptr;
    func_00010230(&active_capture->context);
    stop(ENTRY_RETURNED);
}
}

void func_00010200(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x00010200);
}


void func_00193AE8(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x00193AE8);
}

void func_00446FF0(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x00446FF0);
}

void func_0044F000(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x0044F000);
}

void func_004529A8(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x004529A8);
}

void func_00452DF8(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x00452DF8);
}

void func_00456420(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x00456420);
}

void func_0055D9C4(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x0055D9C4);
}

void func_0055DA24(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x0055DA24);
}

void func_0055DA84(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x0055DA84);
}

void func_0055DAA4(ppu_context *)
{
    stop(ENTRY_UNSUPPORTED_TARGET, 0x0055DAA4);
}
