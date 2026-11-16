#include "atomic_domain.h"
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    ElfTestHostSpace *backing = elf_test_host_space_create(4 * 1048576);
    assert(backing);
    ps3_pthread_guard domain_guard, allocation_guard;
    assert(!ps3_pthread_guard_init(&domain_guard) && !ps3_pthread_guard_init(&allocation_guard));
    ps3_atomic_domain domain = {0};
    ps3_atomic_owner_record owners[2]; ps3_atomic_pin_record pins[2];
    assert(ps3_atomic_domain_init(&domain, elf_test_host_space_port(backing),
                                  ps3_pthread_guard_port(&domain_guard), owners, 2, pins, 2) == 0);
    ps3_user_memory allocator; ps3_user_memory_slot slots[4];
    ps3_user_memory_config config = {0, 4 * 1048576, 2 * 1048576,
                                     ps3_atomic_domain_memory(&domain), ps3_pthread_guard_port(&allocation_guard)};
    assert(ps3_user_memory_init(&allocator, &config, slots, 4) == PS3_USER_MEMORY_OK);
    uint32_t address;
    assert(ps3_user_memory_allocate(&allocator, 1048576, PS3_USER_MEMORY_ALIGN_1M, &address).status == PS3_USER_MEMORY_OK);
    ps3_atomic_pin pin;
    assert(ps3_atomic_pin_acquire(&domain, address + 4, 24, &pin) == 0);
    ps3_user_memory_result blocked = ps3_user_memory_free(&allocator, address);
    assert(blocked.status == PS3_USER_MEMORY_MEMORY_FAILURE && blocked.memory_status == PS3_GUEST_MEMORY_BUSY);
    ps3_user_memory_info info;
    assert(ps3_user_memory_query(&allocator, &info) == PS3_USER_MEMORY_OK && info.used_bytes == 1048576 && info.allocation_count == 1);
    assert(ps3_atomic_pin_release(&domain, &pin) == 0);
    assert(ps3_user_memory_free(&allocator, address).status == PS3_USER_MEMORY_OK);
    assert(ps3_user_memory_query(&allocator, &info) == PS3_USER_MEMORY_OK && info.used_bytes == 0 && info.allocation_count == 0);
    assert(!ps3_pthread_guard_destroy(&allocation_guard) && !ps3_pthread_guard_destroy(&domain_guard));
    elf_test_host_space_destroy(backing);
    puts("ATOMIC_USER_MEMORY_PIN_BUSY_PASS");
    return 0;
}
