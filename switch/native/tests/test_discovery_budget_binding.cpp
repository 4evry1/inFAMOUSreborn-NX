#include "discovery_budget_binding.h"
#include <cstdio>
#include <limits>
#include <type_traits>
using namespace infamous::discovery;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line=%d expression=%s\n", __LINE__, #x); } } while (0)
struct Clock {
    std::uint64_t sample = 1;
    unsigned calls = 0;
    bool fail = false;
    static ps3_clock_status read(void *context, std::uint64_t *output) {
        auto &self = *static_cast<Clock *>(context);
        ++self.calls;
        if (self.fail) return PS3_CLOCK_ARGUMENT;
        *output = self.sample;
        return PS3_CLOCK_OK;
    }
    ps3_clock port() { return {this, read}; }
};
static_assert(!std::is_copy_constructible<DiscoveryBudgetBinding>::value);
static_assert(!std::is_copy_assignable<DiscoveryBudgetBinding>::value);
static_assert(!std::is_move_constructible<DiscoveryBudgetBinding>::value);
static_assert(!std::is_move_assignable<DiscoveryBudgetBinding>::value);
static void sampling()
{
    Clock clock; auto port = clock.port();
    std::uint64_t output = 123;
    CHECK(discovery_budget_sample_start_ns(port, output));
    CHECK(output == 1000 && clock.calls == 1);
    clock.fail = true;
    CHECK(!discovery_budget_sample_start_ns(port, output)); CHECK(output == 1000);
    clock.fail = false;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    clock.sample = max / 1000;
    CHECK(discovery_budget_sample_start_ns(port, output)); CHECK(output == (max / 1000) * 1000);
    clock.sample = max / 1000 + 1;
    CHECK(!discovery_budget_sample_start_ns(port, output)); CHECK(output == (max / 1000) * 1000);
    ps3_clock missing{};
    CHECK(!discovery_budget_sample_start_ns(missing, output)); CHECK(output == (max / 1000) * 1000);
}
static void admission_and_poll()
{
    Clock clock; auto port = clock.port();
    DiscoveryBudget policy({1, 2, 3, 10000}, 1000);
    DiscoveryBudgetBinding binding(policy, port);
    CHECK(binding.valid_for_admission() && clock.calls == 0);
    CHECK(binding.poll() == StopReason::None && clock.calls == 1);
    CHECK(binding.valid_for_admission() && clock.calls == 1);
    clock.sample = 2;
    CHECK(binding.poll() == StopReason::None && clock.calls == 2);
    CHECK(!binding.valid_for_admission() && clock.calls == 2);
    auto decision = binding.admit(Category::Generated);
    CHECK(decision.admitted && decision.reason == StopReason::None && clock.calls == 3);
    const auto before = policy.snapshot();
    clock.sample = 3;
    decision = binding.admit(Category::Generated);
    CHECK(!decision.admitted && decision.reason == StopReason::GeneratedExhausted && clock.calls == 4);
    const auto after = policy.snapshot();
    CHECK(after.generated == 1 && after.hle == 0 && after.indirect == 0 && after.last_ns == before.last_ns);
    CHECK(binding.poll() == StopReason::GeneratedExhausted);
    CHECK(binding.admit(Category::Hle).reason == StopReason::GeneratedExhausted);
    CHECK(clock.calls == 4 && binding.reason() == StopReason::GeneratedExhausted);
    Clock other_clock; auto other_port = other_clock.port();
    DiscoveryBudget other({1, 1, 1, 10000}, 1000);
    DiscoveryBudgetBinding independent(other, other_port);
    CHECK(independent.valid_for_admission() && independent.reason() == StopReason::None);
    CHECK(independent.admit(Category::Hle).admitted);
    CHECK(other_clock.calls == 1 && clock.calls == 4 && other.snapshot().hle == 1);
}
static void failure_boundaries()
{
    for (unsigned kind = 0; kind < 5; ++kind) {
        Clock clock; auto port = clock.port();
        DiscoveryBudget policy({2, 2, 2, 1000}, 1000);
        DiscoveryBudgetBinding binding(policy, port);
        StopReason expected = StopReason::None;
        if (kind == 0) { clock.fail = true; expected = StopReason::InvalidClock; }
        if (kind == 1) { clock.sample = std::numeric_limits<std::uint64_t>::max() / 1000 + 1; expected = StopReason::InvalidClock; }
        if (kind == 2) { clock.sample = 0; expected = StopReason::ClockRegressed; }
        if (kind == 3) { clock.sample = 2; expected = StopReason::Deadline; }
        if (kind == 4) { port.now_microseconds = nullptr; expected = StopReason::InvalidClock; CHECK(!binding.valid_for_admission()); }
        const auto result = binding.admit(Category::Indirect);
        CHECK(!result.admitted && result.reason == expected && binding.reason() == expected);
        const auto state = policy.snapshot();
        CHECK(state.generated == 0 && state.hle == 0 && state.indirect == 0 && state.last_ns == 1000);
        const unsigned calls = clock.calls;
        CHECK(binding.poll() == expected && clock.calls == calls);
        CHECK(!binding.valid_for_admission() && clock.calls == calls);
    }
    Clock clock; auto port = clock.port();
    DiscoveryBudget policy({1, 1, 1, 1000}, 1000);
    DiscoveryBudgetBinding binding(policy, port);
    CHECK(!binding.admit(static_cast<Category>(255)).admitted);
    CHECK(binding.reason() == StopReason::InvalidCategory && clock.calls == 1);
    DiscoveryBudget cancelled({1, 1, 1, 1000}, 1000);
    CHECK(cancelled.poll({1000, true, true}) == StopReason::Cancelled);
    DiscoveryBudgetBinding stopped(cancelled, port);
    CHECK(stopped.poll() == StopReason::Cancelled && clock.calls == 1);
}
int main() {
    sampling(); admission_and_poll(); failure_boundaries();
    std::printf("DISCOVERY_BUDGET_BINDING_%s checks=%u failures=%u direct_adapter_only=true\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
