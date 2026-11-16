#include "discovery_budget.h"

namespace infamous::discovery {
DiscoveryBudget::DiscoveryBudget(const Limits &limits, std::uint64_t valid_start_ns) noexcept
    : limits_(limits), start_ns_(valid_start_ns), last_ns_(valid_start_ns) {}

Decision DiscoveryBudget::latch(StopReason reason) noexcept
{
    stop_ = reason;
    return {false, reason};
}

Decision DiscoveryBudget::admit(Category category, const Observation &observation) noexcept
{
    if (stop_ != StopReason::None) return {false, stop_};
    if (observation.cancel_requested) return latch(StopReason::Cancelled);
    std::uint64_t *count = nullptr;
    std::uint64_t limit = 0;
    StopReason exhausted = StopReason::None;
    switch (category) {
    case Category::Generated:
        count = &generated_; limit = limits_.generated;
        exhausted = StopReason::GeneratedExhausted;
        break;
    case Category::Hle:
        count = &hle_; limit = limits_.hle;
        exhausted = StopReason::HleExhausted;
        break;
    case Category::Indirect:
        count = &indirect_; limit = limits_.indirect;
        exhausted = StopReason::IndirectExhausted;
        break;
    }
    if (!count) return latch(StopReason::InvalidCategory);
    const auto failure = time_failure(observation);
    if (failure != StopReason::None) return latch(failure);
    const auto admission = checked_count_admission(*count, limit);
    if (!admission.admitted) return latch(exhausted);
    *count = admission.next;
    last_ns_ = observation.now_ns;
    return {true, StopReason::None};
}

StopReason DiscoveryBudget::time_failure(const Observation &observation) const noexcept
{
    if (observation.cancel_requested) return StopReason::Cancelled;
    if (!observation.clock_valid) return StopReason::InvalidClock;
    if (observation.now_ns < last_ns_) return StopReason::ClockRegressed;
    if (observation.now_ns - start_ns_ >= limits_.deadline_ns) return StopReason::Deadline;
    return StopReason::None;
}
StopReason DiscoveryBudget::poll(const Observation &observation) noexcept
{
    if (stop_ != StopReason::None) return stop_;
    const auto failure = time_failure(observation);
    if (failure != StopReason::None) return latch(failure).reason;
    last_ns_ = observation.now_ns;
    return StopReason::None;
}
Snapshot DiscoveryBudget::snapshot() const noexcept
{
    return {generated_, hle_, indirect_, start_ns_, last_ns_, stop_};
}
Limits DiscoveryBudget::limits() const noexcept { return limits_; }
bool operator==(const Snapshot &a, const Snapshot &b) noexcept
{
    return a.generated == b.generated && a.hle == b.hle && a.indirect == b.indirect &&
        a.start_ns == b.start_ns && a.last_ns == b.last_ns && a.stop == b.stop;
}
}
