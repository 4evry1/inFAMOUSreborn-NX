#define _GNU_SOURCE
#include "memory/user_memory.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define UNIT UINT64_C(65536)
#define BASE UINT32_C(0x40000000)
typedef struct host { unsigned char *bytes; int fail_commit, fail_decommit; uint32_t busy_address; } host;
static ps3_guest_memory_status commit(void *ctx, uint32_t a, uint64_t n) {
 host *h=ctx; if(h->fail_commit) return PS3_GUEST_MEMORY_NO_MEMORY;
 assert(a>=BASE && (uint64_t)a+n<=BASE+16*UNIT);
 return mprotect(h->bytes+a-BASE,n,PROT_READ|PROT_WRITE)==0?PS3_GUEST_MEMORY_OK:PS3_GUEST_MEMORY_PLATFORM_FAILURE;
}
static ps3_guest_memory_status decommit(void *ctx,uint32_t a,uint64_t n) {
 host *h=ctx; if(h->busy_address==a) return PS3_GUEST_MEMORY_BUSY; if(h->fail_decommit) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
 if(madvise(h->bytes+a-BASE,n,MADV_DONTNEED)) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
 assert(mprotect(h->bytes+a-BASE,n,PROT_NONE)==0); return PS3_GUEST_MEMORY_OK;
}
static void lock(void *p){ assert(pthread_mutex_lock(p)==0); }
static void unlock(void *p){ assert(pthread_mutex_unlock(p)==0); }
static void *worker(void *p) { ps3_user_memory *m=p; for(int i=0;i<2000;i++){ uint32_t a; assert(ps3_user_memory_allocate(m,UNIT,UNIT,&a).status==PS3_USER_MEMORY_OK); assert(ps3_user_memory_free(m,a).status==PS3_USER_MEMORY_OK); } return NULL; }
typedef struct free_request {
    ps3_user_memory *memory;
    uint32_t address;
    ps3_user_memory_result result;
} free_request;
static void *free_worker(void *context) {
    free_request *request = context;
    request->result = ps3_user_memory_free(request->memory, request->address);
    return NULL;
}
int main(void) {
 host h={.bytes=mmap(NULL,16*UNIT,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)}; assert(h.bytes!=MAP_FAILED);
 pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
 ps3_user_memory_slot slots[16]; ps3_user_memory m;
 ps3_user_memory_config c={.window_begin=BASE,.window_end=BASE+16*UNIT,.quota_bytes=8*UNIT,.memory={.context=&h,.commit=commit,.decommit=decommit},.guard={.context=&mutex,.lock=lock,.unlock=unlock}};
 assert(ps3_user_memory_init(&m,&c,slots,16)==PS3_USER_MEMORY_OK);
 uint32_t a=7,b,d;
 assert(ps3_user_memory_allocate(&m,0,UNIT,&a).status==PS3_USER_MEMORY_INVALID_ARGUMENT && a==7);
 assert(ps3_user_memory_allocate(&m,UNIT+1,UNIT,&a).status==PS3_USER_MEMORY_INVALID_ARGUMENT && a==7);
 assert(ps3_user_memory_allocate(&m,UNIT,4096,&a).status==PS3_USER_MEMORY_INVALID_ARGUMENT);
 assert(ps3_user_memory_allocate(&m,UINT64_MAX,UNIT,&a).status==PS3_USER_MEMORY_INVALID_ARGUMENT);
 h.fail_commit=1; assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&a).status==PS3_USER_MEMORY_MEMORY_FAILURE && a==7); h.fail_commit=0;
 assert(ps3_user_memory_allocate(&m,2*UNIT,UNIT,&a).status==PS3_USER_MEMORY_OK && a==BASE);
 assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&b).status==PS3_USER_MEMORY_OK && b==BASE+2*UNIT);
 memset(h.bytes,0x5a,2*UNIT); h.fail_decommit=1;
 assert(ps3_user_memory_free(&m,a).status==PS3_USER_MEMORY_MEMORY_FAILURE && h.bytes[0]==0x5a);
 ps3_user_memory_info info; assert(ps3_user_memory_query(&m,&info)==PS3_USER_MEMORY_OK && info.used_bytes==3*UNIT);
 assert(ps3_user_memory_free(&m,a+1).status==PS3_USER_MEMORY_NOT_OWNED);
 h.fail_decommit=0; assert(ps3_user_memory_free(&m,a).status==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_allocate(&m,2*UNIT,UNIT,&d).status==PS3_USER_MEMORY_OK && d==a && h.bytes[0]==0);
 assert(ps3_user_memory_allocate(&m,6*UNIT,UNIT,&a).status==PS3_USER_MEMORY_QUOTA_EXCEEDED);
 h.fail_decommit=1; assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_MEMORY_FAILURE); h.fail_decommit=0;
 assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_free(&m,d).status==PS3_USER_MEMORY_NOT_OWNED);
 assert(ps3_user_memory_allocate(&m,8*UNIT,UNIT,&a).status==PS3_USER_MEMORY_OK);
 b=91; assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&b).status==PS3_USER_MEMORY_QUOTA_EXCEEDED && b==91);
 assert(ps3_user_memory_query(&m,&info)==PS3_USER_MEMORY_OK && info.available_bytes==0 && info.used_bytes==8*UNIT);
 assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_OK);
 pthread_t threads[4]; for(int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,&m));
 for(int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
 assert(ps3_user_memory_query(&m,&info)==PS3_USER_MEMORY_OK && info.used_bytes==0 && info.allocation_count==0);
 assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&a).status==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&b).status==PS3_USER_MEMORY_OK);
 h.busy_address=a;
 free_request requests[2]={{.memory=&m,.address=a},{.memory=&m,.address=b}};
 pthread_t freeing[2];
 for(int i=0;i<2;i++) assert(!pthread_create(&freeing[i],NULL,free_worker,&requests[i]));
 for(int i=0;i<2;i++) assert(!pthread_join(freeing[i],NULL));
 assert(requests[0].result.status==PS3_USER_MEMORY_MEMORY_FAILURE && requests[0].result.memory_status==PS3_GUEST_MEMORY_BUSY);
 assert(requests[1].result.status==PS3_USER_MEMORY_OK && requests[1].result.memory_status==PS3_GUEST_MEMORY_OK);
 assert(ps3_user_memory_query(&m,&info)==PS3_USER_MEMORY_OK && info.used_bytes==UNIT);
 h.busy_address=0; assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_OK);
 c.quota_bytes=16*UNIT; assert(ps3_user_memory_init(&m,&c,slots,1)==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&a).status==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_allocate(&m,UNIT,UNIT,&b).status==PS3_USER_MEMORY_CAPACITY_EXCEEDED);
 assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_OK);
 assert(ps3_user_memory_allocate(&m,16*UNIT,16*UNIT,&a).status==PS3_USER_MEMORY_OK && a==BASE);
 assert(ps3_user_memory_cleanup(&m).status==PS3_USER_MEMORY_OK);
 c.window_begin=BASE+UNIT; c.window_end=BASE+17*UNIT; c.quota_bytes=16*UNIT;
 assert(ps3_user_memory_init(&m,&c,slots,16)==PS3_USER_MEMORY_OK);
 a=19; assert(ps3_user_memory_allocate(&m,16*UNIT,16*UNIT,&a).status==PS3_USER_MEMORY_NO_SPACE && a==19);
 c.window_end=UINT64_MAX; assert(ps3_user_memory_init(&m,&c,slots,16)==PS3_USER_MEMORY_INVALID_ARGUMENT);
 assert(!munmap(h.bytes,16*UNIT)); assert(!pthread_mutex_destroy(&mutex)); puts("user_memory: PASS real page-backed allocation, failure transactions, reuse, quota, capacity, concurrent accounting");
}
