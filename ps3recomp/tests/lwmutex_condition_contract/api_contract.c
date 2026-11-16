#include "lwmutex.h"
#include "publication.h"
#include <assert.h>
int main(void)
{
    ps3_lwmutex_condition_binding binding = {0};
    ps3_lwmutex_condition_wait token = {0};
    ps3_sync_publication_report report = {0};
    assert(ps3_lwmutex_condition_bind(0, 8, &binding).status == PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_condition_release(0, &binding, (ps3_lwmutex_thread){1,0}, &token, &report).status == PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(report.count == 0);
    return 0;
}
