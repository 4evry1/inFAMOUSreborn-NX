#!/usr/bin/env python3
"""Lift two SPURS policy modules that live inside the player's own libsre image.

usage: gen_firmware_spu_inputs.py --libsre-image <build>/libsre-lift/prx/libsre_image.bin [--out <dir>] [--ps3recomp <dir>]

The libsre image is the relocated PS3 firmware module produced by switch/scripts/40-lift-firmware.sh from the
player's own decrypted libsre.prx. The two policy modules are SPU programs embedded in it:

  pm_taskset   the task-set policy module   image offset 0x23680, 0x1E40 bytes, loaded at LS 0xA00, entry 0xA00, code [0xA00, 0x2680)
  pm_syssrv    the system service module    image offset 0x21480, 0x2200 bytes, loaded at LS 0xA00, entry 0xA00, code [0xA00, 0x2B10)

For each module the steps are those of the repository's own SPU tools (ps3recomp/tools):
  1. wrap_spu_elf.wrap(body, base=0xA00, entry=0xA00)  -> an SPU ELF
  2. spu_lifter.py --auto-functions <elf> --base 0xA00 --symbol-prefix <name>_ --code-end <end> --output <work>/<name>
  3. spu_regcache.py <work>                           -> <name>/spu_recomp_rc.c
and the result (body.bin + spu_recomp_rc.c) is written to <out>/<name>/, where native/spu_lifted/jobs/extra/tasks.json
finds it. Everything written here is derived from firmware and must stay out of version control (see .gitignore).
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SWITCH = os.path.normpath(os.path.join(HERE, '..'))
REPO = os.path.normpath(os.path.join(SWITCH, '..'))

MODULES = {
    'pm_taskset': dict(offset=0x23680, size=0x1E40, base=0xA00, entry=0xA00, code_end=0x2680),
    'pm_syssrv': dict(offset=0x21480, size=0x2200, base=0xA00, entry=0xA00, code_end=0x2B10),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--libsre-image', required=True)
    ap.add_argument('--out', default=os.path.join(SWITCH, 'native', 'spu_lifted', 'jobs', 'extra', 'lift_input'))
    ap.add_argument('--ps3recomp', default=os.path.join(REPO, 'ps3recomp'))
    a = ap.parse_args()
    tools = os.path.join(a.ps3recomp, 'tools')
    sys.path.insert(0, tools)
    from wrap_spu_elf import wrap   # noqa: E402

    image = open(a.libsre_image, 'rb').read()
    with tempfile.TemporaryDirectory(prefix='libsre-spu-') as work:
        for name, m in MODULES.items():
            body = image[m['offset']:m['offset'] + m['size']]
            if len(body) != m['size']:
                sys.exit('%s: libsre image too short (%d bytes)' % (name, len(image)))
            elf = os.path.join(work, name + '.elf')
            open(elf, 'wb').write(wrap(body, base=m['base'], entry=m['entry']))
            lift_dir = os.path.join(work, name)
            os.makedirs(lift_dir)
            subprocess.run([sys.executable, os.path.join(tools, 'spu_lifter.py'), '--auto-functions', elf,
                            '--base', hex(m['base']), '--symbol-prefix', name + '_',
                            '--code-end', hex(m['code_end']), '--output', lift_dir], check=True)
            subprocess.run([sys.executable, os.path.join(tools, 'spu_regcache.py'), work], check=True)
            rc = os.path.join(lift_dir, 'spu_recomp_rc.c')
            if not os.path.exists(rc):
                sys.exit('%s: spu_regcache.py did not write %s' % (name, rc))
            dest = os.path.join(a.out, name)
            os.makedirs(dest, exist_ok=True)
            open(os.path.join(dest, 'body.bin'), 'wb').write(body)
            open(os.path.join(dest, 'spu_recomp_rc.c'), 'wb').write(open(rc, 'rb').read())
            print('%s: body %d bytes, lifted -> %s' % (name, len(body), dest))


if __name__ == '__main__':
    main()
