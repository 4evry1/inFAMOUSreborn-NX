#include "elf_load.h"
#include <string.h>

enum {
    ELF_PERMISSION_EXECUTE = 1,
    ELF_PERMISSION_READ = 4,
    PPU_INSTRUCTION_BYTES = 4,
    PPU_ENTRY_OPD_BYTES = 8
};

static uint64_t page_start(const ElfImageSegment *s) {
    return (uint64_t)s->guest_address & ~(PS3_GUEST_PAGE_BYTES - 1);
}
static uint64_t page_end(const ElfImageSegment *s) {
    return ((uint64_t)s->guest_address + s->memory_size + PS3_GUEST_PAGE_BYTES - 1)
           & ~(PS3_GUEST_PAGE_BYTES - 1);
}
static int contains(const ElfLoadedImage *image, uint32_t address,
                    uint64_t length, uint32_t required_flags) {
    for (size_t i = 0; i < image->plan.segment_count; ++i) {
        const ElfImageSegment *s = &image->segments[i];
        uint64_t end = (uint64_t)s->guest_address + s->memory_size;
        if ((s->flags & required_flags) == required_flags &&
            address >= s->guest_address && address <= end && length <= end - address) {
            return 1;
        }
    }
    return 0;
}
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int tls_template_is_backed(const ElfLoadedImage *image) {
    const ElfImageSegment *tls = &image->plan.tls;
    if (!image->plan.has_tls || !tls->file_size) {
        return 1;
    }
    for (size_t i = 0; i < image->plan.segment_count; ++i) {
        const ElfImageSegment *load = &image->segments[i];
        if (!(load->flags & ELF_PERMISSION_READ) ||
            tls->guest_address < load->guest_address) {
            continue;
        }
        uint64_t displacement = (uint64_t)tls->guest_address - load->guest_address;
        if (displacement <= load->file_size &&
            tls->file_size <= load->file_size - displacement &&
            tls->file_offset == load->file_offset + displacement) {
            return 1;
        }
    }
    return 0;
}
ElfLoadReport elf_load_into_empty_space(const ElfImageReader *r,
                                        const ps3_guest_memory *m,
                                        void *scratch, size_t scratch_size,
                                        ElfLoadedImage *output) {
    ElfLoadReport report = {ELF_LOAD_OK, ELF_LOAD_OK, ELF_IMAGE_OK,
                            PS3_GUEST_MEMORY_OK, PS3_GUEST_MEMORY_OK};
    ElfLoadedImage image;
    size_t committed = 0;
    if (!r || !r->read || !m || !m->commit || !m->decommit || !m->read ||
        !m->write || !m->zero || !scratch || !scratch_size || !output) {
        report.status = ELF_LOAD_ARGUMENT;
        report.cause = ELF_LOAD_ARGUMENT;
        return report;
    }
    memset(&image, 0, sizeof image);
    report.image_status = elf_image_plan(r, image.segments, ELF_IMAGE_MAX_SEGMENTS, &image.plan);
    if (report.image_status != ELF_IMAGE_OK) {
        report.status = ELF_LOAD_IMAGE_INVALID;
        report.cause = ELF_LOAD_IMAGE_INVALID;
        return report;
    }
    if (!tls_template_is_backed(&image)) {
        report.status = ELF_LOAD_IMAGE_INVALID;
        report.cause = ELF_LOAD_IMAGE_INVALID;
        report.image_status = ELF_IMAGE_MALFORMED;
        return report;
    }
    for (size_t i = 0; i < image.plan.segment_count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (page_start(&image.segments[i]) < page_end(&image.segments[j]) &&
                page_start(&image.segments[j]) < page_end(&image.segments[i])) {
                report.status = ELF_LOAD_OVERLAP;
                report.cause = ELF_LOAD_OVERLAP;
                return report;
            }
        }
    }
    if (!contains(&image, image.plan.entry_opd, PPU_ENTRY_OPD_BYTES, ELF_PERMISSION_READ)) {
        report.status = ELF_LOAD_ENTRY_INVALID;
        report.cause = ELF_LOAD_ENTRY_INVALID;
        return report;
    }
    for (size_t i = 0; i < image.plan.segment_count; ++i) {
        const ElfImageSegment *s = &image.segments[i];
        report.memory_status = m->commit(m->context, (uint32_t)page_start(s),
                                         page_end(s) - page_start(s));
        if (report.memory_status != PS3_GUEST_MEMORY_OK) {
            report.status = ELF_LOAD_MEMORY_FAILURE;
            goto rollback;
        }
        ++committed;
    }
    for (size_t i = 0; i < image.plan.segment_count; ++i) {
        const ElfImageSegment *s = &image.segments[i];
        for (uint64_t offset = 0; offset < s->file_size;) {
            uint64_t remaining = s->file_size - offset;
            size_t n = remaining < scratch_size ? (size_t)remaining : scratch_size;
            ElfReadStatus read_status = r->read(r->context, s->file_offset + offset, scratch, n);
            if (read_status != ELF_READ_OK) {
                report.image_status = read_status == ELF_READ_TRUNCATED ?
                                      ELF_IMAGE_TRUNCATED : ELF_IMAGE_READ_FAILURE;
                report.status = ELF_LOAD_READER_FAILURE;
                goto rollback;
            }
            report.memory_status = m->write(m->context, (uint32_t)(s->guest_address + offset), scratch, n);
            if (report.memory_status != PS3_GUEST_MEMORY_OK) {
                report.status = ELF_LOAD_MEMORY_FAILURE;
                goto rollback;
            }
            offset += n;
        }
        if (s->memory_size > s->file_size) {
            report.memory_status = m->zero(m->context, (uint32_t)(s->guest_address + s->file_size),
                                           s->memory_size - s->file_size);
            if (report.memory_status != PS3_GUEST_MEMORY_OK) {
                report.status = ELF_LOAD_MEMORY_FAILURE;
                goto rollback;
            }
        }
    }
    {
        unsigned char opd[PPU_ENTRY_OPD_BYTES];
        report.memory_status = m->read(m->context, image.plan.entry_opd, opd, sizeof opd);
        if (report.memory_status != PS3_GUEST_MEMORY_OK) {
            report.status = ELF_LOAD_MEMORY_FAILURE;
            goto rollback;
        }
        image.entry_pc = be32(opd);
        image.entry_toc = be32(opd + 4);
        if (!image.entry_pc || image.entry_pc % PPU_INSTRUCTION_BYTES ||
            !contains(&image, image.entry_pc, PPU_INSTRUCTION_BYTES, ELF_PERMISSION_EXECUTE) ||
            (image.entry_toc && !contains(&image, image.entry_toc, 1, ELF_PERMISSION_READ))) {
            report.status = ELF_LOAD_ENTRY_INVALID;
            goto rollback;
        }
    }
    *output = image;
    return report;
rollback:
    report.cause = report.status;
    while (committed) {
        --committed;
        const ElfImageSegment *s = &image.segments[committed];
        ps3_guest_memory_status status = m->decommit(m->context, (uint32_t)page_start(s),
                                                    page_end(s) - page_start(s));
        if (status != PS3_GUEST_MEMORY_OK && report.cleanup_status == PS3_GUEST_MEMORY_OK) {
            report.cleanup_status = status;
        }
    }
    if (report.cleanup_status != PS3_GUEST_MEMORY_OK) {
        report.status = ELF_LOAD_CLEANUP_FAILURE;
    }
    return report;
}
