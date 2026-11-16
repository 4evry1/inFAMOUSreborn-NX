#ifndef NATIVE_PPU_MEMORY_TRACE_H
#define NATIVE_PPU_MEMORY_TRACE_H
#include "guest_space.h"
#include <cstddef>
#include <cstdint>

enum class PpuMemoryTraceOrigin { Generated, Hle };
enum class PpuMemoryTraceOperation { Read, Write, LoadReserve, StoreConditional, Add16, Zero128 };
constexpr size_t PPU_MEMORY_TRACE_BYTE_CAPACITY = 24;
struct PpuMemoryTraceEvent {
    PpuMemoryTraceOrigin origin = PpuMemoryTraceOrigin::Generated;
    PpuMemoryTraceOperation operation = PpuMemoryTraceOperation::Read;
    uint32_t address = 0;
    size_t length = 0;
    uint64_t value = 0;
    int32_t increment = 0;
    bool stored = false;
    bool byte_transfer = false;
    unsigned char bytes[PPU_MEMORY_TRACE_BYTE_CAPACITY]{};
    ps3_guest_memory_status status = PS3_GUEST_MEMORY_OK;
};
/* One CPU owner is the sole producer. HLE publications and generated accesses
 * share this journal, preserving cross-class order. Composition keeps storage
 * alive through join. The observer owns no provider, callback or global state.
 * Producers preflight capacity before mutation; no reentry or concurrent append. */
struct PpuMemoryTraceJournal {
    PpuMemoryTraceEvent *events;
    size_t capacity;
    size_t count;
};
bool ppu_memory_trace_available(const PpuMemoryTraceJournal &journal);
bool ppu_memory_trace_append(PpuMemoryTraceJournal &journal, const PpuMemoryTraceEvent &event);
bool ppu_memory_event_equal(const PpuMemoryTraceEvent &, const PpuMemoryTraceEvent &);
bool ppu_memory_trace_equal(const PpuMemoryTraceEvent *, size_t, const PpuMemoryTraceEvent *, size_t);
PpuMemoryTraceEvent ppu_memory_scalar_event(PpuMemoryTraceOrigin, bool write,
    uint32_t address, size_t length, uint64_t value);
/* Caller validates the actual publication before mutation. Input is a readable,
 * nonaliasing 1..capacity byte span; invalid shape is an internal misuse. */
PpuMemoryTraceEvent ppu_memory_bytes_event(PpuMemoryTraceOrigin origin, bool write,
    uint32_t address, const unsigned char *bytes, size_t length);

enum class PpuMemoryTraceRetention { Strict, Suffix };
struct PpuMemoryTraceSample {
    uint64_t sequence = 0;
    PpuMemoryTraceEvent event{};
};
struct PpuMemoryTraceReport {
    bool configured = false;
    size_t capacity = 0;
    size_t retained_count = 0;
    uint64_t published_count = 0;
    uint64_t retained_begin = 0, retained_end = 0;
    uint64_t overwritten_begin = 0, overwritten_end = 0;
    bool batch_active = false;
    size_t remaining_publications = 0;
};

/* Borrowed storage must contain capacity live Event objects, disjoint from this
 * recorder, and remain alive for publication/readback (through worker join).
 * One exclusive producer, no reentry or concurrent reads. A joined owner may
 * inspect a still-open batch: only successful publications appear in read/report.
 * The legacy aggregate/API above remains the strict retention compatibility path. */
class PpuMemoryTraceRecorder {
public:
    PpuMemoryTraceRecorder() noexcept = default;
    PpuMemoryTraceRecorder(const PpuMemoryTraceRecorder &) = delete;
    PpuMemoryTraceRecorder &operator=(const PpuMemoryTraceRecorder &) = delete;

    // Configure only while quiescent with no open batch; failures change nothing.
    // first_sequence labels a resumed observation; the empty default starts at 0.
    bool configure(PpuMemoryTraceEvent *events, size_t capacity,
                   PpuMemoryTraceRetention retention, uint64_t first_sequence = 0) noexcept;
    // Positive maximum publication count; failure preserves state and any credit.
    // Checks complete sequence headroom, including pairs, before guest mutation.
    // Pure preflight: identical validation, never reserves credit or changes events.
    bool can_begin(size_t maximum_publications) const noexcept;
    bool begin(size_t maximum_publications) noexcept;
    // Requires a successful begin and unused credit. Misuse aborts in all builds.
    // Valid publication is infallible: no allocation, callbacks, cancellation or
    // capacity reservation. Sequence advances only for a published event.
    void publish(const PpuMemoryTraceEvent &event) noexcept;
    // Explicit after success/partial cancellation or after the producer has joined.
    // Idempotent; discards unused credit, preserving successful events and sequence.
    // threadExit may skip destructors; later wiring must finish before stopping or
    // let the joined owner finish. No destructor performs implicit publication work.
    void finish() noexcept;
    bool read(size_t chronological_index, PpuMemoryTraceSample &output) const noexcept;
    PpuMemoryTraceReport report() const noexcept;

private:
    PpuMemoryTraceEvent *events_ = nullptr;
    size_t capacity_ = 0, retained_ = 0, next_slot_ = 0;
    uint64_t first_sequence_ = 0, next_sequence_ = 0;
    PpuMemoryTraceRetention retention_ = PpuMemoryTraceRetention::Strict;
    bool configured_ = false, batch_active_ = false;
    size_t remaining_ = 0;
};
#endif
