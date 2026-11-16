#include "../adapters/spu_executor.h"
#include "../adapters/guard_pthread.h"
#include "guest_space_host.h"
#include <assert.h>
#include <fenv.h>
#include <xmmintrin.h>
#include "spu_fp_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ElfTestHostSpace *space; ps3_guest_memory backing; ps3_pthread_guard guard;
    ps3_atomic_domain domain; ps3_atomic_owner_record records[4];
    unsigned fail_read, reads, writes, traces, reserves, finishes; int deny_write, cancel_phase, fail_trace;
    unsigned char traced[16384]; size_t traced_bytes; unsigned checkpoints;
} Fixture;
static ps3_guest_memory_status read_memory(void *v,uint32_t a,void *p,size_t n) {
    Fixture *f=v; ++f->reads;
    if (f->fail_read && f->reads==f->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.read(f->backing.context,a,p,n);
}
static ps3_guest_memory_status write_memory(void *v,uint32_t a,const void *p,size_t n) { Fixture *f=v; ++f->writes; return f->backing.write(f->backing.context,a,p,n); }
static ps3_guest_memory_status commit_memory(void *v,uint32_t a,uint64_t n) { Fixture *f=v; return f->backing.commit(f->backing.context,a,n); }
static ps3_guest_memory_status decommit_memory(void *v,uint32_t a,uint64_t n) { Fixture *f=v; return f->backing.decommit(f->backing.context,a,n); }
static ps3_guest_memory_status zero_memory(void *v,uint32_t a,uint64_t n) { Fixture *f=v; return f->backing.zero(f->backing.context,a,n); }
static int access_memory(void *v,uint64_t a,size_t n,int write) { Fixture *f=v; return a>=0x1000 && a<=0x4000 && n<=0x4000-a && !(write&&f->deny_write) ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS; }
static int checkpoint(void *v,unsigned phase) { Fixture *f=v; ++f->checkpoints; return f->cancel_phase==(int)phase ? PS3_SPU_EXEC_CANCELLED : PS3_SPU_EXEC_OK; }
static int reserve_trace(void *v,const ps3_spu_transfer *request) { Fixture *f=v; assert(request->bytes<=sizeof f->traced); if (f->fail_trace) return PS3_SPU_EXEC_TRACE; ++f->reserves; return PS3_SPU_EXEC_OK; }
static void finish_trace(void *v,const ps3_spu_transfer *request,ps3_guest_memory_status status,int committed,const void *bytes) { Fixture *f=v; ++f->finishes; if (committed) { assert(!status && bytes); memcpy(f->traced,bytes,request->bytes); f->traced_bytes=request->bytes; ++f->traces; } else assert(!bytes); }
static ps3_spu_executor *create(Fixture *f) {
    memset(f,0,sizeof *f); f->space=elf_test_host_space_create(65536); assert(f->space);
    assert(!ps3_pthread_guard_init(&f->guard)); f->backing=elf_test_host_space_port(f->space);
    ps3_guest_memory memory={f,commit_memory,decommit_memory,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->guard),f->records,4,NULL,0)==0);
    assert(ps3_atomic_commit(&f->domain,0x1000,0x3000)==0);
    ps3_spu_executor *owner=calloc(1,sizeof *owner); assert(owner);
    ps3_spu_executor_services services={f,access_memory,checkpoint,reserve_trace,finish_trace};
    assert(ps3_spu_executor_init(owner,&f->domain,0,services)==0); return owner;
}
static void finish(Fixture *f,ps3_spu_executor *owner) { assert(f->reserves==f->finishes); assert(ps3_spu_executor_release(owner)==0); free(owner); assert(!ps3_pthread_guard_destroy(&f->guard)); elf_test_host_space_destroy(f->space); }
static int write_channel(ps3_spu_executor *o,uint32_t ch,uint32_t v) { return ps3_spu_executor_write_channel(o,ch,spu_make_preferred_u32(v)); }
static void parameters(ps3_spu_executor *o,uint32_t lsa,uint32_t ea,uint32_t size,uint32_t tag) {
    assert(write_channel(o,MFC_LSA,lsa)==0); assert(write_channel(o,MFC_EAH,0)==0);
    assert(write_channel(o,MFC_EAL,ea)==0); assert(write_channel(o,MFC_Size,size)==0); assert(write_channel(o,MFC_TagID,tag)==0);
}
static void failed_command(void) {
    Fixture f; ps3_spu_executor *o=create(&f); parameters(o,0,0x1000,16,3); f.fail_read=1;
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_MEMORY);
    assert(!(o->engine.tag_completed&(1u<<3)));
    assert(o->terminal.provider_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE && !f.traces);
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_MEMORY && f.reads==1);
    finish(&f,o);
}
static void seed(Fixture *f,unsigned bytes) {
    unsigned char data[128]; assert(bytes<=sizeof data);
    for (unsigned i=0;i<bytes;++i) data[i]=(unsigned char)(i^0xa5);
    assert(ps3_atomic_write(&f->domain,0x1000,data,bytes)==0);
}
static void descriptor(ps3_spu_executor *o,unsigned address,uint32_t flags,uint32_t ea) {
    spu_ls_write32(&o->core,address,flags); spu_ls_write32(&o->core,address+4,ea);
}
static void successful_transfers(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    parameters(o,0x100,0x1000,32,2); assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==0);
    assert(f.traces==1 && f.traced_bytes==32 && !memcmp(o->core.ls+0x100,f.traced,32));
    assert(o->engine.tag_completed&(1u<<2));
    o->core.ls[0x100]^=0xff; parameters(o,0x100,0x1100,32,3);
    assert(write_channel(o,MFC_Cmd,MFC_PUT_CMD)==0);
    unsigned char data[32]; assert(ps3_atomic_read(&f.domain,0x1100,data,32)==0);
    assert(!memcmp(data,o->core.ls+0x100,32));
    assert(write_channel(o,MFC_WrTagMask,1u<<3)==0);
    assert(write_channel(o,MFC_WrTagUpdate,0)==0 && o->core.mfc_tag_status==(1u<<3));
    assert(mfc_tag_wait(&o->engine,1u<<3,1)==(1u<<3));
    assert(mfc_tag_wait(&o->engine,1u<<3,2)==(1u<<3));
    u128 value; assert(ps3_spu_executor_read_channel(o,MFC_RdTagMask,&value)==0 && value._u32[0]==(1u<<3));
    parameters(o,0,0,0,8);
    assert(write_channel(o,MFC_Cmd,MFC_SYNC_CMD)==0 && (o->engine.tag_completed&(1u<<8)));
    ps3_spu_executor *peer=calloc(1,sizeof *peer); assert(peer);
    assert(ps3_spu_executor_init(peer,&f.domain,1,o->services)==0);
    assert(&peer->engine!=&o->engine && peer->core.ls!=o->core.ls);
    peer->engine.tag_completed=0;
    assert(o->engine.tag_completed==UINT32_MAX);
    assert(ps3_spu_executor_release(peer)==0); free(peer); finish(&f,o);
}
static void list_stall_resume(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,0x80000010u,0x1010);
    parameters(o,0x100,0x400,16,4); assert(write_channel(o,MFC_Cmd,0x44)==0);
    assert(o->core.list_stall_mask==(1u<<4) && !(o->engine.tag_completed&(1u<<4)));
    assert(o->progress.next_element==0x408 && o->progress.remaining==1 && o->progress.destination==0x110);
    u128 notification; assert(ps3_spu_executor_read_channel(o,MFC_RdListStallStat,&notification)==0);
    assert(notification._u32[0]==(1u<<4) && !o->core.list_stall_stat);
    assert(write_channel(o,MFC_WrListStallAck,4)==0);
    assert(o->core.list_stall_mask==(1u<<4) && !(o->engine.tag_completed&(1u<<4)));
    assert(o->progress.next_element==0x410 && !o->progress.remaining && o->progress.destination==0x120);
    assert(write_channel(o,MFC_WrListStallAck,4)==0);
    assert(!o->core.list_stall_mask && (o->engine.tag_completed&(1u<<4)) && f.traces==2);
    unsigned char expected[32]; assert(ps3_atomic_read(&f.domain,0x1000,expected,32)==0);
    assert(!memcmp(o->core.ls+0x100,expected,32));
    finish(&f,o);
}
static void list_failure_and_cancel(void) {
    for (unsigned cancel=0;cancel<2;++cancel) {
        Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
        descriptor(o,0x400,16,0x1000); descriptor(o,0x408,16,0x1010);
        parameters(o,0x100,0x400,16,5);
        if (cancel) f.cancel_phase=PS3_SPU_POST_TRANSFER; else f.fail_read=2;
        assert(write_channel(o,MFC_Cmd,0x44)==(cancel ? PS3_SPU_EXEC_CANCELLED : PS3_SPU_EXEC_MEMORY));
        assert(!(o->engine.tag_completed&(1u<<5)) && f.traces==1);
        assert(o->terminal.progress.next_element==0x408 && o->terminal.progress.remaining==1);
        assert(o->terminal.progress.destination==0x110 && o->terminal.progress.tag==5);
        assert(f.reads==(cancel ? 1u : 2u));
        assert(o->terminal.effect_committed==(cancel ? 1u : 0u));
        unsigned calls=f.reads;
        assert(write_channel(o,MFC_Cmd,0x44)==o->terminal.code && f.reads==calls);
        assert(!pthread_mutex_trylock(&f.guard.mutex)); assert(!pthread_mutex_unlock(&f.guard.mutex));
        finish(&f,o);
    }
}
static void ack_failure(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,9); f.fail_read=2;
    assert(write_channel(o,MFC_Cmd,0x44)==0 && f.traces==1);
    assert(write_channel(o,MFC_WrListStallAck,9)==PS3_SPU_EXEC_MEMORY);
    assert(!(o->engine.tag_completed&(1u<<9)) && f.traces==1 && f.reads==2);
    assert(o->terminal.progress.next_element==0x408 && o->terminal.progress.destination==0x110);
    assert(!pthread_mutex_trylock(&f.guard.mutex)); assert(!pthread_mutex_unlock(&f.guard.mutex));
    finish(&f,o);
}
static void unsupported_count(void) {
    Fixture f; ps3_spu_executor *o=create(&f);
    jmp_buf escape; o->escape=&escape;
    if (!setjmp(escape)) { (void)spu_rchcnt(&o->core,MFC_Cmd); assert(!"unsupported count returned"); }
    o->escape=NULL;
    assert(o->terminal.code==PS3_SPU_EXEC_UNSUPPORTED && o->terminal.channel==MFC_Cmd && !f.reads);
    finish(&f,o);
}
static void post_command_cancel(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,16); parameters(o,0x100,0x1000,16,6);
    f.cancel_phase=PS3_SPU_POST_COMMAND;
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_CANCELLED);
    assert((o->engine.tag_completed&(1u<<6)) && f.traces==1 && o->terminal.effect_committed);
    assert(!memcmp(o->core.ls+0x100,f.traced,16));
    unsigned reads=f.reads;
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_CANCELLED && f.reads==reads);
    finish(&f,o);
}
static void rejected_effects(void) {
    for (unsigned mode=0;mode<6;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); parameters(o,0x100,0x1000,16,7);
        o->core.ls[0x100]=0x5a;
        int expected;
        if (mode==0) { f.deny_write=1; expected=PS3_SPU_EXEC_ACCESS; }
        else if (mode==1) { f.fail_trace=1; expected=PS3_SPU_EXEC_TRACE; }
        else if (mode==2) { f.cancel_phase=PS3_SPU_PRE_COMMAND; expected=PS3_SPU_EXEC_CANCELLED; }
        else if (mode==3) { assert(write_channel(o,MFC_EAH,1)==0); expected=PS3_SPU_EXEC_ACCESS; }
        else if (mode==4) { assert(write_channel(o,MFC_Size,3)==0); expected=PS3_SPU_EXEC_UNSUPPORTED; }
        else { assert(write_channel(o,MFC_EAH,1)==0); expected=PS3_SPU_EXEC_ACCESS; }
        assert(write_channel(o,MFC_Cmd,mode==0 ? MFC_PUT_CMD : mode==5 ? MFC_GETLLAR_CMD : MFC_GET_CMD)==expected);
        assert(!f.traces && !f.reads && !f.writes && !f.reserves && !o->terminal.effect_committed);
        if (mode==2) assert(o->engine.tag_completed&(1u<<7));
        finish(&f,o);
    }
}
static void actual_line_commands(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); parameters(o,0,0x1000,128,0);
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0 && o->core.atomic_stat==4);
    o->core.ls[127]^=0xff;
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLC_CMD)==0 && o->core.atomic_stat==0);
    unsigned char got[128]; assert(ps3_atomic_read(&f.domain,0x1000,got,128)==0 && !memcmp(got,o->core.ls,128));
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);
    assert(ps3_atomic_write(&f.domain,0x1000,got,128)==0);
    unsigned traces=f.traces;
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLC_CMD)==0 && o->core.atomic_stat==1 && f.traces==traces);
    assert(!o->binding.effect_committed);
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLUC_CMD)==0 && o->core.atomic_stat==2);
    finish(&f,o);
}
static void unsupported_channels(void) {
    for (unsigned mode=0;mode<4;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); int status;
        u128 output; memset(&output,0xcc,sizeof output);
        if (mode==0) status=ps3_spu_executor_read_channel(o,MFC_RdAtomicStat,&output);
        else if (mode==1) status=write_channel(o,MFC_WrTagUpdate,3);
        else if (mode==2) { parameters(o,0,0x1000,128,0); status=write_channel(o,MFC_Cmd,MFC_PUTQLLUC_CMD); }
        else status=write_channel(o,MFC_WrListStallAck,0);
        assert(status==PS3_SPU_EXEC_UNSUPPORTED && !f.traces && !f.reads);
        assert(output._u32[0]==0xcccccccc && output._u32[3]==0xcccccccc);
        finish(&f,o);
    }
}
static uint32_t count_of(ps3_spu_executor *o,uint32_t ch) {
    uint32_t n=0xdeadbeefu; int status=ps3_spu_executor_count_channel(o,ch,&n);
    if (status) { fprintf(stderr,"RCHCNT ch=%u status=%d count_untouched=%d\n",ch,status,n==0xdeadbeefu); assert(!"supported status count failed"); }
    return n;
}
static int read_word(ps3_spu_executor *o,uint32_t ch,uint32_t *word) {
    u128 v; memset(&v,0xcc,sizeof v); int status=ps3_spu_executor_read_channel(o,ch,&v);
    if (!status) *word=v._u32[0];
    else assert(v._u32[0]==0xcccccccc && v._u32[1]==0xcccccccc && v._u32[2]==0xcccccccc && v._u32[3]==0xcccccccc);
    return status;
}
static void atomic_success_zero_status(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); parameters(o,0,0x1000,128,0);
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);
    o->core.ls[5]^=0x3c;
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLC_CMD)==0 && o->core.atomic_stat==0);
    assert(count_of(o,MFC_RdAtomicStat)==1);
    assert(count_of(o,MFC_RdAtomicStat)==1);
    uint32_t word=0x77777777u; assert(read_word(o,MFC_RdAtomicStat,&word)==0 && word==0);
    assert(count_of(o,MFC_RdAtomicStat)==0);
    assert(read_word(o,MFC_RdAtomicStat,&word)==PS3_SPU_EXEC_UNSUPPORTED && word==0);
    assert(o->terminal.channel==MFC_RdAtomicStat);
    finish(&f,o);
}
static ps3_spu_status_slots *slots(ps3_spu_executor *o) { return &o->binding.status; }
static void atomic_status_values(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); parameters(o,0,0x1000,128,0);
    assert(count_of(o,MFC_RdAtomicStat)==0 && count_of(o,MFC_RdTagStat)==0);
    uint32_t word=0;
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0 && count_of(o,MFC_RdAtomicStat)==1);
    assert(read_word(o,MFC_RdAtomicStat,&word)==0 && word==4 && count_of(o,MFC_RdAtomicStat)==0);
    o->core.ls[3]^=0x5a;  /* PUTLLC success itself publishes available zero after the slot was consumed */
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLC_CMD)==0 && count_of(o,MFC_RdAtomicStat)==1);
    word=0x33; assert(read_word(o,MFC_RdAtomicStat,&word)==0 && word==0 && count_of(o,MFC_RdAtomicStat)==0);
    unsigned char line[128]; assert(ps3_atomic_read(&f.domain,0x1000,line,128)==0);
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);
    line[0]^=0x11; assert(ps3_atomic_write(&f.domain,0x1000,line,128)==0);
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLC_CMD)==0 && count_of(o,MFC_RdAtomicStat)==1);
    assert(read_word(o,MFC_RdAtomicStat,&word)==0 && word==1);
    assert(write_channel(o,MFC_Cmd,MFC_PUTLLUC_CMD)==0);
    assert(count_of(o,MFC_RdTagStat)==0);  /* atomic publication is channel-independent */
    assert(read_word(o,MFC_RdAtomicStat,&word)==0 && word==2 && !o->core.atomic_stat);
    assert(f.reserves==f.finishes && !o->trace_reserved);
    finish(&f,o);
}
static void atomic_failures_publish_nothing(void) {
    for (unsigned mode=0;mode<4;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); parameters(o,0,0x1000,128,0);
        assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);  /* older unread slot: 4 */
        int expected; uint32_t cmd=MFC_PUTLLC_CMD;
        if (mode==0) { f.deny_write=1; expected=PS3_SPU_EXEC_ACCESS; }
        else if (mode==1) { f.fail_trace=1; expected=PS3_SPU_EXEC_TRACE; }
        else if (mode==2) { f.fail_read=f.reads+1; cmd=MFC_GETLLAR_CMD; expected=PS3_SPU_EXEC_MEMORY; }
        else { cmd=MFC_PUTQLLUC_CMD; expected=PS3_SPU_EXEC_UNSUPPORTED; }
        assert(write_channel(o,MFC_Cmd,cmd)==expected);
        assert(slots(o)->atomic_available==1 && o->core.atomic_stat==4 && !slots(o)->tag_available);
        assert(!o->trace_reserved);
        finish(&f,o);
    }
}
static void tag_immediate_and_all_zero(void) {
    Fixture f; ps3_spu_executor *o=create(&f); uint32_t word=0x55u;
    assert(count_of(o,MFC_RdTagStat)==0);  /* idle engine completion is not a published response */
    assert(write_channel(o,MFC_WrTagMask,0)==0 && count_of(o,MFC_RdTagStat)==0);
    assert(write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_IMMEDIATE)==0 && count_of(o,MFC_RdTagStat)==1);
    assert(count_of(o,MFC_RdTagStat)==1 && read_word(o,MFC_RdTagStat,&word)==0 && word==0);
    assert(count_of(o,MFC_RdTagStat)==0);
    assert(write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0 && count_of(o,MFC_RdTagStat)==1);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE);
    word=0x55u; assert(read_word(o,MFC_RdTagStat,&word)==0 && word==0);
    assert(write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ANY)==0 && count_of(o,MFC_RdTagStat)==0);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ANY);
    assert(write_channel(o,MFC_WrTagMask,1u<<3)==0 && count_of(o,MFC_RdTagStat)==1);  /* mask satisfies ANY */
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE);
    assert(write_channel(o,MFC_WrTagMask,1u<<4)==0 && o->core.mfc_tag_status==(1u<<3));  /* no request: no publication */
    assert(read_word(o,MFC_RdTagStat,&word)==0 && word==(1u<<3) && count_of(o,MFC_RdTagStat)==0);
    assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<9))==0 && count_of(o,MFC_RdTagStat)==0);
    assert(write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0 && count_of(o,MFC_RdTagStat)==1);
    assert(read_word(o,MFC_RdTagStat,&word)==0 && word==((1u<<4)|(1u<<9)));
    assert(read_word(o,MFC_RdTagStat,&word)==PS3_SPU_EXEC_UNSUPPORTED && o->terminal.channel==MFC_RdTagStat);
    finish(&f,o);
}
static void independent_channels_and_owners(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); parameters(o,0,0x1000,128,0);
    ps3_spu_executor *peer=calloc(1,sizeof *peer); assert(peer);
    assert(ps3_spu_executor_init(peer,&f.domain,1,o->services)==0);
    assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);
    assert(write_channel(o,MFC_WrTagMask,1u<<2)==0 && write_channel(o,MFC_WrTagUpdate,0)==0);
    assert(count_of(peer,MFC_RdTagStat)==0 && count_of(peer,MFC_RdAtomicStat)==0);
    uint32_t word=0;
    assert(read_word(o,MFC_RdTagStat,&word)==0 && word==(1u<<2));
    assert(count_of(o,MFC_RdAtomicStat)==1 && o->core.atomic_stat==4);
    assert(read_word(peer,MFC_RdAtomicStat,&word)==PS3_SPU_EXEC_UNSUPPORTED && peer->stopped && !o->stopped);
    assert(count_of(o,MFC_RdAtomicStat)==1 && read_word(o,MFC_RdAtomicStat,&word)==0 && word==4);
    assert(ps3_spu_executor_release(peer)==0); free(peer); finish(&f,o);
}
/* Real three-element GET list: stall, stall, plain. Tag 4 completes on second ACK. */
static void park_list(Fixture *f,ps3_spu_executor *o) {
    seed(f,48);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,0x80000010u,0x1010); descriptor(o,0x410,16,0x1020);
    parameters(o,0x100,0x400,24,4); assert(write_channel(o,MFC_Cmd,0x44)==0);
    assert(o->core.list_stall_mask==(1u<<4) && !(o->engine.tag_completed&(1u<<4)));
}
static void stalled_all_preserves_then_ack_publishes(void) {
    Fixture f; ps3_spu_executor *o=create(&f); park_list(&f,o);
    assert(count_of(o,MFC_RdTagStat)==0);  /* command without a request publishes nothing */
    assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<2))==0 && write_channel(o,MFC_WrTagUpdate,0)==0);
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));  /* older unread slot */
    assert(write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrTagMask,1u<<4)==0);  /* unsatisfied mask change retains slot */
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_UNSUPPORTED);  /* queued behind stall */
    assert(o->core.mfc_tag_status==(1u<<2) && slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL);
    finish(&f,o);
    o=create(&f); park_list(&f,o);
    assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<2))==0 && write_channel(o,MFC_WrTagUpdate,0)==0);
    assert(write_channel(o,MFC_WrTagMask,1u<<4)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0);
    assert(write_channel(o,MFC_WrListStallAck,4)==0 && o->core.list_stall_mask==(1u<<4));  /* reparked: no response */
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2) && slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL);
    assert(write_channel(o,MFC_WrListStallAck,4)==0 && !o->core.list_stall_mask && f.traces==3);
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<4));
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE);
    uint32_t word=0; assert(read_word(o,MFC_RdTagStat,&word)==0 && word==(1u<<4) && count_of(o,MFC_RdTagStat)==0);
    finish(&f,o);
}
static void stalled_mask_changes(void) {
    for (unsigned mode=0;mode<3;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); park_list(&f,o);
        assert(write_channel(o,MFC_WrTagMask,1u<<4)==0);
        assert(write_channel(o,MFC_WrTagUpdate,mode==0 ? MFC_TAG_UPDATE_ANY : MFC_TAG_UPDATE_ALL)==0);
        assert(count_of(o,MFC_RdTagStat)==0 && slots(o)->pending_tag_update!=PS3_SPU_TAG_UPDATE_IMMEDIATE);
        if (mode==0) {  /* ANY satisfied by an idle tag added to the mask */
            assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<5))==0 && count_of(o,MFC_RdTagStat)==1);
            assert(o->core.mfc_tag_status==(1u<<5));
            assert(write_channel(o,MFC_WrTagMask,1u<<6)==0 && o->core.mfc_tag_status==(1u<<5));  /* mode reset */
        } else if (mode==1) {  /* ALL -> mask0 publishes available zero */
            o->core.mfc_tag_status=0x5a5a;  /* stale, unpublished value is not an available slot */
            assert(write_channel(o,MFC_WrTagMask,0)==0 && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==0);
            assert(write_channel(o,MFC_WrListStallAck,4)==0 && write_channel(o,MFC_WrListStallAck,4)==0);
            assert(!o->core.list_stall_mask && o->core.mfc_tag_status==0);  /* no request: completion publishes nothing new */
            uint32_t word=0x99; assert(read_word(o,MFC_RdTagStat,&word)==0 && word==0 && count_of(o,MFC_RdTagStat)==0);
        } else {  /* ALL unsatisfied by a partially idle mask */
            assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<5))==0 && count_of(o,MFC_RdTagStat)==0);
            assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL);
        }
        assert(slots(o)->pending_tag_update==(mode==2 ? (uint32_t)PS3_SPU_TAG_UPDATE_ALL : (uint32_t)PS3_SPU_TAG_UPDATE_IMMEDIATE));
        if (mode==0) {  /* published slot is reportable but its read is not serviced while work is parked */
            uint32_t word=0x31; assert(read_word(o,MFC_RdTagStat,&word)==PS3_SPU_EXEC_UNSUPPORTED && word==0x31);
            assert(slots(o)->tag_available==1 && o->core.mfc_tag_status==(1u<<5) && o->terminal.channel==MFC_RdTagStat);
        }
        finish(&f,o);
    }
}
static void unsolicited_completion_and_failed_ack(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    assert(write_channel(o,MFC_WrTagMask,1u<<2)==0);
    parameters(o,0x100,0x1000,32,2); assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==0);
    assert(count_of(o,MFC_RdTagStat)==0 && (o->engine.tag_completed&(1u<<2)));
    finish(&f,o);
    o=create(&f); seed(&f,32);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,9); f.fail_read=2;
    assert(write_channel(o,MFC_Cmd,0x44)==0);
    assert(write_channel(o,MFC_WrTagMask,1u<<9)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0);
    assert(write_channel(o,MFC_WrListStallAck,9)==PS3_SPU_EXEC_MEMORY);
    assert(!slots(o)->tag_available && slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ALL && !o->trace_reserved);
    finish(&f,o);
}
static void ordinary_get_preserves_unread_tag_slot(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32); uint32_t word=0;
    assert(write_channel(o,MFC_WrTagMask,1u<<2)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_IMMEDIATE)==0);
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrTagMask,1u<<3)==0 && slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE);
    parameters(o,0x100,0x1000,32,3); assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==0 && f.traces==1);
    assert((o->engine.tag_completed&(1u<<3)) && !memcmp(o->core.ls+0x100,f.traced,32));
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));  /* completion without request: old unread value */
    assert(read_word(o,MFC_RdTagStat,&word)==0 && word==(1u<<2) && count_of(o,MFC_RdTagStat)==0);
    finish(&f,o);
}
static void unsatisfied_any_preserves_unread_tag_slot(void) {
    Fixture f; ps3_spu_executor *o=create(&f); park_list(&f,o); uint32_t word=0;
    assert(write_channel(o,MFC_WrTagMask,(1u<<4)|(1u<<2))==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_IMMEDIATE)==0);
    assert(count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrTagMask,1u<<4)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ANY)==0);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrTagMask,0)==0);  /* ANY with an empty mask stays unsatisfied */
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrTagMask,1u<<4)==0);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrListStallAck,4)==0 && o->core.list_stall_mask==(1u<<4));  /* reparked: still unsatisfied */
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && o->core.mfc_tag_status==(1u<<2));
    assert(write_channel(o,MFC_WrListStallAck,4)==0 && !o->core.list_stall_mask);
    assert(slots(o)->pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE && count_of(o,MFC_RdTagStat)==1 && o->core.mfc_tag_status==(1u<<4));
    assert(read_word(o,MFC_RdTagStat,&word)==0 && word==(1u<<4) && count_of(o,MFC_RdTagStat)==0);
    finish(&f,o);
}
static void tag_read_cancellation(void) {
    for (unsigned post=0;post<2;++post) {
        Fixture f; ps3_spu_executor *o=create(&f); uint32_t word=0x42;
        assert(write_channel(o,MFC_WrTagMask,1u<<7)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_IMMEDIATE)==0);
        assert(count_of(o,MFC_RdTagStat)==1);
        f.cancel_phase=post ? PS3_SPU_POST_CHANNEL : PS3_SPU_PRE_CHANNEL;
        assert(read_word(o,MFC_RdTagStat,&word)==PS3_SPU_EXEC_CANCELLED && word==0x42);
        if (post) assert(!slots(o)->tag_available && !o->core.mfc_tag_status);  /* consumed, never restored */
        else assert(slots(o)->tag_available==1 && o->core.mfc_tag_status==(1u<<7));
        assert(o->terminal.effect_committed==post);  /* the consumption is a committed channel effect */
        assert(o->stopped && o->terminal.code==PS3_SPU_EXEC_CANCELLED && !o->trace_reserved);
        finish(&f,o);
    }
}
static void status_cancellation(void) {
    for (unsigned mode=0;mode<8;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); seed(&f,128); uint32_t word=0x42;
        if (mode==0) {  /* post-command cancel after real atomic publication */
            parameters(o,0,0x1000,128,0); f.cancel_phase=PS3_SPU_POST_COMMAND;
            assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==PS3_SPU_EXEC_CANCELLED);
            assert(slots(o)->atomic_available==1 && o->core.atomic_stat==4);
        } else if (mode==1) {  /* post-command cancel after ACK publication */
            finish(&f,o); o=create(&f); park_list(&f,o);
            assert(write_channel(o,MFC_WrTagMask,1u<<4)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ALL)==0);
            assert(write_channel(o,MFC_WrListStallAck,4)==0);
            f.cancel_phase=PS3_SPU_POST_COMMAND;
            assert(write_channel(o,MFC_WrListStallAck,4)==PS3_SPU_EXEC_CANCELLED && !o->core.list_stall_mask);
            assert(slots(o)->tag_available==1 && o->core.mfc_tag_status==(1u<<4) && !slots(o)->pending_tag_update);
        } else if (mode==2) {  /* post-channel cancel after mask publication */
            finish(&f,o); o=create(&f); park_list(&f,o);
            assert(write_channel(o,MFC_WrTagMask,1u<<4)==0 && write_channel(o,MFC_WrTagUpdate,MFC_TAG_UPDATE_ANY)==0);
            f.cancel_phase=PS3_SPU_POST_CHANNEL;
            assert(write_channel(o,MFC_WrTagMask,1u<<5)==PS3_SPU_EXEC_CANCELLED);
            assert(slots(o)->tag_available==1 && o->core.mfc_tag_status==(1u<<5) && !slots(o)->pending_tag_update);
        } else if (mode==3) {  /* pre-channel cancel: update has no effect */
            f.cancel_phase=PS3_SPU_PRE_CHANNEL;
            assert(write_channel(o,MFC_WrTagUpdate,0)==PS3_SPU_EXEC_CANCELLED && !slots(o)->tag_available);
        } else if (mode==4 || mode==5) {  /* post/pre read cancel */
            parameters(o,0,0x1000,128,0); assert(write_channel(o,MFC_Cmd,MFC_GETLLAR_CMD)==0);
            f.cancel_phase=mode==4 ? PS3_SPU_POST_CHANNEL : PS3_SPU_PRE_CHANNEL;
            assert(read_word(o,MFC_RdAtomicStat,&word)==PS3_SPU_EXEC_CANCELLED && word==0x42);
            if (mode==4) assert(!slots(o)->atomic_available && !o->core.atomic_stat);  /* consumed, never restored */
            else assert(slots(o)->atomic_available==1 && o->core.atomic_stat==4);
        } else if (mode==6) {  /* count cancel leaves output and slot untouched */
            assert(write_channel(o,MFC_WrTagUpdate,0)==0); f.cancel_phase=PS3_SPU_PRE_CHANNEL;
            uint32_t n=0x77; assert(ps3_spu_executor_count_channel(o,MFC_RdTagStat,&n)==PS3_SPU_EXEC_CANCELLED && n==0x77);
            assert(slots(o)->tag_available==1);
        } else {  /* unsupported counts and illegal update mode */
            uint32_t n=0x77; const uint32_t channels[]={MFC_Cmd,MFC_RdListStallStat,SPU_RdInMbox};
            assert(write_channel(o,MFC_WrTagUpdate,0)==0 && count_of(o,MFC_RdTagStat)==1);
            assert(count_of(o,MFC_WrTagUpdate)==1);
            assert(write_channel(o,MFC_WrTagUpdate,3)==PS3_SPU_EXEC_UNSUPPORTED);  /* illegal mode: no new response */
            assert(slots(o)->tag_available==1 && !slots(o)->pending_tag_update);
            for (unsigned i=0;i<sizeof(channels)/sizeof(channels[0]);++i) {
                Fixture g; ps3_spu_executor *p=create(&g);
                assert(ps3_spu_executor_count_channel(p,channels[i],&n)==PS3_SPU_EXEC_UNSUPPORTED && n==0x77);
                assert(p->terminal.channel==channels[i]); finish(&g,p);
            }
        }
        assert(o->stopped && !o->trace_reserved);
        if (mode!=7) assert(o->terminal.code==PS3_SPU_EXEC_CANCELLED);
        if (mode==2 || mode==4) assert(o->terminal.effect_committed==1);  /* POST_CHANNEL after a committed channel effect */
        if (mode==3 || mode==5 || mode==6 || mode==7) assert(o->terminal.effect_committed==0);
        finish(&f,o);
    }
}
/* ---- SPU_WrEventMask contract (pinned RPCS3 ad468a13 SPUThread.cpp 6548-6575) ---- */
enum { EVENT_TG=0x1u, EVENT_SN=0x2u, EVENT_MB=0x10u, EVENT_TM=0x20u, EVENT_LR=0x400u };
static void event_mask_accepts_sn_and_zero(void) {
    Fixture f; ps3_spu_executor *o=create(&f);
    o->core.event_status=EVENT_TG;  /* pending but not selected: must survive every mask write */
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==0 && o->core.event_mask==EVENT_SN);
    assert(o->core.event_status==EVENT_TG && !o->core.int_enable && !o->stopped);
    assert(write_channel(o,SPU_WrEventMask,0)==0 && o->core.event_mask==0 && o->core.event_status==EVENT_TG);
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==0 && o->core.event_mask==EVENT_SN);
    assert(!f.reads && !f.traces && !f.reserves);
    finish(&f,o);
}
static void event_mask_rejections_have_no_effect(void) {
    for (unsigned mode=0;mode<8;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); uint32_t value=EVENT_SN;
        if (mode==0) value=EVENT_TG;                       /* no native producer, unimplemented in reference */
        else if (mode==1) value=EVENT_LR;                  /* requires reservation-loss collection */
        else if (mode==2) value=EVENT_TM;                  /* requires decrementer collection */
        else if (mode==3) value=EVENT_MB|EVENT_SN;         /* mailbox producer absent natively */
        else if (mode==4) o->core.event_status=EVENT_SN;   /* would set the unmodeled count bit */
        else if (mode==5) o->core.int_enable=1;            /* interrupt delivery depends on unmodeled count */
        else if (mode==6) o->core.event_mask=EVENT_LR;     /* collection hint covers the old mask */
        else o->core.event_status=EVENT_SN|EVENT_TG;
        uint32_t mask=o->core.event_mask, status=o->core.event_status, enable=o->core.int_enable;
        assert(write_channel(o,SPU_WrEventMask,value)==PS3_SPU_EXEC_UNSUPPORTED);
        assert(o->terminal.channel==SPU_WrEventMask && !o->terminal.effect_committed);
        assert(o->core.event_mask==mask && o->core.event_status==status && o->core.int_enable==enable);
        finish(&f,o);
    }
}
static void event_mask_cancellation_order(void) {
    for (unsigned post=0;post<2;++post) {
        Fixture f; ps3_spu_executor *o=create(&f);
        f.cancel_phase=post ? PS3_SPU_POST_CHANNEL : PS3_SPU_PRE_CHANNEL;
        assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==PS3_SPU_EXEC_CANCELLED);
        assert(o->core.event_mask==(post ? EVENT_SN : 0u) && !o->core.event_status);
        assert(o->terminal.channel==SPU_WrEventMask && o->terminal.code==PS3_SPU_EXEC_CANCELLED);
        assert(o->terminal.effect_committed==post);  /* POST: the mask WAS committed; reported, not hidden */
        finish(&f,o);
    }
}
static void event_mask_per_owner_and_reads_still_limited(void) {
    Fixture f; ps3_spu_executor *o=create(&f);
    ps3_spu_executor *peer=calloc(1,sizeof *peer); assert(peer);
    assert(ps3_spu_executor_init(peer,&f.domain,1,o->services)==0);
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==0 && !peer->core.event_mask);
    assert(ps3_spu_executor_release(peer)==0); free(peer);
    const uint32_t limited[]={SPU_RdEventStat,SPU_RdEventMask,SPU_WrEventAck};
    for (unsigned i=0;i<3;++i) {
        Fixture g; ps3_spu_executor *p=create(&g); uint32_t word=0x42, n=0x77;
        assert(write_channel(p,SPU_WrEventMask,EVENT_SN)==0);
        int status=i==2 ? write_channel(p,limited[i],EVENT_SN) : read_word(p,limited[i],&word);
        assert(status==PS3_SPU_EXEC_UNSUPPORTED && p->terminal.channel==limited[i] && p->core.event_mask==EVENT_SN);
        finish(&g,p); p=create(&g);
        assert(ps3_spu_executor_count_channel(p,SPU_RdEventStat,&n)==PS3_SPU_EXEC_UNSUPPORTED && n==0x77);
        finish(&g,p);
    }
    finish(&f,o);
}
/* A selected stall-and-notify event would need the unmodeled count/interrupt: stop before parking/raising. */
static void selected_sn_producer_stops(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==0);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,4);
    assert(write_channel(o,MFC_Cmd,0x44)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(!o->core.list_stall_mask && !o->core.list_stall_stat && !(o->core.event_status&EVENT_SN));
    assert(o->terminal.channel==MFC_Cmd && o->terminal.effect_committed && f.traces==1);  /* element 0 was real */
    unsigned char prefix[16]; assert(ps3_atomic_read(&f.domain,0x1000,prefix,16)==0);
    assert(!memcmp(o->core.ls+0x100,prefix,16) && !memcmp(f.traced,prefix,16) && f.traced_bytes==16);  /* committed prefix kept */
    assert(o->terminal.request.ea==0x1000 && o->terminal.request.lsa==0x100 && o->terminal.request.bytes==16);
    /* progress = last committed list cursor: here element 0 of this command (next 0x408, 1 left, dest 0x110). */
    assert(o->terminal.progress.next_element==0x408 && o->terminal.progress.remaining==1 &&
           o->terminal.progress.destination==0x110 && o->terminal.progress.tag==4);
    assert(!(o->engine.tag_completed&(1u<<4)));
    finish(&f,o);
    /* Real SN pending from an unselected stall; selecting it later is refused and the event is preserved. */
    o=create(&f); seed(&f,32);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,4); assert(write_channel(o,MFC_Cmd,0x44)==0);
    assert(o->core.list_stall_mask==(1u<<4) && (o->core.event_status&EVENT_SN));
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(!o->core.event_mask && (o->core.event_status&EVENT_SN) && o->core.list_stall_mask==(1u<<4));
    finish(&f,o);
}
/* Native SN lifecycle: the only event_status writer is the list-stall producer (spu_dma.h, |= SN). Reading
 * MFC_RdListStallStat clears list_stall_stat only, and WrListStallAck resumes the list without touching
 * event_status (WrEventAck, the reference SN clear, stays UNSUPPORTED). So after stall -> stat read -> ack the SN
 * stays pending and a later selection is refused before its PRE checkpoint. Consequence (labelled, not faked):
 * the Addendum-A "re-park under a selected SN" path is unreachable under the accepted mask rules with native list
 * state -- parking needs an unselected stall, which leaves SN pending forever, which blocks every later selection. */
static void sn_pending_survives_stat_read_and_ack(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    descriptor(o,0x400,0x80000010u,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,4); assert(write_channel(o,MFC_Cmd,0x44)==0);
    assert(o->core.list_stall_mask==(1u<<4) && o->core.event_status==EVENT_SN && !o->core.event_mask);
    uint32_t word=0; assert(read_word(o,MFC_RdListStallStat,&word)==0 && word==(1u<<4));
    assert(!o->core.list_stall_stat && o->core.event_status==EVENT_SN);  /* stat read does not clear SN */
    assert(write_channel(o,MFC_WrListStallAck,4)==0);
    assert(!o->core.list_stall_mask && (o->engine.tag_completed&(1u<<4)) && f.traces==2);
    assert(o->core.event_status==EVENT_SN);  /* ack does not clear SN */
    f.cancel_phase=PS3_SPU_PRE_CHANNEL;      /* refusal must precede the PRE checkpoint: UNSUPPORTED, not CANCELLED */
    assert(write_channel(o,SPU_WrEventMask,EVENT_SN)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(o->terminal.channel==SPU_WrEventMask && !o->core.event_mask && o->core.event_status==EVENT_SN);
    finish(&f,o);
}
/* terminal.progress is the last committed list cursor and is not reset per command: a later plain GET that stops
 * reports its own request while progress still names the earlier, completed list. */
static void terminal_progress_may_be_from_earlier_command(void) {
    Fixture f; ps3_spu_executor *o=create(&f); seed(&f,32);
    descriptor(o,0x400,16,0x1000); descriptor(o,0x408,16,0x1010);
    parameters(o,0x100,0x400,16,5); assert(write_channel(o,MFC_Cmd,0x44)==0 && (o->engine.tag_completed&(1u<<5)));
    parameters(o,0x200,0x8000,16,6);  /* outside the granted window */
    assert(write_channel(o,MFC_Cmd,MFC_GET_CMD)==PS3_SPU_EXEC_ACCESS);
    assert(o->terminal.request.ea==0x8000 && o->terminal.request.lsa==0x200 && !o->terminal.effect_committed);
    assert(o->terminal.progress.next_element==0x410 && !o->terminal.progress.remaining &&
           o->terminal.progress.destination==0x120 && o->terminal.progress.tag==5);  /* earlier list's cursor */
    finish(&f,o);
}
/* ---- F3: caller FP environment restored through the C nonlocal escape ---- */
static void fp_escape_changes_env(spu_context *ctx) {
    fesetround(FE_TOWARDZERO); feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_INEXACT|FE_DIVBYZERO);
    _mm_setcsr((_mm_getcsr()&~0x6000u)|0x2000u|0x8040u);  /* MXCSR round-down + FTZ/DAZ */
    spu_wrch(ctx,SPU_WrOutMbox,spu_make_preferred_u32(1));  /* unsupported natively: longjmp out */
    assert(!"escape expected");
}
static void fp_escape_inside_bracket(spu_context *ctx) {
    (void)spu_fp_begin_double();  /* bracket never ended: the escape must not leave its mode behind */
    feraiseexcept(FE_UNDERFLOW);
    spu_wrch(ctx,SPU_WrOutMbox,spu_make_preferred_u32(1));
    assert(!"escape expected");
}
/* CONTROLLED clock (scripted microsecond samples): contract input only, never timing evidence. */
typedef struct { uint64_t samples[8]; unsigned count, next, fail_at; ps3_clock_status fail_status; } ScriptClock;
static ps3_clock_status script_sample(void *v,uint64_t *out) {
    ScriptClock *c=v; unsigned i=c->next++;
    if (c->fail_at && i+1==c->fail_at) return c->fail_status;
    assert(i<c->count); *out=c->samples[i]; return PS3_CLOCK_OK;
}
static uint64_t tb_of(uint64_t us) { return (uint64_t)(((unsigned __int128)us*798u)/10u); }
static void fp_escape_changes_env(spu_context *ctx);
static void fp_escape_inside_bracket(spu_context *ctx);
static const ps3_spu_lifted_function g_test_functions[2]={{0x100,fp_escape_changes_env},{0x200,fp_escape_inside_bracket}};
static const ps3_spu_linked_image g_test_image={"executor_test",g_test_functions,2};
static int start_owner(ps3_spu_executor *o,ScriptClock *c,uint32_t option) {
    const ps3_spu_executor_start_config cfg={&g_test_image,0,{c,script_sample},option,16};
    return ps3_spu_executor_start(o,&cfg);
}
static void fp_environment_restored_on_escape(void) {
    ps3_spu_lifted_entry entries[2]={fp_escape_changes_env,fp_escape_inside_bracket};
    for (unsigned i=0;i<2;++i) {
        Fixture f; ps3_spu_executor *o=create(&f); ScriptClock clock={{1},1,0,0,PS3_CLOCK_OK};
        assert(start_owner(o,&clock,PS3_SPU_THREAD_OPTION_NONE)==PS3_SPU_EXEC_OK);
        assert(!fesetround(FE_UPWARD)); feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_OVERFLOW);
        const unsigned csr=_mm_getcsr(); const int flags_before=fetestexcept(FE_ALL_EXCEPT);
        assert((csr&0x6000u)==0x4000u && (flags_before&FE_OVERFLOW) && !(flags_before&FE_DIVBYZERO));  /* caller MXCSR round-up is non-default */
        assert(ps3_spu_executor_enter(o,entries[i])==PS3_SPU_EXEC_UNSUPPORTED && o->terminal.channel==SPU_WrOutMbox);
        const int round=fegetround(), flags=fetestexcept(FE_ALL_EXCEPT); const unsigned after=_mm_getcsr();
        fesetround(FE_TONEAREST); feclearexcept(FE_ALL_EXCEPT);
        if (round!=FE_UPWARD || flags!=flags_before || after!=csr) {
            fprintf(stderr,"FP_RESTORE_FAIL case=%u round=%d flags=0x%x csr=0x%x expected=0x%x\n",i,round,flags,after,csr); abort();
        }
        finish(&f,o);
    }
}
/* ---- owned start lifecycle and SPU_RdDec / SPU_WrDec (controlled clock) ---- */
static void start_validation(void) {
    Fixture f; ps3_spu_executor *o=create(&f); ScriptClock c={{40,41},2,0,0,PS3_CLOCK_OK};
    ps3_spu_executor_start_config cfg={&g_test_image,0,{&c,script_sample},PS3_SPU_THREAD_OPTION_NONE,16};
    const ps3_spu_linked_image empty={"empty",g_test_functions,0};
    ps3_spu_executor_start_config bad[6]; for (unsigned i=0;i<6;++i) bad[i]=cfg;
    bad[0].image=NULL; bad[1].image=&empty; bad[2].clock.now_microseconds=NULL; bad[3].hop_budget=0;
    bad[4].thread_option=1u /* ASYNC_INTR */; bad[5].thread_option=3u;
    for (unsigned i=0;i<6;++i) assert(ps3_spu_executor_start(o,&bad[i])==PS3_SPU_EXEC_INVALID && !o->started && !c.next);
    assert(ps3_spu_executor_start(o,NULL)==PS3_SPU_EXEC_INVALID && ps3_spu_executor_start(NULL,&cfg)==PS3_SPU_EXEC_INVALID);
    assert(ps3_spu_executor_enter(o,fp_escape_changes_env)==PS3_SPU_EXEC_INVALID && !o->stopped);  /* unstarted */
    ScriptClock failing={{0},0,0,1,PS3_CLOCK_PLATFORM_FAILURE}; ps3_spu_executor_start_config fc=cfg; fc.clock.context=&failing;
    assert(ps3_spu_executor_start(o,&fc)==PS3_SPU_EXEC_CLOCK && o->clock_status==PS3_CLOCK_PLATFORM_FAILURE && !o->started);
    assert(ps3_spu_executor_start(o,&cfg)==PS3_SPU_EXEC_OK && o->started && c.next==1 && o->decrementer.start_timebase==tb_of(40));
    assert(ps3_spu_executor_start(o,&cfg)==PS3_SPU_EXEC_INVALID && c.next==1);  /* double start: no sample */
    o->core.image_id=5;  /* bound id is 0: enter refuses before running anything */
    assert(ps3_spu_executor_enter(o,fp_escape_changes_env)==PS3_SPU_EXEC_INVALID && !o->stopped && !f.checkpoints);
    o->core.image_id=0; finish(&f,o);
    puts("EXECUTOR_START_VALIDATION invalid_configs=8 clock_failure=unchanged double_start=refused enter_unstarted=INVALID enter_image_mismatch=INVALID");
}
/* Review F2: start validates the bound image's SHAPE once (1..SPU_LS_SIZE/4 entries, strictly increasing = sorted and
 * unique pcs, each pc a 4-byte-aligned LS address, each function non-NULL) and refuses a bad table with INVALID
 * BEFORE sampling the clock or changing the owner (byte-identical owner). LS pc 0 is a legal table entry (the hop
 * refuses a continuation to pc 0 separately, as desktop LS-0 job end); so is the last aligned LS word. */
static void start_image_shape_validation(void) {
    ps3_spu_lifted_entry f0=fp_escape_changes_env, f1=fp_escape_inside_bracket;
    const ps3_spu_lifted_function t[8][2]={
        {{0x200,f0},{0x100,f1}},                    /* order */
        {{0x100,f0},{0x100,f1}},                    /* duplicate pc */
        {{0x100,f0},{0x200,NULL}},                  /* NULL function */
        {{0x100,f0},{SPU_LS_SIZE,f1}},              /* pc == LS size */
        {{0x100,f0},{0xFFFFFFFCu,f1}},              /* pc far outside LS (aligned) */
        {{0x102,f0},{0x200,f1}},                    /* misaligned first */
        {{0x100,f0},{0x201,f1}},                    /* misaligned last */
        {{0x100,NULL},{0x200,f1}},                  /* NULL first function */
    };
    static const char *const why[11]={"order","duplicate","null_fn","pc_eq_ls_size","pc_outside_ls","misaligned_first",
                                     "misaligned_last","null_first_fn","count_over_ls_words","null_storage","zero_count"};
    ps3_spu_linked_image images[11];
    for (unsigned i=0;i<8;++i) images[i]=(ps3_spu_linked_image){why[i],t[i],2};
    images[8]=(ps3_spu_linked_image){why[8],g_test_functions,SPU_LS_SIZE/4u+1u};  /* refused before reading entry 2 */
    images[9]=(ps3_spu_linked_image){why[9],NULL,2};
    images[10]=(ps3_spu_linked_image){why[10],g_test_functions,0};
    Fixture f; ps3_spu_executor *o=create(&f);
    o->clock_status=PS3_CLOCK_RANGE;  /* sentinel: a refused start must not touch it */
    unsigned char *before=malloc(sizeof *o); assert(before); memcpy(before,o,sizeof *o);
    for (unsigned i=0;i<11;++i) {
        ScriptClock c={{40},1,0,0,PS3_CLOCK_OK};
        const ps3_spu_executor_start_config cfg={&images[i],0,{&c,script_sample},PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,16};
        const int status=ps3_spu_executor_start(o,&cfg);
        if (status!=PS3_SPU_EXEC_INVALID || c.next || memcmp(before,o,sizeof *o)) {
            fprintf(stderr,"IMAGE_SHAPE_FAIL case=%s status=%d clock_samples=%u owner_changed=%d\n",why[i],status,c.next,
                    memcmp(before,o,sizeof *o)!=0);
            abort();
        }
    }
    free(before); o->clock_status=PS3_CLOCK_OK; finish(&f,o);
    /* Legitimate shapes start (one sample) and bind exactly that table. */
    static const ps3_spu_lifted_function legal[3][2]={{{0x0,fp_escape_changes_env},{0x100,fp_escape_inside_bracket}},
                                                      {{0x100,fp_escape_changes_env},{SPU_LS_SIZE-4u,fp_escape_inside_bracket}},
                                                      {{0x3FEC0,fp_escape_changes_env},{0,NULL}}};
    const size_t counts[3]={2,2,1};
    for (unsigned i=0;i<3;++i) {
        const ps3_spu_linked_image image={"legal",legal[i],counts[i]};
        Fixture g; ps3_spu_executor *p=create(&g); ScriptClock c={{40},1,0,0,PS3_CLOCK_OK};
        const ps3_spu_executor_start_config cfg={&image,0,{&c,script_sample},PS3_SPU_THREAD_OPTION_NONE,16};
        if (ps3_spu_executor_start(p,&cfg)!=PS3_SPU_EXEC_OK || c.next!=1 || !p->started || p->start.image!=&image) {
            fprintf(stderr,"IMAGE_SHAPE_FAIL legal=%u\n",i); abort();
        }
        finish(&g,p);
    }
    puts("EXECUTOR_IMAGE_SHAPE invalid=11(order,duplicate,null_fn,range,misaligned,count,storage) refused_before_clock_and_owner_change=1 legal=pc0,last_ls_word,single");
}
static void decrementer_reads(void) {
    static const struct { uint32_t option; uint64_t start_us, read_us; } k[3]={
        {PS3_SPU_THREAD_OPTION_NONE,1000,1001},{PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,123456789,123457000},
        {PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,53821645,53821646}};
    for (unsigned i=0;i<3;++i) {
        Fixture f; ps3_spu_executor *o=create(&f); ScriptClock c={{k[i].start_us,k[i].read_us},2,0,0,PS3_CLOCK_OK};
        assert(start_owner(o,&c,k[i].option)==PS3_SPU_EXEC_OK);
        uint32_t word=0; assert(read_word(o,SPU_RdDec,&word)==PS3_SPU_EXEC_OK && c.next==2 && f.checkpoints==2);  /* PRE+POST */
        const uint32_t start_value=k[i].option ? ~(uint32_t)tb_of(k[i].start_us) : 0u;
        const uint32_t expected=start_value-(uint32_t)(tb_of(k[i].read_us)-tb_of(k[i].start_us));
        if (word!=expected) { fprintf(stderr,"RDDEC case=%u got 0x%08X expected 0x%08X\n",i,word,expected); abort(); }
        if (k[i].option) assert(word==~(uint32_t)tb_of(k[i].read_us));
        assert(o->decrementer_reads==1 && (uint32_t)o->decrementer_last_raw==expected && !o->stopped);
        finish(&f,o);
    }
    puts("EXECUTOR_RDDEC controlled_clock=1 cases=3 none_underflow=0xFFFFFFB1 sync_tb=~low32(tb) checkpoints=PRE,POST");
}
static void decrementer_refusals(void) {
    for (unsigned mode=0;mode<6;++mode) {
        Fixture f; ps3_spu_executor *o=create(&f); ScriptClock c={{10,11},2,0,0,PS3_CLOCK_OK};
        if (mode!=0) assert(start_owner(o,&c,PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE)==PS3_SPU_EXEC_OK);
        if (mode==1) f.cancel_phase=PS3_SPU_PRE_CHANNEL;
        if (mode==2) f.cancel_phase=PS3_SPU_POST_CHANNEL;
        if (mode==3) { c.fail_at=2; c.fail_status=PS3_CLOCK_PLATFORM_FAILURE; }
        if (mode==4) c.samples[1]=9;  /* provider went backwards */
        const ps3_spu_decrementer before=o->decrementer; uint32_t word=0x12345678u; int status;
        if (mode==5) status=write_channel(o,SPU_WrDec,5);
        else status=read_word(o,SPU_RdDec,&word);
        /* decrementer_reads counts successful RdDec clock samples: POST cancellation (mode 2) sampled, then dropped it. */
        assert(word==0x12345678u && o->decrementer_reads==(mode==2 ? 1u : 0u) && !memcmp(&before,&o->decrementer,sizeof before));
        assert(o->terminal.channel==(mode==5 ? SPU_WrDec : SPU_RdDec) && !o->terminal.effect_committed);
        switch (mode) {
        case 0: assert(status==PS3_SPU_EXEC_UNSUPPORTED && !f.checkpoints); break;           /* unstarted: before PRE */
        case 1: assert(status==PS3_SPU_EXEC_CANCELLED && f.checkpoints==1 && c.next==1); break;  /* no sample */
        case 2: assert(status==PS3_SPU_EXEC_CANCELLED && f.checkpoints==2 && c.next==2); break;  /* sampled, dropped */
        case 3: assert(status==PS3_SPU_EXEC_CLOCK && o->terminal.clock_status==PS3_CLOCK_PLATFORM_FAILURE && f.checkpoints==1); break;
        case 4: assert(status==PS3_SPU_EXEC_CLOCK && o->terminal.clock_status==PS3_CLOCK_RANGE && f.checkpoints==1); break;
        case 5: assert(status==PS3_SPU_EXEC_UNSUPPORTED && !f.checkpoints && c.next==1); break;  /* WrDec: TM unmodelled */
        }
        finish(&f,o);
    }
    puts("EXECUTOR_RDDEC_REFUSALS unstarted=UNSUPPORTED(before_PRE) cancel_pre=no_sample cancel_post=value_dropped clock_failure=CLOCK backward=CLOCK(RANGE) wrdec=UNSUPPORTED(no_effect)");
}
/* SPU_RdMachStat (13): served for INTERPRETER owners only -- interrupt-enable bit 0 is 0 because the interpreter
 * never enables interrupts, and the count is 1 -- a lifted owner keeps the existing refusal. */
static void machstat_channel(void) {
    Fixture f; ps3_spu_executor *o=create(&f);
    u128 lifted; memset(&lifted,0xcc,sizeof lifted); uint32_t n=0xdeadbeefu;
    assert(ps3_spu_executor_read_channel(o,SPU_RdMachStat,&lifted)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(lifted._u32[0]==0xcccccccc && lifted._u32[1]==0xcccccccc && lifted._u32[2]==0xcccccccc && lifted._u32[3]==0xcccccccc);
    assert(o->terminal.channel==SPU_RdMachStat && !o->terminal.effect_committed);
    assert(ps3_spu_executor_count_channel(o,SPU_RdMachStat,&n)==PS3_SPU_EXEC_UNSUPPORTED && n==0xdeadbeefu);
    assert(o->terminal.channel==SPU_RdMachStat);
    finish(&f,o);
    Fixture g; ps3_spu_executor *i=create(&g); ps3_spu_executor_set_interpreter(i);
    u128 status; memset(&status,0xcc,sizeof status);
    assert(ps3_spu_executor_read_channel(i,SPU_RdMachStat,&status)==0);
    assert(!status._u32[0] && !status._u32[1] && !status._u32[2] && !status._u32[3]);
    assert(count_of(i,SPU_RdMachStat)==1);
    assert(!i->stopped);
    finish(&g,i);
    puts("EXECUTOR_MACHSTAT interpreter_read=0 interpreter_count=1 lifted_read=UNSUPPORTED lifted_count=UNSUPPORTED");
}
int main(void) {
    start_validation(); start_image_shape_validation(); decrementer_reads(); decrementer_refusals();
    event_mask_accepts_sn_and_zero(); event_mask_rejections_have_no_effect(); event_mask_cancellation_order();
    event_mask_per_owner_and_reads_still_limited(); selected_sn_producer_stops(); sn_pending_survives_stat_read_and_ack();
    terminal_progress_may_be_from_earlier_command(); fp_environment_restored_on_escape();
    atomic_success_zero_status(); atomic_status_values(); atomic_failures_publish_nothing();
    tag_immediate_and_all_zero(); independent_channels_and_owners(); stalled_all_preserves_then_ack_publishes();
    stalled_mask_changes(); unsolicited_completion_and_failed_ack(); status_cancellation();
    ordinary_get_preserves_unread_tag_slot(); unsatisfied_any_preserves_unread_tag_slot(); tag_read_cancellation();
    failed_command(); successful_transfers(); list_stall_resume(); list_failure_and_cancel();
    post_command_cancel(); rejected_effects(); actual_line_commands(); unsupported_channels();
    ack_failure(); unsupported_count(); machstat_channel();
    puts("SPU_EXECUTOR_PASS actual_channel_engine=1 event_mask_contract=1 fp_escape_restore=1 owned_decrementer=1 genuine_entry=0"); return 0;
}
