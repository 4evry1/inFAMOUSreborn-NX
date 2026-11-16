/*
 * sdl_horizon - the SDL2 entry points the sibling's Vulkan backend calls, answered by horizon/src/hz_window.c, without SDL.
 *
 * The sibling links devkitPro's libSDL2 and routes 20 of these calls to hz_window.c with `-Wl,--wrap=<name>`
 * (horizon/src/hz_window.c defines __wrap_<name>; hz_build.sh passes the --wrap list). Our NRO has no SDL2 and no
 * linker --wrap options, so the plain names are defined here and forward to the __wrap_ functions. hz_window.c itself is
 * the vendored file, unmodified.
 *
 *   - __real_SDL_InitSubSystem / __real_SDL_QuitSubSystem: hz_window.c hands the flags it does not own (anything but
 *     video and audio) to the real SDL. There is none; they report success.
 *   - SDL_GetError / SDL_GetTicks / SDL_GetTicks64 are not wrapped by the sibling (real SDL answers them): here the
 *     error text is a fixed hint, the tick count is the system tick counter in milliseconds since the first call.
 *   - g_sibling_surface_state: 0 = no surface asked for yet, 1 = vkCreateViSurfaceNN worked, -1 = it failed. The bridge
 *     reads it for present_ready.
 *
 * Signatures are the C ABI of SDL2's own (pointers and ints; SDL_bool is an int), written without SDL headers so this
 * file needs nothing from the sibling's tree.
 */
#include <stdint.h>
#include <switch.h>

/* hz_window.c */
void *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags);
void __wrap_SDL_DestroyWindow(void *window);
uint32_t __wrap_SDL_GetWindowFlags(void *window);
void __wrap_SDL_GetWindowSize(void *window, int *w, int *h);
void __wrap_SDL_Vulkan_GetDrawableSize(void *window, int *w, int *h);
int __wrap_SDL_SetWindowFullscreen(void *window, uint32_t flags);
int __wrap_SDL_GetWindowDisplayIndex(void *window);
int __wrap_SDL_GetDesktopDisplayMode(int display, void *mode);
int __wrap_SDL_GetCurrentDisplayMode(int display, void *mode);
int __wrap_SDL_Vulkan_GetInstanceExtensions(void *window, unsigned *count, const char **names);
int __wrap_SDL_Vulkan_CreateSurface(void *window, void *instance, void **surface);
int __wrap_SDL_PollEvent(void *event);
int __wrap_SDL_SetRelativeMouseMode(int enabled);
int __wrap_SDL_GetRelativeMouseMode(void);
void __wrap_SDL_SetWindowGrab(void *window, int grabbed);
int __wrap_SDL_ShowCursor(int toggle);
void __wrap_SDL_WarpMouseInWindow(void *window, int x, int y);
int __wrap_SDL_CaptureMouse(int enabled);
int __wrap_SDL_InitSubSystem(uint32_t flags);
void __wrap_SDL_QuitSubSystem(uint32_t flags);
const char *__wrap_SDL_GetCurrentVideoDriver(void);

volatile int g_sibling_surface_state;

/* what hz_window.c asks of "the real SDL": nothing is left for it */
int __real_SDL_InitSubSystem(uint32_t flags) { (void)flags; return 0; }
void __real_SDL_QuitSubSystem(uint32_t flags) { (void)flags; }

int SDL_InitSubSystem(uint32_t flags) { return __wrap_SDL_InitSubSystem(flags); }
void SDL_QuitSubSystem(uint32_t flags) { __wrap_SDL_QuitSubSystem(flags); }
const char *SDL_GetCurrentVideoDriver(void) { return __wrap_SDL_GetCurrentVideoDriver(); }

void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags)
{ return __wrap_SDL_CreateWindow(title, x, y, w, h, flags); }
void SDL_DestroyWindow(void *window) { __wrap_SDL_DestroyWindow(window); }
uint32_t SDL_GetWindowFlags(void *window) { return __wrap_SDL_GetWindowFlags(window); }
void SDL_GetWindowSize(void *window, int *w, int *h) { __wrap_SDL_GetWindowSize(window, w, h); }
void SDL_Vulkan_GetDrawableSize(void *window, int *w, int *h) { __wrap_SDL_Vulkan_GetDrawableSize(window, w, h); }
int SDL_SetWindowFullscreen(void *window, uint32_t flags) { return __wrap_SDL_SetWindowFullscreen(window, flags); }
int SDL_GetWindowDisplayIndex(void *window) { return __wrap_SDL_GetWindowDisplayIndex(window); }
int SDL_GetDesktopDisplayMode(int display, void *mode) { return __wrap_SDL_GetDesktopDisplayMode(display, mode); }
int SDL_GetCurrentDisplayMode(int display, void *mode) { return __wrap_SDL_GetCurrentDisplayMode(display, mode); }

int SDL_Vulkan_GetInstanceExtensions(void *window, unsigned *count, const char **names)
{ return __wrap_SDL_Vulkan_GetInstanceExtensions(window, count, names); }
int SDL_Vulkan_CreateSurface(void *window, void *instance, void **surface)
{
    const int ok = __wrap_SDL_Vulkan_CreateSurface(window, instance, surface);
    g_sibling_surface_state = ok ? 1 : -1;
    return ok;
}

int SDL_PollEvent(void *event) { return __wrap_SDL_PollEvent(event); }
int SDL_SetRelativeMouseMode(int enabled) { return __wrap_SDL_SetRelativeMouseMode(enabled); }
int SDL_GetRelativeMouseMode(void) { return __wrap_SDL_GetRelativeMouseMode(); }
void SDL_SetWindowGrab(void *window, int grabbed) { __wrap_SDL_SetWindowGrab(window, grabbed); }
int SDL_ShowCursor(int toggle) { return __wrap_SDL_ShowCursor(toggle); }
void SDL_WarpMouseInWindow(void *window, int x, int y) { __wrap_SDL_WarpMouseInWindow(window, x, y); }
int SDL_CaptureMouse(int enabled) { return __wrap_SDL_CaptureMouse(enabled); }

/* not wrapped by the sibling; real SDL answers them there */
const char *SDL_GetError(void) { return "Horizon layer: no SDL, see the [hz] lines above"; }
uint64_t SDL_GetTicks64(void)
{
    static uint64_t first;
    const uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ull;
    if (!first) first = now;
    return now - first;
}
uint32_t SDL_GetTicks(void) { return (uint32_t)SDL_GetTicks64(); }
