#ifndef PS3RECOMP_HOST_PROBE_H
#define PS3RECOMP_HOST_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read a short prefix without entering the host CRT file tables. Returns the
 * byte count read, or -1 when the path cannot be opened. */
int ps3_host_probe_prefix(const char* path, unsigned char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
