#include "../../../ps3recomp/runtime/sysutil/callback_registry.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    pthread_mutex_t mutex;
    unsigned held, locks, unlocks;
    ps3_sysutil_callback_registry registry;
} fixture;
static void lock_guard(void *context) {
    fixture *f=context; assert(pthread_mutex_lock(&f->mutex)==0);
    assert(!f->held); f->held=1; ++f->locks;
}
static void unlock_guard(void *context) {
    fixture *f=context; assert(f->held); f->held=0; ++f->unlocks;
    assert(pthread_mutex_unlock(&f->mutex)==0);
}
static ps3_instance_guard guard(fixture *f) {
    ps3_instance_guard g={f,lock_guard,unlock_guard}; return g;
}
static void setup(fixture *f) {
    memset(f,0,sizeof(*f)); assert(pthread_mutex_init(&f->mutex,0)==0);
}
static void start(fixture *f) {
    setup(f); assert(ps3_sysutil_callback_init(&f->registry,guard(f))==PS3_SYSUTIL_CALLBACK_OK);
}
static void finish(fixture *f) {
    assert(!f->held && f->locks==f->unlocks);
    assert(pthread_mutex_destroy(&f->mutex)==0);
}
static void equal_pair(ps3_sysutil_callback_pair a, ps3_sysutil_callback_pair b) {
    assert(a.callback_descriptor_address==b.callback_descriptor_address && a.userdata_address==b.userdata_address);
}
static void initialization(void) {
    fixture f; setup(&f); ps3_sysutil_callback_registry before;
    memcpy(&before,&f.registry,sizeof(before));
    assert(ps3_sysutil_callback_init(0,guard(&f))==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    ps3_instance_guard bad=guard(&f); bad.lock=0;
    assert(ps3_sysutil_callback_init(&f.registry,bad)==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    bad=guard(&f); bad.unlock=0;
    assert(ps3_sysutil_callback_init(&f.registry,bad)==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    assert(!memcmp(&f.registry,&before,sizeof(before)) && !f.locks);
    assert(ps3_sysutil_callback_init(&f.registry,guard(&f))==PS3_SYSUTIL_CALLBACK_OK);
    assert(f.registry.initialized);
    for(uint32_t i=0;i<PS3_SYSUTIL_CALLBACK_SLOT_COUNT;++i) {
        ps3_sysutil_callback_pair p={1,2};
        assert(ps3_sysutil_callback_read(&f.registry,i,&p)==PS3_SYSUTIL_CALLBACK_OK);
        equal_pair(p,(ps3_sysutil_callback_pair){0,0});
    }
    assert(ps3_sysutil_callback_register(&f.registry,2,(ps3_sysutil_callback_pair){7,9}).status==PS3_SYSUTIL_CALLBACK_OK);
    memcpy(&before,&f.registry,sizeof(before));
    assert(ps3_sysutil_callback_init(&f.registry,guard(&f))==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    assert(!memcmp(&f.registry,&before,sizeof(before))); finish(&f);
}
static void invalid_operations(void) {
    fixture f; setup(&f); ps3_sysutil_callback_pair p={0x12345678,0x87654321}, before=p;
    assert(ps3_sysutil_callback_register(0,0,p).status==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    assert(ps3_sysutil_callback_register(&f.registry,0,p).status==PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED);
    ps3_sysutil_callback_result absent=ps3_sysutil_callback_register(0,UINT32_MAX,p);
    assert(absent.status==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT && absent.slot==UINT32_MAX);
    equal_pair(absent.previous,(ps3_sysutil_callback_pair){0,0});
    equal_pair(absent.current,(ps3_sysutil_callback_pair){0,0});
    absent=ps3_sysutil_callback_register(&f.registry,3,p);
    assert(absent.status==PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED && absent.slot==3);
    equal_pair(absent.previous,(ps3_sysutil_callback_pair){0,0});
    equal_pair(absent.current,(ps3_sysutil_callback_pair){0,0});
    assert(ps3_sysutil_callback_read(0,0,&p)==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    assert(ps3_sysutil_callback_read(&f.registry,0,&p)==PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED);
    equal_pair(p,before); assert(!f.locks);
    assert(ps3_sysutil_callback_init(&f.registry,guard(&f))==PS3_SYSUTIL_CALLBACK_OK);
    assert(ps3_sysutil_callback_read(&f.registry,0,0)==PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT);
    assert(ps3_sysutil_callback_register(&f.registry,0,p).status==PS3_SYSUTIL_CALLBACK_OK);
    const uint32_t invalid[]={4,UINT32_MAX,UINT32_C(0x80000000)};
    for(unsigned i=0;i<3;++i) {
        ps3_sysutil_callback_registry saved; memcpy(&saved,&f.registry,sizeof(saved));
        ps3_sysutil_callback_result r=ps3_sysutil_callback_register(&f.registry,invalid[i],(ps3_sysutil_callback_pair){1,2});
        assert(r.status==PS3_SYSUTIL_CALLBACK_INVALID_SLOT && r.slot==invalid[i]);
        equal_pair(r.previous,(ps3_sysutil_callback_pair){0,0});
        equal_pair(r.current,(ps3_sysutil_callback_pair){0,0});
        assert(ps3_sysutil_callback_read(&f.registry,invalid[i],&p)==PS3_SYSUTIL_CALLBACK_INVALID_SLOT);
        equal_pair(p,before); assert(!memcmp(&saved,&f.registry,sizeof(saved)));
    }
    finish(&f);
}
static void replacement_and_opaque_values(void) {
    fixture f; start(&f);
    const ps3_sysutil_callback_pair values[]={{0x7765a0,0},{7,9},{UINT32_MAX,UINT32_C(0x80000001)},{0,UINT32_MAX},{0,0}};
    for(uint32_t slot=0;slot<PS3_SYSUTIL_CALLBACK_SLOT_COUNT;++slot) {
        ps3_sysutil_callback_pair previous={0,0};
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
            ps3_sysutil_callback_result r=ps3_sysutil_callback_register(&f.registry,slot,values[i]);
            assert(r.status==PS3_SYSUTIL_CALLBACK_OK && r.slot==slot);
            equal_pair(r.previous,previous); equal_pair(r.current,values[i]);
            ps3_sysutil_callback_pair read;
            assert(ps3_sysutil_callback_read(&f.registry,slot,&read)==PS3_SYSUTIL_CALLBACK_OK); equal_pair(read,values[i]);
            r=ps3_sysutil_callback_register(&f.registry,slot,values[i]);
            assert(r.status==PS3_SYSUTIL_CALLBACK_OK); equal_pair(r.previous,values[i]); equal_pair(r.current,values[i]);
            previous=values[i];
        }
    }
    finish(&f);
}
static void isolation(void) {
    fixture a,b; start(&a); start(&b);
    ps3_sysutil_callback_pair p={11,22},q={33,44},out;
    assert(ps3_sysutil_callback_register(&a.registry,0,p).status==PS3_SYSUTIL_CALLBACK_OK);
    assert(ps3_sysutil_callback_register(&b.registry,0,q).status==PS3_SYSUTIL_CALLBACK_OK);
    assert(ps3_sysutil_callback_read(&a.registry,0,&out)==PS3_SYSUTIL_CALLBACK_OK); equal_pair(out,p);
    assert(ps3_sysutil_callback_read(&b.registry,0,&out)==PS3_SYSUTIL_CALLBACK_OK); equal_pair(out,q);
    assert(ps3_sysutil_callback_read(&a.registry,1,&out)==PS3_SYSUTIL_CALLBACK_OK); equal_pair(out,(ps3_sysutil_callback_pair){0,0});
    finish(&a); finish(&b);
}
enum { THREADS=4, ITERATIONS=1000, REPLACEMENTS=THREADS*ITERATIONS };
typedef struct worker { fixture *f; unsigned index; uint32_t *predecessors; } worker;
static void *replace_and_snapshot(void *argument) {
    worker *w=argument;
    for(unsigned i=0;i<ITERATIONS;++i) {
        uint32_t tag=1+w->index*ITERATIONS+i;
        ps3_sysutil_callback_pair pair={tag,~tag};
        ps3_sysutil_callback_result r=ps3_sysutil_callback_register(&w->f->registry,0,pair);
        assert(r.status==PS3_SYSUTIL_CALLBACK_OK && r.slot==0); equal_pair(r.current,pair);
        assert(r.previous.userdata_address==~r.previous.callback_descriptor_address);
        w->predecessors[tag-1]=r.previous.callback_descriptor_address;
        ps3_sysutil_callback_pair snapshot;
        assert(ps3_sysutil_callback_read(&w->f->registry,0,&snapshot)==PS3_SYSUTIL_CALLBACK_OK);
        assert(snapshot.callback_descriptor_address<=REPLACEMENTS && snapshot.userdata_address==~snapshot.callback_descriptor_address);
    }
    return 0;
}
static void concurrency(void) {
    fixture f,untouched; start(&f); start(&untouched);
    ps3_sysutil_callback_pair isolated={0x11223344,0x55667788};
    assert(ps3_sysutil_callback_register(&untouched.registry,0,isolated).status==PS3_SYSUTIL_CALLBACK_OK);
    assert(ps3_sysutil_callback_register(&f.registry,0,(ps3_sysutil_callback_pair){0,UINT32_MAX}).status==PS3_SYSUTIL_CALLBACK_OK);
    pthread_t threads[THREADS]; worker workers[THREADS]; uint32_t predecessors[REPLACEMENTS]={0};
    for(unsigned i=0;i<THREADS;++i) { workers[i]=(worker){&f,i,predecessors}; assert(pthread_create(&threads[i],0,replace_and_snapshot,&workers[i])==0); }
    for(unsigned i=0;i<THREADS;++i) assert(pthread_join(threads[i],0)==0);
    ps3_sysutil_callback_pair final;
    assert(ps3_sysutil_callback_read(&f.registry,0,&final)==PS3_SYSUTIL_CALLBACK_OK);
    /* The returned previous/current history must form one complete serial chain. */
    unsigned char seen[REPLACEMENTS]={0}; uint32_t tag=final.callback_descriptor_address;
    for(unsigned i=0;i<REPLACEMENTS;++i) { assert(tag>=1 && tag<=REPLACEMENTS && !seen[tag-1]); seen[tag-1]=1; tag=predecessors[tag-1]; }
    assert(!tag && final.userdata_address==~final.callback_descriptor_address);
    assert(f.locks==2*REPLACEMENTS+2);
    assert(ps3_sysutil_callback_read(&untouched.registry,0,&final)==PS3_SYSUTIL_CALLBACK_OK); equal_pair(final,isolated);
    finish(&f); finish(&untouched);
}
int main(void) {
    initialization(); invalid_operations(); replacement_and_opaque_values(); isolation(); concurrency();
    puts("sysutil registration: 5 behavioral groups PASS; callback_execution=false"); return 0;
}
