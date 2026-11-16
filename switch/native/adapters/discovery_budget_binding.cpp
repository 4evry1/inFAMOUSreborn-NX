#include "discovery_budget_binding.h"
#include <limits>
using namespace infamous::discovery;

bool discovery_budget_sample_start_ns(const ps3_clock &clock, std::uint64_t &output) noexcept
{
    if (!clock.now_microseconds) return false;
    std::uint64_t microseconds = 0;
    if (clock.now_microseconds(clock.context, &microseconds) != PS3_CLOCK_OK ||
        microseconds > std::numeric_limits<std::uint64_t>::max() / 1000) return false;
    output = microseconds * 1000;
    return true;
}
DiscoveryBudgetBinding::DiscoveryBudgetBinding(DiscoveryBudget &policy, const ps3_clock &clock) noexcept
    : policy_(policy), clock_(clock) {}
bool DiscoveryBudgetBinding::valid_for_admission() const noexcept
{
    const auto state = policy_.snapshot();
    return clock_.now_microseconds && state.stop == StopReason::None &&
        !state.generated && !state.hle && !state.indirect && state.last_ns == state.start_ns;
}
Decision DiscoveryBudgetBinding::admit(Category category) noexcept
{
    const auto stopped = reason();
    if (stopped != StopReason::None) return {false, stopped};
    std::uint64_t now = 0;
    const bool valid = discovery_budget_sample_start_ns(clock_, now);
    return policy_.admit(category, {now, valid, false});
}
StopReason DiscoveryBudgetBinding::poll() noexcept
{
    const auto stopped = reason();
    if (stopped != StopReason::None) return stopped;
    std::uint64_t now = 0;
    const bool valid = discovery_budget_sample_start_ns(clock_, now);
    return policy_.poll({now, valid, false});
}
StopReason DiscoveryBudgetBinding::reason() const noexcept { return policy_.snapshot().stop; }
