#ifndef PS3RECOMP_ELF_LOAD_H
#define PS3RECOMP_ELF_LOAD_H
#include "elf_image.h"
#include "../memory/guest_space.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    ELF_LOAD_OK, ELF_LOAD_ARGUMENT, ELF_LOAD_IMAGE_INVALID, ELF_LOAD_OVERLAP,
    ELF_LOAD_ENTRY_INVALID, ELF_LOAD_READER_FAILURE, ELF_LOAD_MEMORY_FAILURE,
    ELF_LOAD_CLEANUP_FAILURE
} ElfLoadStatus;
typedef struct {
    ElfLoadStatus status, cause;
    ElfImageStatus image_status;
    ps3_guest_memory_status memory_status, cleanup_status;
} ElfLoadReport;
typedef struct {
    ElfImagePlan plan;
    ElfImageSegment segments[ELF_IMAGE_MAX_SEGMENTS];
    uint32_t entry_pc, entry_toc;
} ElfLoadedImage;
/* PRECONDITION: caller has exclusive access to a fresh EMPTY address space for
 * the entire call. The port cannot query ownership; composition must enforce
 * this before invocation. Reader and memory contexts remain caller owned.
 * scratch must be nonempty, writable, and disjoint from inputs/output/contexts.
 * Output is published only on complete success; failures roll back every newly
 * committed range. CLEANUP_FAILURE preserves the original cause and reports the
 * first decommit failure; caller must retain ownership and destroy the space.
 * Page overlap is unsupported. Entry uses the existing PS3 two-BE32-word OPD.
 * A zero TOC is accepted; nonzero TOC must point within a readable LOAD.
 * Initialized TLS bytes must correspond to a readable file-backed LOAD;
 * the separately allocated thread-local zero tail need not fit that LOAD. */
ElfLoadReport elf_load_into_empty_space(const ElfImageReader *reader,
                                        const ps3_guest_memory *memory,
                                        void *scratch, size_t scratch_size,
                                        ElfLoadedImage *output);
#ifdef __cplusplus
}
#endif
#endif
