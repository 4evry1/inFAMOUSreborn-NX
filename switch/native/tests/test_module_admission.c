#include "../diagnostics/module_admission.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const uint16_t catalog[] = {0xe,0xa,0x17,9,0,0x14,0x11,0x15,0x33,0x10,0x1c,0xf035,0x16};
typedef struct fixture {
    pthread_mutex_t mutex;
    unsigned locks, unlocks, held;
    ps3_diagnostic_module_state state;
    ps3_diagnostic_module_slot slots[13];
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
    setup(f);
    assert(ps3_diagnostic_module_init(&f->state,guard(f),catalog,13,f->slots,13)==PS3_DIAGNOSTIC_MODULE_OK);
}
static void finish(fixture *f) {
    assert(!f->held && f->locks==f->unlocks); assert(pthread_mutex_destroy(&f->mutex)==0);
}
static void invalid_initialization(void) {
    fixture f; setup(&f); memset(f.slots,0xa5,sizeof(f.slots));
    ps3_diagnostic_module_slot before[13]; memcpy(before,f.slots,sizeof(before));
    ps3_diagnostic_module_state pristine=f.state;
    uint16_t duplicate[]={0,0}; ps3_instance_guard bad=guard(&f); bad.lock=0;
    assert(ps3_diagnostic_module_init(0,guard(&f),catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,bad,catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    bad=guard(&f); bad.unlock=0;
    assert(ps3_diagnostic_module_init(&f.state,bad,catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),0,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,0,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,13,0,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,13,f.slots,12)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),duplicate,2,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(!memcmp(&f.state,&pristine,sizeof(pristine)) && !memcmp(f.slots,before,sizeof(before)) && !f.locks);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_OK);
    pristine=f.state; memcpy(before,f.slots,sizeof(before));
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(!memcmp(&f.state,&pristine,sizeof(pristine)) && !memcmp(f.slots,before,sizeof(before)));
    finish(&f);
}
static void invalid_operations(void) {
    fixture f; setup(&f); ps3_diagnostic_module_result out; memset(&out,0xa5,sizeof(out));
    ps3_diagnostic_module_result before; memcpy(&before,&out,sizeof(out));
    assert(ps3_diagnostic_module_admit(0,0).status==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_admit(&f.state,0).status==PS3_DIAGNOSTIC_MODULE_NOT_INITIALIZED);
    assert(ps3_diagnostic_module_read(0,0,&out)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_read(&f.state,0,&out)==PS3_DIAGNOSTIC_MODULE_NOT_INITIALIZED);
    assert(!memcmp(&out,&before,sizeof(out)) && !f.locks);
    assert(ps3_diagnostic_module_init(&f.state,guard(&f),catalog,13,f.slots,13)==PS3_DIAGNOSTIC_MODULE_OK);
    assert(ps3_diagnostic_module_read(&f.state,0,0)==PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT);
    assert(ps3_diagnostic_module_admit(&f.state,0xffff).status==PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE);
    assert(ps3_diagnostic_module_read(&f.state,0xffff,&out)==PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE);
    assert(!memcmp(&out,&before,sizeof(out)) && !f.state.total_admissions);
    for(size_t i=0;i<13;++i) assert(!f.slots[i].admission_count);
    finish(&f);
}
static void catalog_counts_and_isolation(void) {
    fixture a,b; start(&a); start(&b);
    for(size_t i=0;i<13;++i) {
        ps3_diagnostic_module_result r=ps3_diagnostic_module_admit(&a.state,catalog[i]);
        assert(r.status==PS3_DIAGNOSTIC_MODULE_OK && r.module_id==catalog[i]);
        assert(!r.previous_count && r.current_count==1 && r.total_admissions==i+1);
    }
    ps3_diagnostic_module_result r=ps3_diagnostic_module_admit(&a.state,0);
    assert(r.previous_count==1 && r.current_count==2 && r.total_admissions==14);
    assert(ps3_diagnostic_module_read(&a.state,0,&r)==PS3_DIAGNOSTIC_MODULE_OK);
    assert(r.status==PS3_DIAGNOSTIC_MODULE_OK && r.previous_count==2 && r.current_count==2 && r.total_admissions==14);
    assert(ps3_diagnostic_module_read(&b.state,0,&r)==PS3_DIAGNOSTIC_MODULE_OK);
    assert(!r.current_count && !r.total_admissions);
    fixture c; setup(&c); uint16_t local[]={0x1234};
    assert(ps3_diagnostic_module_init(&c.state,guard(&c),local,1,c.slots,13)==PS3_DIAGNOSTIC_MODULE_OK);
    local[0]=0xabcd;
    assert(ps3_diagnostic_module_admit(&c.state,0x1234).status==PS3_DIAGNOSTIC_MODULE_OK);
    assert(ps3_diagnostic_module_admit(&c.state,0xabcd).status==PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE);
    finish(&a); finish(&b); finish(&c);
}
static void exhaustion(void) {
    fixture f; start(&f);
    /* Quiescent boundary injection, not a production mutation API. */
    f.slots[0].admission_count=UINT32_MAX; f.state.total_admissions=UINT32_MAX;
    ps3_diagnostic_module_result r=ps3_diagnostic_module_admit(&f.state,catalog[0]);
    assert(r.status==PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED && r.previous_count==UINT32_MAX && r.current_count==UINT32_MAX);
    assert(f.slots[0].admission_count==UINT32_MAX && f.state.total_admissions==UINT32_MAX);
    f.slots[0].admission_count=0; f.state.total_admissions=UINT64_MAX;
    r=ps3_diagnostic_module_admit(&f.state,catalog[0]);
    assert(r.status==PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED && !r.current_count && r.total_admissions==UINT64_MAX);
    assert(!f.slots[0].admission_count && f.state.total_admissions==UINT64_MAX);
    f.state.total_admissions=UINT64_MAX-1;
    r=ps3_diagnostic_module_admit(&f.state,catalog[0]);
    assert(r.status==PS3_DIAGNOSTIC_MODULE_OK && r.total_admissions==UINT64_MAX && r.current_count==1);
    assert(ps3_diagnostic_module_admit(&f.state,catalog[0]).status==PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED);
    finish(&f);
}
typedef struct worker { fixture *f; unsigned index; unsigned char *seen; } worker;
static void *contend(void *arg) {
    worker *w=arg;
    for(unsigned i=0;i<1000;++i) {
        ps3_diagnostic_module_result r=ps3_diagnostic_module_admit(&w->f->state,catalog[w->index]);
        assert(r.status==PS3_DIAGNOSTIC_MODULE_OK && r.module_id==catalog[w->index]);
        assert(r.current_count==r.previous_count+1 && r.total_admissions>=1 && r.total_admissions<=4000);
        /* Unique returned tickets independently check atomic admission snapshots. */
        assert(__atomic_exchange_n(&w->seen[r.total_admissions-1],1,__ATOMIC_RELAXED)==0);
        ps3_diagnostic_module_result s;
        assert(ps3_diagnostic_module_read(&w->f->state,catalog[w->index],&s)==PS3_DIAGNOSTIC_MODULE_OK);
        assert(s.current_count>=r.current_count && s.previous_count==s.current_count && s.total_admissions>=r.total_admissions);
    }
    return 0;
}
static void concurrency(void) {
    fixture f; start(&f); pthread_t threads[4]; worker workers[4]; unsigned char seen[4000]={0};
    for(unsigned i=0;i<4;++i) { workers[i]=(worker){&f,i%2,seen}; assert(pthread_create(&threads[i],0,contend,&workers[i])==0); }
    for(unsigned i=0;i<4;++i) assert(pthread_join(threads[i],0)==0);
    assert(f.state.total_admissions==4000 && f.slots[0].admission_count==2000 && f.slots[1].admission_count==2000);
    for(unsigned i=0;i<4000;++i) assert(seen[i]==1);
    assert(f.locks==8000); finish(&f);
}
int main(void) {
    invalid_initialization(); invalid_operations(); catalog_counts_and_isolation(); exhaustion(); concurrency();
    puts("module admission: 5 behavioral groups PASS (diagnostic assumption only)"); return 0;
}
