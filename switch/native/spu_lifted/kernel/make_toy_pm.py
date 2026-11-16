#!/usr/bin/env python3
"""A small policy module of our own for the host test of the lifted SPURS kernel (test_kernel_lifted.c), lifted with the same tools as the real ones.

usage: make_toy_pm.py --out <dir>        writes toy_pm.bin (the module bytes, LS 0xA00), jobmod_toypm_eA00_rc.c, jobmod_toypm_eA00_module.c

The kernel DMAs it to LS 0xA00 like any workload's policy module and calls it through an indirect branch-and-link. Each call: first, with LS 0x3F050 (word 0) nonzero,
  it writes the 80 bytes at LS 0x3F100 over 0x3FFB0 and jumps to the register restore sequence at 0x3FEC0 (pm_wwsjob's, which the test puts there) with
  r79 = its next instruction (Decision 134); then it
  counts itself in LS 0x3F000 (word 0), optionally (LS 0x3F020 word 0 nonzero) calls the kernel's select entry 0x290 with r3 = 1 like
  cellSpursModulePollStatus and stores the answer at LS 0x3F010, then either leaves through the kernel's exit entry 0x808 (`bi`) or, once the
  count passes the limit at LS 0x3F030 (word 0), ends the test with `stop 0x3FFF`. With LS 0x3F040 (word 0) nonzero it first clears the kernel's record of the
  loaded module (LS 0x1D0), so the kernel DMAs the module again over the armed copy at its next dispatch. No firmware byte is involved.
"""
import argparse, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_kernel_lifted as kernel          # noqa: E402  (lift_unit, the tools, the jobs generator)

BASE = 0xA00
NAME = 'jobmod_toypm_eA00'
COUNTER, SELECTED, CALL_SELECT_FLAG, LIMIT, RELOAD_FLAG, RESTORE_FLAG, STUB_TAIL = 0x3F000, 0x3F010, 0x3F020, 0x3F030, 0x3F040, 0x3F050, 0x3F100
TAIL_LS, TAIL_QUADS = 0x3FFB0, 5                # [0x3FFB0, 0x40000): written by the kernel (stack frame, workload info) after the module wrote the sequence
KERNEL_LOADED_INFO = 0x1D0                    # the kernel's copy of the loaded module's workload info (address, argument, unique id)
STOP_DONE = 0x3FFF


def ri16(op9, imm, rt): return (op9 << 23) | ((imm & 0xFFFF) << 7) | rt
def ri10(op8, imm, ra, rt): return (op8 << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
def rr(op11, rb, ra, rt): return (op11 << 21) | (rb << 14) | (ra << 7) | rt
def ila(rt, imm): return (0x21 << 25) | ((imm & 0x3FFFF) << 7) | rt
def il(rt, imm): return ri16(0x081, imm, rt)
def lqa(rt, addr): return ri16(0x061, addr >> 2, rt)
def stqa(rt, addr): return ri16(0x041, addr >> 2, rt)
def ai(rt, ra, imm): return ri10(0x1C, imm, ra, rt)
def clgt(rt, ra, rb): return rr(0x2C0, rb, ra, rt)
def brz(rt, here, target): return ri16(0x040, (target - here) >> 2, rt)
def brnz(rt, here, target): return ri16(0x042, (target - here) >> 2, rt)
def bi(ra): return rr(0x1A8, 0, ra, 0)
def bisl(rt, ra): return rr(0x1A9, 0, ra, rt)
def stop(code): return code & 0x3FFF


def program():
    B = 0xA3C                              # the body starts after the optional restore-sequence call
    restore = [lqa(13, RESTORE_FLAG), brz(13, 0xA04, B)]                       # A00, A04: no call of the run-time register restore sequence?
    for k in range(TAIL_QUADS):            # A08..A2C: write back the sequence's last 80 bytes: the kernel's stack frame and workload-info DMA land on them
        restore += [lqa(14, STUB_TAIL + 16 * k), stqa(14, TAIL_LS + 16 * k)]
    restore += [ila(79, B), ila(2, 0x3FEC0), bi(2)]                             # A30..A38: r79 = the body; r0..r75 = the save area at 0x3F9C0
    assert 4 * len(restore) == B - BASE
    body = [
        il(3, 1),                          # +00  r3 = 1: the argument of a select call (poll)
        lqa(10, COUNTER),                  # +04
        ai(10, 10, 1),                     # +08
        stqa(10, COUNTER),                 # +0C  ++count
        lqa(11, CALL_SELECT_FLAG),         # +10
        brz(11, B + 0x14, B + 0x24),       # +14  no select call?
        il(5, 0x290),                      # +18
        bisl(0, 5),                        # +1C  r3 = kernel select (a lifted kernel call made by the policy module)
        stqa(3, SELECTED),                 # +20
        lqa(10, COUNTER),                  # +24  (again: the select call does not preserve r10)
        lqa(12, LIMIT),                    # +28
        clgt(6, 10, 12),                   # +2C  count > limit (word 0)?
        brnz(6, B + 0x30, B + 0x4C),       # +30
        lqa(13, RELOAD_FLAG),              # +34
        brz(13, B + 0x38, B + 0x44),       # +38  keep the kernel's record of the loaded module?
        il(14, 0),                         # +3C
        stqa(14, KERNEL_LOADED_INFO),      # +40  forget it: the kernel DMAs the module again at its next dispatch (over the armed lifted copy)
        ila(2, 0x808),                     # +44
        bi(2),                             # +48  leave through the kernel's exit entry
        stop(STOP_DONE),                   # +4C
    ]
    words = restore + body
    while len(words) % 4: words.append(stop(0))     # padding to a multiple of 16 bytes
    return b''.join(w.to_bytes(4, 'big') for w in words)


def check_disassembly(code):
    sys.path.insert(0, kernel.TOOLS)
    from spu_disasm import disassemble_spu
    want = ['lqa', 'brz'] + ['lqa', 'stqa'] * TAIL_QUADS + ['ila', 'ila', 'bi', 'il', 'lqa', 'ai', 'stqa', 'lqa', 'brz', 'il', 'bisl', 'stqa', 'lqa', 'lqa', 'clgt', 'brnz', 'lqa', 'brz', 'il', 'stqa', 'ila', 'bi', 'stop']
    want += ['stop'] * (len(code) // 4 - len(want))
    got = [i.mnemonic for i in disassemble_spu(code, BASE)]
    if got != want:
        sys.exit('TOY_PM_ENCODING: %s' % got)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    code = program()
    check_disassembly(code)
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'toy_pm.bin'), 'wb').write(code)
    with tempfile.TemporaryDirectory() as tmp:
        text, sha = kernel.lift_unit(code, BASE, BASE, [], 'toypm', NAME, tmp)
    jobs = kernel.jobs
    policy = {'ranges': [(BASE, BASE + len(code))], 'stub_ranges': [], 'job_hook': None, 'module_kind': 'policy'}
    info = jobs.adapt(NAME, text, code, BASE, [], {}, base=BASE, t_fixed=BASE + len(code), hooks={}, policy=policy)
    if info['stubs']:
        sys.exit('TOY_PM_NOT_LIFTED: %d stubs %s %s' % (info['stubs'], info['reachable_stubs'], info['stop_notes']))
    info.update(sha256_input=sha, image_id=189, entry_pc=BASE, size=len(code))
    jobs.emit(a.out, NAME, info, code, BASE, policy=policy)
    kernel.localise_includes(os.path.join(a.out, NAME + '_rc.c'), [('#include "../jobmod_lifted_prelude.h"', '#include "jobmod_lifted_prelude.h"')])
    kernel.localise_includes(os.path.join(a.out, NAME + '_module.c'), [('#include "../../spu_lifted_hybrid.h"', '#include "spu_lifted_hybrid.h"')])
    print('%s functions=%d lifted=%d out=%s' % (NAME, info['functions'], info['lifted'], a.out))


if __name__ == '__main__':
    main()
