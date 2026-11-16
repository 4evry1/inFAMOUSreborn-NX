/* Decision 33 / B2a: the lv2 sync and object domain (native/domain/lv2_objects.h), on its own.
 *
 * Real guard, real notify port (native/adapters/wait_port_pthread.c) and real host threads, so the
 * blocking paths are exercised for what they are: a sleeper that only a post/set/send on another
 * thread can wake, and a deadline that really expires. Nothing here touches guest memory: the domain
 * is CPU-free by construction, and the ABI (big-endian writes, r3 sign extension, EFAULT writing
 * nothing) belongs to the adapter tests in test_lle_sre.cpp and test_startup_indirect.cpp.
 *
 * Every expectation is derived from the documented lv2 semantics in the header, never captured from
 * a run. */
#include "../domain/lv2_objects.h"
extern "C" {
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
}
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

namespace {

int failures;
#define CHECK(value) do { if (!(value)) { std::printf("LV2_SYNC_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)

constexpr int32_t ANY = 0;

struct Runtime {
    ps3_pthread_guard guard;
    ps3_pthread_wait_port wait;
    ps3_lv2_objects objects{};
    Runtime()
    {
        CHECK(ps3_pthread_guard_init(&guard) == 0);
        CHECK(ps3_pthread_wait_port_init(&wait) == 0);
        CHECK(ps3_lv2_objects_init(&objects, ps3_pthread_guard_port(&guard), ps3_pthread_wait_port_port(&wait)) == PS3_LV2_OK);
    }
    ~Runtime()
    {
        ps3_pthread_wait_port_destroy(&wait);
        ps3_pthread_guard_destroy(&guard);
    }
};

ps3_lv2_semaphore_attr fifo_attr() { ps3_lv2_semaphore_attr a{}; a.protocol = PS3_SYNC_FIFO; return a; }

/* ---------- semaphores ---------- */

void semaphore_create_validation()
{
    Runtime r;
    ps3_lv2_semaphore_attr attr = fifo_attr();
    uint32_t id = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 4) == PS3_LV2_OK);
    CHECK(id != 0);
    uint32_t other = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &other, &attr, 0, 4) == PS3_LV2_OK);
    CHECK(other != id);   // never reused
    /* max <= 0 and initial outside 0..max are EINVAL. */
    CHECK(ps3_lv2_semaphore_create(&r.objects, &other, &attr, 0, 0) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&r.objects, &other, &attr, -1, 4) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&r.objects, &other, &attr, 5, 4) == PS3_LV2_EINVAL);
    /* A bad protocol is EINVAL; a null out is EFAULT. */
    ps3_lv2_semaphore_attr bad = fifo_attr();
    bad.protocol = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &other, &bad, 0, 4) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&r.objects, nullptr, &attr, 0, 4) == PS3_LV2_EFAULT);
    /* A destroy of an absent id is ESRCH. */
    CHECK(ps3_lv2_semaphore_destroy(&r.objects, 0xDEADBEEF) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_semaphore_destroy(&r.objects, id) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_destroy(&r.objects, id) == PS3_LV2_ESRCH);   // already gone
}

void semaphore_trywait_and_post()
{
    Runtime r;
    ps3_lv2_semaphore_attr attr = fifo_attr();
    uint32_t id = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 1, 3) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_trywait(&r.objects, id) == PS3_LV2_OK);      // one available
    CHECK(ps3_lv2_semaphore_trywait(&r.objects, id) == PS3_LV2_EBUSY);   // none left
    CHECK(ps3_lv2_semaphore_post(&r.objects, id, 3) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_post(&r.objects, id, 1) == PS3_LV2_EBUSY);   // would exceed max
    int32_t value = 0;
    CHECK(ps3_lv2_semaphore_get_value(&r.objects, id, &value) == PS3_LV2_OK && value == 3);
    CHECK(ps3_lv2_semaphore_post(&r.objects, id, -1) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_trywait(&r.objects, 0xDEADBEEF) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_semaphore_post(&r.objects, 0xDEADBEEF, 1) == PS3_LV2_ESRCH);
}

struct SemaphoreWaker {
    ps3_lv2_objects *objects;
    uint32_t id;
    unsigned delay_us;
    std::atomic<int> posted{0};
};
void *semaphore_waker(void *context)
{
    auto *waker = static_cast<SemaphoreWaker *>(context);
    usleep(waker->delay_us);
    waker->posted = int(ps3_lv2_semaphore_post(waker->objects, waker->id, 1));
    return nullptr;
}

void semaphore_wait_is_woken_by_another_thread()
{
    Runtime r;
    ps3_lv2_semaphore_attr attr = fifo_attr();
    uint32_t id = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 1) == PS3_LV2_OK);
    SemaphoreWaker waker{&r.objects, id, 20000, {}};
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, semaphore_waker, &waker) == 0);
    uint64_t blocked = 0;
    /* No timeout: only the post can wake this. */
    CHECK(ps3_lv2_semaphore_wait(&r.objects, id, 0, nullptr, &blocked) == PS3_LV2_OK);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(waker.posted.load() == PS3_LV2_OK);
    CHECK(blocked == 0);   // the sleeper cleared its own flag before returning
    int32_t value = 0;
    CHECK(ps3_lv2_semaphore_get_value(&r.objects, id, &value) == PS3_LV2_OK && value == 0);
}

void semaphore_wait_times_out()
{
    Runtime r;
    ps3_lv2_semaphore_attr attr = fifo_attr();
    uint32_t id = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 1) == PS3_LV2_OK);
    /* A real 5 ms deadline that really expires. Never a fake wake. */
    CHECK(ps3_lv2_semaphore_wait(&r.objects, id, 5000, nullptr, nullptr) == PS3_LV2_TIMED_OUT);
}

/* ---------- event flags ---------- */

uint32_t make_flag(Runtime &r, uint32_t type, uint64_t bits)
{
    uint32_t id = 0;
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, type, bits) == PS3_LV2_OK);
    CHECK(id != 0);
    return id;
}

void event_flag_create_validation()
{
    Runtime r;
    uint32_t id = 0;
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, PS3_SYNC_WAITER_SINGLE, 0) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, 0x1234, 0) == PS3_LV2_EINVAL);   // bad type
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, 0, PS3_SYNC_WAITER_MULTIPLE, 0) == PS3_LV2_EINVAL);   // bad protocol
    CHECK(ps3_lv2_event_flag_create(&r.objects, nullptr, PS3_SYNC_FIFO, PS3_SYNC_WAITER_SINGLE, 0) == PS3_LV2_EFAULT);
    CHECK(ps3_lv2_event_flag_destroy(&r.objects, 0xDEADBEEF) == PS3_LV2_ESRCH);
}

void event_flag_or_and_clear_semantics()
{
    Runtime r;
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_MULTIPLE, 0);
    uint64_t result = 0;
    /* OR: any one of the bits. */
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x2) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x3, PS3_EVENT_FLAG_OR, &result) == PS3_LV2_OK);
    CHECK(result == 0x2);   // the bit that was actually set
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x4, PS3_EVENT_FLAG_OR, &result) == PS3_LV2_EBUSY);
    /* AND: every bit. */
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x1) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x3, PS3_EVENT_FLAG_AND, &result) == PS3_LV2_OK);
    CHECK(result == 0x3);
    /* CLEAR: the matched bits go, the others stay. */
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x4) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x3, PS3_EVENT_FLAG_AND | PS3_EVENT_FLAG_CLEAR, &result) == PS3_LV2_OK);
    CHECK(result == 0x3);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x3, PS3_EVENT_FLAG_OR, &result) == PS3_LV2_EBUSY);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x4, PS3_EVENT_FLAG_OR, &result) == PS3_LV2_OK);
    /* CLEAR_ALL: everything goes once the pattern matched. */
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x1) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x8) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_OR | PS3_EVENT_FLAG_CLEAR_ALL, &result) == PS3_LV2_OK);
    CHECK(result == 0x1);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x8, PS3_EVENT_FLAG_OR, &result) == PS3_LV2_EBUSY);
    /* A mode with neither AND nor OR, or with both, is EINVAL; so is an unknown bit. */
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x1, 0, &result) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_AND | PS3_EVENT_FLAG_OR, &result) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_OR | 0x100, &result) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_OR, nullptr) == PS3_LV2_EFAULT);
    /* clear alone takes bits away and never blocks. */
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x3) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_clear(&r.objects, flag, 0x1) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x3, PS3_EVENT_FLAG_AND, &result) == PS3_LV2_EBUSY);
    CHECK(ps3_lv2_event_flag_trywait(&r.objects, flag, 0x2, PS3_EVENT_FLAG_AND, &result) == PS3_LV2_OK);
}

struct FlagWaker {
    ps3_lv2_objects *objects;
    uint32_t id;
    uint64_t bits;
    unsigned delay_us;
    std::atomic<int> result{1};
};
void *event_flag_waker(void *context)
{
    auto *waker = static_cast<FlagWaker *>(context);
    usleep(waker->delay_us);
    waker->result = int(ps3_lv2_event_flag_set(waker->objects, waker->id, waker->bits));
    return nullptr;
}

void event_flag_wait_is_woken()
{
    Runtime r;
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_MULTIPLE, 0);
    FlagWaker waker{&r.objects, flag, 0x20, 20000, {}};
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, event_flag_waker, &waker) == 0);
    uint64_t result = 0;
    CHECK(ps3_lv2_event_flag_wait(&r.objects, flag, 0x20, PS3_EVENT_FLAG_AND, &result, 0, nullptr, nullptr) == PS3_LV2_OK);
    CHECK(result == 0x20);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(waker.result.load() == PS3_LV2_OK);
}

struct FlagSleeper {
    ps3_lv2_objects *objects;
    uint32_t id;
    std::atomic<uint32_t> code{0xFF};
    std::atomic<unsigned> arrived{0};
    uint64_t blocked = 0;   // root (C1b): set to 1 by the domain, under its guard, once the waiter is registered
};
void *event_flag_sleeper(void *context)
{
    auto *sleeper = static_cast<FlagSleeper *>(context);
    uint64_t result = 0;
    sleeper->arrived = sleeper->arrived + 1;
    sleeper->code = ps3_lv2_event_flag_wait(sleeper->objects, sleeper->id, 0x1, PS3_EVENT_FLAG_AND,
        &result, 5000000, nullptr, &sleeper->blocked);
    return nullptr;
}

void event_flag_single_waiter_refuses_a_second()
{
    Runtime r;
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_SINGLE, 0);
    FlagSleeper first{&r.objects, flag, {}, {}};
    pthread_t a;
    CHECK(pthread_create(&a, nullptr, event_flag_sleeper, &first) == 0);
    while (first.arrived.load() == 0) usleep(1000);   // genuinely inside the wait
    uint64_t result = 0;
    /* SINGLE: one waiter at a time, a second is EPERM -- exactly what the console answers. */
    CHECK(ps3_lv2_event_flag_wait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_AND, &result, 1000, nullptr, nullptr) == PS3_LV2_EPERM);
    CHECK(ps3_lv2_event_flag_set(&r.objects, flag, 0x1) == PS3_LV2_OK);
    CHECK(pthread_join(a, nullptr) == 0);
    CHECK(first.code.load() == PS3_LV2_OK);
}

void event_flag_destroy_wakes_the_waiter()
{
    Runtime r;
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_MULTIPLE, 0);
    FlagSleeper sleeper{&r.objects, flag, {}, {}};
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, event_flag_sleeper, &sleeper) == 0);
    /* Root fix (C1b): wait until the waiter is REGISTERED (the domain's blocked word), not merely started: under load
     * the destroy could otherwise land before the wait and correctly answer ESRCH instead of waking a waiter. */
    while (__atomic_load_n(&sleeper.blocked, __ATOMIC_SEQ_CST) == 0) usleep(1000);
    CHECK(ps3_lv2_event_flag_destroy(&r.objects, flag) == PS3_LV2_OK);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(sleeper.code.load() == PS3_LV2_CANCELLED);   // its object is gone: never a fake match
}

void event_flag_wait_times_out()
{
    Runtime r;
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_MULTIPLE, 0);
    uint64_t result = 0;
    CHECK(ps3_lv2_event_flag_wait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_AND, &result, 2000, nullptr, nullptr) == PS3_LV2_TIMED_OUT);
}

/* ---------- queues ---------- */

ps3_lv2_queue_attr queue_attr() { ps3_lv2_queue_attr a{}; a.protocol = PS3_SYNC_FIFO; a.type = PS3_PPU_QUEUE; return a; }

struct QueueReceiver {
    ps3_lv2_objects *objects;
    uint32_t id;
    std::atomic<uint32_t> code{0xFF};
    std::atomic<unsigned> arrived{0};
    ps3_lv2_event event{};
};
void *queue_receiver(void *context)
{
    auto *receiver = static_cast<QueueReceiver *>(context);
    uint64_t blocked = 0;
    receiver->arrived = receiver->arrived + 1;
    receiver->code = ps3_lv2_queue_receive(receiver->objects, receiver->id, &receiver->event, nullptr, &blocked);
    return nullptr;
}

void queue_tryreceive_drains_in_order()
{
    Runtime r;
    ps3_lv2_queue_attr attr = queue_attr();
    uint32_t queue = 0, port = 0;
    CHECK(ps3_lv2_queue_create(&r.objects, &queue, &attr, 0, 8) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_create(&r.objects, &port, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_connect_local(&r.objects, port, queue) == PS3_LV2_OK);
    for (uint64_t index = 0; index < 3; ++index)
        CHECK(ps3_lv2_port_send(&r.objects, port, index, index * 2, index * 3) == PS3_LV2_OK);
    ps3_lv2_event events[4]{};
    uint32_t number = 0xFFFF;
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 4, &number) == PS3_LV2_OK);
    CHECK(number == 3);
    for (uint64_t index = 0; index < 3; ++index) {
        CHECK(events[index].data1 == index);
        CHECK(events[index].data2 == index * 2);
        CHECK(events[index].data3 == index * 3);
    }
    /* Drained: a second call reports zero, not an error. */
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 4, &number) == PS3_LV2_OK && number == 0);
    /* A short array takes only what fits and leaves the rest queued, in order. */
    CHECK(ps3_lv2_port_send(&r.objects, port, 7, 8, 9) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&r.objects, port, 10, 11, 12) == PS3_LV2_OK);
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 1, &number) == PS3_LV2_OK && number == 1);
    CHECK(events[0].data1 == 7);
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 4, &number) == PS3_LV2_OK && number == 1);
    CHECK(events[0].data1 == 10);
    /* Validation. */
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, 0xDEADBEEF, events, 4, &number) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, nullptr, 4, &number) == PS3_LV2_EFAULT);
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 4, nullptr) == PS3_LV2_EFAULT);
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 0, &number) == PS3_LV2_EINVAL);
}

void queue_force_destroy_cancels_a_blocked_receiver()
{
    Runtime r;
    ps3_lv2_queue_attr attr = queue_attr();
    uint32_t queue = 0;
    CHECK(ps3_lv2_queue_create(&r.objects, &queue, &attr, 0, 4) == PS3_LV2_OK);
    QueueReceiver receiver{&r.objects, queue, {}, {}};
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, queue_receiver, &receiver) == 0);
    while (receiver.arrived.load() == 0) usleep(1000);
    /* Without force a registered receiver keeps the old EBUSY refusal. */
    CHECK(ps3_lv2_queue_destroy(&r.objects, queue) == PS3_LV2_EBUSY);
    /* Mode 1 (force) cancels the blocked receiver with ECANCELED. */
    CHECK(ps3_lv2_queue_destroy_force(&r.objects, queue, 1) == PS3_LV2_OK);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(receiver.code.load() == PS3_LV2_ECANCELED);
    CHECK(ps3_lv2_queue_destroy_force(&r.objects, queue, 0) == PS3_LV2_ESRCH);   // already gone
}

/* ---------- ports ---------- */

void port_send_to_a_disconnected_port_is_enotconn()
{
    Runtime r;
    ps3_lv2_queue_attr attr = queue_attr();
    uint32_t queue = 0, port = 0;
    CHECK(ps3_lv2_queue_create(&r.objects, &queue, &attr, 0, 4) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_create(&r.objects, &port, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK);
    /* Never connected: ENOTCONN. */
    CHECK(ps3_lv2_port_send(&r.objects, port, 1, 2, 3) == PS3_LV2_ENOTCONN);
    CHECK(ps3_lv2_port_connect_local(&r.objects, port, queue) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&r.objects, port, 1, 2, 3) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_disconnect(&r.objects, port) == PS3_LV2_OK);
    /* Disconnected again: back to ENOTCONN, and nothing is enqueued. */
    CHECK(ps3_lv2_port_send(&r.objects, port, 4, 5, 6) == PS3_LV2_ENOTCONN);
    ps3_lv2_event events[4]{};
    uint32_t number = 0;
    CHECK(ps3_lv2_queue_tryreceive(&r.objects, queue, events, 4, &number) == PS3_LV2_OK && number == 1);
    CHECK(events[0].data1 == 1);
    CHECK(ps3_lv2_port_disconnect(&r.objects, port) == PS3_LV2_ENOTCONN);
    /* Destroy refuses while connected, then succeeds. */
    CHECK(ps3_lv2_port_connect_local(&r.objects, port, queue) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_destroy(&r.objects, port) == PS3_LV2_EISCONN);
    CHECK(ps3_lv2_port_disconnect(&r.objects, port) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_destroy(&r.objects, port) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_destroy(&r.objects, port) == PS3_LV2_ESRCH);
}

/* ---------- capacity exhaustion ---------- */

void capacity_exhaustion_is_eagain()
{
    Runtime r;
    ps3_lv2_objects_limit(&r.objects, PS3_LV2_KIND_EVENT_FLAG, 2);
    uint32_t id = 0;
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, PS3_SYNC_WAITER_MULTIPLE, 0) == PS3_LV2_OK);
    const uint32_t first = id;
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, PS3_SYNC_WAITER_MULTIPLE, 0) == PS3_LV2_OK);
    /* The third refuses: capacity, not a made-up success. */
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, PS3_SYNC_WAITER_MULTIPLE, 0) == PS3_LV2_EAGAIN);
    CHECK(ps3_lv2_event_flag_destroy(&r.objects, first) == PS3_LV2_OK);
    CHECK(ps3_lv2_event_flag_create(&r.objects, &id, PS3_SYNC_FIFO, PS3_SYNC_WAITER_MULTIPLE, 0) == PS3_LV2_OK);
    ps3_lv2_objects_limit(&r.objects, PS3_LV2_KIND_SEMAPHORE, 1);
    ps3_lv2_semaphore_attr attr = fifo_attr();
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 1) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 1) == PS3_LV2_EAGAIN);
    CHECK(ps3_lv2_live_count(&r.objects, PS3_LV2_KIND_SEMAPHORE) == 1);
    CHECK(ps3_lv2_live_count(&r.objects, PS3_LV2_KIND_EVENT_FLAG) == 2);
}

/* ---------- cancellation ---------- */

void blocked_wait_stops_on_the_shared_cancel_token()
{
    Runtime r;
    ps3_lv2_semaphore_attr attr = fifo_attr();
    uint32_t id = 0;
    CHECK(ps3_lv2_semaphore_create(&r.objects, &id, &attr, 0, 1) == PS3_LV2_OK);
    ps3_notify_cancel cancel{0};
    __atomic_store_n(&cancel.requested, 1, __ATOMIC_SEQ_CST);
    /* Already requested: returns before sleeping, and never a fake success. */
    CHECK(ps3_lv2_semaphore_wait(&r.objects, id, 0, &cancel, nullptr) == PS3_LV2_CANCELLED);
    const uint32_t flag = make_flag(r, PS3_SYNC_WAITER_MULTIPLE, 0);
    uint64_t result = 0;
    CHECK(ps3_lv2_event_flag_wait(&r.objects, flag, 0x1, PS3_EVENT_FLAG_AND, &result, 0, &cancel, nullptr) == PS3_LV2_CANCELLED);
}

}  // namespace

int main()
{
    semaphore_create_validation();
    semaphore_trywait_and_post();
    semaphore_wait_is_woken_by_another_thread();
    semaphore_wait_times_out();
    event_flag_create_validation();
    event_flag_or_and_clear_semantics();
    event_flag_wait_is_woken();
    event_flag_single_waiter_refuses_a_second();
    event_flag_destroy_wakes_the_waiter();
    event_flag_wait_times_out();
    queue_tryreceive_drains_in_order();
    queue_force_destroy_cancels_a_blocked_receiver();
    port_send_to_a_disconnected_port_is_enotconn();
    capacity_exhaustion_is_eagain();
    blocked_wait_stops_on_the_shared_cancel_token();
    std::printf("LV2_SYNC_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
