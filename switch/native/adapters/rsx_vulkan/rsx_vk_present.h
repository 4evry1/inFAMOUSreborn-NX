#ifndef RSX_VK_PRESENT_H
#define RSX_VK_PRESENT_H
/* Presenting a colour target on the Switch screen (rsx_vk_present.c, Decision 66). On a desktop build every
 * function reports "unavailable". */
#include "rsx_vk.h"
#ifdef __cplusplus
extern "C" {
#endif
/* After rsx_vk_device_init. `native_window` is the libnx NWindow* (nwindowGetDefault()). 0 on success. */
int rsx_vk_present_init(rsx_vk_device* d, void* native_window);
/* Submits what the backend has recorded, blits the colour target `surface` to the screen and waits for it. */
int rsx_vk_present_surface(rsx_vk_device* d, u32 surface);
void rsx_vk_present_shutdown(rsx_vk_device* d);
uint64_t rsx_vk_present_count(void);
#ifdef __cplusplus
}
#endif
#endif
