#ifndef PS3_RUNTIME_SYNC_PUBLICATION_H
#define PS3_RUNTIME_SYNC_PUBLICATION_H
#include <stddef.h>
#include <stdint.h>
enum { PS3_SYNC_PUBLICATION_BYTES = 24, PS3_SYNC_PUBLICATION_CAPACITY = 128 };
typedef struct ps3_sync_publication {
    uint32_t address;
    size_t size;
    unsigned char bytes[PS3_SYNC_PUBLICATION_BYTES];
} ps3_sync_publication;
/* Caller-owned, nonaliasing operation report. Append only successful writes.
 * Reserve worst-case space before mutation; never replay these bytes as writes. */
typedef struct ps3_sync_publication_report {
    size_t count;
    ps3_sync_publication writes[PS3_SYNC_PUBLICATION_CAPACITY];
} ps3_sync_publication_report;
#endif
