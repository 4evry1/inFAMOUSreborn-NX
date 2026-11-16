#ifndef NATIVE_GAMEDATA_STORAGE_POSIX_H
#define NATIVE_GAMEDATA_STORAGE_POSIX_H
/* POSIX adapter for the platform-free game-data service (native/domain/gamedata_service.h).
 *
 * One host directory stands for the guest's `/dev_hdd0/game`: every path this adapter touches is
 * `<root>/<dir_name>/...`, and the adapter refuses any name that could leave it (empty, '/', "..").
 *
 * Portability: plain C11 and the POSIX file API only (stat, mkdir, opendir/readdir, fopen/fwrite/
 * fflush/fclose, rename, unlink, statvfs). There is no libnx include and no `__SWITCH__` branch:
 * on the Switch, libnx's `sdmc:` fsdev already provides this API, so the same object works on the
 * host tests and on the console once `root` names "sdmc:/...".
 *
 * Ownership: the caller allocates the struct. It owns no heap memory; `init` only stores `root`
 * (copied) and creates the directory tree. */
#include "../domain/gamedata_service.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds. They exist so one call can never walk an unbounded tree or an unbounded file. */
#define GAMEDATA_STORAGE_POSIX_ROOT_MAX 256u      /* including the NUL terminator */
#define GAMEDATA_STORAGE_POSIX_MAX_DEPTH 8u       /* directory levels below <root>/<dir_name> */
#define GAMEDATA_STORAGE_POSIX_MAX_ENTRIES 4096u  /* entries visited by one stat_dir call */

typedef struct gamedata_storage_posix {
    char root[GAMEDATA_STORAGE_POSIX_ROOT_MAX]; /* host directory that stands for guest /dev_hdd0/game, no trailing '/' */
    unsigned calls_free, calls_stat, calls_read, calls_create, calls_write;
    int last_errno;                            /* errno of the last failing host call, 0 if none */
    gamedata_storage_status last_status;
} gamedata_storage_posix;

/* Copies `root` (must be non-empty, < 256 bytes, absolute or "sdmc:/..."), creates every missing path
 * component (mkdir -p, EEXIST on an existing directory is fine), and verifies the result is a
 * directory. Returns 0 on success, -1 otherwise (errno kept in last_errno). */
int gamedata_storage_posix_init(gamedata_storage_posix *self, const char *root);

/* Fills the five port functions with context = self. */
gamedata_storage_port gamedata_storage_posix_port(gamedata_storage_posix *self);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_GAMEDATA_STORAGE_POSIX_H */
