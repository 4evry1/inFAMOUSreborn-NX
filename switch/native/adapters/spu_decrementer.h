#ifndef PS3_NATIVE_SPU_DECREMENTER_H
#define PS3_NATIVE_SPU_DECREMENTER_H
#include "../../../ps3recomp/runtime/time/clock.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* SPU thread options that affect the decrementer (pinned RPCS3 ad468a13 sys_spu.h:101-105). */
enum { PS3_SPU_THREAD_OPTION_NONE=0u, PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE=2u };
/* Per-SPU-thread decrementer in guest timebase ticks (pinned RPCS3 SPUThread.cpp):
 *   start (cpu_init 1346-1347): start_tb=now_tb; value = option&DEC_SYNC_TB ? ~(u32)start_tb : 0
 *   read  (read_dec 5397-5401): raw = (u64)value - (now_tb - start_tb); RdDec = (u32)raw, raw>>32 != 0 = underflow
 * now_tb = ps3_timebase_from_microseconds(clock sample), the same guest timebase function the native PPU mftb uses,
 * so one composition clock gives one coherent PPU/SPU timebase. Caller-owned storage; serialized use. Not modelled
 * here: WrDec (needs TM event collection) and the frozen state (only produced by WrEventAck). */
typedef struct {
    uint64_t start_microseconds, start_timebase;
    uint32_t value;
    unsigned started;
} ps3_spu_decrementer;
/* Samples the clock once. ARGUMENT for NULL storage/clock, an already started decrementer, or an option outside
 * {NONE, DEC_SYNC_TB_ENABLE}; a failed sample returns the provider status. Every failure leaves *d unchanged. */
ps3_clock_status ps3_spu_decrementer_start(ps3_spu_decrementer *d,ps3_clock clock,uint32_t thread_option);
/* Samples the clock once and writes the full 64-bit read_dec result to *raw (low word = RdDec value). ARGUMENT when
 * unstarted/NULL; a failed sample returns the provider status; a sample earlier than the start sample is RANGE (a
 * non-monotonic provider; never wrapped into a huge elapsed time). *raw is written only on OK; *d is never changed. */
ps3_clock_status ps3_spu_decrementer_read(const ps3_spu_decrementer *d,ps3_clock clock,uint64_t *raw);
#ifdef __cplusplus
}
#endif
#endif
