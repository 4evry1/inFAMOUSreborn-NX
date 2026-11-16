#define main existing_kernel_idle_test_main
#include "test_spurs_kernel1_service.c"
#undef main

typedef struct {
    ps3_atomic_domain *domain;
    ps3_spurs_policy_execution_request requests[8];
    unsigned begins, steps[8], releases, cancels;
    unsigned fail_release;
    unsigned repeat_dispatch, exits;
} Policy;
static ps3_spurs_policy_execution_result policy_begin(void *context, const ps3_spurs_policy_execution_request *request) {
    Policy *p = context;
    CHECK(request->module_address == 0x69c880 && request->module_size == 16);
    CHECK(request->data == UINT64_C(0x404e9300) && request->workload_id == 0);
    CHECK(request->local_store[0xA00] == (p->repeat_dispatch && p->begins ? 0x99 : 0x42) && request->select);
    CHECK(request->module_reloaded == (p->begins == 0));
    CHECK(request->domain == p->domain && request->owner.domain == p->domain);
    p->requests[request->spu_index] = *request; p->steps[request->spu_index] = 0; ++p->begins;
    return (ps3_spurs_policy_execution_result){.status = PS3_SPURS_POLICY_READY};
}
static ps3_spurs_policy_execution_result policy_step(void *context, uint32_t spu, uint64_t budget) {
    Policy *p = context;
    CHECK(budget > 0 && budget <= 1024);
    const ps3_spurs_policy_execution_request *r = &p->requests[spu];
    if (++p->steps[spu] == 1)
        return (ps3_spurs_policy_execution_result){.status = PS3_SPURS_POLICY_SLICE_CONTINUE, .resumable = 1};
    uint64_t selection = UINT64_MAX;
    CHECK(r->select(r->selection_context, spu, 1, &selection) == 0);
    CHECK((selection >> 32) == 0);
    if (p->repeat_dispatch && p->exits++ == 0) {
        r->local_store[0xA00] = 0x99;
        return (ps3_spurs_policy_execution_result){.status = PS3_SPURS_POLICY_KERNEL_EXIT};
    }
    unsigned char zero = 0;
    CHECK(ps3_atomic_owner_write(p->domain, r->owner, B, &zero, 1) == PS3_GUEST_MEMORY_OK);
    return (ps3_spurs_policy_execution_result){.status = PS3_SPURS_POLICY_KERNEL_EXIT};
}
static ps3_spurs_policy_execution_result policy_resume(void *context, uint32_t spu, uint64_t selection) {
    (void)context; (void)spu; (void)selection;
    CHECK(0); return (ps3_spurs_policy_execution_result){.status = PS3_SPURS_POLICY_FAULT};
}
static ps3_spurs_policy_execution_status policy_cancel(void *context, uint32_t spu) {
    (void)spu; ++((Policy *)context)->cancels; return PS3_SPURS_POLICY_CANCELLED;
}
static ps3_spurs_policy_execution_status policy_release(void *context, uint32_t spu) {
    (void)spu; Policy *p = context; ++p->releases;
    return p->fail_release ? PS3_SPURS_POLICY_BUSY : PS3_SPURS_POLICY_READY;
}
static ps3_spurs_policy_execution_port policy_port(Policy *p) {
    return (ps3_spurs_policy_execution_port){p, policy_begin, policy_step, policy_resume, policy_cancel, policy_release};
}
/* Activation has no ready count: it must acknowledge the service request and
 * publish per-SPU status, then genuinely block rather than inventing work. */
static void activation(void) {
    unsigned char image[0x1000];
    build_instance(image, 2);
    memset(image + 0x100, 0, 0x800);
    memset(image + 0xB00, 0, 0x200);
    image[0x80] = 2; /* primary-bank RUNNABLE */
    image[0xBD] = 3; /* update workload for both real SPU contexts */
    image[0x72] = 3;
    image[0x71] = 0; image[0x70] = 0;
    Fixture f;
    setup(&f, 2, image);
    ps3_spurs_k1_config config = config_of(&f, NULL);
    CHECK(ps3_spurs_k1_init(&f.service, &config) == PS3_SPURS_K1_OK);
    Policy policy = {.domain = &f.domain}; ps3_spurs_policy_execution_port port = policy_port(&policy);
    CHECK(ps3_spurs_k1_set_policy_execution(&f.service, &port) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop;
    CHECK(ps3_spurs_k1_start(&f.service, &stop) == PS3_SPURS_K1_OK);
    unsigned char after[0x1000]; read_instance(&f, after);
    CHECK(after[0xBD] == 0 && after[0x72] == 0);
    CHECK(after[0x90] == 3 && after[0x80] == 2);
    CHECK(after[0x20] == 0 && after[0x30] == 0);
    CHECK(after[0x73] == 3);
    CHECK(f.ls[0][0x1EC] == 0x80 && f.ls[0][0x1ED] == 0);
    CHECK(f.ls[1][0x1EC] == 0x80 && f.ls[1][0x1ED] == 0);
    CHECK(!ps3_spurs_k1_poll_stop(&f.service, &stop));
    CHECK(policy.begins == 0);
    CHECK(ps3_spurs_k1_set_policy_execution(&f.service, &port) == PS3_SPURS_K1_INVALID);
    teardown(&f);
    puts("SPURS_WORKLOAD_SCHEDULER activation PASS");
}
static void dispatch_and_exit(unsigned fail_release, unsigned repeat_dispatch) {
    unsigned char image[0x1000]; build_instance(image, 2);
    memset(image + 0xB00, 0, 0x200);
    image[0x80] = 2; image[0xBD] = image[0x72] = 3;
    image[0] = 1; image[0x40] = 1; image[0x50] = 5;
    put32(image + 0xB04, 0x69c880); put32(image + 0xB0C, 0x404e9300);
    put32(image + 0xB10, 16); image[0xB18] = image[0xB19] = 2;
    Fixture f; setup(&f, 2, image);
    unsigned char bytes[16] = {0x42};
    CHECK(ps3_atomic_write(&f.domain, 0x69c880, bytes, sizeof bytes) == PS3_GUEST_MEMORY_OK);
    Policy policy = {.domain = &f.domain}; ps3_spurs_policy_execution_port port = policy_port(&policy);
    policy.fail_release = fail_release;
    policy.repeat_dispatch = repeat_dispatch;
    ps3_spurs_k1_config config = config_of(&f, NULL);
    CHECK(ps3_spurs_k1_init(&f.service, &config) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_set_policy_execution(&f.service, &port) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop;
    CHECK(ps3_spurs_k1_start(&f.service, &stop) == (fail_release ? PS3_SPURS_K1_STOPPED : PS3_SPURS_K1_OK));
    if (fail_release) {
        CHECK(stop.reason == PS3_SPURS_K1_STOP_PROVIDER_FAILURE);
        CHECK(ps3_spurs_k1_cancel_and_join(&f.service) != 0);
        CHECK(f.service.worker_joined && f.service.contexts[0].policy_active);
        CHECK(ps3_spurs_k1_init(&f.service, &config) == PS3_SPURS_K1_INVALID);
        CHECK(ps3_atomic_owner_read(&f.domain, f.owners[0], B, bytes, sizeof bytes) == PS3_GUEST_MEMORY_OK);
        policy.fail_release = 0;
        CHECK(ps3_spurs_k1_cancel_and_join(&f.service) == 0);
        CHECK(!f.service.contexts[0].policy_active && policy.cancels > 0);
        teardown(&f);
        puts("SPURS_WORKLOAD_SCHEDULER failed release retains owner/LS until retry PASS");
        return;
    }
    CHECK(policy.begins == 1 + repeat_dispatch && policy.steps[0] == 2 && policy.releases == 1 + repeat_dispatch && policy.cancels == 0);
    unsigned char after[0x1000]; read_instance(&f, after);
    CHECK(after[0] == 0 && after[0x20] == 0 && after[0x30] == 0);
    CHECK(after[0x90] == 3 && after[0x73] == 3);
    teardown(&f);
    puts("SPURS_WORKLOAD_SCHEDULER real selection/port continuation/exit PASS");
}
static void registration_wake(void) {
    unsigned char image[0x1000]; build_instance(image, 2);
    memset(image + 0xB00, 0, 0x200);
    Fixture f; setup(&f, 2, image);
    Policy policy = {.domain = &f.domain}; ps3_spurs_policy_execution_port port = policy_port(&policy);
    ps3_spurs_k1_config config = config_of(&f, NULL);
    CHECK(ps3_spurs_k1_init(&f.service, &config) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_set_policy_execution(&f.service, &port) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop;
    CHECK(ps3_spurs_k1_start(&f.service, &stop) == PS3_SPURS_K1_OK);
    CHECK(policy.begins == 0);
    uint64_t settled = ps3_spurs_k1_settled_count(&f.service);
    unsigned char info[0x20] = {0}, bytes[16] = {0x42};
    put32(info + 4, 0x69c880); put32(info + 12, 0x404e9300); put32(info + 16, 16);
    info[0x18] = info[0x19] = 2;
    CHECK(ps3_atomic_write(&f.domain, 0x69c880, bytes, sizeof bytes) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f.domain, B + 0xB00, info, sizeof info) == PS3_GUEST_MEMORY_OK);
    unsigned char line1[128];
    CHECK(ps3_atomic_read(&f.domain, B + 0x80, line1, sizeof line1) == PS3_GUEST_MEMORY_OK);
    line1[0] = 2; line1[0xBD - 0x80] = 3;
    CHECK(ps3_atomic_write(&f.domain, B + 0x80, line1, sizeof line1) == PS3_GUEST_MEMORY_OK);
    /* Neither metadata nor L1 invalidates the idle L0 reservation. */
    CHECK(policy.begins == 0);
    unsigned char line0[128];
    CHECK(ps3_atomic_read(&f.domain, B, line0, sizeof line0) == PS3_GUEST_MEMORY_OK);
    line0[0] = 1; line0[0x40] = 1; line0[0x50] = 5; line0[0x72] = 3;
    CHECK(ps3_atomic_write(&f.domain, B, line0, sizeof line0) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_spurs_k1_wait_settled_after(&f.service, settled, UINT64_MAX) == PS3_SPURS_K1_PHASE_QUIESCENT);
    CHECK(policy.begins == 1 && policy.releases == 1 && policy.cancels == 0);
    unsigned char after[0x1000]; read_instance(&f, after);
    CHECK(after[0xBD] == 0 && after[0x72] == 0 && after[0x20] == 0 && after[0x30] == 0);
    teardown(&f);
    puts("SPURS_WORKLOAD_SCHEDULER coherent registration then L0 reservation wake PASS");
}
int main(void) { activation(); dispatch_and_exit(0, 0); dispatch_and_exit(1, 0); dispatch_and_exit(0, 1); registration_wake(); return 0; }
