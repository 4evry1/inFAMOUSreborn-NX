#include "elf_image.h"
#include <string.h>

enum {
    ELF_CLASS_64 = 2,
    ELF_DATA_BIG_ENDIAN = 2,
    ELF_VERSION_CURRENT = 1,
    ELF_TYPE_EXECUTABLE = 2,
    ELF_MACHINE_PPC64 = 21,
    ELF_PROGRAM_LOAD = 1,
    ELF_PROGRAM_TLS = 7,
    ELF_PROGRAM_PS3_PROCESS_PARAMETERS = 0x60000001,
    PS3_PROCESS_PARAMETER_BYTES = 32,
    PS3_PROCESS_PARAMETER_MAGIC = 0x13bcc5f6,
    PS3_PROCESS_STACK_MINIMUM = 0x1000,
    PS3_PROCESS_STACK_MAXIMUM = 0x100000,
    PS3_PROCESS_PRIORITY_MINIMUM = -512,
    PS3_PROCESS_PRIORITY_MAXIMUM = 3071,
    PS3_PROCESS_PRIORITY_DEFAULT = -32768
};

static uint64_t big_endian(const unsigned char *p, size_t n) {
    uint64_t value = 0;
    while (n > 0) {
        value = (value << 8) | *p;
        ++p;
        --n;
    }
    return value;
}
static int fits(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}
static ElfImageStatus read_bytes(const ElfImageReader *r, uint64_t offset,
                                void *out, size_t length) {
    if (!fits(offset, length, r->size)) return ELF_IMAGE_TRUNCATED;
    switch (r->read(r->context, offset, out, length)) {
    case ELF_READ_OK: return ELF_IMAGE_OK;
    case ELF_READ_TRUNCATED: return ELF_IMAGE_TRUNCATED;
    default: return ELF_IMAGE_READ_FAILURE;
    }
}
static ElfImageStatus decode_segment(const unsigned char *p, uint64_t size,
                                    ElfImageSegment *s) {
    uint64_t address = big_endian(p + 16, 8);
    s->flags = (uint32_t)big_endian(p + 4, 4);
    s->file_offset = big_endian(p + 8, 8);
    s->file_size = big_endian(p + 32, 8);
    s->memory_size = big_endian(p + 40, 8);
    s->alignment = big_endian(p + 48, 8);
    if (s->file_size > s->memory_size) return ELF_IMAGE_MALFORMED;
    if (!fits(s->file_offset, s->file_size, size)) return ELF_IMAGE_TRUNCATED;
    if (address > UINT32_MAX || !fits(address, s->memory_size, UINT64_C(0x100000000)))
        return ELF_IMAGE_MALFORMED;
    if (s->alignment > 1 &&
        ((s->alignment & (s->alignment - 1)) ||
         (s->memory_size && address % s->alignment != s->file_offset % s->alignment)))
        return ELF_IMAGE_MALFORMED;
    s->guest_address = (uint32_t)address;
    return ELF_IMAGE_OK;
}
static ElfImageStatus decode_process_parameters(
    const ElfImageReader *reader, const ElfImageSegment *segment,
    ElfImageProcessParameters *parameters) {
    unsigned char image[PS3_PROCESS_PARAMETER_BYTES];
    if (segment->file_size < sizeof(image)) return ELF_IMAGE_MALFORMED;
    if (segment->file_size > sizeof(image)) return ELF_IMAGE_UNSUPPORTED;
    ElfImageStatus status = read_bytes(reader, segment->file_offset, image, sizeof(image));
    if (status != ELF_IMAGE_OK) return status;
    uint32_t priority_bits = (uint32_t)big_endian(image + 16, 4);
    ElfImageProcessParameters value = {
        .size = (uint32_t)big_endian(image, 4),
        .magic = (uint32_t)big_endian(image + 4, 4),
        .version = (uint32_t)big_endian(image + 8, 4),
        .sdk_version = (uint32_t)big_endian(image + 12, 4),
        .primary_priority = priority_bits <= INT32_MAX ? (int32_t)priority_bits :
            -1 - (int32_t)(UINT32_MAX - priority_bits),
        .primary_stack_size = (uint32_t)big_endian(image + 20, 4),
        .malloc_page_size = (uint32_t)big_endian(image + 24, 4),
        .ppc_segment = (uint32_t)big_endian(image + 28, 4)
    };
    if (value.size != sizeof(image) || value.magic != PS3_PROCESS_PARAMETER_MAGIC)
        return ELF_IMAGE_MALFORMED;
    if (value.version != 1 && value.version != 0x8400 &&
        value.version != 0x9000 && value.version != 0x330000)
        return ELF_IMAGE_UNSUPPORTED;
    if (value.primary_priority == PS3_PROCESS_PRIORITY_DEFAULT ||
        value.primary_stack_size == UINT32_MAX || value.sdk_version == UINT32_MAX) return ELF_IMAGE_UNSUPPORTED;
    if (value.primary_priority < PS3_PROCESS_PRIORITY_MINIMUM ||
        value.primary_priority > PS3_PROCESS_PRIORITY_MAXIMUM) return ELF_IMAGE_MALFORMED;
    int stack_selector = value.primary_stack_size >= 0x10 &&
        value.primary_stack_size <= 0x70 && value.primary_stack_size % 0x10 == 0;
    int stack_bytes = value.primary_stack_size >= PS3_PROCESS_STACK_MINIMUM &&
        value.primary_stack_size <= PS3_PROCESS_STACK_MAXIMUM &&
        value.primary_stack_size % PS3_PROCESS_STACK_MINIMUM == 0;
    if (!stack_selector && !stack_bytes) return ELF_IMAGE_MALFORMED;
    if (value.malloc_page_size != 0 && value.malloc_page_size != 0x10000 &&
        value.malloc_page_size != 0x100000) return ELF_IMAGE_UNSUPPORTED;
    if (value.ppc_segment > 2) return ELF_IMAGE_UNSUPPORTED;
    *parameters = value;
    return ELF_IMAGE_OK;
}
ElfImageStatus elf_image_plan(const ElfImageReader *r, ElfImageSegment *segments,
                              size_t capacity, ElfImagePlan *plan) {
    unsigned char header[64], ph[56];
    ElfImageSegment staged[ELF_IMAGE_MAX_SEGMENTS];
    ElfImagePlan result;
    ElfImageStatus status;
    uint64_t table, entry;
    uint16_t stride, count;
    size_t used = 0;
    if (!r || !r->read || !plan || (capacity && !segments)) return ELF_IMAGE_ARGUMENT;
    status = read_bytes(r, 0, header, sizeof header);
    if (status != ELF_IMAGE_OK) return status;
    if (memcmp(header, "\177ELF", 4)) return ELF_IMAGE_MALFORMED;
    if (header[4] != ELF_CLASS_64 || header[5] != ELF_DATA_BIG_ENDIAN)
        return ELF_IMAGE_UNSUPPORTED;
    if (header[6] != ELF_VERSION_CURRENT || big_endian(header + 20, 4) != ELF_VERSION_CURRENT)
        return ELF_IMAGE_MALFORMED;
    if (big_endian(header + 16, 2) != ELF_TYPE_EXECUTABLE ||
        big_endian(header + 18, 2) != ELF_MACHINE_PPC64)
        return ELF_IMAGE_UNSUPPORTED;
    if (big_endian(header + 52, 2) != 64) return ELF_IMAGE_MALFORMED;
    table = big_endian(header + 32, 8);
    stride = (uint16_t)big_endian(header + 54, 2);
    count = (uint16_t)big_endian(header + 56, 2);
    if (count == 0xffff || count > ELF_IMAGE_MAX_HEADERS) return ELF_IMAGE_UNSUPPORTED;
    if (stride != 56 || !count || table < 64) return ELF_IMAGE_MALFORMED;
    if (!fits(table, (uint64_t)stride * count, r->size)) return ELF_IMAGE_TRUNCATED;
    entry = big_endian(header + 24, 8);
    if (entry > UINT32_MAX) return ELF_IMAGE_UNSUPPORTED;
    memset(&result, 0, sizeof result);
    result.entry_opd = (uint32_t)entry;
    for (uint16_t i = 0; i < count; ++i) {
        ElfImageSegment segment;
        uint32_t type;
        status = read_bytes(r, table + (uint64_t)i * stride, ph, sizeof ph);
        if (status != ELF_IMAGE_OK) return status;
        type = (uint32_t)big_endian(ph, 4);
        if (type != ELF_PROGRAM_LOAD && type != ELF_PROGRAM_TLS &&
            type != ELF_PROGRAM_PS3_PROCESS_PARAMETERS) continue;
        memset(&segment, 0, sizeof segment);
        status = decode_segment(ph, r->size, &segment);
        if (status != ELF_IMAGE_OK) return status;
        if (type == ELF_PROGRAM_PS3_PROCESS_PARAMETERS) {
            if (result.has_process_parameters) return ELF_IMAGE_MALFORMED;
            status = decode_process_parameters(r, &segment, &result.process_parameters);
            if (status != ELF_IMAGE_OK) return status;
            result.has_process_parameters = 1;
        } else if (type == ELF_PROGRAM_TLS) {
            if (result.has_tls) return ELF_IMAGE_MALFORMED;
            result.has_tls = 1;
            result.tls = segment;
        } else if (segment.memory_size) {
            if (used == ELF_IMAGE_MAX_SEGMENTS || used == capacity) return ELF_IMAGE_CAPACITY;
            staged[used] = segment;
            ++used;
        }
    }
    if (!used) return ELF_IMAGE_MALFORMED;
    result.segment_count = used;
    memcpy(segments, staged, used * sizeof staged[0]);
    *plan = result;
    return ELF_IMAGE_OK;
}
