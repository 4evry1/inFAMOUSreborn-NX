# Overlay for evidence/lle-sre-b1b/lle.mk: the NRO with the inFAMOUS 2 port's video library behind our render bridge API.
# Root experiment 2026-11-13. Read AFTER native/Makefile, wws.mk and lle.mk (build_lle_nro_sibling.sh puts it last on the
# make command line); nothing in those files is changed. Active only with LLE_SIBLING_RENDER=1, and then LLE_RSX_RENDER must
# stay 0 (lle.mk's own renderer block, which would add our draw engine and backend, is not active).
#
# What it does, and nothing else:
#   - builds the vendored sibling video library (native/sibling_video/engine/libs/video, 14 C units + rsx_shader_spirv.cpp),
#     the two Horizon shim sources it needs (hz_window.c, hz_misc.c), the glue (native/sibling_video/glue/*.c) and
#     native/adapters/rsx_render_bridge_sibling.c, flags as in compile_a64.sh/flags.sh (gcc port of the sibling's own
#     compile_commands.json); NOT with the NRO's CPPFLAGS: those put source/ps3recomp/include on the path, whose ps3emu/ headers
#     would shadow the sibling's;
#   - turns on -DPS3_NATIVE_RSX_RENDER for the composition (startup_indirect_test.o), as lle.mk does for LLE_RSX_RENDER=1;
#   - links those objects (instead of render_rsx_*.o, render_bridge.o) with the NXVK archives lle.mk uses, plus the sibling's
#     aarch64 glslang (your own build, GLSLANG_DIR).
LLE_SIBLING_RENDER ?= 0
ifeq ($(LLE_SIBLING_RENDER),1)
ifeq ($(LLE_RSX_RENDER),1)
$(error LLE_SIBLING_RENDER=1 replaces LLE_RSX_RENDER=1: set only one)
endif

SIBV := $(ROOT)/native/sibling_video
NXVK ?= $(ROOT)/third_party/nxvk
NXVK_CROSS := $(NXVK)/switch/build/cross
SIBENG := $(SIBV)/engine
SIB_ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections
SIB_INCLUDES := -I$(SIBENG)/include -I$(SIBENG)/libs/system -I$(SIBENG)/libs/filesystem -I$(SIBENG)/libs/input \
	-I$(SIBENG)/libs/audio -I$(SIBENG)/libs/video -I$(SIBENG)/libs/network -I$(SIBENG)/libs/spurs -I$(SIBENG)/libs/sync \
	-I$(SIBENG)/libs/codec -I$(SIBENG)/libs/font -I$(SIBENG)/libs/misc -I$(SIBENG)/runtime/spu \
	-I$(SIBV)/glue \
	-isystem $(DEVKITPRO)/portlibs/switch/include/SDL2 -isystem $(SIBV)/deps/glslang-install-switch/include \
	-isystem $(SIBV)/horizon/include -include $(SIBV)/horizon/horizon_pre.h -isystem $(DEVKITPRO)/libnx/include \
	-isystem $(NXVK)/include -isystem $(DEVKITPRO)/portlibs/switch/include
SIB_COMMON := -D__SWITCH__ -DPS3RECOMP_RSX_VULKAN -D_GNU_SOURCE -DNDEBUG $(SIB_ARCH) -O2 -g -fno-strict-aliasing \
	-ffp-contract=off -Wall -Wextra -Wno-unused-parameter -MMD -MP $(SIB_INCLUDES)
SIB_CFLAGS := -std=gnu17 $(SIB_COMMON)
SIB_CXXFLAGS := -std=gnu++20 -fno-exceptions -fno-rtti -DPS3RECOMP_HAVE_SPIRV_TRANSLATION=1 $(SIB_COMMON)

SIB_VIDEO_C := rsx_draw_engine rsx_vulkan_backend rsx_dispatch rsx_vertex_compact rsx_vertex_fetch rsx_texture_layout \
	rsx_vp_decompiler rsx_fp_decompiler rsx_wtrack rsx_split rsx_commands vk_postfx rsx_image_io rsx_button_icons
SIB_GLUE := sibling_stubs_a64 sibling_stubs_052_a64 sdl_horizon hz_screen_handover vk_loader_nxvk rename_replace   # sibling_stubs_052_a64: v0.5.2 (vk_overlay_lowend_auto)
SIB_OBJECTS = $(foreach n,$(SIB_VIDEO_C),$(BUILD)/sibv_$(n).o) $(BUILD)/sibv_rsx_shader_spirv.o \
	$(BUILD)/sibh_hz_window.o $(BUILD)/sibh_hz_misc.o $(foreach n,$(SIB_GLUE),$(BUILD)/sibg_$(n).o) \
	$(BUILD)/render_bridge_sibling.o $(BUILD)/render_nvk_compat.o
# the Switch build of glslang (not shipped; see switch/README.md): <GLSLANG_DIR>/lib/libglslang.a and libglslang-default-resource-limits.a
GLSLANG_DIR ?= $(SIBV)/deps/glslang-install-switch
SIB_GLSLANG_LIBS := $(GLSLANG_DIR)/lib/libglslang.a $(GLSLANG_DIR)/lib/libglslang-default-resource-limits.a

# Root (Decision 109): rename() onto an existing file fails on the SD card; the video sources get a replacing one
# (glue/rename_replace.c), or pipelines.bin is never updated after its first save.
$(BUILD)/sibv_%.o: $(SIBV)/engine/libs/video/%.c | $(BUILD)
	$(CC) $(SIB_CFLAGS) -Drename=sibling_rename_replace -c $< -o $@
$(BUILD)/sibv_rsx_shader_spirv.o: $(SIBV)/engine/libs/video/rsx_shader_spirv.cpp | $(BUILD)
	$(CXX) $(SIB_CXXFLAGS) -c $< -o $@
$(BUILD)/sibh_hz_window.o: $(SIBV)/horizon/src/hz_window.c | $(BUILD)
	$(CC) $(SIB_CFLAGS) -c $< -o $@
# hz_misc.c defines sysconf and posix_memalign; NXVK's nvk_compat.c (render_nvk_compat.o) defines them too. The sibling met the
# same clash with Mesa's libvulkan.a and weakened its copy; here the weak ones are the vendored file's, so the strong ones win.
$(BUILD)/sibh_hz_misc.o: $(SIBV)/horizon/src/hz_misc.c | $(BUILD)
	$(CC) $(SIB_CFLAGS) -c $< -o $(BUILD)/sibh_hz_misc.raw.o
	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-objcopy --weaken-symbol=sysconf --weaken-symbol=posix_memalign \
		$(BUILD)/sibh_hz_misc.raw.o $@
$(BUILD)/sibg_%.o: $(SIBV)/glue/%.c | $(BUILD)
	$(CC) $(SIB_CFLAGS) -c $< -o $@
# the bridge sees the sibling's headers first and ours (rsx_render_bridge.h) after
$(BUILD)/render_bridge_sibling.o: $(ROOT)/native/adapters/rsx_render_bridge_sibling.c | $(BUILD)
	$(CC) $(SIB_CFLAGS) -I$(ROOT)/native/adapters -c $< -o $@
# the same newlib gap fills as lle.mk's renderer block (sysconf, posix_memalign, getrandom, regcomp...), for the NXVK archives
$(BUILD)/render_nvk_compat.o: $(NXVK)/switch/smoke/nvk_compat.c | $(BUILD)
	$(CC) -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -DVK_USE_PLATFORM_VI_NN -I$(NXVK)/include -I$(DEVKITPRO)/libnx/include \
		$(filter-out -std=gnu11 -Wextra -MMD -MP,$(CFLAGS)) -c $< -o $@

-include $(SIB_OBJECTS:.o=.d)

$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_RSX_RENDER

NXVK_ARCHIVES := $(addprefix $(NXVK_CROSS)/,src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a \
	src/c11/impl/libmesa_util_c11.a src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a \
	src/compiler/rust/libcompiler_c_helpers.a src/nouveau/headers/libnvidia_headers_c.a src/nouveau/nil/libnil.a \
	src/nouveau/nil/liblibnil_format_table.a src/compiler/nir/libnir.a src/compiler/libcompiler.a \
	src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a src/vulkan/util/libvulkan_util.a \
	src/compiler/spirv/libvtn.a src/util/libxmlconfig.a)
# lle.mk's LLE_LINK_ELF drops $(RENDER_OBJECTS) from the object list and puts $(RENDER_LINK) in its place
RENDER_OBJECTS = $(SIB_OBJECTS)
RENDER_LINK = -Wl,-u,vk_icdGetInstanceProcAddr $(RENDER_OBJECTS) \
	-Wl,--whole-archive $(NXVK_CROSS)/src/nouveau/vulkan/libnvk.a -Wl,--no-whole-archive \
	-Wl,--start-group $(NXVK_ARCHIVES) $(DEVKITPRO)/portlibs/switch/lib/libz.a $(DEVKITPRO)/portlibs/switch/lib/libexpat.a \
	$(SIB_GLSLANG_LIBS) -lnx -lc -lm -lstdc++ -pthread -Wl,--end-group
$(BUILD)/infamous-native-probe.elf: $(SIB_OBJECTS)

# only the sibling objects (a quick compile check without the lift and the rest of the NRO)
.PHONY: sibling-objects
sibling-objects: $(SIB_OBJECTS)
endif

# Decision 130 (2026-11-16, user request): LLE_LEAN=1 = a build for measuring speed with the heavy diagnostics off: no GPU timestamp
# profiler / batch stats by default (-DPS3_LEAN_BUILD). Pass LLE_PROFILER=0 LLE_SPU_ESCAPE_DIAG=0 on the same make line (build_lle_nro_sibling.sh
# does that for LLE_LEAN=1); the 10 s heartbeat (flips) stays.
LLE_LEAN ?= 0
ifeq ($(LLE_LEAN),1)
CPPFLAGS += -DPS3_LEAN_BUILD
endif

# Decision 136 (2026-11-16): LLE_EMU=1 = a build for the Eglantine emulator (32 MiB flat-guest slabs: the emulator refuses more than ~81
# code-memory mappings). Not for the console.
LLE_EMU ?= 0
ifeq ($(LLE_EMU),1)
CPPFLAGS += -DPS3_FLAT_SLAB_BYTES=0x2000000
endif
