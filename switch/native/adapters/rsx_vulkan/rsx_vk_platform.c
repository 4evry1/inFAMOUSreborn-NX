/*
 * ps3recomp - Vulkan backend: the platform split
 *
 * This is the ONLY file in native/adapters/rsx_vulkan/ allowed to contain a
 * #if defined(__SWITCH__). Everything else is VK_NO_PROTOTYPES plus the entry
 * points loaded here, so the format table, the rings, the handle table and
 * every command recorded later are the same bytes on both targets.
 *
 * desktop: libvulkan.so.1 through dlopen + dlsym(vkGetInstanceProcAddr),
 *          headless (no surface, no WSI). lavapipe is the CI device.
 * switch:  NXVK is linked statically and exports vk_icdGetInstanceProcAddr
 *          only, so that IS vkGetInstanceProcAddr here. Slice S5 fills this
 *          branch in; S1 compiles it and nothing else.
 */
#if !defined(_POSIX_C_SOURCE)
/* dlopen/dlsym on the desktop and setenv on the Switch. Declared before every
 * other header so the feature macro is not clobbered; devkitA64's newlib
 * exposes setenv without it, glibc does not. */
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>

#if !defined(__SWITCH__)
#  include <dlfcn.h>
#endif

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"

#if defined(__SWITCH__)
/* NXVK, linked into the .nro. Declared by hand because <vulkan/vk_icd.h> is a
 * desktop-loader header and the Switch build links no loader. */
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
#endif

/* ---- availability ------------------------------------------------------ */

int rsx_vk_platform_available(void)
{
#if defined(__SWITCH__)
    return 1;
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return 0;
    dlclose(lib);
    return 1;
#endif
}

const char* rsx_vk_platform_driver(void)
{
#if defined(__SWITCH__)
    return "NXVK (linked ICD, vk_icdGetInstanceProcAddr)";
#else
    return "desktop loader (libvulkan.so.1)";
#endif
}

uint32_t rsx_vk_platform_default_in_flight(void)
{
#if defined(__SWITCH__)
    /* A swapchain's caps.minImageCount raises this in S5 (DESIGN §0 fact 1,
     * §4.5). Headless on the device nothing presents, so the floor is 3. */
    return 3u;
#else
    return 3u;
#endif
}

uint32_t rsx_vk_platform_instance_extensions(const char** out, uint32_t cap)
{
#if defined(__SWITCH__)
    /* Root (2026-11-12, Decision 66): the window system of the console, for rsx_vk_present.c. The same two names
     * the nvk_vi_swapchain smoke program enables. */
    uint32_t n = 0;
    if (out && cap >= 2) { out[n++] = "VK_KHR_surface"; out[n++] = "VK_NN_vi_surface"; }
    return n;
#else
    /* Headless on the desktop: no VK_KHR_surface, no WSI. */
    (void)out;
    (void)cap;
    return 0;
#endif
}

/* ---- entry points ------------------------------------------------------ */

uint64_t rsx_vk_platform_instance_proc(rsx_vk_procs* p, uint64_t instance,
                                       const char* name)
{
#if defined(__SWITCH__)
    return (uint64_t)(uintptr_t)vk_icdGetInstanceProcAddr(
        (VkInstance)(uintptr_t)instance, name);
#else
    PFN_vkVoidFunction fn;
    PFN_vkGetInstanceProcAddr gipa;
    if (!p->loader)
        return 0;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(p->loader,
                                            "vkGetInstanceProcAddr");
    if (!gipa)
        return 0;
    fn = gipa((VkInstance)(uintptr_t)instance, name);
    return (uint64_t)(uintptr_t)fn;
#endif
}

uint64_t rsx_vk_platform_device_proc(rsx_vk_procs* p, uint64_t device,
                                     const char* name)
{
    /* Both targets reach device entry points the same way: the loader's
     * vkGetDeviceProcAddr on the desktop, NXVK's instance proc addr on the
     * device (it returns device entry points for a live device too). */
    if (!device)
        return 0;
#if defined(__SWITCH__)
    {
        /* Root (2026-11-12): the way the proven smoke programs do it (switch/smoke/nvk_harness.h, LOAD_DEV):
         * vkGetDeviceProcAddr is asked for with the INSTANCE, then called with the device. The earlier code
         * passed the device handle to the instance lookup, which no driver is required to accept. */
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(
            (VkInstance)(uintptr_t)p->instance_for_device_procs, "vkGetDeviceProcAddr");
        if (!gdpa)
            return 0;
        return (uint64_t)(uintptr_t)gdpa((VkDevice)(uintptr_t)device, name);
    }
#else
    {
        PFN_vkGetInstanceProcAddr gipa;
        PFN_vkGetDeviceProcAddr gdpa;
        if (!p->loader)
            return 0;
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(p->loader,
                                                "vkGetInstanceProcAddr");
        if (!gipa)
            return 0;
        /* vkGetDeviceProcAddr is an instance-level entry point, so it must be
         * asked for with a real instance: the loader does not answer it with
         * VK_NULL_HANDLE, and a null answer here would make every device proc
         * look missing. rsx_vk_platform_open already proved an instance
         * exists by the time a device does, so the instance handle the caller
         * holds is what the lookup uses. */
        gdpa = (PFN_vkGetDeviceProcAddr)gipa(
            (VkInstance)(uintptr_t)p->instance_for_device_procs,
            "vkGetDeviceProcAddr");
        if (!gdpa)
            return 0;
        return (uint64_t)(uintptr_t)gdpa((VkDevice)(uintptr_t)device, name);
    }
#endif
}

/* ---- open / close ------------------------------------------------------ */

int rsx_vk_platform_open(rsx_vk_device* d)
{
    d->in_flight = rsx_vk_platform_default_in_flight();
    if (d->in_flight > RSX_VK_MAX_IN_FLIGHT)
        d->in_flight = RSX_VK_MAX_IN_FLIGHT;
#if defined(__SWITCH__)
    /* DESIGN §5 and switch/smoke/nvk_harness.h:141: this has to happen before
     * vkCreateInstance, and NXVK needs no loader handle at all. */
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
    d->procs.loader = NULL;
#else
    d->procs.loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!d->procs.loader)
        return RSX_VK_PLATFORM_ERR_LOADER;
#endif
    if (!rsx_vk_platform_instance_proc(&d->procs, 0, "vkCreateInstance"))
        return RSX_VK_PLATFORM_ERR_PROC;
    return RSX_VK_PLATFORM_OK;
}

void rsx_vk_platform_close(rsx_vk_device* d)
{
#if defined(__SWITCH__)
    d->procs.loader = NULL;
#else
    if (d->procs.loader) {
        dlclose(d->procs.loader);
        d->procs.loader = NULL;
    }
#endif
}
