#ifndef INFAMOUS_DISCOVERY_BUDGET_BINDING_H
#define INFAMOUS_DISCOVERY_BUDGET_BINDING_H
#include "../diagnostics/discovery_budget.h"
#include "clock.h"

// Borrows stable policy and operational clock storage through confirmed join.
// Exclusive mutation transfers to the worker at launch. Callers perform their
// established cancellation check before sampling. This is not the HLE clock.
bool discovery_budget_sample_start_ns(const ps3_clock &, std::uint64_t &) noexcept;
class DiscoveryBudgetBinding {
public:
    DiscoveryBudgetBinding(infamous::discovery::DiscoveryBudget &, const ps3_clock &) noexcept;
    DiscoveryBudgetBinding(const DiscoveryBudgetBinding &) = delete;
    DiscoveryBudgetBinding &operator=(const DiscoveryBudgetBinding &) = delete;
    DiscoveryBudgetBinding(DiscoveryBudgetBinding &&) = delete;
    DiscoveryBudgetBinding &operator=(DiscoveryBudgetBinding &&) = delete;
    bool valid_for_admission() const noexcept;
    infamous::discovery::Decision admit(infamous::discovery::Category) noexcept;
    infamous::discovery::StopReason poll() noexcept;
    infamous::discovery::StopReason reason() const noexcept;
private:
    infamous::discovery::DiscoveryBudget &policy_;
    const ps3_clock &clock_;
};
#endif
