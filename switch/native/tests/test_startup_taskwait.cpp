#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
struct Pause {
    Runtime *runtime = nullptr;
    unsigned calls = 0;
    bool fail = false, cancel_after = false, expire_after = false;
    static bool once(void *context) noexcept {
        auto &p=*static_cast<Pause *>(context); ++p.calls;
        if (p.cancel_after) p.runtime->control->cancelled.store(true,std::memory_order_release);
        if (p.expire_after) { p.runtime->control->deadline_tick=1;tick_calls.store(2); }
        return !p.fail;
    }
};
void invoke(Runtime &r, void (*hook)()) {
    r.control->stop=StartupStop::None;
    active=r.control.get();r.control->cpu=&r.execution.ppu.cpu;
    try { hook(); active=nullptr; } catch(const Terminal &) {}
}
void balanced_and_nested_regions() {
    Runtime r;Pause p{&r};r.execution.taskwait={&p,Pause::once};
    invoke(r,ps3_taskwait_begin);invoke(r,ps3_taskwait_begin);
    invoke(r,ps3_taskwait_spin);invoke(r,ps3_taskwait_end);invoke(r,ps3_taskwait_end);
    const auto &o=r.execution.taskwait_observation;
    CHECK(r.control->stop==StartupStop::None && p.calls==1);
    CHECK(o.begins==2 && o.spins==1 && o.ends==2 && o.depth==0 && o.max_depth==2);
    CHECK(o.pauses_attempted==1 && o.pauses_completed==1 && o.failure==StartupTaskwaitFailure::None);
    Runtime without_pause;
    invoke(without_pause,ps3_taskwait_begin);invoke(without_pause,ps3_taskwait_end);
    CHECK(without_pause.control->stop==StartupStop::None && without_pause.execution.taskwait_observation.depth==0);
}
void missing_and_failed_provider_retain_scope() {
    Runtime r;invoke(r,ps3_taskwait_begin);invoke(r,ps3_taskwait_spin);
    CHECK(r.control->stop==StartupStop::TaskwaitStop && r.execution.taskwait_observation.depth==1);
    CHECK(r.execution.taskwait_observation.failure==StartupTaskwaitFailure::MissingProvider);
    CHECK(r.execution.taskwait_observation.pauses_attempted==0);
    Runtime s;Pause p{&s};p.fail=true;s.execution.taskwait={&p,Pause::once};
    invoke(s,ps3_taskwait_begin);invoke(s,ps3_taskwait_spin);
    CHECK(s.control->stop==StartupStop::TaskwaitStop && p.calls==1);
    CHECK(s.execution.taskwait_observation.failure==StartupTaskwaitFailure::ProviderFailure);
    CHECK(s.execution.taskwait_observation.depth==1 && s.execution.taskwait_observation.pauses_attempted==1 && s.execution.taskwait_observation.pauses_completed==0);
}
void boundary_prevents_or_follows_pause() {
    Runtime r;Pause p{&r};r.execution.taskwait={&p,Pause::once};
    invoke(r,ps3_taskwait_begin);r.control->cancelled.store(true,std::memory_order_release);
    invoke(r,ps3_taskwait_spin);CHECK(r.control->stop==StartupStop::Cancelled && p.calls==0 && r.execution.taskwait_observation.depth==1);
    Runtime s;Pause q{&s};q.cancel_after=true;s.execution.taskwait={&q,Pause::once};
    invoke(s,ps3_taskwait_begin);invoke(s,ps3_taskwait_spin);
    CHECK(s.control->stop==StartupStop::Cancelled && q.calls==1 && s.execution.taskwait_observation.pauses_completed==1 && s.execution.taskwait_observation.depth==1);
    Runtime t;Pause v{&t};v.expire_after=true;t.execution.taskwait={&v,Pause::once};
    invoke(t,ps3_taskwait_begin);invoke(t,ps3_taskwait_spin);
    CHECK(t.control->stop==StartupStop::Deadline && v.calls==1 && t.execution.taskwait_observation.depth==1);
    Runtime u;u.control->deadline_tick=1;tick_calls.store(2);invoke(u,ps3_taskwait_begin);
    CHECK(u.control->stop==StartupStop::Deadline && u.execution.taskwait_observation.begins==0);
}
void malformed_scope_and_independent_execution() {
    Runtime r;invoke(r,ps3_taskwait_end);
    CHECK(r.control->stop==StartupStop::TaskwaitStop && r.execution.taskwait_observation.failure==StartupTaskwaitFailure::UnbalancedEnd);
    Runtime s;Pause p{&s};s.execution.taskwait={&p,Pause::once};
    invoke(s,ps3_taskwait_begin);invoke(s,ps3_taskwait_spin);invoke(s,ps3_taskwait_end);
    CHECK(s.control->stop==StartupStop::None && s.execution.taskwait_observation.depth==0 && p.calls==1);
    CHECK(r.execution.taskwait_observation.begins==0);
    Runtime overflow;overflow.execution.taskwait_observation.depth=UINT32_MAX;
    invoke(overflow,ps3_taskwait_begin);
    CHECK(overflow.control->stop==StartupStop::TaskwaitStop && overflow.execution.taskwait_observation.depth==UINT32_MAX);
    CHECK(overflow.execution.taskwait_observation.failure==StartupTaskwaitFailure::DepthOverflow);
}
struct BudgetPause {
    uint64_t now=1;
    unsigned calls=0;
    static ps3_clock_status sample(void *context,uint64_t *value) {
        *value=static_cast<BudgetPause *>(context)->now;return PS3_CLOCK_OK;
    }
    static bool once(void *context) noexcept {
        auto &p=*static_cast<BudgetPause *>(context);++p.calls;p.now=UINT64_C(2000000000);return true;
    }
};
void operational_budget_is_polled_on_both_sides() {
    Runtime r;BudgetPause p;
    const ps3_clock clock{&p,BudgetPause::sample};
    infamous::discovery::DiscoveryBudget policy{{64,64,64,UINT64_C(1000000000)},0};
    DiscoveryBudgetBinding budget{policy,clock};
    auto *original=r.execution.ppu.budget;r.execution.ppu.budget=&budget;
    r.execution.taskwait={&p,BudgetPause::once};invoke(r,ps3_taskwait_begin);invoke(r,ps3_taskwait_spin);
    CHECK(r.control->stop==StartupStop::Deadline && p.calls==1 && r.execution.taskwait_observation.pauses_completed==1);
    invoke(r,ps3_taskwait_spin);CHECK(r.control->stop==StartupStop::Deadline && p.calls==1);
    CHECK(r.execution.taskwait_observation.depth==1);
    r.execution.ppu.budget=original;
}
// A contract fixture for CF98's entry predicate, not a claim that this is the compiled game function.
// All seven halfwords are read through the real coherent domain on every scan.
bool coherent_minimum(Runtime &r, uint32_t address, uint16_t target) {
    uint16_t minimum=UINT16_MAX;
    for(unsigned i=0;i<7;++i) { const uint16_t v=uint16_t(r.get(address+2*i,2)); if(v<minimum)minimum=v; }
    return minimum>=target;
}
struct ProgressPause {
    Runtime *runtime;unsigned calls=0;
    static bool once(void *context) noexcept {
        auto &p=*static_cast<ProgressPause *>(context);++p.calls;
        // Actual provider-side coherent publication, kept distinct from hook behavior.
        const unsigned char value[2]{0,5};
        return ps3_atomic_write(&p.runtime->domain,INSTANCE1+0x72+12,value,2)==PS3_GUEST_MEMORY_OK;
    }
};
void real_coherent_progress_is_required() {
    Runtime r;const unsigned char five[2]{0,5},zero[2]{0,0};
    for(unsigned i=0;i<7;++i)r.put(INSTANCE1+0x72+2*i,five,2);
    r.put(INSTANCE1+0x72+12,zero,2);CHECK(!coherent_minimum(r,INSTANCE1+0x72,5));
    ProgressPause p{&r};r.execution.taskwait={&p,ProgressPause::once};
    invoke(r,ps3_taskwait_begin);invoke(r,ps3_taskwait_spin);
    CHECK(coherent_minimum(r,INSTANCE1+0x72,5));invoke(r,ps3_taskwait_end);
    CHECK(r.control->stop==StartupStop::None && p.calls==1 && r.execution.taskwait_observation.ends==1);
    // Hooks without a provider memory update cannot manufacture progress.
    r.put(INSTANCE1+0x72+12,zero,2);Pause q{&r};r.execution.taskwait={&q,Pause::once};
    invoke(r,ps3_taskwait_begin);invoke(r,ps3_taskwait_spin);
    CHECK(!coherent_minimum(r,INSTANCE1+0x72,5) && r.execution.taskwait_observation.depth==1);
    r.control->cancelled.store(true,std::memory_order_release);invoke(r,ps3_taskwait_end);
    CHECK(r.control->stop==StartupStop::Cancelled && r.execution.taskwait_observation.depth==1);
}
}
int main() {
    balanced_and_nested_regions();missing_and_failed_provider_retain_scope();
    boundary_prevents_or_follows_pause();malformed_scope_and_independent_execution();
    operational_budget_is_polled_on_both_sides();real_coherent_progress_is_required();
    std::printf("TASKWAIT_HOOK_FAILURES=%d\n",failures);return failures?1:0;
}
