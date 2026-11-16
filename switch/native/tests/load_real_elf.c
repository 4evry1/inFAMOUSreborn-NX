#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "elf_load.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

static ElfReadStatus read_file(void *context,uint64_t offset,void *out,size_t n) {
    FILE *file=context;
    if(offset>INT64_MAX || fseeko(file,(off_t)offset,SEEK_SET)) return ELF_READ_ERROR;
    if(fread(out,1,n,file)==n) return ELF_READ_OK;
    return ferror(file)?ELF_READ_ERROR:ELF_READ_TRUNCATED;
}
/* Caller creates a fresh exclusively owned EMPTY space before this diagnostic
 * and destroys it afterwards, including on failure. No guest code is executed. */
int native_load_real_elf(const ps3_guest_memory *memory,const char *path,
                         void *scratch,size_t scratch_size) {
    if(!scratch || scratch_size<2 || scratch_size>16384) return 1;
    unsigned char *expected=scratch;
    size_t chunk_size=scratch_size/2;
    unsigned char *actual=expected+chunk_size;
    ElfLoadedImage image;
    FILE *file=fopen(path,"rb");
    if(!file) { printf("ELF_LOAD_REAL_FAIL open\n");return 1; }
    if(fseeko(file,0,SEEK_END)) { fclose(file);printf("ELF_LOAD_REAL_FAIL seek\n");return 1; }
    off_t size=ftello(file);
    if(size<0) { fclose(file);printf("ELF_LOAD_REAL_FAIL size\n");return 1; }
    ElfImageReader reader={read_file,file,(uint64_t)size};
    ElfLoadReport report=elf_load_into_empty_space(&reader,memory,expected,chunk_size,&image);
    if(report.status!=ELF_LOAD_OK) {
        fclose(file);
        printf("ELF_LOAD_REAL_FAIL status=%d cause=%d image=%d memory=%d cleanup=%d\n",
               report.status,report.cause,report.image_status,report.memory_status,report.cleanup_status);
        return 1;
    }
    uint64_t verified_file=0,verified_zero=0;
    for(size_t i=0;i<image.plan.segment_count;++i) {
        const ElfImageSegment *s=&image.segments[i];
        for(uint64_t offset=0;offset<s->memory_size;) {
            int is_file=offset<s->file_size;
            uint64_t remaining=(is_file?s->file_size:s->memory_size)-offset;
            size_t n=remaining<chunk_size?(size_t)remaining:chunk_size;
            if(is_file) {
                if(read_file(file,s->file_offset+offset,expected,n)!=ELF_READ_OK) goto verify_failure;
            } else memset(expected,0,n);
            if(memory->read(memory->context,(uint32_t)(s->guest_address+offset),actual,n)!=PS3_GUEST_MEMORY_OK ||
               memcmp(expected,actual,n)) goto verify_failure;
            if(is_file) verified_file+=n;else verified_zero+=n;
            offset+=n;
        }
    }
    if(fclose(file)) { printf("ELF_LOAD_REAL_FAIL close\n");return 1; }
    printf("ELF_LOAD_REAL_PASS segments=%zu entry_opd=0x%08" PRIx32
           " pc=0x%08" PRIx32 " toc=0x%08" PRIx32
           " file_bytes=%" PRIu64 " bss_bytes=%" PRIu64 "\n",
           image.plan.segment_count,image.plan.entry_opd,image.entry_pc,image.entry_toc,
           verified_file,verified_zero);
    return 0;
verify_failure:
    fclose(file);printf("ELF_LOAD_REAL_FAIL readback\n");return 1;
}
#ifdef ELF_LOAD_HOST_MAIN
#include "guest_space_host.h"
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    ElfTestHostSpace *space=elf_test_host_space_create(16u*1024u*1024u);
    if(!space) return 2;
    ps3_guest_memory memory=elf_test_host_space_port(space);
    unsigned char scratch[16384];
    int status=native_load_real_elf(&memory,argv[1],scratch,sizeof scratch);
    elf_test_host_space_destroy(space);return status;
}
#endif
