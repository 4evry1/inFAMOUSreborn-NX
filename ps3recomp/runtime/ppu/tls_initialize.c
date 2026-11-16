#include "tls_initialize.h"

enum {
    PS3_TLS_THREAD_POINTER_BIAS = 0x7000,
    PS3_TLS_EXTRA_PAGE_BYTES = 0x1000
};

static Ps3TlsReport tls_report(Ps3TlsStatus status) {
    Ps3TlsReport report = {status, status, PS3_GUEST_MEMORY_OK, PS3_GUEST_MEMORY_OK};
    return report;
}

/* Shared layout validation (range, then space, then overlap) for the range
 * [next,end) that receives the TLS block at next; *size is the required extent. */
static Ps3TlsStatus tls_layout(uint64_t next, uint64_t end, const Ps3TlsTemplate *tls_template,
                               uint64_t *size) {
    uint64_t overhead = PS3_TLS_THREAD_POINTER_BIAS + PS3_TLS_EXTRA_PAGE_BYTES;
    if (next > end || end > PS3_GUEST_ADDRESS_BYTES ||
        next % PS3_GUEST_PAGE_BYTES || end % PS3_GUEST_PAGE_BYTES ||
        tls_template->file_size > tls_template->memory_size ||
        tls_template->memory_size > PS3_GUEST_ADDRESS_BYTES - overhead ||
        tls_template->file_size > PS3_GUEST_ADDRESS_BYTES - tls_template->source ||
        (tls_template->file_size && !tls_template->source)) {
        return PS3_TLS_INVALID_RANGE;
    }
    uint64_t required = (tls_template->memory_size + overhead + PS3_GUEST_PAGE_BYTES - 1)
                        & ~(PS3_GUEST_PAGE_BYTES - 1);
    if (required > end - next) return PS3_TLS_NO_SPACE;
    if (tls_template->file_size && tls_template->source < next + required &&
        next < (uint64_t)tls_template->source + tls_template->file_size) {
        return PS3_TLS_OVERLAP;
    }
    *size = required;
    return PS3_TLS_OK;
}

/* Shared byte initialization: copy file bytes in scratch chunks, then zero the tail. */
static ps3_guest_memory_status tls_write_bytes(const ps3_guest_memory *memory, uint32_t block,
                                               const Ps3TlsTemplate *tls_template,
                                               void *scratch, size_t scratch_size) {
    ps3_guest_memory_status status;
    for (uint64_t offset = 0; offset < tls_template->file_size;) {
        uint64_t remaining = tls_template->file_size - offset;
        size_t length = remaining < scratch_size ? (size_t)remaining : scratch_size;
        status = memory->read(memory->context, (uint32_t)(tls_template->source + offset),
                              scratch, length);
        if (status != PS3_GUEST_MEMORY_OK) return status;
        status = memory->write(memory->context, (uint32_t)(block + offset), scratch, length);
        if (status != PS3_GUEST_MEMORY_OK) return status;
        offset += length;
    }
    if (tls_template->memory_size > tls_template->file_size) {
        status = memory->zero(memory->context, (uint32_t)(block + tls_template->file_size),
                              tls_template->memory_size - tls_template->file_size);
        if (status != PS3_GUEST_MEMORY_OK) return status;
    }
    return PS3_GUEST_MEMORY_OK;
}

Ps3TlsReport ps3_tls_initialize(const ps3_guest_memory *memory, Ps3TlsArena *arena,
                               const Ps3TlsTemplate *tls_template,
                               void *scratch, size_t scratch_size,
                               Ps3TlsImage *output) {
    Ps3TlsReport report = tls_report(PS3_TLS_OK);
    if (!memory || !memory->commit || !memory->decommit || !memory->read ||
        !memory->write || !memory->zero || !arena || !tls_template || !scratch ||
        !scratch_size || !output) {
        return tls_report(PS3_TLS_ARGUMENT);
    }
    uint64_t size;
    Ps3TlsStatus layout = tls_layout(arena->next, arena->end, tls_template, &size);
    if (layout != PS3_TLS_OK) return tls_report(layout);
    uint64_t end = arena->next + size;
    uint32_t block = (uint32_t)arena->next;
    report.memory_status = memory->commit(memory->context, block, size);
    if (report.memory_status != PS3_GUEST_MEMORY_OK) {
        report.status = PS3_TLS_MEMORY_FAILURE;
        report.cause = PS3_TLS_MEMORY_FAILURE;
        return report;
    }
    report.memory_status = tls_write_bytes(memory, block, tls_template, scratch, scratch_size);
    if (report.memory_status != PS3_GUEST_MEMORY_OK) goto rollback;
    {
        Ps3TlsImage result = {block, (uint32_t)(block + PS3_TLS_THREAD_POINTER_BIAS), size};
        *output = result;
        arena->next = end;
        return report;
    }

rollback:
    report.status = PS3_TLS_MEMORY_FAILURE;
    report.cause = PS3_TLS_MEMORY_FAILURE;
    report.cleanup_status = memory->decommit(memory->context, block, size);
    if (report.cleanup_status != PS3_GUEST_MEMORY_OK) report.status = PS3_TLS_CLEANUP_FAILURE;
    return report;
}

Ps3TlsReport ps3_tls_initialize_allocated(const ps3_guest_memory *memory,
                                          uint32_t block, uint64_t capacity,
                                          const Ps3TlsTemplate *tls_template,
                                          void *scratch, size_t scratch_size,
                                          Ps3TlsImage *output) {
    /* commit/decommit are intentionally neither required nor ever called. */
    if (!memory || !memory->read || !memory->write || !memory->zero ||
        !tls_template || !scratch || !scratch_size || !output) {
        return tls_report(PS3_TLS_ARGUMENT);
    }
    /* A capacity running past 4 GiB (including u64 wrap) is an invalid range. */
    if (capacity > PS3_GUEST_ADDRESS_BYTES - block) return tls_report(PS3_TLS_INVALID_RANGE);
    uint64_t size;
    Ps3TlsStatus layout = tls_layout(block, (uint64_t)block + capacity, tls_template, &size);
    if (layout != PS3_TLS_OK) return tls_report(layout);
    Ps3TlsReport report = tls_report(PS3_TLS_OK);
    report.memory_status = tls_write_bytes(memory, block, tls_template, scratch, scratch_size);
    if (report.memory_status != PS3_GUEST_MEMORY_OK) {
        /* No rollback: the caller still owns the (possibly partially initialized) storage. */
        report.status = PS3_TLS_MEMORY_FAILURE;
        report.cause = PS3_TLS_MEMORY_FAILURE;
        return report;
    }
    Ps3TlsImage result = {block, (uint32_t)(block + PS3_TLS_THREAD_POINTER_BIAS), size};
    *output = result;
    return report;
}
