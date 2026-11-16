# Layout: <repo>/switch/native/Makefile. ROOT is the switch/ folder, REPO the checkout (ps3recomp, infamous-recomp).
# GEN holds what the build derives from the player's own game or firmware (switch/scripts/*); it is never committed.
ROOT := $(abspath ..)
REPO ?= $(abspath ../..)
GEN ?= $(ROOT)/build/generated
SDK := $(REPO)/ps3recomp
BUILD ?= $(ROOT)/build/switch
ifeq ($(ENABLE_HEAP_THREAD_ID),1)
PROBE_VERSION ?= 0.20.0
PROBE_TITLE ?= inFAMOUS Native Heap Thread ID Probe
else
PROBE_VERSION ?= 0.7.0
PROBE_TITLE ?= inFAMOUS Native CPU Probe
endif
NRO_HEAP_BYTES ?= 0
EXEFS_ELF := $(BUILD)/infamous-native-probe-exefs.elf
ifneq ($(NRO_HEAP_BYTES),0)
NRO_HEAP_CPPFLAGS := -DPS3_NATIVE_NRO_HEAP_BYTES=$(NRO_HEAP_BYTES)
EXEFS_ELF := $(BUILD)/infamous-native-probe.elf
endif
DEVKITPRO ?= /opt/devkitpro
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
CPPFLAGS := $(NRO_HEAP_CPPFLAGS) -DPS3_NATIVE_PROBE_VERSION=\"$(PROBE_VERSION)\" -D__SWITCH__ -DPS3_SPU_EXTERNAL_FP_CONTROL -I$(SDK)/include -I$(SDK)/runtime/spu -I$(SDK)/runtime/loader -I$(SDK)/runtime/memory -I$(DEVKITPRO)/libnx/include
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -fPIE -ffunction-sections -fdata-sections -frounding-math -ffp-contract=off -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -MMD -MP
OBJECTS := $(BUILD)/main.o $(BUILD)/helpers.o $(BUILD)/numeric.o $(BUILD)/double.o $(BUILD)/fp_state.o $(BUILD)/xfk.o $(BUILD)/memory_layout.o $(BUILD)/sparse_pages.o $(BUILD)/physical_pages.o $(BUILD)/guest_space.o $(BUILD)/guest_space_test.o $(BUILD)/elf_image.o $(BUILD)/elf_load.o $(BUILD)/real_elf.o $(BUILD)/generated_ppu_test.o $(BUILD)/ppu_probe_memory.o $(GEN)/ppu_recomp_000.aarch64.o $(BUILD)/support.o $(BUILD)/fp_control.o

# N12: native SPURS initialize (aa6269a8). Off by default; with ENABLE_SPURS_INITIALIZE=1 the complete
# thread-id/timebase/context startup composition plus its sync/policy/SPURS-attribute bridges are selected.
ifeq ($(ENABLE_SPURS_INITIALIZE),1)
ENABLE_THREAD_ID_QUERY := 1
CPPFLAGS += -DPS3_NATIVE_STARTUP_SPURS_INITIALIZE -DPS3_NATIVE_STARTUP_SPURS_ATTRIBUTES -DPS3_NATIVE_STARTUP_SPU_INITIALIZE -DPS3_NATIVE_STARTUP_LWCOND
endif

ifeq ($(ENABLE_ENTRY_CHECKPOINT),1)
CPPFLAGS += -DPS3_NATIVE_ENTRY_CHECKPOINT -I$(SDK)/runtime/ppu
OBJECTS := $(filter-out $(BUILD)/generated_ppu_test.o $(BUILD)/ppu_probe_memory.o $(GEN)/ppu_recomp_000.aarch64.o,$(OBJECTS))
OBJECTS += $(BUILD)/entry_checkpoint.o $(BUILD)/entry_checkpoint_test.o $(BUILD)/tls_initialize.o $(ROOT)/build/stage5-entry/entry_000.o $(ROOT)/build/stage5-entry/entry_002.o
endif

ifeq ($(ENABLE_HEAP_THREAD_ID),1)
ifneq ($(filter 1,$(ENABLE_HEAP_STARTUP) $(ENABLE_ENTRY_CHECKPOINT) $(ENABLE_GPU_CLEAR) $(ENABLE_GENERATED_PPU) $(ENABLE_USER_MEMORY) $(ENABLE_LWMUTEX) $(MEMORY_SCOUT)),)
$(error Heap thread-ID cannot be mixed with heap prefix, raw/legacy entry, GPU or standalone memory modes)
endif
ifneq ($(filter-out 1,$(ENABLE_THREAD_ID_QUERY) $(ENABLE_TIMEBASE_QUERY) $(ENABLE_INDIRECT_DISPATCH) $(ENABLE_CONTEXT_MEMORY) $(ENABLE_STARTUP_CLOCK) $(ENABLE_STARTUP_MEMORY) $(ENABLE_STARTUP_LWMUTEX) $(ENABLE_STARTUP_RESUME) $(INDIRECT_SCOUT)),)
$(error Heap thread-ID requires the complete thread-ID/timebase/context scout)
endif
ENABLE_THREAD_ID_QUERY := 1
CPPFLAGS += -DPS3_NATIVE_HEAP_THREAD_ID
endif

ifeq ($(ENABLE_HEAP_STARTUP),1)
ifneq ($(filter 1,$(ENABLE_THREAD_ID_QUERY) $(ENABLE_TIMEBASE_QUERY) $(INDIRECT_SCOUT) $(MEMORY_SCOUT) $(ENABLE_ENTRY_CHECKPOINT) $(ENABLE_GPU_CLEAR) $(ENABLE_GENERATED_PPU) $(ENABLE_USER_MEMORY) $(ENABLE_LWMUTEX)),)
$(error Heap startup cannot be mixed with scout, raw/legacy entry, GPU or standalone memory modes)
endif
ifneq ($(filter-out 1,$(ENABLE_INDIRECT_DISPATCH) $(ENABLE_CONTEXT_MEMORY) $(ENABLE_STARTUP_CLOCK) $(ENABLE_STARTUP_MEMORY) $(ENABLE_STARTUP_LWMUTEX) $(ENABLE_STARTUP_RESUME)),)
$(error Heap startup requires the complete context/indirect supported prefix)
endif
ENABLE_INDIRECT_DISPATCH := 1
CPPFLAGS += -DPS3_NATIVE_HEAP_STARTUP
endif

ifneq ($(filter 1,$(ENABLE_HEAP_STARTUP) $(ENABLE_HEAP_THREAD_ID)),)
CPPFLAGS += -DPS3_NATIVE_HEAP_BACKEND
OBJECTS += $(BUILD)/heap_guest_space.o
endif

ifeq ($(ENABLE_THREAD_ID_QUERY),1)
ENABLE_TIMEBASE_QUERY := 1
CPPFLAGS += -DPS3_NATIVE_THREAD_ID_QUERY
endif

ifeq ($(ENABLE_TIMEBASE_QUERY),1)
ENABLE_INDIRECT_DISPATCH := 1
INDIRECT_SCOUT := 1
CPPFLAGS += -DPS3_NATIVE_STARTUP_TIMEBASE -I$(SDK)/runtime/time
endif

ifeq ($(ENABLE_INDIRECT_DISPATCH),1)
ENABLE_CONTEXT_MEMORY := 1
CPPFLAGS += -DPS3_NATIVE_INDIRECT_DISPATCH -I$(GEN)/compiled-code-index
ifeq ($(INDIRECT_SCOUT),1)
CPPFLAGS += -DPS3_NATIVE_INDIRECT_SCOUT
endif
endif

ifeq ($(ENABLE_CONTEXT_MEMORY),1)
ifneq ($(filter 1,$(ENABLE_ENTRY_CHECKPOINT) $(ENABLE_GPU_CLEAR)),)
$(error Context-memory startup cannot be mixed with entry or GPU composition)
endif
ENABLE_STARTUP_CLOCK := 1
CPPFLAGS := -I$(GEN)/ppu-memory-context $(CPPFLAGS)
CPPFLAGS += -DPS3_NATIVE_CONTEXT_MEMORY -DPPU_NO_INLINE_LOADS=1 -DPPU_NO_INLINE_STORES=1 -DPPU_NO_INLINE_STVX=1
endif

ifeq ($(ENABLE_STARTUP_CLOCK),1)
ENABLE_STARTUP_MEMORY := 1
CPPFLAGS += -DPS3_NATIVE_STARTUP_CLOCK
endif

ifeq ($(ENABLE_STARTUP_MEMORY),1)
ENABLE_STARTUP_LWMUTEX := 1
CPPFLAGS += -DPS3_NATIVE_STARTUP_MEMORY
ifeq ($(MEMORY_SCOUT),1)
CPPFLAGS += -DPS3_NATIVE_MEMORY_SCOUT
endif
endif

ifeq ($(ENABLE_STARTUP_LWMUTEX),1)
ENABLE_STARTUP_RESUME := 1
CPPFLAGS += -DPS3_NATIVE_STARTUP_LWMUTEX -I$(SDK)/runtime/sync
STARTUP_OWNER_FLAGS := --mutex-family
endif

ifeq ($(ENABLE_STARTUP_RESUME),1)
CPPFLAGS += -DPS3_NATIVE_STARTUP_RESUME -I$(SDK)/runtime/ppu
OBJECTS := $(filter-out $(BUILD)/generated_ppu_test.o $(BUILD)/ppu_probe_memory.o $(GEN)/ppu_recomp_000.aarch64.o,$(OBJECTS))
OBJECTS += $(BUILD)/startup_legacy_014.o $(BUILD)/startup_unsupported.o $(BUILD)/startup_resume_test.o $(BUILD)/tls_initialize.o
OBJECTS += $(foreach unit,000 001 002 003 004,$(ROOT)/build/ppu-native-o2/20261101T123642Z/ppu_recomp_$(unit).o)
endif

ifeq ($(ENABLE_USER_MEMORY),1)
CPPFLAGS += -DPS3_NATIVE_USER_MEMORY
OBJECTS += $(BUILD)/user_memory.o $(BUILD)/atomic_domain.o $(BUILD)/user_memory_test.o
endif

ifeq ($(ENABLE_LWMUTEX),1)
CPPFLAGS += -DPS3_NATIVE_LWMUTEX -I$(SDK)/runtime/sync
OBJECTS += $(BUILD)/atomic_domain.o $(BUILD)/lwmutex.o $(BUILD)/lwmutex_wait.o $(BUILD)/lwmutex_test.o
endif

ifeq ($(ENABLE_STARTUP_LWMUTEX),1)
OBJECTS := $(filter-out $(BUILD)/startup_resume_test.o,$(OBJECTS))
OBJECTS += $(BUILD)/startup_lwmutex_test.o $(BUILD)/atomic_domain.o $(BUILD)/lwmutex.o $(BUILD)/lwmutex_wait.o
endif

ifeq ($(ENABLE_STARTUP_MEMORY),1)
STARTUP_OWNER_FLAGS += --memory-family
OBJECTS := $(filter-out $(BUILD)/startup_lwmutex_test.o,$(OBJECTS))
OBJECTS += $(BUILD)/startup_memory_test.o $(BUILD)/user_memory.o $(BUILD)/user_memory_service.o
endif

ifeq ($(ENABLE_STARTUP_CLOCK),1)
STARTUP_OWNER_FLAGS += --clock-prefix
OBJECTS := $(filter-out $(BUILD)/startup_memory_test.o,$(OBJECTS))
OBJECTS += $(BUILD)/startup_clock_test.o $(BUILD)/clock.o $(BUILD)/clock_horizon.o $(BUILD)/clock_horizon_test.o $(BUILD)/clock_test.o
endif

ifeq ($(ENABLE_CONTEXT_MEMORY),1)
OBJECTS := $(filter-out $(BUILD)/startup_legacy_014.o $(BUILD)/startup_clock_test.o $(ROOT)/build/ppu-native-o2/20261101T123642Z/%,$(OBJECTS))
OBJECTS += $(BUILD)/startup_execution.o $(BUILD)/ppu_memory_context.o $(BUILD)/ppu_memory_trace.o $(BUILD)/startup_context_test.o $(BUILD)/ppu_memory_trace_test.o
OBJECTS += $(foreach unit,000 001 002 003 004,$(ROOT)/build/ppu-context-native-o2/20261101T164147Z/ppu_recomp_$(unit).o)
STARTUP_OWNER_FLAGS += --context-memory
endif

ifeq ($(ENABLE_INDIRECT_DISPATCH),1)
OBJECTS := $(filter-out $(BUILD)/startup_context_test.o,$(OBJECTS))
OBJECTS += $(BUILD)/startup_indirect_test.o $(BUILD)/compiled_dispatch.o $(BUILD)/compiled_code_table.o $(BUILD)/compiled_code_index.o
STARTUP_OWNER_FLAGS += --indirect-dispatch
endif

ifeq ($(ENABLE_SPURS_INITIALIZE),1)
# lv2_objects/user_memory are required by the extended spu_resource_policy; the Horizon notify/thread
# adapters realise the injected ports. The composition must still construct ps3_spurs_runtime and set
# StartupExecution::spurs (otherwise aa6269a8 stops with MissingServices: gated, never faked).
SPURS_OBJECTS := $(foreach o,lv2_objects ppu_threads spu_resource_policy spurs_kernel1_service spurs_instance spurs_attributes lwcond wait_port_horizon service_thread_horizon user_memory atomic_domain lwmutex lwmutex_wait discovery_budget discovery_budget_binding startup_observation,$(BUILD)/$(o).o)
OBJECTS := $(filter-out $(SPURS_OBJECTS),$(OBJECTS)) $(SPURS_OBJECTS)
endif

# N12 startup composition: the accepted Stage33 continuation composition (HEAP_THREAD_ID prefix, timebase/
# context/lwmutex/lwcond/exitspawn/sysmodule/sysutil/spu-initialize/thread-priority/spurs-attributes flags,
# BUDGET_TEST + DISCOVERY one-shot owner) plus the native SPURS initialize bridge and its composition.
# Select with: ENABLE_HEAP_THREAD_ID=1 ENABLE_N12_STARTUP=1 N12_REUSED="<names>"; the link list is the accepted
# Stage33 order. N12_REUSED names objects taken IN PLACE from the accepted build directory: only objects whose
# accepted object hash and complete project-dependency closure are proven unchanged (evidence build_n12.py);
# every other object is compiled here with the flags below. Objects with no rule here must be reused.
ifeq ($(ENABLE_N12_STARTUP),1)
ifneq ($(ENABLE_HEAP_THREAD_ID),1)
$(error N12 startup composition requires ENABLE_HEAP_THREAD_ID=1 (accepted Stage33 prefix))
endif
N12_ACCEPTED ?= $(GEN)/accepted-objects-unused
N12_PPU_DIR ?= $(GEN)/ppu-objects
N12_STARTUP_FEATURES := -DPS3_NATIVE_STARTUP_CURRENT_TIME -DPS3_NATIVE_STARTUP_TIMEZONE -DPS3_NATIVE_STARTUP_TIMEBASE_COUNTER -DPS3_NATIVE_STARTUP_LWCOND -DPS3_NATIVE_STARTUP_EXITSPAWN -DPS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION -DPS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS -DPS3_NATIVE_STARTUP_SPU_INITIALIZE -DPS3_NATIVE_STARTUP_THREAD_PRIORITY -DPS3_NATIVE_STARTUP_SPURS_ATTRIBUTES -DPS3_NATIVE_STARTUP_SPURS_INITIALIZE
N12_TEST_FEATURES := -DPS3_NATIVE_STARTUP_BUDGET_TEST -DPS3_NATIVE_STARTUP_DISCOVERY -Wformat=2 -Werror=format
N12_NAMES_BEFORE_PPU := main helpers numeric double fp_state xfk memory_layout sparse_pages physical_pages guest_space guest_space_test elf_image elf_load real_elf support fp_control heap_guest_space startup_unsupported tls_initialize atomic_domain lwmutex lwmutex_wait user_memory user_memory_service clock clock_horizon clock_horizon_test clock_test startup_execution ppu_memory_context ppu_memory_trace ppu_memory_trace_test
N12_NAMES_AFTER_PPU := startup_indirect_test compiled_dispatch compiled_code_table compiled_code_index discovery_budget discovery_budget_binding startup_observation calendar_clock calendar_clock_horizon timebase lwcond exitspawn_callbacks module_admission sysutil_callback_registry spu_resource_policy spurs_attributes
N12_NAMES_NEW := lv2_objects ppu_threads spurs_kernel1_service spurs_instance wait_port_horizon service_thread_horizon startup_spurs_composition startup_child_ppu startup_child_composition
N12_OBJECT = $(if $(filter $(1),$(N12_REUSED)),$(N12_ACCEPTED)/$(1).o,$(BUILD)/$(1).o)
OBJECTS := $(foreach n,$(N12_NAMES_BEFORE_PPU),$(call N12_OBJECT,$(n))) $(foreach unit,000 001 002 003 004,$(N12_PPU_DIR)/ppu_recomp_$(unit).o) $(foreach n,$(N12_NAMES_AFTER_PPU) $(N12_NAMES_NEW),$(call N12_OBJECT,$(n)))
# Every translation unit that includes startup_execution.h shares one feature set (one StartupControl layout).
$(BUILD)/startup_execution.o $(BUILD)/startup_spurs_composition.o $(BUILD)/startup_child_ppu.o $(BUILD)/startup_child_composition.o $(BUILD)/startup_indirect_test.o: CPPFLAGS += $(N12_STARTUP_FEATURES)
$(BUILD)/startup_indirect_test.o: CPPFLAGS += $(N12_TEST_FEATURES)
$(BUILD)/startup_execution.o $(BUILD)/startup_child_ppu.o $(BUILD)/startup_child_composition.o $(BUILD)/startup_indirect_test.o: CFLAGS += -fstack-usage
# Application-data prefix (flags/logs/EBOOT) of the accepted Stage33 runner; switch_main.c keeps the generic probe directory otherwise.
$(BUILD)/main.o: CPPFLAGS += -DPS3_NATIVE_APPDATA_STAGE31
endif

.PHONY: all
all: $(BUILD)/infamous-native-probe.nro

$(BUILD):
	mkdir -p $@

$(BUILD)/main.o: switch_main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/helpers.o: $(SDK)/runtime/spu/tests/test_spu_helpers.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dmain=native_spu_helper_tests -c $< -o $@
$(BUILD)/numeric.o: $(SDK)/runtime/spu/tests/test_spu_numeric_semantics.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dmain=native_spu_numeric_tests -c $< -o $@
$(BUILD)/double.o: tests/test_double_mode.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/fp_state.o: tests/test_fp_state.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/memory_layout.o: tests/test_memory_layout.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/sparse_pages.o: tests/test_sparse_pages.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/physical_pages.o: tests/test_physical_pages.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/xfk.o: tests/test_xfk.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/support.o: tests/test_support.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/fp_control.o: adapters/fp_control_arm64.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/infamous-native-probe.elf: $(OBJECTS)
	$(CXX) -specs=$(DEVKITPRO)/libnx/switch.specs $(filter-out -std=gnu11,$(CFLAGS)) -Wl,--gc-sections,-Map,$(BUILD)/infamous-native-probe.map $^ -L$(DEVKITPRO)/libnx/lib $(EXTRA_LIBS) -lnx -lm -o $@

$(BUILD)/infamous-native-probe.nacp: Makefile | $(BUILD)
	$(DEVKITPRO)/tools/bin/nacptool --create "$(PROBE_TITLE)" "Local development" "$(PROBE_VERSION)" $@

$(BUILD)/infamous-native-probe.nro: $(BUILD)/infamous-native-probe.elf $(BUILD)/infamous-native-probe.nacp
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=$(BUILD)/infamous-native-probe.nacp --icon=$(CURDIR)/icon.jpg

-include $(OBJECTS:.o=.d)
-include $(BUILD)/main_exefs.d

.PHONY: exefs
exefs: $(BUILD)/exefs/main $(BUILD)/exefs/main.npdm

$(BUILD)/exefs:
	mkdir -p $@

$(BUILD)/exefs/main: $(EXEFS_ELF) | $(BUILD)/exefs
	$(DEVKITPRO)/tools/bin/elf2nso $< $@

$(BUILD)/exefs/main.npdm: probe-exefs.json | $(BUILD)/exefs
	$(DEVKITPRO)/tools/bin/npdmtool $< $@

.PHONY: exefs-pfs0
exefs-pfs0: $(BUILD)/infamous-native-probe-exefs.nsp

$(BUILD)/infamous-native-probe-exefs.nsp: $(BUILD)/exefs/main $(BUILD)/exefs/main.npdm
	$(DEVKITPRO)/tools/bin/build_pfs0 $(BUILD)/exefs $@

$(BUILD)/guest_space.o: adapters/guest_space_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/guest_space_test.o: tests/test_guest_space.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/main_exefs.o: switch_main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPS3_NATIVE_BOUNDED_HEAP -c $< -o $@

$(BUILD)/infamous-native-probe-exefs.elf: $(filter-out $(BUILD)/main.o,$(OBJECTS)) $(BUILD)/main_exefs.o
	$(CXX) -specs=$(DEVKITPRO)/libnx/switch.specs $(filter-out -std=gnu11,$(CFLAGS)) -Wl,--gc-sections,-Map,$(BUILD)/infamous-native-probe-exefs.map $^ -L$(DEVKITPRO)/libnx/lib $(EXTRA_LIBS) -lnx -lm -o $@

$(BUILD)/elf_image.o: $(SDK)/runtime/loader/elf_image.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/elf_load.o: $(SDK)/runtime/loader/elf_load.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/real_elf.o: tests/load_real_elf.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/generated_ppu_test.o: tests/test_generated_ppu.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@

$(BUILD)/ppu_probe_memory.o: adapters/ppu_probe_memory.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@

ifeq ($(ENABLE_GPU_CLEAR),1)
CPPFLAGS += -DPS3_NATIVE_GPU_CLEAR
OBJECTS += $(BUILD)/gpu_clear.o $(BUILD)/gpu_clear_test.o
EXTRA_LIBS += -ldeko3d
endif
$(BUILD)/gpu_clear.o: adapters/gpu_clear_deko.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -Iadapters -c $< -o $@
$(BUILD)/gpu_clear_test.o: tests/test_gpu_clear.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -Iadapters -c $< -o $@

ifeq ($(ENABLE_GPU_CLEAR),1)
$(BUILD)/infamous-native-probe.elf $(BUILD)/infamous-native-probe-exefs.elf: $(BUILD)/gpu_clear.o $(BUILD)/gpu_clear_test.o
endif

$(BUILD)/entry_checkpoint.o: adapters/entry_checkpoint.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/entry_checkpoint_test.o: tests/test_entry_checkpoint.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/tls_initialize.o: $(SDK)/runtime/ppu/tls_initialize.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
-include $(BUILD)/gpu_clear.d $(BUILD)/gpu_clear_test.d

$(BUILD)/startup_legacy_014.o: legacy/startup_execution_014.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/startup_execution.o: adapters/startup_execution.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/startup_spurs_composition.o: adapters/startup_spurs_composition.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/startup_child_ppu.o: adapters/startup_child_ppu.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/startup_child_composition.o: adapters/startup_child_composition.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/startup_unsupported.o: adapters/startup_unsupported.S | $(BUILD)
	$(CC) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -c $< -o $@
$(BUILD)/startup_resume_test.o: tests/test_startup_resume.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@

ifeq ($(ENABLE_N12_STARTUP),1)
# The N12 composition links the accepted Stage33 PPU objects; their frozen hashes and the link are verified by the
# evidence build driver (check_startup_owner.py inspects the older generic PPU object set instead).
else ifeq ($(ENABLE_STARTUP_RESUME),1)
.PHONY: startup-owner-check
startup-owner-check: | $(BUILD)
	python3 $(ROOT)/tools/check_startup_owner.py --output $(BUILD)/startup-owner-check.json $(STARTUP_OWNER_FLAGS)
$(BUILD)/infamous-native-probe.elf $(BUILD)/infamous-native-probe-exefs.elf: | startup-owner-check
endif

$(BUILD)/user_memory.o: $(SDK)/runtime/memory/user_memory.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/atomic_domain.o: $(SDK)/runtime/memory/atomic_domain.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/user_memory_test.o: tests/test_user_memory_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/lwmutex.o: $(SDK)/runtime/sync/lwmutex.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/lwmutex_wait.o: adapters/lwmutex_wait_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/lwmutex_test.o: tests/test_lwmutex_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/startup_lwmutex_test.o: tests/test_startup_lwmutex.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@

$(BUILD)/startup_memory_test.o: tests/test_startup_memory.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@
$(BUILD)/user_memory_service.o: $(SDK)/runtime/memory/user_memory_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/clock.o: $(SDK)/runtime/time/clock.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/clock_horizon.o: adapters/clock_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/clock_horizon_test.o: tests/test_clock_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/clock_test.o: tests/test_clock.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(SDK)/runtime -Dmain=native_clock_conversion_tests -c $< -o $@
$(BUILD)/startup_clock_test.o: tests/test_startup_clock.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -DPPU_INLINE_LOADS=0 -I$(REPO)/infamous-recomp/recompiled -c $< -o $@

$(BUILD)/ppu_memory_context.o: adapters/ppu_memory_context.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@
$(BUILD)/ppu_memory_trace.o: adapters/ppu_memory_trace.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@
$(BUILD)/ppu_memory_trace_test.o: tests/test_ppu_memory_trace.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -Dmain=native_test_ppu_memory_trace -Iadapters -c $< -o $@
$(BUILD)/startup_context_test.o: tests/test_startup_context.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@

$(BUILD)/compiled_dispatch.o: $(SDK)/runtime/ppu/compiled_dispatch.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/compiled_code_table.o: adapters/compiled_code_table.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@
$(BUILD)/compiled_code_index.o: $(GEN)/compiled-code-index/compiled_code_index.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@
$(BUILD)/startup_indirect_test.o: tests/test_startup_indirect.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@

$(BUILD)/lv2_objects.o: domain/lv2_objects.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/ppu_threads.o: domain/ppu_threads.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/spu_resource_policy.o: domain/spu_resource_policy.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/spurs_kernel1_service.o: domain/spurs_kernel1_service.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/spurs_instance.o: domain/spurs_instance.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/spurs_attributes.o: domain/spurs_attributes.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/lwcond.o: $(SDK)/runtime/sync/lwcond.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/wait_port_horizon.o: adapters/wait_port_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/service_thread_horizon.o: adapters/service_thread_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/discovery_budget.o: diagnostics/discovery_budget.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@
$(BUILD)/discovery_budget_binding.o: adapters/discovery_budget_binding.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@
$(BUILD)/startup_observation.o: diagnostics/startup_observation.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17 -c $< -o $@

$(BUILD)/heap_guest_space.o: adapters/heap_guest_space.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: gpu-only
gpu-only:
	$(MAKE) -f Makefile.gpu
