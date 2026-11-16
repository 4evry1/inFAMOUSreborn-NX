#include "discovery_budget.h"
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>
using namespace infamous::discovery;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line=%d expression=%s\n", __LINE__, #x); } } while (0)
static_assert(!std::is_copy_constructible<DiscoveryBudget>::value);
static_assert(!std::is_copy_assignable<DiscoveryBudget>::value);
static_assert(!std::is_move_constructible<DiscoveryBudget>::value);
static_assert(!std::is_move_assignable<DiscoveryBudget>::value);
static_assert(std::is_same<decltype(std::declval<const DiscoveryBudget &>().snapshot()), Snapshot>::value);
constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
constexpr Category categories[] = {Category::Generated, Category::Hle, Category::Indirect};
constexpr StopReason exhausted[] = {StopReason::GeneratedExhausted, StopReason::HleExhausted, StopReason::IndirectExhausted};
static void expect(DiscoveryBudget &budget, Category category, Observation observation,
                   bool admitted, StopReason reason, Snapshot expected)
{
    const auto result = budget.admit(category, observation);
    CHECK(result.admitted == admitted);
    CHECK(result.reason == reason);
    const auto actual = budget.snapshot();
    CHECK(actual.generated == expected.generated);
    CHECK(actual.hle == expected.hle);
    CHECK(actual.indirect == expected.indirect);
    CHECK(actual.start_ns == expected.start_ns);
    CHECK(actual.last_ns == expected.last_ns);
    CHECK(actual.stop == expected.stop);
    CHECK(actual == expected);
}
static void limits_and_independence()
{
    for (unsigned selected = 0; selected < 3; ++selected) {
        DiscoveryBudget budget({2, 2, 2, 1000}, 100);
        Snapshot expected{0, 0, 0, 100, 100, StopReason::None};
        for (unsigned step = 1; step <= 2; ++step) {
            if (selected == 0) expected.generated = step;
            if (selected == 1) expected.hle = step;
            if (selected == 2) expected.indirect = step;
            expected.last_ns = 100 + step;
            expect(budget, categories[selected], {100 + step, true, false}, true, StopReason::None, expected);
        }
        expected.stop = exhausted[selected];
        expect(budget, categories[selected], {103, true, false}, false, exhausted[selected], expected);
        for (Category later : categories)
            expect(budget, later, {0, false, true}, false, exhausted[selected], expected);
        expect(budget, static_cast<Category>(255), {maximum, false, true}, false, exhausted[selected], expected);
        DiscoveryBudget fresh({2, 2, 2, 1000}, 100);
        CHECK((fresh.snapshot() == Snapshot{0, 0, 0, 100, 100, StopReason::None}));
        const auto copied = fresh.snapshot();
        auto changed = copied;
        changed.generated = 9;
        CHECK(fresh.snapshot() == copied);
        CHECK(!(fresh.snapshot() == changed));
        const auto limits = fresh.limits();
        CHECK(limits.generated == 2 && limits.hle == 2 && limits.indirect == 2 && limits.deadline_ns == 1000);
        Limits zero{1, 1, 1, 1000};
        if (selected == 0) zero.generated = 0;
        if (selected == 1) zero.hle = 0;
        if (selected == 2) zero.indirect = 0;
        DiscoveryBudget denied(zero, 100);
        expect(denied, categories[selected], {100, true, false}, false, exhausted[selected],
               {0, 0, 0, 100, 100, exhausted[selected]});
    }
    DiscoveryBudget shared({1, 1, 1, 1000}, 100);
    expect(shared, Category::Generated, {101, true, false}, true, StopReason::None, {1, 0, 0, 100, 101, StopReason::None});
    expect(shared, Category::Hle, {102, true, false}, true, StopReason::None, {1, 1, 0, 100, 102, StopReason::None});
    expect(shared, Category::Indirect, {103, true, false}, true, StopReason::None, {1, 1, 1, 100, 103, StopReason::None});
}
static void precedence_and_time()
{
    struct Row { Category category; Observation observation; StopReason reason; };
    const Row rows[] = {
        {Category::Generated, {1100, true, true}, StopReason::Cancelled},
        {static_cast<Category>(3), {0, false, true}, StopReason::Cancelled},
        {static_cast<Category>(3), {1100, false, false}, StopReason::InvalidCategory},
        {static_cast<Category>(255), {1100, true, false}, StopReason::InvalidCategory},
        {Category::Generated, {1100, false, false}, StopReason::InvalidClock},
        {Category::Generated, {99, true, false}, StopReason::ClockRegressed},
        {Category::Generated, {1100, true, false}, StopReason::Deadline}
    };
    for (const auto &row : rows) {
        DiscoveryBudget budget({0, 0, 0, 1000}, 100);
        const Snapshot stopped{0, 0, 0, 100, 100, row.reason};
        expect(budget, row.category, row.observation, false, row.reason, stopped);
        expect(budget, Category::Indirect, {100, true, false}, false, row.reason, stopped);
    }
    DiscoveryBudget equal({3, 3, 3, 1000}, 100);
    expect(equal, Category::Generated, {200, true, false}, true, StopReason::None, {1, 0, 0, 100, 200, StopReason::None});
    expect(equal, Category::Generated, {200, true, false}, true, StopReason::None, {2, 0, 0, 100, 200, StopReason::None});
    expect(equal, Category::Hle, {150, true, false}, false, StopReason::ClockRegressed, {2, 0, 0, 100, 200, StopReason::ClockRegressed});
    DiscoveryBudget edge({3, 3, 3, 1000}, 100);
    expect(edge, Category::Generated, {1099, true, false}, true, StopReason::None, {1, 0, 0, 100, 1099, StopReason::None});
    expect(edge, Category::Generated, {1100, true, false}, false, StopReason::Deadline, {1, 0, 0, 100, 1099, StopReason::Deadline});
    DiscoveryBudget zero({maximum, maximum, maximum, 0}, 100);
    expect(zero, Category::Hle, {100, true, false}, false, StopReason::Deadline, {0, 0, 0, 100, 100, StopReason::Deadline});
    DiscoveryBudget max_deadline({maximum, maximum, maximum, maximum}, 0);
    expect(max_deadline, Category::Indirect, {maximum - 1, true, false}, true, StopReason::None, {0, 0, 1, 0, maximum - 1, StopReason::None});
    expect(max_deadline, Category::Indirect, {maximum, true, false}, false, StopReason::Deadline, {0, 0, 1, 0, maximum - 1, StopReason::Deadline});
    DiscoveryBudget wrap({3, 3, 3, 10}, maximum - 5);
    expect(wrap, Category::Generated, {maximum, true, false}, true, StopReason::None, {1, 0, 0, maximum - 5, maximum, StopReason::None});
    expect(wrap, Category::Generated, {3, true, false}, false, StopReason::ClockRegressed, {1, 0, 0, maximum - 5, maximum, StopReason::ClockRegressed});
}
static void poll_boundaries()
{
    DiscoveryBudget budget({1, 1, 1, 1000}, 100);
    CHECK(budget.poll({200, true, false}) == StopReason::None);
    CHECK((budget.snapshot() == Snapshot{0, 0, 0, 100, 200, StopReason::None}));
    expect(budget, Category::Generated, {300, true, false}, true, StopReason::None, {1, 0, 0, 100, 300, StopReason::None});
    CHECK(budget.poll({400, true, false}) == StopReason::None);
    expect(budget, Category::Generated, {500, true, false}, false, StopReason::GeneratedExhausted, {1, 0, 0, 100, 400, StopReason::GeneratedExhausted});
    CHECK(budget.poll({0, false, true}) == StopReason::GeneratedExhausted);
    CHECK((budget.snapshot() == Snapshot{1, 0, 0, 100, 400, StopReason::GeneratedExhausted}));
    struct Row { Observation observation; StopReason reason; };
    const Row rows[] = {{{1100, false, true}, StopReason::Cancelled}, {{1100, false, false}, StopReason::InvalidClock},
        {{99, true, false}, StopReason::ClockRegressed}, {{1100, true, false}, StopReason::Deadline}};
    for (const auto &row : rows) {
        DiscoveryBudget fresh({0, 0, 0, 1000}, 100);
        CHECK(fresh.poll(row.observation) == row.reason);
        expect(fresh, Category::Indirect, {100, true, false}, false, row.reason, {0, 0, 0, 100, 100, row.reason});
    }
    DiscoveryBudget equal({0, 0, 0, 1000}, 100);
    CHECK(equal.poll({100, true, false}) == StopReason::None);
    CHECK(equal.poll({1099, true, false}) == StopReason::None);
    CHECK(equal.poll({1099, true, false}) == StopReason::None);
    CHECK(equal.poll({100, true, false}) == StopReason::ClockRegressed);
    DiscoveryBudget wrapped({0, 0, 0, 10}, maximum - 5);
    CHECK(wrapped.poll({maximum, true, false}) == StopReason::None);
    CHECK(wrapped.poll({3, true, false}) == StopReason::ClockRegressed);
    DiscoveryBudget zero({1, 1, 1, 0}, 100);
    CHECK(zero.poll({100, true, false}) == StopReason::Deadline);
    DiscoveryBudget max_time({0, 0, 0, maximum}, 0);
    CHECK(max_time.poll({maximum - 1, true, false}) == StopReason::None);
    CHECK(max_time.poll({maximum, true, false}) == StopReason::Deadline);
}
static void equality_fields()
{
    const Snapshot baseline{1, 2, 3, 100, 110, StopReason::None};
    CHECK(baseline == baseline);
    auto changed = baseline; ++changed.generated; CHECK(!(baseline == changed));
    changed = baseline; ++changed.hle; CHECK(!(baseline == changed));
    changed = baseline; ++changed.indirect; CHECK(!(baseline == changed));
    changed = baseline; ++changed.start_ns; CHECK(!(baseline == changed));
    changed = baseline; ++changed.last_ns; CHECK(!(baseline == changed));
    changed = baseline; changed.stop = StopReason::Deadline; CHECK(!(baseline == changed));
}
static void checked_arithmetic()
{
    struct Row { std::uint64_t count, limit; bool admitted; std::uint64_t next; };
    const Row rows[] = {{maximum - 1, maximum, true, maximum}, {maximum, maximum, false, maximum},
        {0, 0, false, 0}, {7, 5, false, 7}, {4, 5, true, 5}, {maximum, 0, false, maximum}};
    for (const auto &row : rows) {
        const auto actual = checked_count_admission(row.count, row.limit);
        CHECK(actual.admitted == row.admitted && actual.next == row.next);
    }
    DiscoveryBudget budget({2000000, 10000, 10000, 10000000000ULL}, 100);
    for (std::uint64_t n = 0; n < 10000; ++n) {
        const auto result = budget.admit(Category::Hle, {100 + n, true, false});
        CHECK(result.admitted && result.reason == StopReason::None);
    }
    expect(budget, Category::Hle, {10100, true, false}, false, StopReason::HleExhausted,
           {0, 10000, 0, 100, 10099, StopReason::HleExhausted});
}
int main()
{
    limits_and_independence(); precedence_and_time(); checked_arithmetic(); equality_fields(); poll_boundaries();
    std::printf("DISCOVERY_BUDGET_%s checks=%u failures=%u pure_policy_only=true\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
