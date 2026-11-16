#include "startup_observation.h"
#include <cstdlib>

namespace infamous::diagnostics {
namespace {
struct Extent { std::uintptr_t begin, end; };
template<class T> bool extent(T *buffer, std::size_t count, Extent &out) noexcept
{
    if (!buffer || !count || count > std::numeric_limits<std::size_t>::max() / sizeof(T)) return false;
    const auto capacity64 = static_cast<std::uint64_t>(count);
    if (static_cast<std::size_t>(capacity64) != count) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(buffer);
    const std::size_t bytes = count * sizeof(T);
    if (begin % alignof(T) || bytes > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    out = {begin, begin + bytes};
    return true;
}
bool overlaps(Extent a, Extent b) noexcept { return a.begin < b.end && b.begin < a.end; }
void require(bool condition) noexcept { if (!condition) std::abort(); }
bool same_entry(DispatchEntry a, DispatchEntry b) noexcept
{
    return a.captured_ctr == b.captured_ctr && a.lr == b.lr && a.sp == b.sp && a.depth == b.depth;
}
SuffixRange suffix(std::uint64_t total, std::size_t capacity) noexcept
{
    const auto width = static_cast<std::uint64_t>(capacity);
    const auto begin = total > width ? total - width : 0;
    return {total, begin, total, begin, capacity};
}
}
bool startup_observation_checked_increment(std::uint64_t current, std::uint64_t &next) noexcept
{
    if (current == std::numeric_limits<std::uint64_t>::max()) return false;
    next = current + 1;
    return true;
}
bool StartupObservation::configure(ImportRecord *imports, std::size_t import_capacity,
    CompletedDispatch *completed, std::size_t completed_capacity,
    PendingDispatch *pending, std::size_t pending_capacity) noexcept
{
    if (configured_ || pending_capacity > UINT32_MAX) return false;
    Extent import_extent{}, completed_extent{}, pending_extent{}, self_extent{};
    if (!extent(imports, import_capacity, import_extent) ||
        !extent(completed, completed_capacity, completed_extent) ||
        !extent(pending, pending_capacity, pending_extent) || !extent(this, 1, self_extent)) return false;
    if (overlaps(import_extent, completed_extent) || overlaps(import_extent, pending_extent) ||
        overlaps(completed_extent, pending_extent) || overlaps(import_extent, self_extent) ||
        overlaps(completed_extent, self_extent) || overlaps(pending_extent, self_extent)) return false;
    for (std::size_t index = 0; index < pending_capacity; ++index) pending[index].occupied = false;
    imports_ = imports; import_capacity_ = import_capacity;
    completed_ = completed; completed_capacity_ = completed_capacity;
    pending_ = pending; pending_capacity_ = pending_capacity;
    configured_ = true;
    return true;
}
bool StartupObservation::valid_for_admission() const noexcept
{
    if (!configured_ || import_total_ || completed_total_ || started_total_ || pending_count_ || immediate_outstanding_)
        return false;
    for (std::size_t index = 0; index < pending_capacity_; ++index) if (pending_[index].occupied) return false;
    return true;
}
bool StartupObservation::try_record_import(std::uint32_t nid) noexcept
{
    require(configured_);
    std::uint64_t next = 0;
    if (!startup_observation_checked_increment(import_total_, next)) return false;
    imports_[static_cast<std::size_t>(import_total_ % import_capacity_)] = {import_total_, nid};
    import_total_ = next;
    return true;
}
bool StartupObservation::try_begin_dispatch(DispatchEntry entry, DispatchStorage storage, DispatchTicket &out) noexcept
{
    require(configured_);
    require(storage == DispatchStorage::Pending || storage == DispatchStorage::ImmediateNonInvoking);
    require(!immediate_outstanding_);
    if (storage == DispatchStorage::Pending) {
        require(entry.depth < pending_capacity_ && pending_count_ == entry.depth);
        require(!pending_[entry.depth].occupied);
        if (entry.depth) require(pending_[entry.depth - 1].occupied);
    }
    std::uint64_t next = 0;
    if (!startup_observation_checked_increment(started_total_, next)) return false;
    DispatchTicket ticket;
    ticket.owner_ = this; ticket.call_id_ = started_total_;
    ticket.entry_ = entry; ticket.mode_ = storage;
    if (entry.depth && entry.depth - 1 < pending_capacity_ && pending_[entry.depth - 1].occupied)
        ticket.parent_id_ = pending_[entry.depth - 1].call_id;
    if (storage == DispatchStorage::Pending) {
        pending_[entry.depth] = {ticket.call_id_, ticket.parent_id_, entry, true};
        ++pending_count_;
    } else {
        immediate_id_ = ticket.call_id_;
        immediate_depth_ = entry.depth;
        immediate_outstanding_ = true;
    }
    started_total_ = next;
    out = ticket;
    return true;
}
void StartupObservation::finish_dispatch(const DispatchTicket &ticket, std::uint32_t depth_after,
    DispatchResult result) noexcept
{
    require(configured_ && ticket.owner_ == this && depth_after == ticket.entry_.depth);
    if (ticket.mode_ == DispatchStorage::Pending) {
        require(!immediate_outstanding_ && ticket.entry_.depth < pending_capacity_);
        require(pending_count_ == std::size_t(ticket.entry_.depth) + 1);
        const auto &slot = pending_[ticket.entry_.depth];
        require(slot.occupied && slot.call_id == ticket.call_id_ && slot.parent_id == ticket.parent_id_ &&
            same_entry(slot.entry, ticket.entry_));
    } else {
        require(ticket.mode_ == DispatchStorage::ImmediateNonInvoking);
        require(immediate_outstanding_ && immediate_id_ == ticket.call_id_ && immediate_depth_ == ticket.entry_.depth);
    }
    require(completed_total_ < started_total_);
    std::uint64_t next = 0;
    require(startup_observation_checked_increment(completed_total_, next));
    completed_[static_cast<std::size_t>(completed_total_ % completed_capacity_)] = {
        completed_total_, ticket.call_id_, ticket.parent_id_, ticket.entry_, depth_after, result};
    completed_total_ = next;
    if (ticket.mode_ == DispatchStorage::Pending) {
        pending_[ticket.entry_.depth].occupied = false;
        --pending_count_;
    } else {
        immediate_outstanding_ = false;
    }
}
ObservationReport StartupObservation::report() const noexcept
{
    return {configured_, suffix(import_total_, import_capacity_), suffix(completed_total_, completed_capacity_),
        started_total_, pending_count_, immediate_outstanding_, pending_capacity_};
}
bool StartupObservation::read_output_valid(const void *output, std::size_t bytes) const noexcept
{
    Extent destination{}, imports{}, completed{}, pending{}, self{};
    if (!extent(static_cast<const unsigned char *>(output), bytes, destination) ||
        !extent(imports_, import_capacity_, imports) || !extent(completed_, completed_capacity_, completed) ||
        !extent(pending_, pending_capacity_, pending) || !extent(this, 1, self)) return false;
    return !overlaps(destination, imports) && !overlaps(destination, completed) &&
        !overlaps(destination, pending) && !overlaps(destination, self);
}
bool StartupObservation::read_import(std::uint64_t sequence, ImportRecord &out) const noexcept
{
    if (!configured_ || !read_output_valid(&out, sizeof(out))) return false;
    const auto range = suffix(import_total_, import_capacity_);
    if (sequence < range.retained_begin || sequence >= range.retained_end) return false;
    out = imports_[static_cast<std::size_t>(sequence % import_capacity_)];
    return true;
}
bool StartupObservation::read_completed(std::uint64_t sequence, CompletedDispatch &out) const noexcept
{
    if (!configured_ || !read_output_valid(&out, sizeof(out))) return false;
    const auto range = suffix(completed_total_, completed_capacity_);
    if (sequence < range.retained_begin || sequence >= range.retained_end) return false;
    out = completed_[static_cast<std::size_t>(sequence % completed_capacity_)];
    return true;
}
bool StartupObservation::read_pending(std::size_t depth_index, PendingDispatch &out) const noexcept
{
    if (!configured_ || !read_output_valid(&out, sizeof(out)) ||
        depth_index >= pending_capacity_ || !pending_[depth_index].occupied) return false;
    out = pending_[depth_index];
    return true;
}
}
