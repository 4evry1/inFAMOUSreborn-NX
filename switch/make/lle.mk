# Decision 33, phase B1b: build a REAL NRO where the game's libsre calls run Sony's lifted code.
#
# Included after native/Makefile (which wws.mk is too), and includes wws.mk unchanged just as
# evidence/lle-sre-b1/syntax.mk does.
#
# What changes against the flag-off build, and nothing else:
#   OBJECTS   : startup_execution.o -> startup_execution_lle.o (the -DPS3_NATIVE_LLE_SRE build of the
#               SAME source); startup_indirect_test.o -> startup_indirect_lle_test.o (the composition
#               root that appends the module RAM range, maps it and chains the code lookup);
#               plus lle_sre.o, the generated glue unit and the lifted ppu_recomp_*.o.
#   Nothing else is added or removed, so the flag-off build this overlay never runs is untouched.
include $(ROOT)/make/wws.mk

# Root (C1b, 2026-11-11): the LLE capacity profile (8 child PPU slots, 12 PPU service records). It sizes shared
# structs, so it is added to EVERY object of this build, never to single targets.
CPPFLAGS += -DPS3_NATIVE_LLE_PROFILE
# Root (2026-11-12, Decision 70): guest threads without a chosen core are spread over cores 0..2. LLE_SPREAD_CORES=0 turns it off.
# Root (2026-11-12, Decision 78): guest threads run at priority 59, the level Horizon time-slices on cores 0..2.
LLE_GUEST_PRIORITY ?= 0x3B
CPPFLAGS += -DPS3_NATIVE_GUEST_PRIORITY=$(LLE_GUEST_PRIORITY)
LLE_SPREAD_CORES ?= 1
ifeq ($(LLE_SPREAD_CORES),1)
CPPFLAGS += -DPS3_NATIVE_SPREAD_CORES
endif

LLE_LIFT ?= $(BUILD)/lift/lift
LLE_GLUE ?= $(BUILD)/glue
# Root diagnostic: make ... LLE_LIVE_TRACE=-DPS3_NATIVE_LIVE_TRACE (default empty).
LLE_LIVE_TRACE ?=
# Root (C1b, 2026-11-11): the same optional defines reach main.o, so `-DPS3_NATIVE_NXLINK` (given together with or
# instead of the trace flag) turns on the stdout tee in switch_main.c. main.o ignores the trace flag itself.
$(BUILD)/main.o: CPPFLAGS += $(LLE_LIVE_TRACE)
LLE_INCLUDES := -I$(ROOT)/native/adapters -I$(LLE_GLUE) -I$(LLE_LIFT)
LLE_LIFTED_SOURCES := $(LLE_LIFT)/ppu_recomp_000.cpp

# Root fix (B1b review): the flagged objects are the ORIGINAL startup_execution.o and
# startup_indirect_test.o, built by native/Makefile's own rules with -DPS3_NATIVE_LLE_SRE appended.
# The worker's renamed copies (startup_execution_lle.o, startup_indirect_lle_test.o) lost every
# target-specific feature define that native/Makefile and wws.mk attach to the original targets
# (GCM_INIT, GAMEDATA, TTY, fs, the SPURS arms): 20 symbols were missing against r37.
# B2a adds -DPS3_NATIVE_LV2_SYNC the same way: the lv2 sync/object syscalls Sony's lifted libsre makes
# (14, 25, 90..94, 82..87, 118, 129, 131, 135, 137, 138, 141, 351) are served by the adapter arms in
# startup_execution.cpp, on the SAME ps3_lv2_objects registry the existing 128/130/134/136 arms use
# (native/domain/lv2_objects.o is already a base object of this build, so no object is added here).
# Root (C1b): -DPS3_NATIVE_SPU_RUN runs sys_spu_thread_group_start on the native SPU interpreter (spu_run_port.h).
# ppu-speed-20261113 item 1 (default ON, after host tests): the lock-free sys_lwmutex_lock/unlock path (source/ps3recomp/runtime/sync/lwmutex.c
# fast path, enabled by the composition on the flat guest window) and the import arm's guard-free TOC-slot/control-block touches.
# LLE_PPU_FAST_SYNC=0 gives the previous behaviour back (the library code stays compiled, its fast path is never enabled).
LLE_PPU_FAST_SYNC ?= 1
ifeq ($(LLE_PPU_FAST_SYNC),1)
$(BUILD)/startup_execution.o: CPPFLAGS += -DPS3_NATIVE_FAST_SYNC
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_FAST_SYNC
endif
# ppu-speed-20261113 task 3: the sleep of one guest wait-loop pause (ps3_taskwait_spin), in nanoseconds. 1000000 was the old value.
LLE_TASKWAIT_SLEEP_NS ?= 250000
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DSTARTUP_TASKWAIT_SLEEP_NS=$(LLE_TASKWAIT_SLEEP_NS)
$(BUILD)/startup_execution.o: CPPFLAGS += -DPS3_NATIVE_LLE_SRE -DPS3_NATIVE_LV2_SYNC -DPS3_NATIVE_LV2_SPU -DPS3_NATIVE_SPU_RUN $(LLE_INCLUDES) $(LLE_LIVE_TRACE)
# Root (B2b): the child-slot policy accepts any stack that fits the slot under the LLE flag.
$(BUILD)/startup_child_composition.o: CPPFLAGS += -DPS3_NATIVE_LLE_SRE
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_LLE_SRE -DPS3_NATIVE_LV2_SYNC -DPS3_NATIVE_LV2_SPU -DPS3_NATIVE_SPU_RUN $(LLE_INCLUDES) $(LLE_LIVE_TRACE)

$(BUILD)/lle_sre.o: $(ROOT)/native/adapters/lle_sre.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti \
		-DPS3_NATIVE_LLE_SRE $(LLE_INCLUDES) -c $< -o $@

$(BUILD)/lle_sre_generated.o: $(LLE_GLUE)/lle_sre_generated.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti \
		$(LLE_INCLUDES) -c $< -o $@

# Root (2026-11-12): lifted code reads and writes the vector registers through uint32_t* / uint64_t* casts of
# ppu_vr. That is only defined without type-based alias analysis (Decision 56): -fno-strict-aliasing.
$(BUILD)/lle_ppu_recomp_000.o: $(LLE_LIFT)/ppu_recomp_000.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-strict-aliasing \
		-I$(LLE_LIFT) -I$(SDK)/runtime/ppu -I$(SDK)/runtime/memory -I$(SDK)/runtime/time \
		-I$(SDK)/runtime/spu -DPPU_NO_INLINE_LOADS -DPPU_NO_INLINE_STORES -DPPU_NO_INLINE_STVX \
		-c $< -o $@

# Recursive (=), NOT immediate (:=): $(BUILD) is only final once every include has been read.
$(BUILD)/lv2_spu.o: $(ROOT)/native/domain/lv2_spu.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

# Root (C1b): the C1a SPU interpreter (spu_executor.c, already linked as wws_spu_executor.o, now calls it) and the
# C1b group runner. Same defines as the WWS SPU objects.
$(BUILD)/spu_interp_native.o: $(ROOT)/native/adapters/spu_interp_native.c | $(BUILD)
	$(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(WWS_INCLUDES) $(CFLAGS) -c $< -o $@

# Root (2026-11-12, Decision 84): the movie decoder's SPU program as lifted C (native/spu_lifted), on by default;
# LLE_SPU_LIFTED=0 builds the runner without it (every SPU thread on the interpreter, as before).
# -ffp-contract=off: the lifted code must not get fused multiply-adds the interpreter does not make.
# -fno-strict-aliasing: lifted code type-puns its vector registers (the lesson of Decision 56).
#
# Root (2026-11-13, Decision 104): the SPURS job modules as lifted C, and per-image accounting. One switch per group, all default on:
#   LLE_SPU_LIFTED=1       the Bink movie decoder (static module, matched at thread start)            -> -DPS3_SPU_LIFTED_BINK
#   LLE_SPU_LIFTED_JOBS=1  the 21 WWS job/task modules (dynamic: matched by content when the policy module calls a job in LS 0x4000,
#                          native/spu_lifted/jobs)                                                       -> -DPS3_SPU_LIFTED_JOBS
#   LLE_SPU_JOBMODS=all    which of the 21 modules are compiled in: "all", or a list of names or name fragments, e.g.
#                          LLE_SPU_JOBMODS="jobmod_42f7 jobmod_be8b". A module that is not listed stays on the interpreter.
#   LLE_SPU_ACCOUNT=1      per-image accounting, the STARTUP_SPU_IMAGES log lines (the interpreter attributes instructions at region
#                          changes only, nothing per instruction)                                        -> -DPS3_SPU_IMAGE_ACCOUNT
# LLE_SPU_LIFTED_JOBS=0 LLE_SPU_ACCOUNT=0 gives the Decision 84 build back. Not implemented (analysis in
# evidence/spu-jobs-lifted-20261113/NOTES.txt): lifted pm_wwsjob, the SPURS kernel, the sound image.
# The docker run of build_lle_nro*.sh does not forward the environment, so a switch is flipped either here (the ?= defaults) or, without
# touching this file, in evidence/spu-jobs-lifted-20261113/switches.mk (optional; see switches.mk.example there): it is read first.
-include $(ROOT)/make/switches.mk
LLE_SPU_LIFTED ?= 1
LLE_SPU_LIFTED_JOBS ?= 1
LLE_SPU_JOBMODS ?= all
LLE_SPU_ACCOUNT ?= 1
# Round 2 (2026-11-13): the Edge zlib task (jobmod_422c6e0242f5_e3090, the SPURS task that inflated 13.8 G interpreted instructions in run rv39s). Off until its
# differential tests have passed (evidence/spu-jobs-lifted-20261113/NOTES.txt), then 1: passed 2026-11-13 (22 modules: 44,000 trials / 22,858 compared / 0 mismatches default fp; rz 11,436 / 0; irq-resume 518 / 0).
LLE_SPU_TASKMODS ?= 1
# STARTUP_SPU_IMAGES: why lifted job code escapes to the interpreter and the three heaviest target pcs per module (host tests passed 2026-11-13: tables vs exact histogram).
LLE_SPU_ESCAPE_DIAG ?= 1
# Round 2: the Edge zlib task's raw inflate (entry 0x3750) as the toolchain's zlib (native/spu_lifted/jobs/native_inflate.c). Needs libz.a from devkitPro portlibs
# (-lz). Differential test passed 2026-11-13 (JOBMODS_NATIVE_INFLATE=1 verify_jobmods_lifted.sh --inflate build/inflate-corpus.bin --inflate-synthetic 1500: 10,342 cases, 0 mismatches,
# 4,425 native, the rest fall back to the lifted function); on by default.
LLE_SPU_NATIVE_INFLATE ?= 1
# Round 2 safety net (run rv47s: loading stalled with all instance-0 SPURS threads parked in the kernel's reservation-lost wait): an SPU thread parked in RdEventStat
# with LR (0x400) in its event mask and nothing pending for LLE_SPU_SPURIOUS_LR_US microseconds gets a reservation-lost event (native/adapters/spu_group_runner.c).
# Safe by the architecture (SPURS re-reads its line after every loss); bounds any lost wake-up. STARTUP_SPU_STATE shows spurious_lr=N useful=M instr_per=K.
LLE_SPU_SPURIOUS_LR ?= 1
LLE_SPU_SPURIOUS_LR_US ?= 2000
# Decision 112 (run rv50s: loading stalled again, one request pushed to the loader's SPURS queue and never popped, all instance-0 SPU threads idle): a PPU reservation
# (lwarx..stwcx) on a line the SPUs also reserve fails when anything wrote the line since the lwarx, as on hardware (the value check alone missed a putllc of another
# word of the line; source/ps3recomp/runtime/memory/atomic_domain.h). 0 gives the old value-only check back. STARTUP_PPU_RESERVATION shows noted/checked/lost.
LLE_PPU_GRANULE_RESERVATION ?= 1
CPPFLAGS += -DPS3_ATOMIC_GRANULE_RESERVATIONS=$(LLE_PPU_GRANULE_RESERVATION)
# 2026-11-13 (console run rv56s: the guest main thread spent 25 % in lean_store, stalled after the acquire loads of the atomic domain's locked_ops / reserved_map / busy_map; the
# fields every guarded operation wrote shared cache lines with them). The domain is laid out in separate line groups now (atomic_domain.h; not switchable: it is a layout).
# LLE_ATOMIC_DEBUG_LINE: 0 (default) compiles out the per-operation writes of debug_line and the enter counters (diagnostics only; the heartbeat then prints debug_line 0); 1 keeps them.
# LLE_ATOMIC_FAST_STORE_TESTS: 1 (default) the lock-free store paths read the sticky bitmap and reserved_map with plain loads and locked_ops / busy_map with acquire loads
# (ordering argument in atomic_domain.h); 0 makes them seq-cst loads again (the old code). Host tests (atomic domain, granule reservations, litmus, TSan) passed 2026-11-13.
LLE_ATOMIC_DEBUG_LINE ?= 0
LLE_ATOMIC_FAST_STORE_TESTS ?= 1
CPPFLAGS += -DPS3_ATOMIC_DEBUG_LINE=$(LLE_ATOMIC_DEBUG_LINE) -DPS3_ATOMIC_FAST_STORE_TESTS=$(LLE_ATOMIC_FAST_STORE_TESTS)
LIFTED_INCLUDES = -I$(ROOT)/native/spu_lifted -I$(ROOT)/native/spu_lifted/bink -I$(ROOT)/native/spu_lifted/jobs -I$(REPO)/ps3recomp/runtime/sync
LIFTED_DEFINES =
LIFTED_OBJECTS = $(BUILD)/spu_lifted_hybrid.o
ifeq ($(LLE_SPU_LIFTED),1)
LIFTED_DEFINES += -DPS3_SPU_LIFTED_BINK
LIFTED_OBJECTS += $(BUILD)/bink_module.o $(BUILD)/bink_lifted_rc.o $(BUILD)/bink_lifted_rows.o $(BUILD)/spu_neon_selftest.o
endif
ifeq ($(LLE_SPU_LIFTED_JOBS),1)
LIFTED_DEFINES += -DPS3_SPU_LIFTED_JOBS
endif
ifeq ($(LLE_SPU_ESCAPE_DIAG),1)
LIFTED_DEFINES += -DPS3_SPU_ESCAPE_DIAG
endif
ifeq ($(LLE_SPU_SPURIOUS_LR),1)
LIFTED_DEFINES += -DPS3_SPU_SPURIOUS_LR_US=$(LLE_SPU_SPURIOUS_LR_US)
endif
ifeq ($(LLE_SPU_NATIVE_INFLATE),1)
LIFTED_DEFINES += -DPS3_SPU_NATIVE_INFLATE
LIFTED_INCLUDES += -I$(DEVKITPRO)/portlibs/switch/include
LIFTED_OBJECTS += $(BUILD)/native_inflate.o
EXTRA_LIBS += -L$(DEVKITPRO)/portlibs/switch/lib -lz
$(BUILD)/native_inflate.o: $(ROOT)/native/spu_lifted/jobs/native_inflate.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@
endif
ifneq ($(filter 1,$(LLE_SPU_LIFTED_JOBS) $(LLE_SPU_ACCOUNT)),)
LIFTED_DEFINES += -DPS3_SPU_IMAGE_ACCOUNT
LIFTED_OBJECTS += $(BUILD)/spu_image_account.o
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_SPU_IMAGE_ACCOUNT
endif
# Root (2026-11-12, Decision 101): the NEON forms of the SPU helpers are compared with their scalar references on
# the console at start (no AArch64 execution exists on the laptop). Same flags as the interpreter's unit.
$(BUILD)/spu_neon_selftest.o: $(ROOT)/native/spu_neon/spu_neon_selftest.c | $(BUILD)
	$(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(WWS_INCLUDES) $(CFLAGS) -c $< -o $@
LIFTED_CC = $(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(LIFTED_DEFINES) $(WWS_INCLUDES) $(LIFTED_INCLUDES) $(CFLAGS) -ffp-contract=off -fno-strict-aliasing -Wno-misleading-indentation
$(BUILD)/spu_lifted_hybrid.o: $(ROOT)/native/spu_lifted/spu_lifted_hybrid.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@
$(BUILD)/spu_image_account.o: $(ROOT)/native/adapters/spu_image_account.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@
$(BUILD)/bink_module.o: $(ROOT)/native/spu_lifted/bink/bink_module.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@
$(BUILD)/bink_lifted_rc.o: $(ROOT)/native/spu_lifted/bink/generated/bink_lifted_rc.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@
$(BUILD)/bink_lifted_rows.o: $(ROOT)/native/spu_lifted/bink/generated/bink_lifted_rows.c | $(BUILD)
	$(LIFTED_CC) -c $< -o $@

# The job modules (native/spu_lifted/jobs/generated, written by gen_jobmods_lifted.py): one object per selected module, -O2 without debug
# info (the NRO does not carry it), and the table that lists them.
ifeq ($(LLE_SPU_LIFTED_JOBS),1)
JOBMOD_DIR := $(ROOT)/native/spu_lifted/jobs/generated
JOBMOD_TASKS := jobmod_422c6e0242f5_e3090
# the policy module pm_wwsjob (generated beside the jobs; round 2 item 1): compiled in only with LLE_SPU_PM=1. Host suite passed 2026-11-13 (23 modules: default fp 54,000 trials /
# 30,054 compared / 0 mismatches; rz 27,000 / 15,034 / 0; irq-resume incl. the policy module, alone and composite with four job modules: 0 mismatches). On by default.
JOBMOD_POLICY := jobmod_423ab002424d_e2BF0
LLE_SPU_PM ?= 1
# the libsre task-set policy module (the one that runs the Edge zlib task): own switch. Host suite passed 2026-11-13 (24 modules: default fp 62,000 trials / 32,851 compared / 0 mismatches;
# rz 31,000 / 16,375 / 0; irq-resume 0 mismatches). Policy modules are armed when the SPURS kernel branches to their entry (and at the first job call).
JOBMOD_POLICY_TASKSET := jobmod_4306dc024322_eA00
LLE_SPU_PM_TASKSET ?= 1
# the libsre SPURS system service module (runs when no workload is ready: where idle SPU threads park): own switch. Host suite passed 2026-11-13 (25 modules: default fp 72,000 trials /
# 37,646 compared / 0 mismatches; rz 36,000 / 18,757 / 0; irq-resume 0 mismatches).
JOBMOD_POLICY_SYSSRV := jobmod_423770024283_eA00
LLE_SPU_PM_SYSSRV ?= 1
JOBMOD_ALL := $(filter-out $(if $(filter 1,$(LLE_SPU_TASKMODS)),,$(JOBMOD_TASKS)) $(if $(filter 1,$(LLE_SPU_PM)),,$(JOBMOD_POLICY)) $(if $(filter 1,$(LLE_SPU_PM_TASKSET)),,$(JOBMOD_POLICY_TASKSET)) $(if $(filter 1,$(LLE_SPU_PM_SYSSRV)),,$(JOBMOD_POLICY_SYSSRV)),$(patsubst %_module.c,%,$(notdir $(wildcard $(JOBMOD_DIR)/*_module.c))))
JOBMOD_SEL := $(if $(filter all,$(LLE_SPU_JOBMODS)),$(JOBMOD_ALL),$(sort $(foreach w,$(LLE_SPU_JOBMODS),$(foreach n,$(JOBMOD_ALL),$(if $(findstring $(w),$(n)),$(n))))))
JOBMOD_CC = $(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(LIFTED_DEFINES) $(WWS_INCLUDES) $(LIFTED_INCLUDES) $(filter-out -g,$(CFLAGS)) -ffp-contract=off -fno-strict-aliasing -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-but-set-variable
# pattern rules: $(BUILD) is recursive and may not be final while this file is read
$(BUILD)/jobmod_%_rc.o: $(JOBMOD_DIR)/jobmod_%_rc.c | $(BUILD)
	$(JOBMOD_CC) -c $< -o $@
$(BUILD)/jobmod_%_module.o: $(JOBMOD_DIR)/jobmod_%_module.c | $(BUILD)
	$(JOBMOD_CC) -c $< -o $@
$(BUILD)/jobmods_table.o: $(JOBMOD_DIR)/jobmods_table.c | $(BUILD)
	$(JOBMOD_CC) $(addprefix -DPS3_JOBMOD_,$(JOBMOD_SEL)) -c $< -o $@
LIFTED_OBJECTS += $(BUILD)/jobmods_table.o $(foreach n,$(JOBMOD_SEL),$(BUILD)/$(n)_rc.o $(BUILD)/$(n)_module.o)
endif

# Decision 133 (2026-11-16): the SPURS kernel (the program every SPURS SPU thread starts with, LS 0x100..0x880 of the user's own libsre) as lifted C, armed by content
# at the first slice of every dynamic SPU thread and laid into every bound image (native/spu_lifted/kernel). LLE_SPU_KERNEL=0 keeps it on the interpreter.
# Its C is DERIVED from the firmware at build time: gen_kernel_lifted.py reads the libsre image of THIS build's own lift (step 1 of build_lle_nro_sibling.sh) and
# writes only into $(BUILD)/spu_kernel. Host suite passed 2026-11-16 (native/spu_lifted/kernel/verify_kernel_lifted.sh: interpreter vs lifted kernel, lifted kernel
# + lifted policy module, unknown policy module, changed kernel, no kernel module: 0 mismatches).
# Decision 135 (2026-11-16): plain SPU DMA (GET/PUT) copies directly where the atomic domain proves it the same as the guarded copy
# (ps3_atomic_dma_read / _write; lines no read-modify-write ever touched; line-atomic via per-line stripes). LLE_SPU_LOCKFREE_DMA=0: every DMA guarded as before.
LLE_SPU_LOCKFREE_DMA ?= 1
ifneq ($(LLE_SPU_LOCKFREE_DMA),1)
CPPFLAGS += -DPS3_ATOMIC_LOCKFREE_DMA=0 -DPS3_SPU_LOCKFREE_DMA=0
endif
LLE_SPU_KERNEL ?= 1
ifeq ($(LLE_SPU_KERNEL),1)
LLE_LIBSRE_IMAGE ?= $(LLE_LIFT)/../prx/libsre_image.bin
KERNEL_GEN := $(ROOT)/native/spu_lifted/kernel/gen_kernel_lifted.py
KERNEL_SRC = $(BUILD)/spu_kernel/jobmod_spurs_kernel_e818
LIFTED_DEFINES += -DPS3_SPU_LIFTED_KERNEL
LIFTED_OBJECTS += $(BUILD)/spurs_kernel_rc.o $(BUILD)/spurs_kernel_module.o
KERNEL_CC = $(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(LIFTED_DEFINES) $(WWS_INCLUDES) $(LIFTED_INCLUDES) $(filter-out -g,$(CFLAGS)) -ffp-contract=off -fno-strict-aliasing -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-but-set-variable
$(BUILD)/spu_kernel/jobmod_spurs_kernel_e818_rc.c: $(KERNEL_GEN) $(ROOT)/native/spu_lifted/jobs/gen_jobmods_lifted.py $(LLE_LIBSRE_IMAGE) | $(BUILD)
	python3 -I $(KERNEL_GEN) --libsre-image $(LLE_LIBSRE_IMAGE) --out $(BUILD)/spu_kernel
$(BUILD)/spu_kernel/jobmod_spurs_kernel_e818_module.c: $(BUILD)/spu_kernel/jobmod_spurs_kernel_e818_rc.c
$(BUILD)/spurs_kernel_rc.o: $(BUILD)/spu_kernel/jobmod_spurs_kernel_e818_rc.c | $(BUILD)
	$(KERNEL_CC) -c $< -o $@
$(BUILD)/spurs_kernel_module.o: $(BUILD)/spu_kernel/jobmod_spurs_kernel_e818_module.c | $(BUILD)
	$(KERNEL_CC) -c $< -o $@
# Decision 134: pm_wwsjob's run-time register restore sequence at LS 0x3FEC0 (the "other (stubs, top of LS)" row), a self-checking lifted function laid over the kernel.
LIFTED_OBJECTS += $(BUILD)/wws_restore_stub.o
$(BUILD)/wws_restore_stub.o: $(ROOT)/native/spu_lifted/kernel/wws_restore_stub.c | $(BUILD)
	$(KERNEL_CC) -c $< -o $@
endif

$(BUILD)/spu_group_runner.o: $(ROOT)/native/adapters/spu_group_runner.c | $(BUILD)
	$(CC) $(CPPFLAGS) -DPS3_NATIVE_SPU_EXECUTOR $(LIFTED_DEFINES) $(WWS_INCLUDES) $(LIFTED_INCLUDES) $(CFLAGS) -c $< -o $@

# Root (D2): the Horizon HID controller behind the pad service's injected pad_port. Header-only use of libnx
# (switch.h comes from the toolchain's own include path, as for every other adapter object here).
# Root (2026-11-11): the live RSX command-buffer consumer.
$(BUILD)/rsx_live_fifo.o: $(ROOT)/native/domain/rsx_live_fifo.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

# Root (2026-11-11): lv2 timer objects (syscalls 70..76).
$(BUILD)/lv2_timer.o: $(ROOT)/native/domain/lv2_timer.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

# Root (2026-11-11): the platform-free audio service behind the 13 audio import arms.
$(BUILD)/audio_service.o: $(ROOT)/native/domain/audio_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

$(BUILD)/pad_port_horizon.o: $(ROOT)/native/adapters/pad_port_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

# Root (2026-11-12, Decision 57): flat 4 GiB guest window for the NRO; switch_main.c prefers it over heap blocks.
$(BUILD)/guest_space_flat_horizon.o: $(ROOT)/native/adapters/guest_space_flat_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/main.o: CPPFLAGS += -DPS3_NATIVE_FLAT_GUEST
# Decision 60: generated-code memory access goes straight to the flat window (lock-free path).
$(BUILD)/ppu_memory_context.o: CPPFLAGS += -DPS3_NATIVE_FLAT_GUEST

# 2026-11-13 (coordinator, after run rv52s): a sampling profiler of the guest main thread (and, coarsely, every other thread). LLE_PROFILER=1 (default) compiles native/adapters/ps3_profile_*.c,
# wraps libnx's threadCreate (so every thread is known: native/adapters/ps3_profile_switch.c __wrap_threadCreate) and starts the sampler from the composition root (-DPS3_NATIVE_PROFILER). It writes
# <app dir>/profile-<seconds>.txt once a minute (and at once when profile-now.txt appears or changes); profile-off.txt next to it turns it off, profile-rate.txt sets the rate (100..2000 Hz, default 1000).
# tools/ps3_profile_report.py turns a dump and build/<tag>-nro-*/infamous-native-probe.elf into the top functions. Cost: one pause/resume pair per ms on the main thread.
LLE_PROFILER ?= 1
ifeq ($(LLE_PROFILER),1)
CPPFLAGS += -DPS3_NATIVE_PROFILER
EXTRA_LIBS += -Wl,--wrap=threadCreate
$(BUILD)/ps3_profile_core.o: $(ROOT)/native/adapters/ps3_profile_core.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -c $< -o $@
$(BUILD)/ps3_profile_switch.o: $(ROOT)/native/adapters/ps3_profile_switch.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -c $< -o $@
PROFILER_OBJECTS = $(BUILD)/ps3_profile_core.o $(BUILD)/ps3_profile_switch.o
endif

LLE_EXTRA_OBJECTS = $(BUILD)/lle_sre.o $(BUILD)/lle_sre_generated.o $(BUILD)/lle_ppu_recomp_000.o \
	$(BUILD)/lv2_spu.o $(BUILD)/spu_interp_native.o $(BUILD)/spu_group_runner.o $(BUILD)/pad_port_horizon.o $(BUILD)/audio_service.o $(BUILD)/lv2_timer.o $(BUILD)/rsx_live_fifo.o $(BUILD)/guest_space_flat_horizon.o $(LIFTED_OBJECTS) $(PROFILER_OBJECTS)

# ---- Root (2026-11-12, Decision 67): the live renderer, only with LLE_RSX_RENDER=1 -------------------------------
# ps3recomp's draw engine + native/adapters/rsx_vulkan (no shader compiler: -DRSX_VK_NO_GLSLANG, shaders come from
# <app dir>/spv-cache) + the bridge, linked against the static NXVK driver proven by the nvk_* smoke programs
# (third_party/nxvk-c3abea1; the archive list and link order are those of its switch/build/build-nro.sh, as in
# evidence/root-tools/overnight-20261111/build_replay_switch.sh). Never run when written.
LLE_RSX_RENDER ?= 0
RENDER_LINK =
ifeq ($(LLE_RSX_RENDER),1)
NXVK ?= $(ROOT)/third_party/nxvk
NXVK_CROSS := $(NXVK)/switch/build/cross
RSXVK := $(ROOT)/native/adapters/rsx_vulkan
RENDER_FLAGS = -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -DVK_NO_PROTOTYPES=1 -DRSX_VK_NO_GLSLANG=1 \
	-I$(NXVK)/include -I$(DEVKITPRO)/libnx/include -I$(SDK)/include -I$(SDK)/libs/video -I$(RSXVK) -I$(ROOT)/native/adapters \
	-I$(SDK)/runtime/memory -I$(SDK)/runtime \
	$(filter-out -std=gnu11 -Wextra -MMD -MP,$(CFLAGS)) -Wno-unused-parameter -Wno-unused-function
RENDER_BACKEND_NAMES := rsx_vk_device rsx_vk_platform rsx_vk_handle rsx_vk_target rsx_vk_texture rsx_vk_sampler rsx_vk_view \
	rsx_vk_backend rsx_vk_pipeline rsx_vk_present rsx_vk_spv_cache
RENDER_ENGINE_NAMES := rsx_draw_engine rsx_vp_decompiler rsx_fp_decompiler rsx_dispatch rsx_vertex_compact rsx_texture_layout
RENDER_OBJECTS = $(foreach n,$(RENDER_BACKEND_NAMES) $(RENDER_ENGINE_NAMES),$(BUILD)/render_$(n).o) \
	$(BUILD)/render_rsx_vk_shader.o $(BUILD)/render_bridge.o $(BUILD)/render_nvk_compat.o
$(BUILD)/render_rsx_vk_%.o: $(RSXVK)/rsx_vk_%.c | $(BUILD)
	$(CC) -std=gnu11 $(RENDER_FLAGS) -c $< -o $@
$(BUILD)/render_rsx_vk_shader.o: $(RSXVK)/rsx_vk_shader.cpp | $(BUILD)
	$(CXX) -std=gnu++17 -fno-exceptions -fno-rtti $(RENDER_FLAGS) -c $< -o $@
$(BUILD)/render_rsx_%.o: $(SDK)/libs/video/rsx_%.c | $(BUILD)
	$(CC) -std=gnu11 $(RENDER_FLAGS) -c $< -o $@
$(BUILD)/render_bridge.o: $(ROOT)/native/adapters/rsx_render_bridge.c | $(BUILD)
	$(CC) -std=gnu11 $(RENDER_FLAGS) -c $< -o $@
$(BUILD)/render_nvk_compat.o: $(NXVK)/switch/smoke/nvk_compat.c | $(BUILD)
	$(CC) -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -DVK_USE_PLATFORM_VI_NN -I$(NXVK)/include -I$(DEVKITPRO)/libnx/include \
		$(filter-out -std=gnu11 -Wextra -MMD -MP,$(CFLAGS)) -c $< -o $@
$(BUILD)/startup_indirect_test.o: CPPFLAGS += -DPS3_NATIVE_RSX_RENDER
NXVK_ARCHIVES := $(addprefix $(NXVK_CROSS)/,src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a \
	src/c11/impl/libmesa_util_c11.a src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a \
	src/compiler/rust/libcompiler_c_helpers.a src/nouveau/headers/libnvidia_headers_c.a src/nouveau/nil/libnil.a \
	src/nouveau/nil/liblibnil_format_table.a src/compiler/nir/libnir.a src/compiler/libcompiler.a \
	src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a src/vulkan/util/libvulkan_util.a \
	src/compiler/spirv/libvtn.a src/util/libxmlconfig.a)
RENDER_LINK = -Wl,-u,vk_icdGetInstanceProcAddr $(RENDER_OBJECTS) \
	-Wl,--whole-archive $(NXVK_CROSS)/src/nouveau/vulkan/libnvk.a -Wl,--no-whole-archive \
	-Wl,--start-group $(NXVK_ARCHIVES) $(DEVKITPRO)/portlibs/switch/lib/libz.a $(DEVKITPRO)/portlibs/switch/lib/libexpat.a \
	-lnx -lc -lm -lstdc++ -pthread -Wl,--end-group
$(BUILD)/infamous-native-probe.elf: $(RENDER_OBJECTS)
endif

# native/Makefile keeps REASSIGNING OBJECTS (:=-style) in feature blocks, and wws.mk already attached
# its own prerequisites to the .elf rule. Rather than fight the prerequisite list, this replaces the
# RECIPE with the same command applied to a filtered $^: the two objects this feature substitutes are
# dropped and the lifted module is added. $(BUILD) is recursive here, so it is final at recipe time.
LLE_REPLACED =
define LLE_LINK_ELF
	$(CXX) -specs=$(DEVKITPRO)/libnx/switch.specs $(filter-out -std=gnu11,$(CFLAGS)) \
		-Wl,--gc-sections,-Map,$(BUILD)/infamous-native-probe.map \
		$(filter-out $(RENDER_OBJECTS),$(LLE_LINK_OBJECTS)) \
		-L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib $(RENDER_LINK) $(EXTRA_LIBS) -lnx -lm -o $@
endef

# $^ already carries LLE_EXTRA_OBJECTS (this rule declares them), so the recipe builds the final list
# from $^ alone: drop what this feature replaces, and drop the extras before re-adding them once.
LLE_LINK_OBJECTS = $(filter-out $(LLE_REPLACED) $(LLE_EXTRA_OBJECTS),$^) $(LLE_EXTRA_OBJECTS)

$(BUILD)/infamous-native-probe.elf: $(LLE_EXTRA_OBJECTS)
$(BUILD)/infamous-native-probe.elf:
	$(LLE_LINK_ELF)

.PHONY: lle-nro
lle-nro: $(BUILD)/infamous-native-probe.nro
	@echo "LLE_NRO_BUILT $(BUILD)/infamous-native-probe.nro"
