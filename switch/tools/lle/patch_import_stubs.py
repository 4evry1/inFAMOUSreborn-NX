"""Root fix (B2a): give every lifted HLE import stub the TOC save its real PS3 stub performs.

A firmware import stub saves the TOC pointer (r2) into the caller's stack slot at r1+0x28, loads the callee's
entry and TOC from its function descriptor and branches through the count register.
ppu_lifter.py --hle-stubs replaces the whole stub with `ps3_hle_call(nid, ctx); return;`, dropping the
TOC save. Every caller then reloads r2 from r1+0x28 after the call (the TOC-restore slot of
the PPC64 ELF ABI), so without the save it reloads stack garbage as its TOC. On Eden-equivalent host
runs this surfaced as an InvalidMemory stop at 0xffff8160 (TOC 0 minus 0x7EA0) right after libsre's
_sys_memset import in cellSpursInitialize.

usage: python3 -I patch_import_stubs.py <lift dir>   (edits ppu_recomp_*.cpp in place; idempotent check)
"""
import glob
import re
import sys

lift = sys.argv[1]
pattern = re.compile(r'^(\s*)ps3_hle_call\((0x[0-9A-Fa-f]+u), ctx\); return;  /\* import stub \*/$', re.M)
marker = 'vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]); /* std r2,0x28(r1) */ '
total = 0
for unit in sorted(glob.glob(lift + '/ppu_recomp_*.cpp')):
    text = open(unit).read()
    if marker in text:
        sys.exit('REFUSED: ' + unit + ' already patched')
    text, count = pattern.subn(lambda m: m.group(1) + marker + 'ps3_hle_call(' + m.group(2) + ', ctx); return;  /* import stub */', text)
    total += count
    open(unit, 'w').write(text)
print('IMPORT_STUBS_PATCHED', total)
if total == 0:
    sys.exit('REFUSED: no import stubs found')
