#!/usr/bin/env python3
"""The SPURS kernel of the user's own libsre.prx as lifted C for the hybrid runner (Decision 133, 2026-11-16).

usage: gen_kernel_lifted.py --libsre-image <build>/lift/prx/libsre_image.bin --out <dir under build/>

The kernel is the program every SPURS SPU thread starts with: an SPU ELF inside libsre (image offset 0x20380, one RWX segment
LS 0x100..0x880, entry 0x818). It selects a workload (GETLLAR/PUTLLC on the SPURS control block), DMAs the workload's policy module
to LS 0xA00 and calls it through an indirect branch-and-link (function 0x7D8); a policy module leaves through the kernel's exit entry 0x808, which
resets the stack and selects again. Nothing derived from the firmware is written outside --out (the build directory).

Steps (the same tools and passes as every other lifted SPU module of this repository):
  1. read the kernel ELF out of the libsre image and check it: SPU ELF, entry 0x818, one PT_LOAD at LS 0x100 of 0x780 bytes, and the
     local store the lv2 loader produces from it ([0, 0x880)) hashes to the image key the console run reported (STARTUP_SPU_IMAGES
     key 04b499e5fbbd1dc5 for group 0x9E000000 and cc32d3654dc108c5 for group 0x9E000100 are this one hash xor the group term);
  2. wrap the code range [0x230, 0x880) as an SPU ELF, lift it (ps3recomp/tools/spu_lifter.py --auto-functions, the exit entry
     0x808 as an extra function), register-cache it (spu_regcache.py), run spu_ibfast.py and spu_v200.py;
  3. adapt it with native/spu_lifted/jobs/gen_jobmods_lifted.py (adapt / emit, module kind 'kernel': channel operations carry their
     instruction address, stops tied to their stop word) and insert ps3_lifted_pm_resolve(ctx) at the policy-module call, so the glue
     identifies the module the kernel has just DMA'd before the lifted call looks it up.
Outputs: jobmod_spurs_kernel_e818_rc.c, jobmod_spurs_kernel_e818_module.c (rows + descriptor `ps3_spurs_kernel_module`), kernel_report.json.
"""
import argparse, hashlib, json, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SWITCH = os.path.normpath(os.path.join(HERE, '..', '..', '..'))     # <repo>/switch
REPO = os.path.normpath(os.path.join(SWITCH, '..'))                  # <repo>
TOOLS = os.path.join(REPO, 'ps3recomp', 'tools')
JOBS = os.path.join(SWITCH, 'native', 'spu_lifted', 'jobs')
sys.path.insert(0, JOBS)
sys.path.insert(0, TOOLS)
import gen_jobmods_lifted as jobs          # noqa: E402  (adapt, emit, hash64w)
from wrap_spu_elf import wrap               # noqa: E402

NAME = 'jobmod_spurs_kernel_e818'         # the jobmod_ prefix: spu_ibfast.py processes jobmod_* units only
UNIT = 'pm_kernel'                           # the lifter's symbol prefix, replaced by NAME
KERNEL_ELF_OFFSET = 0x20380                  # in libsre_image.bin
SEGMENT_VADDR, SEGMENT_SIZE, ENTRY = 0x100, 0x780, 0x818
LOADED_END = SEGMENT_VADDR + SEGMENT_SIZE    # 0x880: the end of the local store image the loader produces
LIFT_BASE = 0x230                            # code starts here; [0x100, 0x230) is the kernel's variables
IDENTITY_RANGE = (0x280, 0x880)              # constant at run time (console LS dump); its first 16 bytes are the signature
EXIT_PC, SELECT_PC, PM_CALL_FUNCTION = 0x808, 0x290, 0x7D8
IMAGE_ID = 190                               # job modules use 200 + index
EXPECTED_IMAGE_HASH = 0x783B61710DBD1DC5     # hash64w of LS [0, 0x880) after the load (see step 1)
PM_CALL_SITE = 'ctx->pc = _bt; ctx->host_depth++; SPU_IBF_CALL('                    # the bisl bracket after spu_ibfast.py (v178)
PM_CALL_HOOKED = 'ctx->pc = _bt; ps3_lifted_pm_resolve(ctx); ctx->host_depth++; SPU_IBF_CALL('


def read_kernel_segment(image):
    """The kernel's PT_LOAD bytes, after checking the ELF matches the layout this generator was written for."""
    b = KERNEL_ELF_OFFSET
    if image[b:b + 4] != b'\x7fELF' or struct.unpack('>H', image[b + 0x12:b + 0x14])[0] != 23:
        sys.exit('KERNEL_NOT_FOUND: no SPU ELF at libsre image offset 0x%X' % b)
    entry, phoff = struct.unpack('>II', image[b + 24:b + 32])
    phentsize, phnum = struct.unpack('>HH', image[b + 42:b + 46])
    loads = []
    for k in range(phnum):
        o = b + phoff + k * phentsize
        ptype, off, vaddr, _pa, filesz, memsz, flags, _al = struct.unpack('>IIIIIIII', image[o:o + 32])
        if ptype == 1:
            loads.append((off, vaddr, filesz, memsz))
    if entry != ENTRY or len(loads) != 1 or loads[0][1:] != (SEGMENT_VADDR, SEGMENT_SIZE, SEGMENT_SIZE):
        sys.exit('KERNEL_LAYOUT_CHANGED: entry 0x%X, loads %s' % (entry, [tuple(hex(x) for x in l) for l in loads]))
    off = loads[0][0]
    return image[b + off:b + off + SEGMENT_SIZE]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s failed (rc %d): %s' % (os.path.basename(cmd[2] if cmd[1] == '-I' else cmd[1]), r.returncode, (r.stderr or r.stdout)[-600:]))
    return r.stdout


def lift_unit(code, base, entry, extra_funcs, unit, name, tmp):
    """spu_lifter + spu_regcache + spu_ibfast + spu_v200 over `code` loaded at `base`: the rc text with `name` (a jobmod_* name) as its symbol prefix, and the
    sha256 of the lifter's register-cached output."""
    elf = os.path.join(tmp, unit + '.elf')
    open(elf, 'wb').write(wrap(code, base=base, entry=entry))
    unit_dir = os.path.join(tmp, 'spu', unit)
    os.makedirs(unit_dir)
    lifter = [sys.executable, '-I', os.path.join(TOOLS, 'spu_lifter.py'), '--auto-functions', elf, '--base', '0x%X' % base,
              '--symbol-prefix', unit + '_', '--output', unit_dir]
    if extra_funcs:
        lifter += ['--extra-funcs', ','.join('0x%X' % f for f in extra_funcs)]
    run(lifter)
    run([sys.executable, '-I', os.path.join(TOOLS, 'spu_regcache.py'), os.path.join(tmp, 'spu')])
    lifter_rc = open(os.path.join(unit_dir, 'spu_recomp_rc.c'), 'rb').read()
    text = lifter_rc.decode('utf-8', errors='surrogateescape').replace(unit, name)
    named = os.path.join(tmp, 'passes', name)
    os.makedirs(named)
    path = os.path.join(named, 'spu_recomp_rc.c')
    open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(text)
    run([sys.executable, '-I', os.path.join(TOOLS, 'spu_ibfast.py'), os.path.join(tmp, 'passes')])
    run([sys.executable, '-I', os.path.join(TOOLS, 'spu_v200.py'), path])
    return open(path, encoding='utf-8', errors='surrogateescape').read(), hashlib.sha256(lifter_rc).hexdigest()


def hook_pm_call(c_text):
    """ps3_lifted_pm_resolve before the kernel's call of the policy module (function 0x7D8, the indirect call at 0x800)."""
    head = 'void %s_rc_spu_func_%08X(spu_context* ctx) {\n' % (NAME, PM_CALL_FUNCTION)
    start = c_text.index(head)
    end = c_text.index('\n}\n', start)
    body = c_text[start:end]
    if body.count(PM_CALL_SITE) != 1 or 'ps3_lifted_escape(ctx)' in body:
        sys.exit('PM_CALL_SITE: the policy-module call of 0x%X is not lifted exactly once' % PM_CALL_FUNCTION)
    return c_text[:start] + body.replace(PM_CALL_SITE, PM_CALL_HOOKED) + c_text[end:]


def localise_includes(path, pairs):
    text = open(path).read()
    for old, new in pairs:
        if old not in text:
            sys.exit('include %s not found in %s' % (old, path))
        text = text.replace(old, new)
    open(path, 'w').write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--libsre-image', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    image = open(a.libsre_image, 'rb').read()
    segment = read_kernel_segment(image)
    loaded = bytearray(LOADED_END)
    loaded[SEGMENT_VADDR:LOADED_END] = segment
    image_hash = jobs.hash64w(bytes(loaded))
    if image_hash != EXPECTED_IMAGE_HASH:
        sys.exit('KERNEL_BYTES_DIFFER: loaded image hash %016X, the console ran %016X' % (image_hash, EXPECTED_IMAGE_HASH))
    os.makedirs(a.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        text, lifter_sha = lift_unit(segment[LIFT_BASE - SEGMENT_VADDR:], LIFT_BASE, ENTRY, [EXIT_PC], UNIT, NAME, tmp)
    policy = {'ranges': [IDENTITY_RANGE], 'stub_ranges': [], 'job_hook': None, 'module_kind': 'kernel'}
    binary = bytes(segment)
    info = jobs.adapt(NAME, text, binary, ENTRY, [], {}, base=SEGMENT_VADDR, t_fixed=IDENTITY_RANGE[1], hooks={}, policy=policy)
    reachable_stubs = [s for s in info['reachable_stubs']]
    if info['stubs'] or reachable_stubs:
        sys.exit('KERNEL_NOT_FULLY_LIFTED: %d stubs %s %s' % (info['stubs'], reachable_stubs, info['stop_notes']))
    info['c_text'] = hook_pm_call(info['c_text'])
    info['sha256_input'] = lifter_sha
    info['image_id'] = IMAGE_ID
    info['entry_pc'] = ENTRY
    info['size'] = len(binary)
    jobs.emit(a.out, NAME, info, binary, SEGMENT_VADDR, policy=policy)
    rc_c, module_c = os.path.join(a.out, NAME + '_rc.c'), os.path.join(a.out, NAME + '_module.c')
    localise_includes(rc_c, [('#include "../jobmod_lifted_prelude.h"', '#include "jobmod_lifted_prelude.h"')])
    localise_includes(module_c, [('#include "../../spu_lifted_hybrid.h"', '#include "spu_lifted_hybrid.h"'),
                                 ('const ps3_spu_lifted_module %s_module = {' % NAME, 'const ps3_spu_lifted_module ps3_spurs_kernel_module = {'),
                                 ('PS3_LIFTED_KIND_KERNEL };', 'PS3_LIFTED_KIND_KERNEL, 0x%Xu };' % EXIT_PC)])
    identity = jobs.hash_ranges(binary, SEGMENT_VADDR, [IDENTITY_RANGE])
    report = {'libsre_image_sha256': hashlib.sha256(image).hexdigest(), 'kernel_elf_offset': '0x%X' % KERNEL_ELF_OFFSET,
              'segment_sha256': hashlib.sha256(binary).hexdigest(), 'loaded_image_hash64w': '%016X' % image_hash,
              'identity_range': ['0x%X' % x for x in IDENTITY_RANGE], 'identity_hash': '%016X' % identity,
              'functions': info['functions'], 'lifted': info['lifted'], 'stubs': info['stubs'], 'halts': info['halts'],
              'lifter_rc_sha256': lifter_sha, 'rows': ['0x%X' % r[0] for r in info['rows']]}
    json.dump(report, open(os.path.join(a.out, 'kernel_report.json'), 'w'), indent=1)
    print('%s functions=%d lifted=%d stubs=%d image_hash=%016X identity=%016X out=%s' % (
        NAME, info['functions'], info['lifted'], info['stubs'], image_hash, identity, a.out))


if __name__ == '__main__':
    main()
