#pragma once

// Presentation of captured SPURS synchronization diagnostics; this adapter never queries a registry.
#include "startup_execution.h"
#include <cstdio>

#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
extern "C++" {
/* One bounded diagnostic line (no newline) for a record / the totals; always NUL terminated and truncated to `size`.
 * Returns the untruncated length (snprintf contract). Never touches a registry. */
static inline const char *spurs_sync_operation_name(StartupSpursSyncOperation operation)
{
    return operation == StartupSpursSyncOperation::LwmutexCreate ? "lwmutex_create" :
           operation == StartupSpursSyncOperation::LwcondCreate ? "lwcond_create" : "none";
}
static inline void spurs_sync_snapshot_text(char *text, size_t size, const char *prefix, const StartupSpursSyncRegistrySnapshot &s)
{
    std::snprintf(text, size, "%s_taken=%d %s_capacity=%zu %s_active=%zu %s_reusable=%zu %s_exhausted_generation=%zu %s_overlap_evaluated=%d "
        "%s_overlapping=%zu %s_next_queue_id=%u %s_queue_id_available=%d %s_binding_count=%zu %s_binding_capacity=%zu",
        prefix, int(s.taken), prefix, s.object_capacity, prefix, s.active_objects, prefix, s.reusable_objects, prefix, s.exhausted_generation_objects,
        prefix, int(s.overlap_evaluated), prefix, s.overlapping_objects, prefix, unsigned(s.next_queue_id), prefix, int(s.queue_id_available),
        prefix, s.condition_binding_count, prefix, s.condition_binding_capacity);
}
inline size_t startup_spurs_sync_create_format(char *buffer, size_t size, const char *label, const StartupSpursSyncCreateRecord &record)
{
    char mutexes[384], conditions[384];
    spurs_sync_snapshot_text(mutexes, sizeof mutexes, "mtx", record.mutexes);
    spurs_sync_snapshot_text(conditions, sizeof conditions, "cnd", record.conditions);
    if (!buffer) size = 0;
    const int length = std::snprintf(buffer, size, "STARTUP_SPURS_SYNC_CREATE slot=%s seq=%u operation=%s address=0x%08x mutex_address=0x%08x "
        "protocol=%u recursive=%u call_performed=%d native_status=%d memory_status=%d nested_mutex_status=%d mapped_rc=0x%08x publications=%zu "
        "%s %s snapshot_atomic=false gameplay_claim=false",
        label ? label : "?", unsigned(record.sequence), spurs_sync_operation_name(record.operation), unsigned(record.address),
        unsigned(record.mutex_address), unsigned(record.protocol), unsigned(record.recursive), int(record.real_call_performed),
        record.native_status, record.memory_status, record.nested_mutex_status, unsigned(record.mapped_rc), record.publication_count,
        mutexes, conditions);
    return length < 0 ? 0 : size_t(length);
}
inline size_t startup_spurs_sync_create_format_summary(char *buffer, size_t size, size_t calls, size_t retained, size_t dropped,
    bool overflow, bool has_last_failed)
{
    if (!buffer) size = 0;
    const int length = std::snprintf(buffer, size, "STARTUP_SPURS_SYNC_CREATE_SUMMARY calls=%zu retained=%zu dropped=%zu overflow=%d "
        "last_failed_present=%d capacity=%zu snapshot_atomic=false gameplay_claim=false",
        calls, retained, dropped, int(overflow), int(has_last_failed), size_t(STARTUP_SPURS_SYNC_CREATE_CAPACITY));
    return length < 0 ? 0 : size_t(length);
}
}   // extern "C++"
#endif
