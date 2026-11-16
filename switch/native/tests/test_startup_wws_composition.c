#include "../adapters/startup_wws_composition.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static ps3_clock_status clock_sample(void *context,uint64_t *value) { ++*(unsigned *)context; *value=1; return PS3_CLOCK_OK; }
static int cancelled(void *context) { return *(int *)context; }
int main(void) {
    unsigned samples=0; int cancel=0;
    ps3_startup_wws_composition *first=NULL,*second=NULL;
    ps3_startup_wws_config config={{&samples,clock_sample},&cancel,cancelled};
    assert(ps3_startup_wws_create(NULL,&first)==PS3_SPURS_POLICY_INVALID && !first);
    ps3_startup_wws_config invalid=config; invalid.is_cancelled=NULL;
    assert(ps3_startup_wws_create(&invalid,&first)==PS3_SPURS_POLICY_INVALID && !first);
    assert(ps3_startup_wws_create(&config,&first)==PS3_SPURS_POLICY_READY && first);
    ps3_startup_wws_composition *same=first;
    assert(ps3_startup_wws_create(&config,&first)==PS3_SPURS_POLICY_BUSY && first==same);
    assert(ps3_startup_wws_create(&config,&second)==PS3_SPURS_POLICY_READY && second && second!=first);
    ps3_spurs_policy_execution_port a=ps3_startup_wws_port(first),b=ps3_startup_wws_port(second);
    assert(a.context && b.context && a.context!=b.context && a.begin && a.step && a.cancel && a.release);
    for (unsigned lane=0;lane<PS3_STARTUP_WWS_LANES;++lane) {
        ps3_startup_wws_diagnostics report; memset(&report,0xA5,sizeof(report));
        assert(ps3_startup_wws_read_joined(first,lane,&report)==PS3_SPURS_POLICY_READY);
        assert(report.initialized==1 && !report.active && !report.entered && !report.retained);
        assert(!report.last.hops && !report.request_bytes);
    }
    ps3_startup_wws_diagnostics before; memset(&before,0xA5,sizeof(before));
    ps3_startup_wws_diagnostics out=before;
    assert(ps3_startup_wws_read_joined(first,PS3_STARTUP_WWS_LANES,&out)==PS3_SPURS_POLICY_INVALID);
    assert(!memcmp(&out,&before,sizeof(out)) && !samples);
    assert(ps3_startup_wws_destroy_joined(&first)==PS3_SPURS_POLICY_READY && !first);
    assert(ps3_startup_wws_destroy_joined(&second)==PS3_SPURS_POLICY_READY && !second);
    assert(ps3_startup_wws_destroy_joined(&first)==PS3_SPURS_POLICY_INVALID);
    puts("OPAQUE_WWS_COMPOSITION_PASS independent_owners=2 copied_scalar_reports=1 generated_entry_called=0");
    return 0;
}
