#include "ppu_memory_trace.h"
#include <cstring>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace {
static_assert(std::is_trivially_copyable<PpuMemoryTraceEvent>::value,
              "Publication copies must not allocate or invoke event callbacks");

bool strict_space(const PpuMemoryTraceEvent *events, size_t capacity,
                  size_t count, size_t requested) noexcept
{
    return events && count <= capacity && requested <= capacity - count;
}
}

bool ppu_memory_trace_available(const PpuMemoryTraceJournal &journal)
{
    return strict_space(journal.events, journal.capacity, journal.count, 1);
}
bool ppu_memory_trace_append(PpuMemoryTraceJournal &journal, const PpuMemoryTraceEvent &event)
{
    if (!ppu_memory_trace_available(journal)) return false;
    journal.events[journal.count++] = event;
    return true;
}
bool ppu_memory_event_equal(const PpuMemoryTraceEvent &left, const PpuMemoryTraceEvent &right)
{
    return left.origin == right.origin && left.operation == right.operation &&
        left.address == right.address && left.length == right.length &&
        left.value == right.value && left.increment == right.increment &&
        left.stored == right.stored && left.byte_transfer == right.byte_transfer &&
        left.status == right.status && std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}
bool ppu_memory_trace_equal(const PpuMemoryTraceEvent *left, size_t left_count,
                            const PpuMemoryTraceEvent *right, size_t right_count)
{
    if (left_count != right_count || (left_count && (!left || !right))) return false;
    for (size_t index = 0; index < left_count; ++index)
        if (!ppu_memory_event_equal(left[index], right[index])) return false;
    return true;
}
PpuMemoryTraceEvent ppu_memory_scalar_event(PpuMemoryTraceOrigin origin, bool write,
    uint32_t address, size_t length, uint64_t value)
{
    PpuMemoryTraceEvent event{};
    event.origin = origin;
    event.operation = write ? PpuMemoryTraceOperation::Write : PpuMemoryTraceOperation::Read;
    event.address = address;
    event.length = length;
    event.value = value;
    return event;
}
PpuMemoryTraceEvent ppu_memory_bytes_event(PpuMemoryTraceOrigin origin, bool write,
    uint32_t address, const unsigned char *bytes, size_t length)
{
    if (!bytes || length == 0 || length > PPU_MEMORY_TRACE_BYTE_CAPACITY)
        std::abort();
    PpuMemoryTraceEvent event{};
    event.origin = origin;
    event.operation = write ? PpuMemoryTraceOperation::Write : PpuMemoryTraceOperation::Read;
    event.address = address;
    event.length = length;
    event.byte_transfer = true;
    std::memcpy(event.bytes, bytes, length);
    return event;
}


bool PpuMemoryTraceRecorder::configure(PpuMemoryTraceEvent *events, size_t capacity,
    PpuMemoryTraceRetention retention, uint64_t first_sequence) noexcept
{
    if (batch_active_ || !events || !capacity ||
        (retention != PpuMemoryTraceRetention::Strict && retention != PpuMemoryTraceRetention::Suffix) ||
        capacity > std::numeric_limits<size_t>::max() / sizeof(*events)) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(events);
    const size_t bytes = capacity * sizeof(*events);
    if (address % alignof(PpuMemoryTraceEvent) || bytes > std::numeric_limits<uintptr_t>::max() - address)
        return false;
    events_ = events;
    capacity_ = capacity;
    retained_ = next_slot_ = remaining_ = 0;
    first_sequence_ = next_sequence_ = first_sequence;
    retention_ = retention;
    configured_ = true;
    return true;
}

bool PpuMemoryTraceRecorder::can_begin(size_t maximum_publications) const noexcept
{
    if (!configured_ || batch_active_ || !maximum_publications ||
        maximum_publications > std::numeric_limits<uint64_t>::max() - next_sequence_)
        return false;
    if (retention_ == PpuMemoryTraceRetention::Strict &&
        !strict_space(events_, capacity_, retained_, maximum_publications)) return false;
    return true;
}

bool PpuMemoryTraceRecorder::begin(size_t maximum_publications) noexcept
{
    if (!can_begin(maximum_publications)) return false;
    remaining_ = maximum_publications;
    batch_active_ = true;
    return true;
}

void PpuMemoryTraceRecorder::publish(const PpuMemoryTraceEvent &event) noexcept
{
    if (!configured_ || !batch_active_ || !remaining_) std::abort();
    events_[next_slot_] = event;
    if (retained_ < capacity_) ++retained_;
    next_slot_ = next_slot_ + 1 == capacity_ ? 0 : next_slot_ + 1;
    ++next_sequence_;
    --remaining_;
}

void PpuMemoryTraceRecorder::finish() noexcept
{
    remaining_ = 0;
    batch_active_ = false;
}

bool PpuMemoryTraceRecorder::read(size_t chronological_index, PpuMemoryTraceSample &output) const noexcept
{
    if (!configured_ || chronological_index >= retained_) return false;
    const size_t oldest_slot = retained_ == capacity_ ? next_slot_ : 0;
    const size_t index = chronological_index >= capacity_ - oldest_slot ?
        chronological_index - (capacity_ - oldest_slot) : oldest_slot + chronological_index;
    PpuMemoryTraceSample sample{};
    sample.sequence = next_sequence_ - retained_ + chronological_index;
    sample.event = events_[index];
    output = sample;
    return true;
}

PpuMemoryTraceReport PpuMemoryTraceRecorder::report() const noexcept
{
    PpuMemoryTraceReport result{};
    result.configured = configured_;
    result.capacity = capacity_;
    result.retained_count = retained_;
    result.published_count = next_sequence_ - first_sequence_;
    result.retained_begin = result.overwritten_end = next_sequence_ - retained_;
    result.retained_end = next_sequence_;
    result.overwritten_begin = first_sequence_;
    result.batch_active = batch_active_;
    result.remaining_publications = remaining_;
    return result;
}
