#ifndef PS3EMU_HOST_CAPABILITIES_H
#define PS3EMU_HOST_CAPABILITIES_H

/* Compiled provider presence, not initialization or asset readiness. These
 * providers currently exist only inside the Windows D3D implementation guards. */
#if defined(PS3_HOST_OVERLAY) || defined(PS3_HZ_TELEMETRY) || defined(PS3_RSX_WRITE_WATCH_CACHE)
#error "host capabilities must not be predefined; select coherent providers first"
#endif

#if defined(_WIN32)
#define PS3_HOST_OVERLAY() 1
#define PS3_HZ_TELEMETRY() 1
#define PS3_RSX_WRITE_WATCH_CACHE() 1
#else
#define PS3_HOST_OVERLAY() 0
#define PS3_HZ_TELEMETRY() 0
#define PS3_RSX_WRITE_WATCH_CACHE() 0
#endif

#endif
