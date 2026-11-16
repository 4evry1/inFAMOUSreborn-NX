/* One feature-off admission contract, reusing the real bridge/domain setup.
 * The inherited control mains are not executed. */
#include "../diagnostics/startup_observation.h"
#ifndef STARTUP_OBSERVATION_RECORDER_FIXTURE
#define STARTUP_OBSERVATION_RECORDER_FIXTURE "test_startup_recorder.cpp"
#endif
#define main inherited_startup_recorder_main
#include STARTUP_OBSERVATION_RECORDER_FIXTURE
#undef main
int main()
{
    Runtime runtime;
    CHECK(runtime.recorder.configure(runtime.events, 4, PpuMemoryTraceRetention::Suffix));
    unsigned calls = 0;
    const ps3_clock clock{&calls, [](void *p, uint64_t *out) {
        ++*static_cast<unsigned *>(p); *out = 0; return PS3_CLOCK_OK;
    }};
    infamous::discovery::DiscoveryBudget policy({4, 4, 4, 1000000000}, 0);
    DiscoveryBudgetBinding budget(policy, clock);
    infamous::diagnostics::ImportRecord imports[1]{};
    infamous::diagnostics::CompletedDispatch completed[1]{};
    infamous::diagnostics::PendingDispatch pending[8]{};
    infamous::diagnostics::StartupObservation observation;
    CHECK(observation.configure(imports, 1, completed, 1, pending, 8));
    CHECK(!native_startup_admit_discovery(runtime.execution, runtime.binding,
        runtime.services, runtime.recorder, budget, observation));
    CHECK(!runtime.execution.ppu.admitted && !runtime.execution.ppu.recorder &&
        !runtime.execution.ppu.budget && !runtime.execution.observation && !runtime.control->cpu);
    CHECK(calls == 0 && budget.valid_for_admission() && observation.valid_for_admission());
    std::printf("Startup observation feature-off admission failures=%d\n", failures);
    return failures ? 1 : 0;
}
