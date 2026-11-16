#include "compiled_dispatch.h"
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <pthread.h>
#include <thread>

// Deliberately a private fixture type: the core must not inspect CPU layout.
// These callbacks prove dispatch behavior, not generated game-target behavior.
struct FixtureOwner;
struct ppu_context {
    uint64_t argument = 0;
    uint64_t toc = 0x78bf78;
    uint64_t lr = 0x463fa4;
    FixtureOwner *owner = nullptr;
};
struct FixtureOwner {
    ps3_compiled_dispatch_state nesting{0, 4};
    ppu_context cpu{};
    ps3_compiled_code_status lookup_status = PS3_COMPILED_CODE_OK;
    ps3_compiled_dispatch_stop_reason reason = PS3_COMPILED_DISPATCH_CONTINUE;
    ps3_compiled_function target = nullptr;
    uint32_t expected_address = 0x462380;
    unsigned lookups = 0;
    unsigned stop_checks = 0;
    unsigned target_calls = 0;
    bool stop_after_lookup = false;
    ps3_compiled_dispatch_stop_reason after_lookup_reason = PS3_COMPILED_DISPATCH_CANCELLED;
    bool returned_after_dispatch = false;
    bool observed_cpu_identity = false;
    ps3_compiled_dispatch_result nested{};
    FixtureOwner() { cpu.owner = this; }
};
static unsigned checks;
#define CHECK(expression) do { ++checks; if (!(expression)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return 1; } } while (0)
static ps3_compiled_dispatch_stop_reason stop_requested(void *context)
{
    auto &owner = *static_cast<FixtureOwner *>(context);
    ++owner.stop_checks;
    return owner.reason;
}
static ps3_compiled_dispatch_stop_reason unknown_stop(void *)
{
    return static_cast<ps3_compiled_dispatch_stop_reason>(99);
}
static ps3_compiled_dispatch_stop_reason unknown_after_lookup(void *context)
{
    auto &owner = *static_cast<FixtureOwner *>(context);
    ++owner.stop_checks;
    return owner.stop_checks == 1 ? PS3_COMPILED_DISPATCH_CONTINUE :
        static_cast<ps3_compiled_dispatch_stop_reason>(99);
}
static ps3_compiled_code_status lookup(const void *context, uint32_t address,
    ps3_compiled_function *target)
{
    auto &owner = *const_cast<FixtureOwner *>(static_cast<const FixtureOwner *>(context));
    ++owner.lookups;
    if (address != owner.expected_address) {
        return PS3_COMPILED_CODE_NOT_FOUND;
    }
    if (owner.stop_after_lookup) {
        owner.reason = owner.after_lookup_reason;
    }
    *target = owner.target;
    return owner.lookup_status;
}
static ps3_compiled_dispatch_result dispatch(FixtureOwner &owner,
    uint64_t ctr = 0x462380)
{
    return ps3_compiled_dispatch(&owner.nesting, {&owner, lookup},
        {&owner, stop_requested}, &owner.cpu, ctr);
}
static void mutate(ppu_context *cpu)
{
    auto &owner = *cpu->owner;
    owner.observed_cpu_identity = cpu == &owner.cpu;
    ++owner.target_calls;
    cpu->argument += 7;
}
static void mutate_abi(ppu_context *cpu)
{
    mutate(cpu);
    cpu->toc = 0x1234;
    cpu->lr = 0x5678;
}
static void nested(ppu_context *cpu)
{
    auto &owner = *cpu->owner;
    ++owner.target_calls;
    owner.target = mutate;
    owner.nested = dispatch(owner);
}
static void terminate_worker(ppu_context *cpu)
{
    ++cpu->owner->target_calls;
    pthread_exit(nullptr);
}
static void *worker(void *context)
{
    auto &owner = *static_cast<FixtureOwner *>(context);
    dispatch(owner);
    owner.returned_after_dispatch = true;
    return nullptr;
}
int main()
{
    FixtureOwner owner;
    owner.target = mutate;
    auto result = dispatch(owner, 0xabcdef0000462380ull);
    CHECK(result.status == PS3_COMPILED_DISPATCH_OK);
    CHECK(result.lookup_status == PS3_COMPILED_CODE_OK);
    CHECK(owner.lookups == 1 && owner.stop_checks == 2 && owner.target_calls == 1);
    CHECK(owner.observed_cpu_identity && owner.cpu.argument == 7);
    CHECK(owner.cpu.toc == 0x78bf78 && owner.cpu.lr == 0x463fa4 && owner.nesting.depth == 0);
    for (uint64_t ctr : {0ull, 1ull, 0x462382ull, 0x100000000ull}) {
        const unsigned lookups = owner.lookups;
        const unsigned calls = owner.target_calls;
        CHECK(dispatch(owner, ctr).status == PS3_COMPILED_DISPATCH_INVALID_ADDRESS);
        CHECK(owner.lookups == lookups && owner.target_calls == calls && owner.nesting.depth == 0);
    }
    result = dispatch(owner, 0x500000);
    CHECK(result.status == PS3_COMPILED_DISPATCH_LOOKUP_FAILURE);
    CHECK(result.lookup_status == PS3_COMPILED_CODE_NOT_FOUND && owner.target_calls == 1);
    owner.lookup_status = PS3_COMPILED_CODE_INVALID_TABLE;
    result = dispatch(owner);
    CHECK(result.status == PS3_COMPILED_DISPATCH_LOOKUP_FAILURE);
    CHECK(result.lookup_status == PS3_COMPILED_CODE_INVALID_TABLE && owner.target_calls == 1);
    owner.lookup_status = PS3_COMPILED_CODE_OK;
    owner.target = nullptr;
    CHECK(dispatch(owner).status == PS3_COMPILED_DISPATCH_NULL_TARGET);
    CHECK(owner.target_calls == 1 && owner.nesting.depth == 0);
    owner.target = mutate;
    for (auto reason : {PS3_COMPILED_DISPATCH_CANCELLED, PS3_COMPILED_DISPATCH_DEADLINE}) {
        owner.reason = reason;
        const unsigned lookups = owner.lookups;
        result = dispatch(owner);
        CHECK(result.status == (reason == PS3_COMPILED_DISPATCH_CANCELLED ?
            PS3_COMPILED_DISPATCH_STOP_CANCELLED : PS3_COMPILED_DISPATCH_STOP_DEADLINE));
        CHECK(owner.lookups == lookups && owner.target_calls == 1 && owner.nesting.depth == 0);
    }
    owner.reason = PS3_COMPILED_DISPATCH_CONTINUE;
    owner.stop_after_lookup = true;
    CHECK(dispatch(owner).status == PS3_COMPILED_DISPATCH_STOP_CANCELLED);
    CHECK(owner.target_calls == 1 && owner.nesting.depth == 0);
    CHECK(dispatch(owner).lookup_attempted == false); // cancellation remains latched
    owner.reason = PS3_COMPILED_DISPATCH_CONTINUE;
    owner.after_lookup_reason = PS3_COMPILED_DISPATCH_DEADLINE;
    CHECK(dispatch(owner).status == PS3_COMPILED_DISPATCH_STOP_DEADLINE);
    CHECK(owner.target_calls == 1 && owner.nesting.depth == 0);
    owner.stop_after_lookup = false;
    owner.reason = PS3_COMPILED_DISPATCH_CONTINUE;
    owner.nesting.depth = owner.nesting.limit;
    const unsigned lookups = owner.lookups;
    CHECK(dispatch(owner).status == PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED);
    CHECK(owner.lookups == lookups && owner.target_calls == 1);
    owner.nesting.depth = 0;
    owner.target = mutate_abi;
    CHECK(dispatch(owner).status == PS3_COMPILED_DISPATCH_OK);
    CHECK(owner.cpu.toc == 0x1234 && owner.cpu.lr == 0x5678);
    FixtureOwner malformed;
    malformed.target = mutate;
    malformed.nesting.limit = 0;
    result = dispatch(malformed);
    CHECK(result.status == PS3_COMPILED_DISPATCH_INVALID_STATE && !result.lookup_attempted);
    CHECK(malformed.lookups == 0 && malformed.stop_checks == 0 && malformed.target_calls == 0);
    malformed.nesting = {5, 4};
    CHECK(dispatch(malformed).status == PS3_COMPILED_DISPATCH_INVALID_STATE);
    CHECK(malformed.nesting.depth == 5 && malformed.target_calls == 0);
    malformed.nesting = {0, 4};
    result = ps3_compiled_dispatch(&malformed.nesting, {&malformed, lookup},
        {nullptr, unknown_stop}, &malformed.cpu, 0x462380);
    CHECK(result.status == PS3_COMPILED_DISPATCH_INVALID_STOP_REASON && !result.lookup_attempted);
    CHECK(malformed.lookups == 0 && malformed.target_calls == 0);
    result = ps3_compiled_dispatch(&malformed.nesting, {&malformed, lookup},
        {&malformed, unknown_after_lookup}, &malformed.cpu, 0x462380);
    CHECK(result.status == PS3_COMPILED_DISPATCH_INVALID_STOP_REASON && result.lookup_attempted);
    CHECK(malformed.lookups == 1 && malformed.target_calls == 0 && malformed.nesting.depth == 0);
    CHECK(ps3_compiled_dispatch(nullptr, {&malformed, lookup}, {&malformed, stop_requested},
        &malformed.cpu, 0x462380).status == PS3_COMPILED_DISPATCH_INVALID_ARGUMENT);
    CHECK(ps3_compiled_dispatch(&malformed.nesting, {&malformed, lookup}, {&malformed, stop_requested},
        nullptr, 0x462380).status == PS3_COMPILED_DISPATCH_INVALID_ARGUMENT);
    CHECK(ps3_compiled_dispatch(&malformed.nesting, {&malformed, nullptr}, {&malformed, stop_requested},
        &malformed.cpu, 0x462380).status == PS3_COMPILED_DISPATCH_INVALID_ARGUMENT);
    CHECK(ps3_compiled_dispatch(&malformed.nesting, {&malformed, lookup}, {&malformed, nullptr},
        &malformed.cpu, 0x462380).status == PS3_COMPILED_DISPATCH_INVALID_ARGUMENT);
    FixtureOwner recursive;
    recursive.target = nested;
    recursive.nesting.limit = 1;
    CHECK(dispatch(recursive).status == PS3_COMPILED_DISPATCH_OK);
    CHECK(recursive.nested.status == PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED);
    CHECK(recursive.nesting.depth == 0 && recursive.target_calls == 1 && recursive.cpu.argument == 0);
    recursive.target = nested;
    recursive.nesting.limit = 2;
    CHECK(dispatch(recursive).status == PS3_COMPILED_DISPATCH_OK);
    CHECK(recursive.nested.status == PS3_COMPILED_DISPATCH_OK);
    CHECK(recursive.nesting.depth == 0 && recursive.target_calls == 3 && recursive.cpu.argument == 7);
    FixtureOwner one, two;
    one.target = mutate; two.target = mutate;
    std::atomic<bool> failed{false};
    auto repeated = [&](FixtureOwner &current) {
        for (unsigned i = 0; i < 10000; ++i) {
            if (dispatch(current).status != PS3_COMPILED_DISPATCH_OK) {
                failed.store(true);
            }
        }
    };
    std::thread a([&] { repeated(one); });
    std::thread b([&] { repeated(two); });
    a.join(); b.join();
    CHECK(!failed.load() && one.cpu.argument == 70000 && two.cpu.argument == 70000);
    CHECK(one.nesting.depth == 0 && two.nesting.depth == 0);
    FixtureOwner terminating;
    terminating.target = terminate_worker;
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, worker, &terminating) == 0);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(!terminating.returned_after_dispatch && terminating.target_calls == 1);
    CHECK(terminating.nesting.depth == 1);
    std::printf("DISPATCH_TERMINATION joined=1 returned=0 retained_depth=%u\n", terminating.nesting.depth);
    // Explicit reuse only after the worker has joined, never in a destructor.
    terminating.nesting.depth = 0;
    terminating.target = mutate;
    CHECK(dispatch(terminating).status == PS3_COMPILED_DISPATCH_OK);
    CHECK(terminating.nesting.depth == 0 && terminating.cpu.argument == 7);
    std::printf("COMPILED_DISPATCH PASS checks=%u\n", checks);
    return 0;
}
