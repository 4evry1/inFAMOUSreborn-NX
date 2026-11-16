#include "deko_queue_owner.h"
#include <cstdio>
#include <cstdlib>
DekoQueueOwner::~DekoQueueOwner() {
    require_completed();
    release_commands();
    release_queue();
    if (command_memory) dkMemBlockDestroy(command_memory);
    if (device) dkDeviceDestroy(device);
}
void DekoQueueOwner::require_completed() const {
    if (state != State::ready) {
        std::fputs("DEKO_OWNER_UNCERTAIN_NO_RESOURCE_RELEASE\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
}
void DekoQueueOwner::release_commands() {
    require_completed();
    if (commands) { dkCmdBufDestroy(commands); commands = {}; }
}
void DekoQueueOwner::release_queue() {
    require_completed();
    if (queue) { dkQueueDestroy(queue); queue = {}; }
}
bool DekoQueueOwner::submit(DkCmdList list) {
    if (state != State::ready || !queue || !list) return false;
    state = State::pending;
    dkQueueSubmitCommands(queue, list);
    return true;
}
bool DekoQueueOwner::reset_commands() {
    if (state != State::ready || !commands) return false;
    dkCmdBufClear(commands);
    return true;
}
DekoCompletion DekoQueueOwner::wait(const char *stage, const char *label,
                                   int64_t timeout, DekoWaitClock clock) {
    if (state == State::unknown || !queue) return DekoCompletion::unknown;
    state = State::pending;
    dkQueueSignalFence(queue, &completion, true);
    dkQueueFlush(queue);
    const uint64_t start = clock.ticks();
    const DkResult result = dkFenceWait(&completion, timeout);
    const uint64_t end = clock.ticks();
    const bool queue_error = dkQueueIsInErrorState(queue);
    const char *name = result == DkResult_Success ? "Success" :
                       result == DkResult_Timeout ? "Timeout" :
                       result == DkResult_Fail ? "Fail" : "Other";
    std::fprintf(stderr,
        "%s_STAGE stage=%s result=%u name=%s elapsed_ticks=%llu tick_frequency_hz=%llu queue_error=%u timeout_ns=%lld\n",
        label, stage, unsigned(result), name,
        static_cast<unsigned long long>(end - start),
        static_cast<unsigned long long>(clock.frequency()), unsigned(queue_error),
        static_cast<long long>(timeout));
    std::fflush(stderr);
    state = result == DkResult_Success && !queue_error ? State::ready : State::unknown;
    return state == State::ready ? DekoCompletion::completed : DekoCompletion::unknown;
}
