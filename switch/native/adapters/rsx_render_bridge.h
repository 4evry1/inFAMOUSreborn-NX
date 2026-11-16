#ifndef RSX_RENDER_BRIDGE_H
#define RSX_RENDER_BRIDGE_H
/* The live game's RSX methods into the draw engine and its flips onto the screen (rsx_render_bridge.c,
 * Decision 67). Plain C types only, so the composition root can include it without the renderer's headers.
 * Every function must be called from the one thread that pumps the RSX consumer. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rsx_render_bridge_config {
    const char *app_dir;                 /* holds spv-cache/ and receives render.log */
    uint32_t io_ea, io_size;             /* the init IO mapping: IO offset 0 is this guest address */
    uint32_t local_base, local_size;     /* RSX local memory */
} rsx_render_bridge_config;
typedef struct rsx_render_bridge_stats {
    int state;                           /* 0 not started, 1 running, -1 failed or stopped */
    int present_ready;
    uint64_t methods, flips, presented, reports, reads_refused, reads_uncommitted;
} rsx_render_bridge_stats;
/* Once; a failure is final for the run (the game goes on with the null renderer). 0 on success. */
int rsx_render_bridge_start(const rsx_render_bridge_config *config);
void rsx_render_bridge_display(uint32_t buffer, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height);
void rsx_render_bridge_method(uint32_t subchannel, uint32_t method, uint32_t data);
void rsx_render_bridge_flip(uint32_t buffer);
void rsx_render_bridge_stop(void);
rsx_render_bridge_stats rsx_render_bridge_get_stats(void);
#ifdef __cplusplus
}
#endif
#endif
