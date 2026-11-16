/* Diagnostic fail-stop EXTERNAL runtime operations. Never successful stand-ins. */
/* Root (2026-11-11): _Z19infamous_movie_poolj is now a real function in startup_execution.cpp (it reads guest memory). */
/* Root (2026-11-11): _Z24infamous_movie_pool_waitj is now a real function in startup_execution.cpp (it reads guest memory). */
.section .text.unsupported_3,"ax",%progbits
.p2align 2
.global car_v233_dscale
.type car_v233_dscale, %function
car_v233_dscale:
    /* Root (2026-11-11): Windows-port car draw-distance scale (v233A). Neutral factor 1.0: the game's own distance. */
    fmov d0, #1.0
    ret
.size car_v233_dscale, .-car_v233_dscale
.section .text.unsupported_4,"ax",%progbits
.p2align 2
.global car_v233_vis
.type car_v233_vis, %function
car_v233_vis:
    /* Root (2026-11-11): Windows-port car visibility probe (v233A). Diagnostic there: no-op. */
    ret
.size car_v233_vis, .-car_v233_vis
.section .text.unsupported_5,"ax",%progbits
.p2align 2
.global inf_cell_probe
.type inf_cell_probe, %function
inf_cell_probe:
    /* Root (2026-11-11): Windows-port streaming-cell probe (v151). Diagnostic there: no-op. */
    ret
.size inf_cell_probe, .-inf_cell_probe
.section .text.unsupported_6,"ax",%progbits
.p2align 2
.global infamous_anim_lod_note
.type infamous_anim_lod_note, %function
infamous_anim_lod_note:
    /* Root (2026-11-11): Windows-port animation LOD bookkeeping (v216): no-op. */
    ret
.size infamous_anim_lod_note, .-infamous_anim_lod_note
.section .text.unsupported_7,"ax",%progbits
.p2align 2
.global infamous_anim_lod_skip
.type infamous_anim_lod_skip, %function
infamous_anim_lod_skip:
    /* Root (2026-11-11): Windows-port animation LOD (v216): never skip, the game animates everything as on the console. */
    mov w0, #0
    ret
.size infamous_anim_lod_skip, .-infamous_anim_lod_skip
/* Root (2026-11-12, Decision 62): infamous_bink_wait_unlock / _relock are real functions now
 * (startup_execution.cpp, "the Bink reader's mutex handoff"). */
.section .text.unsupported_10,"ax",%progbits
.p2align 2
.global infamous_trainer_move_pre
.type infamous_trainer_move_pre, %function
infamous_trainer_move_pre:
    /* Root (2026-11-11): Windows-port trainer hook (Super Speed): no trainer here. */
    ret
.size infamous_trainer_move_pre, .-infamous_trainer_move_pre
.section .text.unsupported_11,"ax",%progbits
.p2align 2
.global infamous_trainer_move_set
.type infamous_trainer_move_set, %function
infamous_trainer_move_set:
    /* Root (2026-11-11): Windows-port trainer hook (Super Speed): no trainer here. */
    ret
.size infamous_trainer_move_set, .-infamous_trainer_move_set
#ifndef PS3_NATIVE_STARTUP_LWMUTEX
.section .text.unsupported_12,"ax",%progbits
.p2align 2
.global lv2_syscall
.type lv2_syscall, %function
lv2_syscall:
    mov w0, #12
    b native_startup_unsupported
.size lv2_syscall, .-lv2_syscall
#endif
#ifndef PS3_NATIVE_CONTEXT_MEMORY
.section .text.unsupported_13,"ax",%progbits
.p2align 2
.global ppu_atomic_add16
.type ppu_atomic_add16, %function
ppu_atomic_add16:
    mov w0, #13
    b native_startup_unsupported
.size ppu_atomic_add16, .-ppu_atomic_add16
.section .text.unsupported_14,"ax",%progbits
.p2align 2
.global ppu_dcbz128
.type ppu_dcbz128, %function
ppu_dcbz128:
    mov w0, #14
    b native_startup_unsupported
.size ppu_dcbz128, .-ppu_dcbz128
.section .text.unsupported_15,"ax",%progbits
.p2align 2
.global ppu_stdcx64
.type ppu_stdcx64, %function
ppu_stdcx64:
    mov w0, #15
    b native_startup_unsupported
.size ppu_stdcx64, .-ppu_stdcx64
.section .text.unsupported_16,"ax",%progbits
.p2align 2
.global ppu_stvx16
.type ppu_stvx16, %function
ppu_stvx16:
    mov w0, #16
    b native_startup_unsupported
.size ppu_stvx16, .-ppu_stvx16
.section .text.unsupported_17,"ax",%progbits
.p2align 2
.global ppu_stwcx32
.type ppu_stwcx32, %function
ppu_stwcx32:
    mov w0, #17
    b native_startup_unsupported
.size ppu_stwcx32, .-ppu_stwcx32
#endif
.section .text.unsupported_18,"ax",%progbits
.p2align 2
.global ppu_timebase_now
.type ppu_timebase_now, %function
ppu_timebase_now:
    mov w0, #18
    b native_startup_unsupported
.size ppu_timebase_now, .-ppu_timebase_now
#ifndef PS3_NATIVE_INDIRECT_DISPATCH
.section .text.unsupported_19,"ax",%progbits
.p2align 2
.global ps3_indirect_call
.type ps3_indirect_call, %function
ps3_indirect_call:
    mov w0, #19
    b native_startup_unsupported
.size ps3_indirect_call, .-ps3_indirect_call
#endif
.section .text.unsupported_20,"ax",%progbits
.p2align 2
.global ps3_pad_mouse_cam_spring
.type ps3_pad_mouse_cam_spring, %function
ps3_pad_mouse_cam_spring:
    /* Root (2026-11-11): Windows-port mouse camera hook (v152). No mouse: no-op. */
    ret
.size ps3_pad_mouse_cam_spring, .-ps3_pad_mouse_cam_spring
.section .text.unsupported_21,"ax",%progbits
.p2align 2
.global ps3_pad_mouse_inject
.type ps3_pad_mouse_inject, %function
ps3_pad_mouse_inject:
    /* Root (2026-11-11): Windows-port mouse-look hook after the pad read (v151). No mouse on this port: nothing is injected. */
    ret
.size ps3_pad_mouse_inject, .-ps3_pad_mouse_inject
.section .text.unsupported_22,"ax",%progbits
.p2align 2
.global ps3_pad_mouse_raw_turn
.type ps3_pad_mouse_raw_turn, %function
ps3_pad_mouse_raw_turn:
    /* Root (2026-11-11): Windows-port query "is the mouse turning the camera" (v153). Always 0, so the game keeps its original turn acceleration and axis snap. */
    mov w0, #0
    ret
.size ps3_pad_mouse_raw_turn, .-ps3_pad_mouse_raw_turn
.section .text.unsupported_23,"ax",%progbits
.p2align 2
.global ps3_pad_rstick_read
.type ps3_pad_rstick_read, %function
ps3_pad_rstick_read:
    /* Root (2026-11-11): Windows-port note that the camera read the right stick (v236). Diagnostic only on that port: no-op. */
    ret
.size ps3_pad_rstick_read, .-ps3_pad_rstick_read
#ifndef PS3_NATIVE_STARTUP_TASKWAIT
.section .text.unsupported_24,"ax",%progbits
.p2align 2
.global ps3_taskwait_begin
.type ps3_taskwait_begin, %function
ps3_taskwait_begin:
    /* Root (2026-11-11): Windows-port job-poll priority boost marker (v83/v129): no-op. */
    ret
.size ps3_taskwait_begin, .-ps3_taskwait_begin
.section .text.unsupported_25,"ax",%progbits
.p2align 2
.global ps3_taskwait_end
.type ps3_taskwait_end, %function
ps3_taskwait_end:
    /* Root (2026-11-11): Windows-port job-poll priority boost marker (v83/v129): no-op. */
    ret
.size ps3_taskwait_end, .-ps3_taskwait_end
.section .text.unsupported_26,"ax",%progbits
.p2align 2
.global ps3_taskwait_spin
.type ps3_taskwait_spin, %function
ps3_taskwait_spin:
    /* Root (2026-11-11): Windows-port spin hint in the job-poll loops: give the core away once (svcSleepThread(0)), because equal-priority Horizon threads on one core are not time-sliced. */
    mov x0, #0
    svc 0xB
    ret
.size ps3_taskwait_spin, .-ps3_taskwait_spin
#endif
.section .note.GNU-stack,"",%progbits
