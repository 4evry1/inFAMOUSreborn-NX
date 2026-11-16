#ifndef DEKO_QUEUE_OWNER_H
#define DEKO_QUEUE_OWNER_H
#include <deko3d.h>
#include <cstdint>
enum class DekoCompletion { completed, unknown };
struct DekoWaitClock {
    uint64_t (*ticks)();
    uint64_t (*frequency)();
};
class DekoQueueOwner {
public:
    DkDevice device{};
    DkQueue queue{};
    DkCmdBuf commands{};
    DkMemBlock command_memory{};
    DekoQueueOwner() = default;
    DekoQueueOwner(const DekoQueueOwner &) = delete;
    DekoQueueOwner &operator=(const DekoQueueOwner &) = delete;
    ~DekoQueueOwner();
    bool submit(DkCmdList list);
    DekoCompletion wait(const char *stage, const char *label, int64_t timeout, DekoWaitClock clock);
    bool reset_commands();
    void require_completed() const;
    void release_commands();
    void release_queue();
private:
    enum class State { ready, pending, unknown } state{State::ready};
    DkFence completion{};
};
#endif
