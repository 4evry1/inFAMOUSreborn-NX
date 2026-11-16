#ifndef INFAMOUS_STARTUP_OBSERVATION_H
#define INFAMOUS_STARTUP_OBSERVATION_H
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace infamous::diagnostics {
constexpr std::uint64_t NO_PARENT_ID = std::numeric_limits<std::uint64_t>::max();
struct ImportRecord { std::uint64_t sequence; std::uint32_t nid; };
struct DispatchEntry { std::uint64_t captured_ctr, lr, sp; std::uint32_t depth; };
struct DispatchResult { std::uint32_t status; bool lookup_attempted; std::uint32_t lookup_status; };
struct PendingDispatch { std::uint64_t call_id, parent_id; DispatchEntry entry; bool occupied; };
struct CompletedDispatch {
    std::uint64_t sequence, call_id, parent_id;
    DispatchEntry entry;
    std::uint32_t depth_after;
    DispatchResult result;
};
enum class DispatchStorage : std::uint8_t { Pending, ImmediateNonInvoking };
class StartupObservation;
// Copies remain valid only within the originating live observer lifetime.
// Retirement makes every copy stale; tickets must not outlive their owner.
class DispatchTicket {
public:
    DispatchTicket() = default;
private:
    friend class StartupObservation;
    const StartupObservation *owner_ = nullptr;
    std::uint64_t call_id_ = 0, parent_id_ = NO_PARENT_ID;
    DispatchEntry entry_{};
    DispatchStorage mode_ = DispatchStorage::Pending;
};
static_assert(std::is_trivially_copyable<DispatchTicket>::value);
static_assert(std::is_trivially_destructible<DispatchTicket>::value);
static_assert(sizeof(DispatchTicket) <= 64, "Tickets must not carry CPU or ring state");
struct SuffixRange {
    std::uint64_t total, retained_begin, retained_end, overwritten_end;
    std::size_t capacity;
};
struct ObservationReport {
    bool configured;
    SuffixRange imports, completed;
    std::uint64_t dispatch_started;
    std::size_t pending_count;
    bool immediate_outstanding;
    std::size_t pending_capacity;
};
// Production headroom guard; failure leaves output unchanged.
bool startup_observation_checked_increment(std::uint64_t current, std::uint64_t &next) noexcept;
// One exclusive producer, no concurrent readers. Borrowed typed live arrays and
// this object stay stable through confirmed worker join. There is no reset,
// destructor cleanup, provider callback or inferred OS lifecycle here.
class StartupObservation {
public:
    StartupObservation() = default;
    StartupObservation(const StartupObservation &) = delete;
    StartupObservation &operator=(const StartupObservation &) = delete;
    StartupObservation(StartupObservation &&) = delete;
    StartupObservation &operator=(StartupObservation &&) = delete;
    bool configure(ImportRecord *imports, std::size_t import_capacity,
                   CompletedDispatch *completed, std::size_t completed_capacity,
                   PendingDispatch *pending, std::size_t pending_capacity) noexcept;
    bool valid_for_admission() const noexcept;
    bool try_record_import(std::uint32_t nid) noexcept;
    bool try_begin_dispatch(DispatchEntry entry, DispatchStorage storage, DispatchTicket &out) noexcept;
    void finish_dispatch(const DispatchTicket &ticket, std::uint32_t depth_after, DispatchResult result) noexcept;
    ObservationReport report() const noexcept;
    bool read_import(std::uint64_t sequence, ImportRecord &out) const noexcept;
    bool read_completed(std::uint64_t sequence, CompletedDispatch &out) const noexcept;
    bool read_pending(std::size_t depth_index, PendingDispatch &out) const noexcept;
private:
    bool read_output_valid(const void *output, std::size_t bytes) const noexcept;
    ImportRecord *imports_ = nullptr;
    CompletedDispatch *completed_ = nullptr;
    PendingDispatch *pending_ = nullptr;
    std::size_t import_capacity_ = 0, completed_capacity_ = 0, pending_capacity_ = 0, pending_count_ = 0;
    std::uint64_t import_total_ = 0, completed_total_ = 0, started_total_ = 0, immediate_id_ = 0;
    std::uint32_t immediate_depth_ = 0;
    bool configured_ = false, immediate_outstanding_ = false;
};
}
#endif
