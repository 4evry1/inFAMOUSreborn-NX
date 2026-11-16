#ifndef INFAMOUS_DISCOVERY_BUDGET_H
#define INFAMOUS_DISCOVERY_BUDGET_H
#include <cstdint>

namespace infamous::discovery {
enum class Category : std::uint8_t { Generated, Hle, Indirect };
enum class StopReason : std::uint8_t {
    None, Cancelled, InvalidCategory, InvalidClock, ClockRegressed, Deadline,
    GeneratedExhausted, HleExhausted, IndirectExhausted
};
struct Limits {
    std::uint64_t generated, hle, indirect, deadline_ns;
};
struct Observation {
    std::uint64_t now_ns;
    bool clock_valid;
    bool cancel_requested;
};
struct Decision { bool admitted; StopReason reason; };
struct Snapshot {
    std::uint64_t generated, hle, indirect, start_ns, last_ns;
    StopReason stop;
};
bool operator==(const Snapshot &, const Snapshot &) noexcept;
struct CountAdmission { bool admitted; std::uint64_t next; };
constexpr CountAdmission checked_count_admission(std::uint64_t count,
                                                std::uint64_t limit) noexcept
{
    return count < limit ? CountAdmission{true, count + 1} : CountAdmission{false, count};
}
// One producer; observations must be fresh monotonic nanoseconds. Cancellation
// and clock acquisition remain caller-owned. No modular clock wrap is accepted.
// Coordinator snapshots require confirmed join; no state may be restored.
class DiscoveryBudget {
public:
    DiscoveryBudget(const Limits &, std::uint64_t valid_start_ns) noexcept;
    DiscoveryBudget(const DiscoveryBudget &) = delete;
    DiscoveryBudget &operator=(const DiscoveryBudget &) = delete;
    DiscoveryBudget(DiscoveryBudget &&) = delete;
    DiscoveryBudget &operator=(DiscoveryBudget &&) = delete;
    Decision admit(Category, const Observation &) noexcept;
    StopReason poll(const Observation &) noexcept;
    Snapshot snapshot() const noexcept;
    Limits limits() const noexcept;
private:
    Decision latch(StopReason) noexcept;
    StopReason time_failure(const Observation &) const noexcept;
    Limits limits_;
    std::uint64_t generated_ = 0, hle_ = 0, indirect_ = 0;
    std::uint64_t start_ns_, last_ns_;
    StopReason stop_ = StopReason::None;
};
}
#endif
