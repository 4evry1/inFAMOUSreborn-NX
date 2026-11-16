#!/usr/bin/env python3
"""Checks that the block-to-block transfers of the adapted lifted object are jumps, not calls.

usage: check_tail_transfers.py <object.o> [objdump-command]
       (x86-64 or AArch64 relocatable object built from generated/bink_lifted_rc.c; for the Switch pass
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump)

A lifted loop is a chain of transfers between lifted functions (SPU_FAST_XFER). Compiled as `call; ret` each hop would
grow the C stack without bound; compiled as a jump it does not. GCC >= 15 / clang enforce this with musttail at compile
time; for any other compiler this script is the check. Counts, per object:
  jumps_to_lifted   branches (unconditional or conditional) from one lifted function into another  -- good
  nested_calls      calls to a lifted function that are NOT followed by a return (the brsl/bisl call bracket) -- legitimate
  missed_tail_calls calls to a lifted function directly followed by a return -- BAD, a transfer that grows the stack
Exit status 1 when a missed tail call exists or no jump to a lifted function was found at all."""
import re, subprocess, sys

PREFIX = 'spu_0001_at_00673E80_rc_spu_func_'
INSN = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')
FUNC = re.compile(r'^[0-9a-f]+ <([^>]+)>:')
RELOC = re.compile(r'^\s+[0-9a-f]+:\s+R_\w+\s+(\S+)')
CALLS = ('call', 'callq', 'bl', 'blr')
RETS = ('ret', 'retq', 'retn')


def target_of(operand, reloc_line):
    if reloc_line:
        m = RELOC.match(reloc_line)
        if m:
            return re.sub(r'[+-]0x[0-9a-f]+$', '', m.group(1))
    m = re.search(r'<([^>+]+)', operand)
    return m.group(1) if m else ''


def main():
    obj = sys.argv[1]
    objdump = sys.argv[2] if len(sys.argv) > 2 else 'objdump'
    text = subprocess.run([objdump, '-d', '-r', '--no-show-raw-insn', obj], capture_output=True, text=True, check=True).stdout
    lines = text.split('\n')
    jumps = nested = missed = 0
    first_missed = []
    current = ''
    for i, line in enumerate(lines):
        fm = FUNC.match(line)
        if fm:
            current = fm.group(1)
            continue
        im = INSN.match(line)
        if not im:
            continue
        mnemonic, operand = im.group(1), im.group(2)
        is_call = mnemonic in CALLS
        is_branch = (mnemonic.startswith('j') or mnemonic == 'b' or mnemonic.startswith('b.') or mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'))
        if not (is_call or is_branch):
            continue
        j = i + 1
        reloc_line = lines[j] if j < len(lines) and RELOC.match(lines[j]) else None
        target = target_of(operand, reloc_line)
        if PREFIX not in target or target == current:
            continue
        if is_branch:
            jumps += 1
            continue
        k = j + (1 if reloc_line else 0)
        nxt = lines[k] if k < len(lines) else ''
        nm = INSN.match(nxt)
        if nm and nm.group(1) in RETS:
            missed += 1
            if len(first_missed) < 5:
                first_missed.append('%s -> %s' % (current, target))
        else:
            nested += 1
    print('TAIL_CHECK jumps_to_lifted=%d nested_calls=%d missed_tail_calls=%d' % (jumps, nested, missed))
    for t in first_missed:
        print('  missed tail call:', t)
    sys.exit(1 if missed or not jumps else 0)


if __name__ == '__main__':
    main()
