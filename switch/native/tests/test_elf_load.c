#include "elf_load.h"
#include "guest_space_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char file[512], bytes[65536], mapped[16];
    unsigned commits, decommits, writes, zeros, reads;
    unsigned fail_commit, fail_write, fail_zero, fail_read, fail_decommit;
    uint64_t fail_file_at;
    int truncate_file;
    uint32_t rollback[16];
} Fixture;
static unsigned tests;
static void put(unsigned char *p,uint64_t v,unsigned n) {
    while(n) { p[--n]=(unsigned char)v; v>>=8; }
}
static void init(Fixture *f) {
    memset(f,0,sizeof *f); f->fail_file_at=UINT64_MAX;
    unsigned char *b=f->file;
    memcpy(b,"\177ELF",4); b[4]=2;b[5]=2;b[6]=1;
    put(b+16,2,2);put(b+18,21,2);put(b+20,1,4);put(b+24,0x3000,8);
    put(b+32,64,8);put(b+52,64,2);put(b+54,56,2);put(b+56,2,2);
    for(unsigned i=0;i<2;++i) {
        unsigned char *p=b+64+i*56;
        put(p,1,4);put(p+4,i?6:5,4);put(p+8,i?384:256,8);
        put(p+16,i?0x3000:0x1000,8);put(p+32,64,8);put(p+40,128,8);put(p+48,1,8);
    }
    for(unsigned i=256;i<512;++i) b[i]=(unsigned char)i;
    put(b+384,0x1000,4);put(b+388,0x3040,4);
}
static void add_tls(Fixture *f) {
    unsigned char *p=f->file+176;
    put(f->file+56,3,2);
    put(p,7,4);
    put(p+4,4,4);
    put(p+8,400,8);
    put(p+16,0x3010,8);
    put(p+32,8,8);
    /* Thread-local zero tail need not be backed by the process LOAD. */
    put(p+40,512,8);
    put(p+48,1,8);
}
static ElfReadStatus read_file(void *ctx,uint64_t off,void *out,size_t n) {
    Fixture *f=ctx;
    if(off>=f->fail_file_at) return f->truncate_file?ELF_READ_TRUNCATED:ELF_READ_ERROR;
    if(off>sizeof f->file || n>sizeof f->file-off) return ELF_READ_TRUNCATED;
    memcpy(out,f->file+off,n);return ELF_READ_OK;
}
static int in_range(uint32_t a,uint64_t n) { return a<=65536 && n<=65536-a; }
static int is_mapped(Fixture *f,uint32_t a,uint64_t n) {
    if(!in_range(a,n)) return 0;
    if(n) for(uint64_t p=a/4096;p<=((uint64_t)a+n-1)/4096;++p) if(!f->mapped[p]) return 0;
    return 1;
}
static ps3_guest_memory_status commit(void *ctx,uint32_t a,uint64_t n) {
    Fixture *f=ctx; ++f->commits;
    if(f->commits==f->fail_commit) return PS3_GUEST_MEMORY_NO_MEMORY;
    if(!n || a%4096 || n%4096 || !in_range(a,n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    for(uint64_t p=a/4096;p<((uint64_t)a+n)/4096;++p) if(!f->mapped[p]) {
        memset(f->bytes+p*4096,0,4096); f->mapped[p]=1;
    }
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status decommit(void *ctx,uint32_t a,uint64_t n) {
    Fixture *f=ctx; f->rollback[f->decommits++]=a;
    if(f->decommits==f->fail_decommit) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if(!n || a%4096 || n%4096 || !in_range(a,n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    for(uint64_t p=a/4096;p<((uint64_t)a+n)/4096;++p) {
        f->mapped[p]=0;memset(f->bytes+p*4096,0,4096);
    }
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status read_memory(void *ctx,uint32_t a,void *out,size_t n) {
    Fixture *f=ctx;if(++f->reads==f->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if(!is_mapped(f,a,n)) return PS3_GUEST_MEMORY_UNMAPPED;
    memcpy(out,f->bytes+a,n);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status write_memory(void *ctx,uint32_t a,const void *in,size_t n) {
    Fixture *f=ctx;if(++f->writes==f->fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if(!is_mapped(f,a,n)) return PS3_GUEST_MEMORY_UNMAPPED;
    memcpy(f->bytes+a,in,n);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status zero_memory(void *ctx,uint32_t a,uint64_t n) {
    Fixture *f=ctx;if(++f->zeros==f->fail_zero) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if(!is_mapped(f,a,n)) return PS3_GUEST_MEMORY_UNMAPPED;
    memset(f->bytes+a,0,(size_t)n);return PS3_GUEST_MEMORY_OK;
}
static ElfLoadReport load(Fixture *f,ElfLoadedImage *out) {
    unsigned char scratch[16]; ElfImageReader r={read_file,f,sizeof f->file};
    ps3_guest_memory m={f,commit,decommit,read_memory,write_memory,zero_memory};
    return elf_load_into_empty_space(&r,&m,scratch,sizeof scratch,out);
}
static void fail(Fixture *f,ElfLoadStatus expected,unsigned commits,unsigned decommits) {
    ElfLoadedImage out,old;memset(&out,0xa5,sizeof out);old=out;
    ElfLoadReport report=load(f,&out);
    assert(report.status==expected && memcmp(&out,&old,sizeof out)==0);
    if(expected==ELF_LOAD_CLEANUP_FAILURE) {
        assert(report.cause==ELF_LOAD_MEMORY_FAILURE);
        assert(report.cleanup_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    } else assert(report.cause==expected);
    assert(f->commits==commits && f->decommits==decommits);
    if(expected!=ELF_LOAD_CLEANUP_FAILURE) for(unsigned i=0;i<16;++i) assert(!f->mapped[i]);
    ++tests;
}
int main(void) {
    Fixture *f=malloc(sizeof *f); assert(f); ElfLoadedImage image;
    init(f);assert(load(f,&image).status==ELF_LOAD_OK);
    assert(image.entry_pc==0x1000 && image.entry_toc==0x3040 && image.plan.segment_count==2);
    assert(!memcmp(f->bytes+0x1000,f->file+256,64));assert(!memcmp(f->bytes+0x3000,f->file+384,64));
    for(unsigned i=64;i<128;++i) assert(f->bytes[0x1000+i]==0 && f->bytes[0x3000+i]==0);
    assert(f->commits==2 && f->decommits==0);++tests;
    init(f);f->fail_file_at=288;fail(f,ELF_LOAD_READER_FAILURE,2,2);
    assert(f->rollback[0]==0x3000 && f->rollback[1]==0x1000);
    init(f);f->fail_file_at=288;f->truncate_file=1;fail(f,ELF_LOAD_READER_FAILURE,2,2);
    init(f);f->fail_commit=1;fail(f,ELF_LOAD_MEMORY_FAILURE,1,0);
    init(f);f->fail_commit=2;fail(f,ELF_LOAD_MEMORY_FAILURE,2,1);
    init(f);f->fail_write=2;fail(f,ELF_LOAD_MEMORY_FAILURE,2,2);
    init(f);f->fail_zero=1;fail(f,ELF_LOAD_MEMORY_FAILURE,2,2);
    init(f);f->fail_read=1;fail(f,ELF_LOAD_MEMORY_FAILURE,2,2);
    init(f);f->fail_write=2;f->fail_decommit=1;fail(f,ELF_LOAD_CLEANUP_FAILURE,2,2);
    init(f);put(f->file+64+56+16,0x1080,8);fail(f,ELF_LOAD_OVERLAP,0,0);
    init(f);put(f->file+24,0x307c,8);fail(f,ELF_LOAD_ENTRY_INVALID,0,0);
    init(f);put(f->file+384,0x3000,4);fail(f,ELF_LOAD_ENTRY_INVALID,2,2);
    init(f);put(f->file+384,0x1001,4);fail(f,ELF_LOAD_ENTRY_INVALID,2,2);
    init(f);put(f->file+388,0x9000,4);fail(f,ELF_LOAD_ENTRY_INVALID,2,2);
    init(f);put(f->file+388,0,4);assert(load(f,&image).status==ELF_LOAD_OK && !image.entry_toc);++tests;
    init(f);f->file[0]=0;fail(f,ELF_LOAD_IMAGE_INVALID,0,0);
    init(f);add_tls(f);
    assert(load(f,&image).status==ELF_LOAD_OK && image.plan.has_tls);++tests;
    init(f);add_tls(f);put(f->file+176+8,401,8);
    fail(f,ELF_LOAD_IMAGE_INVALID,0,0);
    init(f);add_tls(f);put(f->file+176+16,0x3050,8);put(f->file+176+8,464,8);
    fail(f,ELF_LOAD_IMAGE_INVALID,0,0);
    init(f);add_tls(f);put(f->file+64+56+4,2,4);
    fail(f,ELF_LOAD_IMAGE_INVALID,0,0);
    init(f);add_tls(f);put(f->file+176+16,0x9000,8);
    fail(f,ELF_LOAD_IMAGE_INVALID,0,0);
    init(f);add_tls(f);put(f->file+176+32,0,8);put(f->file+176+16,0x9000,8);
    assert(load(f,&image).status==ELF_LOAD_OK);++tests;
    /* Real bounded host adapter: recommit preserves bytes and operations that
     * cross an unmapped page fail before touching the committed prefix. */
    {
        ElfTestHostSpace *host=elf_test_host_space_create(65536);assert(host);
        ps3_guest_memory m=elf_test_host_space_port(host);
        unsigned char value=0xa5,check=0,buffer[32];memset(buffer,0xcc,sizeof buffer);
        assert(m.commit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK);
        assert(m.write(m.context,0x1fff,&value,1)==PS3_GUEST_MEMORY_OK);
        assert(m.commit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK);
        assert(m.write(m.context,0x1ff0,buffer,sizeof buffer)==PS3_GUEST_MEMORY_UNMAPPED);
        assert(m.zero(m.context,0x1ff0,32)==PS3_GUEST_MEMORY_UNMAPPED);
        assert(m.read(m.context,0x1fff,&check,1)==PS3_GUEST_MEMORY_OK && check==value);
        assert(m.read(m.context,0x1ff0,buffer,sizeof buffer)==PS3_GUEST_MEMORY_UNMAPPED);
        for(size_t i=0;i<sizeof buffer;++i) assert(buffer[i]==0xcc);
        assert(m.decommit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK);
        assert(m.commit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK);
        assert(m.read(m.context,0x1fff,&check,1)==PS3_GUEST_MEMORY_OK && check==0);
        assert(m.decommit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK);++tests;
        init(f);unsigned char scratch[16];ElfImageReader r={read_file,f,sizeof f->file};
        assert(elf_load_into_empty_space(&r,&m,scratch,sizeof scratch,&image).status==ELF_LOAD_OK);
        assert(m.read(m.context,0x1000,buffer,32)==PS3_GUEST_MEMORY_OK);
        assert(!memcmp(buffer,f->file+256,32));++tests;
        elf_test_host_space_destroy(host);
        host=elf_test_host_space_create(65536);assert(host);m=elf_test_host_space_port(host);
        f->fail_file_at=288;
        assert(elf_load_into_empty_space(&r,&m,scratch,sizeof scratch,&image).status==ELF_LOAD_READER_FAILURE);
        assert(m.read(m.context,0x1000,buffer,1)==PS3_GUEST_MEMORY_UNMAPPED);
        assert(m.read(m.context,0x3000,buffer,1)==PS3_GUEST_MEMORY_UNMAPPED);++tests;
        elf_test_host_space_destroy(host);
    }
    assert(elf_load_into_empty_space(NULL,NULL,NULL,0,NULL).status==ELF_LOAD_ARGUMENT);++tests;
    free(f);printf("ELF_LOAD_PASS tests=%u\n",tests);return 0;
}
