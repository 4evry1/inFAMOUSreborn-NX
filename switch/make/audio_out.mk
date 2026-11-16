# Overlay for evidence/lle-sre-b1b/lle.mk: real sound. The audio service (native/domain/audio_service.c, Decision 36/37) serves the
# game's cellAudio calls and keeps its ring of blocks; until now nothing played them. This overlay builds the four units that do and
# turns the hook on in the composition.
# Root experiment 2026-11-15. Read AFTER native/Makefile, wws.mk and lle.mk (build_lle_nro_sibling.sh would put it last on the make
# command line, after sibling.mk and restore_objects.mk); nothing in those files is changed. Independent of sibling.mk: it works with
# or without LLE_SIBLING_RENDER. LLE_AUDIO_OUT=0 on the make command line leaves the overlay inert.
#
# What it does, and nothing else:
#   - compiles audio_mix.c (downmix, silence test), audio_out_policy.c (ring plan, queue plan), audio_out_horizon.c (libnx audout and
#     its thread) with the NRO's own flags, as audio_service.o is built, and startup_audio_out.cpp (the pump: ticks the audio service,
#     takes finished blocks from the guest ring, clears them) with the feature set of startup_execution.o, because it includes
#     startup_execution.h ("one feature set, one StartupControl layout");
#   - turns on -DPS3_NATIVE_AUDIO_OUT for the composition (startup_indirect_test.o): the start and stop calls in its audio ticker area;
#   - links the four objects (prerequisites of the .elf: lle.mk's recipe takes every prerequisite through $^).
# Without this file, or with LLE_AUDIO_OUT=0, no line of the build changes.
LLE_AUDIO_OUT ?= 1
ifeq ($(LLE_AUDIO_OUT),1)

AUDIO_OUT_OBJECTS = $(BUILD)/audio_mix.o $(BUILD)/audio_out_policy.o $(BUILD)/audio_out_horizon.o $(BUILD)/startup_audio_out.o

$(BUILD)/audio_mix.o: $(ROOT)/native/domain/audio_mix.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
$(BUILD)/audio_out_policy.o: $(ROOT)/native/domain/audio_out_policy.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
# libnx's <switch.h> only: no SPU header in this unit (their u128 typedef clashes with libnx's)
$(BUILD)/audio_out_horizon.o: $(ROOT)/native/adapters/audio_out_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

# the feature set of startup_execution.o (native/Makefile N12_STARTUP_FEATURES, wws.mk, lle.mk), written out because a target-specific
# variable cannot be read from another target
AUDIO_OUT_STARTUP_FEATURES := $(N12_STARTUP_FEATURES) -DPS3_NATIVE_STARTUP_TASKWAIT -DPS3_NATIVE_GCM_INIT -DPS3_NATIVE_GAMEDATA \
	-DPS3_NATIVE_STARTUP_TTY -DPS3_NATIVE_STARTUP_PAD -DPS3_NATIVE_STARTUP_FS -DPS3_NATIVE_LLE_SRE -DPS3_NATIVE_LV2_SYNC \
	-DPS3_NATIVE_LV2_SPU -DPS3_NATIVE_SPU_RUN $(LLE_INCLUDES)
ifeq ($(LLE_PPU_FAST_SYNC),1)
AUDIO_OUT_STARTUP_FEATURES += -DPS3_NATIVE_FAST_SYNC
endif
$(BUILD)/startup_audio_out.o: CPPFLAGS += $(AUDIO_OUT_STARTUP_FEATURES)
$(BUILD)/startup_audio_out.o: $(ROOT)/native/adapters/startup_audio_out.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 \
		-I$(REPO)/infamous-recomp/recompiled -c $< -o $@

-include $(AUDIO_OUT_OBJECTS:.o=.d)

# the composition's start and stop calls (test_startup_indirect.cpp, audio ticker area)
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_AUDIO_OUT

$(BUILD)/infamous-native-probe.elf: $(AUDIO_OUT_OBJECTS)

# only the four objects (a quick compile check without the lift and the rest of the NRO)
.PHONY: audio-out-objects
audio-out-objects: $(AUDIO_OUT_OBJECTS)
endif
