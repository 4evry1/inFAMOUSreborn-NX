#include "guest_space_host.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
struct ElfTestHostSpace {
    unsigned char *bytes,*mapped;
    size_t size;
    pthread_mutex_t lock;
};
static int valid(ElfTestHostSpace *s,uint32_t a,uint64_t n) {
    return a<=s->size && n<=s->size-a;
}
static int mapped(ElfTestHostSpace *s,uint32_t a,uint64_t n) {
    if(!valid(s,a,n)) return 0;
    if(n) for(uint64_t p=a/4096;p<=((uint64_t)a+n-1)/4096;++p) if(!s->mapped[p]) return 0;
    return 1;
}
static ps3_guest_memory_status pages(void *ctx,uint32_t a,uint64_t n,int committing) {
    ElfTestHostSpace *s=ctx;
    pthread_mutex_lock(&s->lock);
    if(!n || a%4096 || n%4096 || !valid(s,a,n)) {
        pthread_mutex_unlock(&s->lock);return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    for(uint64_t p=a/4096;p<((uint64_t)a+n)/4096;++p) {
        if(!committing || !s->mapped[p]) memset(s->bytes+p*4096,0,4096);
        s->mapped[p]=(unsigned char)committing;
    }
    pthread_mutex_unlock(&s->lock);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status commit(void *ctx,uint32_t a,uint64_t n) { return pages(ctx,a,n,1); }
static ps3_guest_memory_status decommit(void *ctx,uint32_t a,uint64_t n) { return pages(ctx,a,n,0); }
static ps3_guest_memory_status transfer(void *ctx,uint32_t a,void *buffer,uint64_t n,int op) {
    ElfTestHostSpace *s=ctx;
    pthread_mutex_lock(&s->lock);
    if(!valid(s,a,n)) { pthread_mutex_unlock(&s->lock);return PS3_GUEST_MEMORY_INVALID_RANGE; }
    if(!mapped(s,a,n)) { pthread_mutex_unlock(&s->lock);return PS3_GUEST_MEMORY_UNMAPPED; }
    if(n) {
        if(op==0) memcpy(buffer,s->bytes+a,(size_t)n);
        else if(op==1) memcpy(s->bytes+a,buffer,(size_t)n);
        else memset(s->bytes+a,0,(size_t)n);
    }
    pthread_mutex_unlock(&s->lock);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status read_memory(void *ctx,uint32_t a,void *p,size_t n) {
    return transfer(ctx,a,p,n,0);
}
static ps3_guest_memory_status write_memory(void *ctx,uint32_t a,const void *p,size_t n) {
    return transfer(ctx,a,(void *)p,n,1);
}
static ps3_guest_memory_status zero_memory(void *ctx,uint32_t a,uint64_t n) {
    return transfer(ctx,a,NULL,n,2);
}
ElfTestHostSpace *elf_test_host_space_create(size_t bytes) {
    if(!bytes || bytes%4096 || bytes>64u*1024u*1024u) return NULL;
    ElfTestHostSpace *s=calloc(1,sizeof *s);
    if(!s) return NULL;
    s->bytes=calloc(1,bytes);s->mapped=calloc(1,bytes/4096);s->size=bytes;
    if(!s->bytes || !s->mapped || pthread_mutex_init(&s->lock,NULL)) {
        free(s->bytes);free(s->mapped);free(s);return NULL;
    }
    return s;
}
void elf_test_host_space_destroy(ElfTestHostSpace *s) {
    if(s) { pthread_mutex_destroy(&s->lock);free(s->bytes);free(s->mapped);free(s); }
}
const void *elf_test_host_space_direct(void *space,uint32_t a,uint64_t n) {
    ElfTestHostSpace *s=space;
    if(!s || !n || !mapped(s,a,n)) return NULL;
    return s->bytes+a;
}
ps3_guest_memory elf_test_host_space_port(ElfTestHostSpace *s) {
    ps3_guest_memory m={s,commit,decommit,read_memory,write_memory,zero_memory};return m;
}
