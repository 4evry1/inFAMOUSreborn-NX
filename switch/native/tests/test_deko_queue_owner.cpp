#include "deko_queue_owner.h"
#include <cassert>
#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
static std::string events;
static DkResult result = DkResult_Success;
static bool error_state;
static int event_pipe = -1;
static void record(char event) {
    events += event;
    if (event_pipe >= 0) assert(write(event_pipe, &event, 1) == 1);
}
extern "C" {
void dkQueueSignalFence(DkQueue, DkFence *, bool flush) { assert(flush); record('S'); }
void dkQueueFlush(DkQueue) { record('F'); }
DkResult dkFenceWait(DkFence *, int64_t timeout) { assert(timeout == 50); record('W'); return result; }
bool dkQueueIsInErrorState(DkQueue) { record('E'); return error_state; }
void dkQueueSubmitCommands(DkQueue, DkCmdList) { record('U'); }
void dkCmdBufClear(DkCmdBuf) { record('R'); }
void dkCmdBufDestroy(DkCmdBuf) { record('C'); }
void dkMemBlockDestroy(DkMemBlock) { record('M'); }
void dkQueueDestroy(DkQueue) { record('Q'); }
void dkDeviceDestroy(DkDevice) { record('D'); }
}
static uint64_t ticks() { return 10; }
static uint64_t frequency() { return 1000; }
static void populate(DekoQueueOwner &owner) {
    owner.device = reinterpret_cast<DkDevice>(1);
    owner.queue = reinterpret_cast<DkQueue>(2);
    owner.commands = reinterpret_cast<DkCmdBuf>(3);
    owner.command_memory = reinterpret_cast<DkMemBlock>(4);
}
int main() {
    DekoWaitClock clock{ticks, frequency};
    {
        DekoQueueOwner owner; populate(owner);
        assert(owner.submit(1));
        assert(!owner.reset_commands());
        assert(owner.wait("draw", "TEST", 50, clock) == DekoCompletion::completed);
        assert(owner.reset_commands());
    }
    assert(events == "USFWERCQMD");
    events.clear();
    {
        DekoQueueOwner owner; populate(owner);
        assert(owner.wait("init", "TEST", 50, clock) == DekoCompletion::completed);
    }
    assert(events == "SFWECQMD");
    int pipe_fds[2]; assert(pipe(pipe_fds) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(pipe_fds[0]); event_pipe = pipe_fds[1];
        DekoQueueOwner owner; populate(owner);
        result = DkResult_Timeout;
        assert(owner.submit(1));
        assert(owner.wait("draw", "TEST", 50, clock) == DekoCompletion::unknown);
        assert(!owner.reset_commands() && !owner.submit(1));
        /* Destructor must fail-stop before any command/queue/memory/device release. */
        return 0;
    }
    close(pipe_fds[1]); int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    char bytes[64]; ssize_t count = read(pipe_fds[0], bytes, sizeof bytes); assert(count >= 0);
    assert(std::string(bytes, static_cast<size_t>(count)) == "USFWE");
    close(pipe_fds[0]);
    return 0;
}
