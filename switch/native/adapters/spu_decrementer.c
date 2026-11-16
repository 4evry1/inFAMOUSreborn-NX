#include "spu_decrementer.h"
#include "../../../ps3recomp/runtime/time/timebase.h"
/* Contract: spu_decrementer.h. Pure: the only dependency is the caller's clock port and the shared guest timebase
 * conversion (79.8 MHz, 128-bit floor), so the SPU decrementer and the native PPU mftb share one timebase. */
ps3_clock_status ps3_spu_decrementer_start(ps3_spu_decrementer *d,ps3_clock clock,uint32_t thread_option) {
    if (!d || !clock.now_microseconds || d->started ||
        (thread_option!=PS3_SPU_THREAD_OPTION_NONE && thread_option!=PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE))
        return PS3_CLOCK_ARGUMENT;
    uint64_t now=0;
    const ps3_clock_status status=clock.now_microseconds(clock.context,&now);
    if (status!=PS3_CLOCK_OK) return status;
    const uint64_t timebase=ps3_timebase_from_microseconds(now);
    d->start_microseconds=now; d->start_timebase=timebase;
    d->value=thread_option==PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE ? ~(uint32_t)timebase : 0u;
    d->started=1;
    return PS3_CLOCK_OK;
}
ps3_clock_status ps3_spu_decrementer_read(const ps3_spu_decrementer *d,ps3_clock clock,uint64_t *raw) {
    if (!d || !d->started || !clock.now_microseconds || !raw) return PS3_CLOCK_ARGUMENT;
    uint64_t now=0;
    const ps3_clock_status status=clock.now_microseconds(clock.context,&now);
    if (status!=PS3_CLOCK_OK) return status;
    if (now<d->start_microseconds) return PS3_CLOCK_RANGE;  /* non-monotonic provider: never a huge wrapped elapsed */
    /* Both samples converted, then subtracted (get_timebased_time()-start); modular u64 like read_dec. */
    *raw=(uint64_t)d->value-(ps3_timebase_from_microseconds(now)-d->start_timebase);
    return PS3_CLOCK_OK;
}
