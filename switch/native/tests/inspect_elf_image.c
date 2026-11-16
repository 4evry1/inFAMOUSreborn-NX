#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "elf_image.h"
#include <inttypes.h>
#include <stdio.h>
#include <sys/types.h>

static ElfReadStatus file_read(void *context, uint64_t offset,
                               void *destination, size_t length) {
    FILE *file = context;
    if (offset > INT64_MAX || fseeko(file,(off_t)offset,SEEK_SET)) return ELF_READ_ERROR;
    if (fread(destination,1,length,file)==length) return ELF_READ_OK;
    return ferror(file) ? ELF_READ_ERROR : ELF_READ_TRUNCATED;
}
static void print_segment(const ElfImageSegment *s) {
    printf("{\"guest_address\":%" PRIu32 ",\"file_offset\":%" PRIu64
           ",\"file_size\":%" PRIu64 ",\"memory_size\":%" PRIu64
           ",\"flags\":%" PRIu32 ",\"alignment\":%" PRIu64 "}",
           s->guest_address,s->file_offset,s->file_size,s->memory_size,s->flags,s->alignment);
}
int main(int argc, char **argv) {
    ElfImageSegment segments[ELF_IMAGE_MAX_SEGMENTS];
    ElfImagePlan plan;
    if (argc!=2) { fprintf(stderr,"usage: inspect_elf_image ELF\n"); return 2; }
    FILE *file=fopen(argv[1],"rb");
    if (!file) { perror("open ELF"); return 2; }
    if (fseeko(file,0,SEEK_END)) { perror("seek ELF"); fclose(file); return 2; }
    off_t size=ftello(file);
    if (size<0) { perror("size ELF"); fclose(file); return 2; }
    ElfImageReader reader={file_read,file,(uint64_t)size};
    ElfImageStatus status=elf_image_plan(&reader,segments,ELF_IMAGE_MAX_SEGMENTS,&plan);
    int close_error=fclose(file);
    if (status!=ELF_IMAGE_OK || close_error) {
        fprintf(stderr,"ELF inspection failed: parser_status=%d close_error=%d\n",status,close_error);
        return 1;
    }
    printf("{\"status\":\"ok\",\"file_size\":%" PRIu64 ",\"entry_opd\":%" PRIu32
           ",\"segments\":[",reader.size,plan.entry_opd);
    for (size_t i=0;i<plan.segment_count;++i) {
        if (i) putchar(',');
        print_segment(&segments[i]);
    }
    printf("],\"tls\":");
    if (plan.has_tls) print_segment(&plan.tls); else printf("null");
    printf(",\"process_parameters\":");
    if (plan.has_process_parameters) {
        const ElfImageProcessParameters *parameters = &plan.process_parameters;
        printf("{\"size\":%" PRIu32 ",\"magic\":%" PRIu32
               ",\"version\":%" PRIu32 ",\"sdk_version\":%" PRIu32
               ",\"primary_priority\":%" PRId32 ",\"primary_stack_size\":%" PRIu32
               ",\"malloc_page_size\":%" PRIu32 ",\"ppc_segment\":%" PRIu32 "}",
               parameters->size, parameters->magic, parameters->version, parameters->sdk_version,
               parameters->primary_priority, parameters->primary_stack_size,
               parameters->malloc_page_size, parameters->ppc_segment);
    } else printf("null");
    printf("}\n");
    return ferror(stdout) ? 1 : 0;
}
