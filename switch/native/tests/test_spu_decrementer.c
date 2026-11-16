/* Contract tests for the owned SPU decrementer on a CONTROLLED (scripted) clock. These are deterministic contract
 * inputs, never hardware timing. Expected values use this file's own floor(us*798/10) formula (128-bit), not the
 * production conversion. */
#include "../adapters/spu_decrementer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { const uint64_t *samples; unsigned count, next; ps3_clock_status fail_at_status; unsigned fail_at; } Script;
static ps3_clock_status scripted(void *v,uint64_t *out) {
    Script *s=v; unsigned i=s->next++;
    if (s->fail_at && i+1==s->fail_at) return s->fail_at_status;
    assert(i<s->count); *out=s->samples[i]; return PS3_CLOCK_OK;
}
static uint64_t tb(uint64_t us) { return (uint64_t)(((unsigned __int128)us*798u)/10u); }  /* mod 2^64 */
static ps3_clock port(Script *s) { return (ps3_clock){s,scripted}; }
static uint64_t expected_raw(uint32_t option,uint64_t start_us,uint64_t now_us) {
    const uint64_t start=tb(start_us); const uint32_t value=option ? ~(uint32_t)start : 0u;
    return (uint64_t)value-(tb(now_us)-start);
}
static unsigned g_cases;
static void check_read(uint32_t option,uint64_t start_us,uint64_t now_us) {
    const uint64_t samples[2]={start_us,now_us}; Script s={samples,2,0,PS3_CLOCK_OK,0};
    ps3_spu_decrementer d; memset(&d,0,sizeof d);
    assert(ps3_spu_decrementer_start(&d,port(&s),option)==PS3_CLOCK_OK && d.started && s.next==1);
    assert(d.start_microseconds==start_us && d.start_timebase==tb(start_us) && d.value==(option ? ~(uint32_t)tb(start_us) : 0u));
    uint64_t raw=0x5A5A5A5A5A5A5A5Aull; assert(ps3_spu_decrementer_read(&d,port(&s),&raw)==PS3_CLOCK_OK && s.next==2);
    if (raw!=expected_raw(option,start_us,now_us)) {
        fprintf(stderr,"DEC_FAIL option=%u start=%llu now=%llu raw=0x%016llx expected=0x%016llx\n",option,(unsigned long long)start_us,
                (unsigned long long)now_us,(unsigned long long)raw,(unsigned long long)expected_raw(option,start_us,now_us)); assert(0);
    }
    if (option) assert((uint32_t)raw==~(uint32_t)tb(now_us));  /* DEC_SYNC_TB identity: RdDec == ~low32(timebase) */
    ++g_cases;
}
static void reads(void) {
    check_read(PS3_SPU_THREAD_OPTION_NONE,1000,1001);            /* 1 us = 79 ticks (79.8 floored): 0-79 underflows */
    { const uint64_t samples[2]={1000,1001}; Script s={samples,2,0,PS3_CLOCK_OK,0}; ps3_spu_decrementer d={0};
      assert(!ps3_spu_decrementer_start(&d,port(&s),0)); uint64_t raw; assert(!ps3_spu_decrementer_read(&d,port(&s),&raw));
      assert((uint32_t)raw==0xFFFFFFB1u && (raw>>32)!=0); }      /* low word -79; high word = underflow (TM source) */
    check_read(PS3_SPU_THREAD_OPTION_NONE,0,0);                  /* equal samples: raw == value */
    check_read(PS3_SPU_THREAD_OPTION_NONE,5,4000000000ull);      /* > 2^32 ticks elapsed: low word wraps */
    check_read(PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,123456789,123456790);
    check_read(PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,53821645,53821646);   /* tb crosses 2^32: 4294967271 -> 4294967350 */
    check_read(PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,UINT64_MAX-10,UINT64_MAX); /* conversion wraps mod 2^64 */
    check_read(PS3_SPU_THREAD_OPTION_NONE,UINT64_MAX-10,UINT64_MAX);
    check_read(PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE,0,1);
    /* Two reads from one start: 79 then 80 ticks per 1 us step (floor of 877.8 and 957.6: rounding visible). */
    { const uint64_t samples[3]={10,11,12}; Script s={samples,3,0,PS3_CLOCK_OK,0}; ps3_spu_decrementer d={0};
      assert(!ps3_spu_decrementer_start(&d,port(&s),0)); uint64_t a,b;
      assert(!ps3_spu_decrementer_read(&d,port(&s),&a) && !ps3_spu_decrementer_read(&d,port(&s),&b));
      assert((uint32_t)a==(uint32_t)(0u-(uint32_t)(tb(11)-tb(10))) && (uint32_t)a-(uint32_t)b==(uint32_t)(tb(12)-tb(11)));
      assert(tb(11)-tb(10)==79 && tb(12)-tb(11)==80); ++g_cases; }
}
static void failures(void) {
    ps3_spu_decrementer d, before; memset(&d,0xA5,sizeof d); d.started=0; before=d;
    const uint64_t samples[2]={77,70}; uint64_t raw=0x1111;
    /* Rejected options are refused before sampling; storage unchanged. */
    static const uint32_t bad_options[]={1u,3u,4u,0xFFFFFFFFu};
    for (unsigned i=0;i<4;++i) { Script s={samples,2,0,PS3_CLOCK_OK,0};
        assert(ps3_spu_decrementer_start(&d,port(&s),bad_options[i])==PS3_CLOCK_ARGUMENT && !s.next && !memcmp(&d,&before,sizeof d)); }
    { Script s={samples,2,0,PS3_CLOCK_OK,0}; ps3_clock none={&s,NULL};
      assert(ps3_spu_decrementer_start(&d,none,0)==PS3_CLOCK_ARGUMENT && !memcmp(&d,&before,sizeof d));
      assert(ps3_spu_decrementer_start(NULL,port(&s),0)==PS3_CLOCK_ARGUMENT && !s.next); }
    /* Provider failure at start: status propagated, storage unchanged. */
    { Script s={samples,2,0,PS3_CLOCK_PLATFORM_FAILURE,1};
      assert(ps3_spu_decrementer_start(&d,port(&s),0)==PS3_CLOCK_PLATFORM_FAILURE && !memcmp(&d,&before,sizeof d)); }
    /* Unstarted read: refused before sampling. */
    { Script s={samples,2,0,PS3_CLOCK_OK,0};
      assert(ps3_spu_decrementer_read(&d,port(&s),&raw)==PS3_CLOCK_ARGUMENT && !s.next && raw==0x1111); }
    /* Started: double start refused (no sample), provider failure and backward sample at read leave raw unchanged. */
    { Script s={samples,2,0,PS3_CLOCK_OK,0}; memset(&d,0,sizeof d);
      assert(!ps3_spu_decrementer_start(&d,port(&s),PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE)); before=d;
      assert(ps3_spu_decrementer_start(&d,port(&s),0)==PS3_CLOCK_ARGUMENT && s.next==1 && !memcmp(&d,&before,sizeof d));
      assert(ps3_spu_decrementer_read(&d,port(&s),&raw)==PS3_CLOCK_RANGE && raw==0x1111 && !memcmp(&d,&before,sizeof d)); }
    { const uint64_t ok[2]={5,6}; Script s={ok,2,0,PS3_CLOCK_RANGE,2}; memset(&d,0,sizeof d);
      assert(!ps3_spu_decrementer_start(&d,port(&s),0)); before=d;
      assert(ps3_spu_decrementer_read(&d,port(&s),&raw)==PS3_CLOCK_RANGE && raw==0x1111 && !memcmp(&d,&before,sizeof d)); }
    { const uint64_t ok[2]={5,6}; Script s={ok,2,0,PS3_CLOCK_PLATFORM_FAILURE,2}; memset(&d,0,sizeof d);
      assert(!ps3_spu_decrementer_start(&d,port(&s),0));
      assert(ps3_spu_decrementer_read(&d,port(&s),&raw)==PS3_CLOCK_PLATFORM_FAILURE && raw==0x1111);
      assert(ps3_spu_decrementer_read(&d,port(&s),NULL)==PS3_CLOCK_ARGUMENT); }
    ++g_cases;
}
int main(void) {
    reads(); failures();
    printf("SPU_DECREMENTER_PASS controlled_clock_contract_cases=%u sync_tb_identity=1 underflow_high_word=1 rounding=floor wrap=mod64 "
           "clock_failure=unchanged backward_sample=RANGE options_rejected_before_sample=1\n",g_cases);
    return 0;
}
