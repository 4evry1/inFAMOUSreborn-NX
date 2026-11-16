/*
 * vk_loader_nxvk - vkGetInstanceProcAddr for the sibling's Vulkan loader shim.
 *
 * The sibling's backend finds its driver with dlopen("libvulkan.so.1") + dlsym("vkGetInstanceProcAddr"); on Horizon
 * horizon/src/hz_misc.c (vendored, unmodified) answers dlopen with a token for any name containing "vulkan" and dlsym of
 * "vkGetInstanceProcAddr" with the address of a function of that name, which the sibling's link takes from the portlibs
 * libvulkan.a (Mesa NVK). Our NRO links the NXVK archives instead (third_party/nxvk-c3abea1), which export only the ICD entry
 * vk_icdGetInstanceProcAddr (-Wl,-u,vk_icdGetInstanceProcAddr; native/adapters/rsx_vulkan/rsx_vk_platform.c: "that IS
 * vkGetInstanceProcAddr here"). Same signature, same behaviour for a NULL instance (global commands) and for an instance.
 */
typedef void (*hz_pfn_void)(void);
extern hz_pfn_void vk_icdGetInstanceProcAddr(void *instance, const char *name);

hz_pfn_void vkGetInstanceProcAddr(void *instance, const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}
