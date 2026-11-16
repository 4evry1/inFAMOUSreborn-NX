/*
 * rename_replace - rename() that replaces an existing destination, for the vendored video library on Horizon.
 *
 * Root (2026-11-13, Decision 109). The backend saves pipelines.bin (and pipelines.rcp) as temp file + rename
 * (rsx_vulkan_backend.c, vk_sc_write_file). On the SD card's file system rename() onto an existing file fails, so the
 * pipeline cache never grew past its first save: hardware run rv37s logged "[RSX vulkan] shader cache: cannot write
 * .../pipelines.bin". The inFAMOUS 2 session met the same fault and fixed it the same way (remove the destination,
 * retry). sibling.mk compiles the vendored video sources with -Drename=sibling_rename_replace; this file is compiled
 * without that define, so the calls below reach the C library.
 */
#include <stdio.h>

int sibling_rename_replace(const char *from, const char *to)
{
    if (rename(from, to) == 0) return 0;
    (void)remove(to);
    return rename(from, to);
}
