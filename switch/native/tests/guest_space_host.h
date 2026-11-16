#ifndef TEST_GUEST_SPACE_HOST_H
#define TEST_GUEST_SPACE_HOST_H
#include "../../../ps3recomp/runtime/memory/guest_space.h"
typedef struct ElfTestHostSpace ElfTestHostSpace;
/* Bounded host test adapter, not a 4 GiB production address-space backend. */
ElfTestHostSpace *elf_test_host_space_create(size_t bytes);
void elf_test_host_space_destroy(ElfTestHostSpace *space);
ps3_guest_memory elf_test_host_space_port(ElfTestHostSpace *space);
/* Decision 135: the direct resolver of ps3_atomic_domain_set_direct for this space (context = the space): the host address of a mapped range,
 * else NULL. Reads the page map without the space's lock, like the console's flat-space resolver (tests do not unmap memory under DMA). */
const void *elf_test_host_space_direct(void *space, uint32_t address, uint64_t length);
#endif
