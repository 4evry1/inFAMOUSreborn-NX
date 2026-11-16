# Included after native/Makefile. Extra policy objects are always fresh.
# Keep legacy helper tests outside PS3_NATIVE_SPU_EXECUTOR.
WWS_OBJECTS := $(BUILD)/wws_workload_info.o $(BUILD)/wws_exception_handler.o $(BUILD)/wws_ready_count.o $(BUILD)/wws_idle_request.o $(BUILD)/wws_startup_composition.o $(BUILD)/wws_workload_registration.o $(BUILD)/wws_policy_execution.o $(BUILD)/wws_spu_executor.o $(BUILD)/wws_spu_memory.o $(BUILD)/wws_spu_lifted_boundary.o $(BUILD)/wws_spu_decrementer.o $(BUILD)/wws_spu_channels.o $(BUILD)/wws_generated_policy.o $(BUILD)/wws_linked_image.o
$(BUILD)/infamous-native-probe.elf: $(WWS_OBJECTS)
WWS_INCLUDES := -I$(ROOT)/native/adapters -I$(REPO)/infamous-recomp/spu/pm_wwsjob
$(WWS_OBJECTS): CPPFLAGS += -DPS3_NATIVE_SPU_EXECUTOR $(WWS_INCLUDES)

$(BUILD)/wws_workload_registration.o: $(ROOT)/native/domain/spurs_workload_registration.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_policy_execution.o: $(ROOT)/native/adapters/spurs_policy_execution.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_spu_executor.o: $(ROOT)/native/adapters/spu_executor.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_spu_memory.o: $(ROOT)/native/adapters/spu_memory.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_spu_lifted_boundary.o: $(ROOT)/native/adapters/spu_lifted_boundary.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_spu_decrementer.o: $(ROOT)/native/adapters/spu_decrementer.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_spu_channels.o: $(REPO)/ps3recomp/runtime/spu/spu_channels.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_generated_policy.o: $(REPO)/infamous-recomp/spu/pm_wwsjob/spu_recomp.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wws_linked_image.o: $(GEN)/wws_linked_image.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(BUILD)/wws_workload_registration.d $(BUILD)/wws_policy_execution.d $(BUILD)/wws_spu_executor.d $(BUILD)/wws_spu_memory.d $(BUILD)/wws_spu_lifted_boundary.d $(BUILD)/wws_spu_decrementer.d $(BUILD)/wws_spu_channels.d $(BUILD)/wws_generated_policy.d $(BUILD)/wws_linked_image.d

.PHONY: wws-symbols
wws-symbols: $(BUILD)/infamous-native-probe.elf
	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-nm --defined-only $< > $(BUILD)/symbols.txt
	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-nm --undefined-only $< > $(BUILD)/undefined.txt
	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-nm --defined-only $(BUILD)/startup_execution.o > $(BUILD)/startup_execution.symbols.txt
	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-nm --defined-only $(BUILD)/startup_unsupported.o > $(BUILD)/startup_unsupported.symbols.txt
	$(CC) $(CPPFLAGS) -E -P -x assembler-with-cpp $(ROOT)/native/adapters/startup_unsupported.S -o $(BUILD)/taskwait-native-off.S
	$(CC) $(CPPFLAGS) -DPS3_NATIVE_STARTUP_TASKWAIT -E -P -x assembler-with-cpp $(ROOT)/native/adapters/startup_unsupported.S -o $(BUILD)/taskwait-native-on.S

$(BUILD)/wws_startup_composition.o: $(ROOT)/native/adapters/startup_wws_composition.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/wws_startup_composition.d

$(BUILD)/wws_idle_request.o: $(ROOT)/native/domain/spurs_idle_request.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/wws_idle_request.d

$(BUILD)/wws_exception_handler.o: $(ROOT)/native/domain/spurs_exception_handler.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/wws_exception_handler.d

$(BUILD)/wws_ready_count.o: $(ROOT)/native/domain/spurs_ready_count.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/wws_ready_count.d

# Only the hook consumer, composition, and displaced stubs enable this feature.
$(BUILD)/startup_execution.o $(BUILD)/startup_indirect_test.o $(BUILD)/startup_unsupported.o: CPPFLAGS += -DPS3_NATIVE_STARTUP_TASKWAIT
$(BUILD)/infamous-native-probe.elf: $(BUILD)/taskwait_horizon.o
$(BUILD)/taskwait_horizon.o: $(ROOT)/native/adapters/taskwait_horizon.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CFLAGS) -fstack-usage -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@
-include $(BUILD)/taskwait_horizon.d

$(BUILD)/wws_workload_info.o: $(ROOT)/native/domain/spurs_workload_info.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/wws_workload_info.d

# Composition-time system language adapter: independent C++ unit, no SPU flags.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/system_language_horizon.o
$(BUILD)/system_language_horizon.o: $(ROOT)/native/adapters/system_language_horizon.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CFLAGS) -fstack-usage -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@
-include $(BUILD)/system_language_horizon.d

# Pure profile validation domain unit: independent C++, no SPU flags.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/system_profile.o
$(BUILD)/system_profile.o: $(ROOT)/native/domain/system_profile.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CFLAGS) -fstack-usage -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@
-include $(BUILD)/system_profile.d

# Process-owned disc-change retention domain: independent C unit, no SPU flags.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/disc_change_callbacks.o
$(BUILD)/disc_change_callbacks.o: $(ROOT)/native/domain/disc_change_callbacks.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/disc_change_callbacks.d

# r26 GCM-init domain units: independent C units, no SPU flags. Each is linked into the probe and
# carries its own dependency file, following the disc_change_callbacks pattern exactly.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/gcm_rsx_frontend.o
$(BUILD)/gcm_rsx_frontend.o: $(ROOT)/native/domain/gcm_rsx_frontend.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gcm_rsx_frontend.d

$(BUILD)/infamous-native-probe.elf: $(BUILD)/gcm_init_service.o
$(BUILD)/gcm_init_service.o: $(ROOT)/native/domain/gcm_init_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gcm_init_service.d

$(BUILD)/infamous-native-probe.elf: $(BUILD)/gcm_init_image.o
$(BUILD)/gcm_init_image.o: $(ROOT)/native/domain/gcm_init_image.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gcm_init_image.d

$(BUILD)/infamous-native-probe.elf: $(BUILD)/rsx_fifo_walker.o
$(BUILD)/rsx_fifo_walker.o: $(ROOT)/native/domain/rsx_fifo_walker.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/rsx_fifo_walker.d

$(BUILD)/infamous-native-probe.elf: $(BUILD)/gcm_address_space.o
$(BUILD)/gcm_address_space.o: $(ROOT)/native/domain/gcm_address_space.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gcm_address_space.d

# r26 local-memory policy B: the guest-memory decorator that commits the RSX local window
# ([0xC0000000, 0x0F900000) at 64 KiB granules) on demand. Follows the gcm_address_space block exactly:
# its own object, its own dependency file, linked into the probe.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/demand_window.o
$(BUILD)/demand_window.o: $(ROOT)/native/adapters/demand_window.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/demand_window.d

# The GCM-init route is a compile-time feature enabled only on the adapter and its composition test.
$(BUILD)/startup_execution.o $(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_GCM_INIT

# game-data thread (r29 delta, retained): the platform-free game-data service domain unit. Follows the demand_window block
# exactly: its own object, a plain C11 compile with no SPU flags, its own dependency file, linked into the
# probe. It owns no globals and makes no OS calls; storage arrives through an injected port.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/gamedata_service.o
$(BUILD)/gamedata_service.o: $(ROOT)/native/domain/gamedata_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gamedata_service.d

# game-data thread (r29 delta, retained): the POSIX storage adapter that implements the injected game-data storage port
# (free space, directory create/stat, atomic PARAM.SFO write/read) under an app-owned host root.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/gamedata_storage_posix.o
$(BUILD)/gamedata_storage_posix.o: $(ROOT)/native/adapters/gamedata_storage_posix.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/gamedata_storage_posix.d

# Game data is a compile-time feature of the adapter (and of its composition test) only. Struct layouts do
# not depend on it: every game-data type and member in startup_execution.h is unconditional, and only the
# behaviour stays behind the flag. native/tests/verify_startup_gamedata.sh proves the layout is identical
# with the flag on and off.
$(BUILD)/startup_execution.o $(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_GAMEDATA

# Decision 16: lv2 syscall 403 sys_tty_write is a native console sink. The dispatch lives in the adapter
# (startup_execution.cpp) behind PS3_NATIVE_STARTUP_TTY, which is a compile-time feature of the adapter and
# of its composition test only. Every TTY type and member in startup_execution.h is unconditional, so the
# layout does not depend on the flag; native/tests/verify_startup_tty.sh proves it with the flag on and off.
$(BUILD)/startup_execution.o $(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_STARTUP_TTY

# Decision 17 (r33 delta): _cellSpursTasksetAttributeInitialize (NID 0x16394A4E) and _cellSpursEventFlagInitialize
# (NID 0x5EF96465) are served by native/domain/spurs_taskset_init.h, which is header-instantiated into the adapter
# (static inline through PS3_SPURS_TSI, its default linkage). The two ps3_hle_call cases sit in the existing
# PS3_NATIVE_STARTUP_SPURS_INITIALIZE block, which ENABLE_SPURS_INITIALIZE=1 already sets through native/Makefile for
# the whole build, so no new feature flag and no new object is added here and extra-sources.json stays at 26 entries.
# native/tests/verify_spurs_taskset_init.sh compiles the same code through native/domain/spurs_taskset_init.c, and
# native/tests/verify_startup_spurs_getinfo.sh proves the feature-off absence gate.

# Decision 21 (r35 delta, retained by r36): cellSpursCreateTasksetWithAttribute (NID 0xC10931CB) is served by
# native/domain/spurs_taskset_create.h, which is header-instantiated into the adapter (static inline through
# PS3_SPURS_TSC, its default linkage; spurs_taskset_create.c redefines the macro to plain extern linkage for the
# host test and the Switch compile check). The ps3_hle_call case sits in the existing
# PS3_NATIVE_STARTUP_SPURS_INITIALIZE block, which ENABLE_SPURS_INITIALIZE=1 already sets through native/Makefile for
# the whole build, so no new feature flag and no new object is added here and extra-sources.json stays at 26 entries.
# native/tests/verify_spurs_taskset_init.sh, verify_startup_spurs_getinfo.sh and
# verify_startup_spurs_registration.sh exercise the same code.

# Decision 22 (r35 delta, the first batched build, retained by r36): three new production units are added, each exactly like the
# r27 gamedata_service block above -- its own object, a plain C11 compile with no SPU flags, its own dependency
# file, linked into the probe ELF. extra-sources.json grows from 26 to 29 entries; the unit count itself is
# derived from that file, never restated as a literal (decision 13).
#
# Slice b3 (the cellPad domain, decision 22): the platform-free pad service. It owns the pad state and makes a
# failing cellPadInit visible as a counter; the port is injected, so it makes no OS calls of its own.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/pad_service.o
$(BUILD)/pad_service.o: $(ROOT)/native/domain/pad_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/pad_service.d

# Slice b2 (the file-system domain, decision 22): the platform-free fs service that serves lv2 syscalls
# 805/806/807/808/813/814 against an injected storage port and records every call for the B2-S5 capture.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/fs_service.o
$(BUILD)/fs_service.o: $(ROOT)/native/domain/fs_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/fs_service.d

# Slice b2 (the file-system domain, decision 22): the POSIX storage adapter that implements the injected fs
# storage port under an app-owned host root.
$(BUILD)/infamous-native-probe.elf: $(BUILD)/fs_storage_posix.o
$(BUILD)/fs_storage_posix.o: $(ROOT)/native/adapters/fs_storage_posix.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@
-include $(BUILD)/fs_storage_posix.d

# Decision 22: the pad and file-system routes are compile-time features of the adapter and of its composition
# test only, exactly like the TTY flag above. Every pad and fs type and member in startup_execution.h is
# unconditional, so the layout does not depend on either flag; native/tests/verify_startup_pad_fs.sh proves it
# with both flags on and off.
$(BUILD)/startup_execution.o $(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_STARTUP_PAD -DPS3_NATIVE_STARTUP_FS

# Decision 22: _cellSpursLFQueueInitialize (NID 0x011EE38B, slice g14 corrected by g14b) and
# cellSpursLFQueueAttachLv2EventQueue (NID 0x1656D49F, slice b1) are served by native/domain/spurs_lfqueue.h and
# native/domain/spurs_lfqueue_attach.h, which are header-instantiated into the adapter (static inline through
# PS3_SPURS_LFQ_FN / PS3_SPURS_LFQA_FN, their default linkage). Their .c files only redefine those macros to plain
# extern linkage for the host tests and the Switch compile check. They are therefore NOT production units: no new
# object is added for them here, and extra-sources.json carries only the three sources above.
# Decision 27 (r36 delta, slices G16 / G16b): the attach owner resolution inside
# native/domain/spurs_lfqueue_attach_sdk25.h now reads the CellSpurs from queue+0x74 on every arm, before the SDK
# compare. That header is already one of the includes listed here, so r36 changes no rule and adds no object.
# Decision 33 (r37 delta, slice G17 with its root G17d fix): cellSpursCreateTask (NID 0xBEB600AC) is served by
# native/domain/spurs_task_create.h, which is header-instantiated into the adapter exactly like the LFQueue
# builders above (static inline through PS3_SPURS_TKC, its default linkage), and its .c only redefines that macro
# to plain extern linkage for the host tests and the Switch compile check. It is therefore NOT a production unit:
# no object is added for it here, extra-sources.json stays at 29 entries, and the G17 stage is a host gate, not a
# link rule.
