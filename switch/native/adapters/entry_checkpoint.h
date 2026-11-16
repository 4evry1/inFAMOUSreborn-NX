#ifndef NATIVE_ENTRY_CHECKPOINT_H
#define NATIVE_ENTRY_CHECKPOINT_H
#include "ppu_recomp.h"
#include <stddef.h>
#include <stdint.h>

enum EntryStopReason { ENTRY_NOT_STOPPED, ENTRY_IMPORT_DISPATCH, ENTRY_UNSUPPORTED_ACCESS,
                       ENTRY_UNSUPPORTED_TARGET, ENTRY_UNSUPPORTED_IMPORT, ENTRY_RETURNED, ENTRY_TRACE_OVERFLOW };
struct EntrySpan { uint32_t address; uint64_t length; bool writable; };
struct EntryAccess { uint64_t address, value; unsigned bytes; bool write; };
struct EntryCapture {
    ppu_context context;
    uintptr_t base;
    EntrySpan spans[4];
    EntryAccess accesses[32];
    size_t access_count;
    EntryStopReason reason;
    uint32_t nid, target;
    uint64_t invalid_address;
};
/* Worker owns no resources or nontrivial automatic objects. Caller retains all
 * memory and capture storage until threadWaitForExit and threadClose complete.
 * threadExit traps dispatch BEFORE handler return; root completes the real HLE. */
extern "C" void native_entry_checkpoint_worker(void *capture);
extern "C" void native_entry_unknown_import_worker(void *capture);
extern "C" void native_entry_unreached_target_worker(void *capture);
#endif
