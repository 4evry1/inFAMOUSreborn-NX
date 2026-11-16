#include "tls_initialize.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOURCE_PAGE = 0x785000, DESTINATION = 0x0e000000, PAGE_SIZE = 4096, DEST_SIZE = 131072 };
typedef struct {
    unsigned char source[PAGE_SIZE], destination[DEST_SIZE], mapped[DEST_SIZE / PAGE_SIZE];
    unsigned commits, reads, writes, zeros, decommits;
    unsigned fail_commit, fail_read, fail_write, fail_zero, fail_decommit;
    int source_mapped;
} Fixture;
static unsigned tests;
static unsigned char *range(Fixture *f, uint32_t address, uint64_t size, int *source) {
    if (address >= SOURCE_PAGE && address <= SOURCE_PAGE + PAGE_SIZE &&
        size <= SOURCE_PAGE + PAGE_SIZE - address) {
        *source = 1;
        return f->source + address - SOURCE_PAGE;
    }
    if (address >= DESTINATION && address <= DESTINATION + DEST_SIZE &&
        size <= DESTINATION + DEST_SIZE - address) {
        *source = 0;
        return f->destination + address - DESTINATION;
    }
    return NULL;
}
static int mapped(Fixture *f, uint32_t address, uint64_t size) {
    if (!size) return 1;
    for (uint64_t p = ((uint64_t)address - DESTINATION) / PAGE_SIZE;
         p <= ((uint64_t)address - DESTINATION + size - 1) / PAGE_SIZE; ++p) {
        if (!f->mapped[p]) return 0;
    }
    return 1;
}
static ps3_guest_memory_status commit(void *context, uint32_t address, uint64_t size) {
    Fixture *f = context;
    if (++f->commits == f->fail_commit) return PS3_GUEST_MEMORY_NO_MEMORY;
    int source;
    unsigned char *p = range(f, address, size, &source);
    if (!p || !size || address % PAGE_SIZE || size % PAGE_SIZE)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (source) {
        if (!f->source_mapped) memset(p, 0, (size_t)size);
        f->source_mapped = 1;
        return PS3_GUEST_MEMORY_OK;
    }
    for (uint64_t i = (address - DESTINATION) / PAGE_SIZE;
         i < ((uint64_t)address - DESTINATION + size) / PAGE_SIZE; ++i) {
        if (!f->mapped[i]) memset(f->destination + i * PAGE_SIZE, 0, PAGE_SIZE);
        f->mapped[i] = 1;
    }
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status decommit(void *context, uint32_t address, uint64_t size) {
    Fixture *f = context;
    if (++f->decommits == f->fail_decommit) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    int source;
    unsigned char *p = range(f, address, size, &source);
    if (!p || !size || address % PAGE_SIZE || size % PAGE_SIZE)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    memset(p, 0, (size_t)size);
    if (source) {
        f->source_mapped = 0;
        return PS3_GUEST_MEMORY_OK;
    }
    memset(f->mapped + (address - DESTINATION) / PAGE_SIZE, 0, (size_t)size / PAGE_SIZE);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status transfer(Fixture *f, uint32_t address, void *buffer, uint64_t size, int operation) {
    int source;
    unsigned char *p = range(f, address, size, &source);
    if (!p) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (source && !f->source_mapped) return PS3_GUEST_MEMORY_UNMAPPED;
    if (!source && !mapped(f, address, size)) return PS3_GUEST_MEMORY_UNMAPPED;
    if (operation == 0) memcpy(buffer, p, (size_t)size);
    else if (operation == 1) memcpy(p, buffer, (size_t)size);
    else memset(p, 0, (size_t)size);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status read_memory(void *context, uint32_t address, void *out, size_t size) {
    Fixture *f = context;
    if (++f->reads == f->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return transfer(f, address, out, size, 0);
}
static ps3_guest_memory_status write_memory(void *context, uint32_t address, const void *in, size_t size) {
    Fixture *f = context;
    if (++f->writes == f->fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return transfer(f, address, (void *)in, size, 1);
}
static ps3_guest_memory_status zero_memory(void *context, uint32_t address, uint64_t size) {
    Fixture *f = context;
    if (++f->zeros == f->fail_zero) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return transfer(f, address, NULL, size, 2);
}
static void init(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->source_mapped = 1;
    f->source[0xe7c] = 0x12;
    f->source[0xe7d] = 0x34;
    f->source[0xe7e] = 0xab;
    f->source[0xe7f] = 0xcd;
}
static Ps3TlsReport initialize(Fixture *f, Ps3TlsArena *arena, Ps3TlsTemplate template, Ps3TlsImage *image) {
    ps3_guest_memory memory = {f, commit, decommit, read_memory, write_memory, zero_memory};
    unsigned char scratch[2];
    return ps3_tls_initialize(&memory, arena, &template, scratch, sizeof scratch, image);
}
static void failure(Fixture *f, Ps3TlsArena arena, Ps3TlsTemplate template,
                    Ps3TlsStatus status, unsigned commits, unsigned decommits) {
    Ps3TlsArena old_arena = arena;
    Ps3TlsImage image, old_image;
    memset(&image, 0xa5, sizeof image);
    old_image = image;
    Ps3TlsReport report = initialize(f, &arena, template, &image);
    assert(report.status == status);
    assert(!memcmp(&arena, &old_arena, sizeof arena));
    assert(!memcmp(&image, &old_image, sizeof image));
    assert(f->commits == commits && f->decommits == decommits);
    if (status != PS3_TLS_CLEANUP_FAILURE) {
        for (size_t i = 0; i < sizeof f->mapped; ++i) assert(!f->mapped[i]);
    } else {
        assert(report.cause == PS3_TLS_MEMORY_FAILURE);
        assert(report.cleanup_status == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    }
    assert(f->source[0xe7c] == 0x12 && f->source[0xe7f] == 0xcd);
    ++tests;
}
int main(void) {
    Fixture *f = malloc(sizeof *f);
    assert(f);
    Ps3TlsTemplate template = {0x785e7c, 4, 0x200};
    Ps3TlsArena arena = {DESTINATION, DESTINATION + DEST_SIZE};
    Ps3TlsImage image;
    init(f);
    Ps3TlsReport report = initialize(f, &arena, template, &image);
    assert(report.status == PS3_TLS_OK);
    assert(image.block == DESTINATION && image.thread_pointer == DESTINATION + 0x7000);
    assert(image.allocation_size == 0x9000 && arena.next == DESTINATION + 0x9000);
    assert(!memcmp(f->destination, f->source + 0xe7c, 4));
    for (size_t i = 4; i < 0x9000; ++i) assert(!f->destination[i]);
    assert(f->commits == 1 && f->reads == 2 && f->writes == 2 && f->zeros == 1);
    ++tests;
    /* A second call owns a distinct block and preserves the previous TLS. */
    report = initialize(f, &arena, template, &image);
    assert(report.status == PS3_TLS_OK && image.block == DESTINATION + 0x9000);
    assert(!memcmp(f->destination, f->source + 0xe7c, 4));
    assert(!memcmp(f->destination + 0x9000, f->source + 0xe7c, 4));
    ++tests;
    arena = (Ps3TlsArena){DESTINATION, DESTINATION + DEST_SIZE};
    init(f); f->fail_commit = 1; failure(f, arena, template, PS3_TLS_MEMORY_FAILURE, 1, 0);
    init(f); f->fail_read = 2; failure(f, arena, template, PS3_TLS_MEMORY_FAILURE, 1, 1);
    init(f); f->fail_write = 2; failure(f, arena, template, PS3_TLS_MEMORY_FAILURE, 1, 1);
    init(f); f->fail_zero = 1; failure(f, arena, template, PS3_TLS_MEMORY_FAILURE, 1, 1);
    init(f); f->fail_read = 1; f->fail_decommit = 1;
    failure(f, arena, template, PS3_TLS_CLEANUP_FAILURE, 1, 1);
    init(f); f->source_mapped = 0;
    failure(f, arena, template, PS3_TLS_MEMORY_FAILURE, 1, 1);
    init(f); template.memory_size = 3; failure(f, arena, template, PS3_TLS_INVALID_RANGE, 0, 0);
    template = (Ps3TlsTemplate){0x785e7c, 4, UINT64_MAX};
    init(f); failure(f, arena, template, PS3_TLS_INVALID_RANGE, 0, 0);
    template = (Ps3TlsTemplate){UINT32_MAX, 4, 512};
    init(f); failure(f, arena, template, PS3_TLS_INVALID_RANGE, 0, 0);
    template = (Ps3TlsTemplate){DESTINATION, 4, 512};
    init(f); failure(f, arena, template, PS3_TLS_OVERLAP, 0, 0);
    template = (Ps3TlsTemplate){0, 4, 512};
    init(f); failure(f, arena, template, PS3_TLS_INVALID_RANGE, 0, 0);
    template = (Ps3TlsTemplate){0x785e7c, 4, 512};
    init(f); failure(f, (Ps3TlsArena){DESTINATION, DESTINATION + 0x8000}, template, PS3_TLS_NO_SPACE, 0, 0);
    init(f); failure(f, (Ps3TlsArena){DESTINATION + 1, DESTINATION + DEST_SIZE}, template, PS3_TLS_INVALID_RANGE, 0, 0);
    init(f); failure(f, (Ps3TlsArena){DESTINATION + DEST_SIZE, DESTINATION}, template, PS3_TLS_INVALID_RANGE, 0, 0);
    init(f); failure(f, (Ps3TlsArena){DESTINATION, PS3_GUEST_ADDRESS_BYTES + 4096}, template, PS3_TLS_INVALID_RANGE, 0, 0);
    init(f); failure(f, (Ps3TlsArena){PS3_GUEST_ADDRESS_BYTES - 0x8000, PS3_GUEST_ADDRESS_BYTES}, template, PS3_TLS_NO_SPACE, 0, 0);
    init(f); template = (Ps3TlsTemplate){0, 0, 0};
    report = initialize(f, &arena, template, &image);
    assert(report.status == PS3_TLS_OK && image.allocation_size == 0x8000);
    assert(!f->reads && !f->writes && !f->zeros); ++tests;
    assert(ps3_tls_initialize(NULL, NULL, NULL, NULL, 0, NULL).status == PS3_TLS_ARGUMENT); ++tests;
    free(f);
    printf("TLS_INITIALIZE_PASS tests=%u\n", tests);
    return 0;
}
