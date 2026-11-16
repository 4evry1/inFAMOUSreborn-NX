#ifndef PS3RECOMP_ELF_IMAGE_H
#define PS3RECOMP_ELF_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { ELF_IMAGE_MAX_SEGMENTS = 64, ELF_IMAGE_MAX_HEADERS = 1024 };
typedef enum { ELF_READ_OK, ELF_READ_TRUNCATED, ELF_READ_ERROR } ElfReadStatus;
/* Read exactly length bytes. The context and backing image must remain stable
 * for the call. No callback receives a range outside the declared image size. */
typedef ElfReadStatus (*ElfImageRead)(void *context, uint64_t offset,
                                    void *destination, size_t length);
typedef struct {
    ElfImageRead read;
    void *context;
    uint64_t size;
} ElfImageReader;
typedef enum {
    ELF_IMAGE_OK, ELF_IMAGE_ARGUMENT, ELF_IMAGE_MALFORMED,
    ELF_IMAGE_TRUNCATED, ELF_IMAGE_READ_FAILURE, ELF_IMAGE_CAPACITY,
    ELF_IMAGE_UNSUPPORTED
} ElfImageStatus;
typedef struct {
    uint64_t file_offset, file_size, memory_size, alignment;
    uint32_t guest_address, flags;
} ElfImageSegment;
typedef struct {
    uint32_t size, magic, version, sdk_version;
    int32_t primary_priority;
    /* Raw stack selector (0x10..0x70) or page-aligned byte count, unresolved. */
    uint32_t primary_stack_size;
    uint32_t malloc_page_size, ppc_segment;
} ElfImageProcessParameters;
typedef struct {
    uint32_t entry_opd;
    size_t segment_count;
    int has_tls;
    ElfImageSegment tls;
    int has_process_parameters;
    ElfImageProcessParameters process_parameters;
} ElfImagePlan;
/* ELF64, big endian, PPC64 ET_EXEC only; extended header numbering unsupported.
 * Outputs are unchanged on failure. Output storage must not alias reader state
 * or each other. The raw ELF entry is the PS3 entry OPD address; this parser
 * neither resolves it nor proves it is backed by a loaded segment. Mapping,
 * overlap policy, permissions, TLS coverage, and payload reads belong to caller.
 * Only nonempty PT_LOAD descriptors consume capacity. Limits above bound work
 * and stack use, independently of untrusted image metadata.
 * Optional PS3 process parameters use a unique 32-byte PT0x60000001 record.
 * Known versions 1/0x8400/0x9000/0x330000 share this supported layout; unknown
 * versions, default sentinels and unsupported encodings fail explicitly. Raw
 * signed priority/stack values are preserved. Privilege checks, selector
 * resolution and native capability/default policy belong to composition.
 * Absence publishes has_process_parameters=0 and zero metadata, never defaults. */
ElfImageStatus elf_image_plan(const ElfImageReader *reader,
                              ElfImageSegment *segments, size_t capacity,
                              ElfImagePlan *plan);
#ifdef __cplusplus
}
#endif
#endif
