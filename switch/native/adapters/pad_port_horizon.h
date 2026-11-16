#ifndef PS3_NATIVE_PAD_PORT_HORIZON_H
#define PS3_NATIVE_PAD_PORT_HORIZON_H
#include <switch.h>
#include "../domain/pad_service.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Root (D2): the Horizon HID adapter behind the pad service's injected `pad_port`.
 *
 * One instance describes the ONE controller port this port serves (port 0): the handheld panel's own controls or
 * the first controller that paired, whichever libnx's default PadState selects. No rumble, no motion sensors, no
 * second port; the guest sees the buttons and the two sticks.
 *
 * Threading contract: the caller serialises every call. The dispatcher holds StartupPadProcess::lock for the whole
 * duration of one pad call, so a read_state that shifted the stick mapping against another thread's read cannot
 * happen. Nothing here takes a lock of its own, and nothing here is safe to call before pad_port_horizon_init. */
typedef struct pad_port_horizon {
    PadState pad;
    uint32_t configured;
    uint64_t reads;
} pad_port_horizon;

/* Configures the applet's single HidNpadStandard slot and binds the default pad to it. Builds on libnx's process-wide
 * pad setup (the console compositor configures the same one), so calling it twice is harmless: it is cached. */
void pad_port_horizon_init(pad_port_horizon *self);

/* The domain-facing port over `self`. `self` must outlive the port and every call made through it. */
pad_port pad_port_horizon_port(pad_port_horizon *self);
#ifdef __cplusplus
}
#endif
#endif
