#!/usr/bin/env python3
"""ps3_profile_report.py -- turn the console profiler's dumps into function names.

usage: ps3_profile_report.py --elf build/<tag>-nro-*/infamous-native-probe.elf [--top 40] profile-60.txt [profile-120.txt ...]

The dumps are <app dir>/profile-<seconds>.txt (native/adapters/ps3_profile_core.h describes the format). Several dumps are added up. The ELF is the one the NRO was
made from (the build keeps it next to the .nro): its symbols name the host addresses. The load base of the NRO is derived from the REF lines of the dump (the run-time
address of a few known functions) against their addresses in the ELF.

Prints, for the main guest thread:
  1. the top functions by samples (self time: the function the host pc was in), with the group each belongs to;
  2. the time by group (generated game code, libsre lifted code, lwmutex/lwcond HLE, memory access, atomic domain, renderer, SPU, kernel/libc, other);
  3. the "game function" view: every sample is attributed to the innermost generated game function (func_XXXXXXXX = guest address) on its host frame chain, which is
     the guest call stack (the lifted game functions are C functions): the functions the 62 % of generated code is really spent in, including what they call;
  4. who calls the memory-access / lwmutex / atomic helpers (the innermost generated function above them);
  5. the other threads, by entry function, with their top functions;
  6. the SPU worker threads (entry worker_main / spu_*; the dump holds frame chains for every thread since 2026-11-13): per thread and together, the self time by
     function and group, the innermost LIFTED SPU function (jobmod_*, spu_lifted_*, bink_*) on each chain, and who calls the runtime helpers (DMA, channels, the atomic
     domain guard).
Load bases: the REF lines of one dump give the run-time address of a few functions; the image is NOT one block (run rv56s: everything linked before the generated
code sat 0x25700 lower than the REF base of the later objects said). Every base the REFs give is a candidate; return addresses (link registers and the frames of the
stacks) vote for the base that makes the instruction before them a bl/blr, per 64 KB of address space; an address with no vote takes the nearest voted region. Heavy
return addresses that no REF base explains are searched for a shift of up to +-1 MB (older dumps with REFs from one region only).
Python 3 only; no third-party package (a small ELF symbol reader is built in). Names are demangled with c++filt when it is installed."""
import argparse, bisect, collections, re, shutil, struct, subprocess, sys

# the generated functions are C++ symbols: func_00112233(ppu_context*) once demangled, _Z13func_00112233P11ppu_context in the ELF
GAME = re.compile(r'^(?:func_([0-9A-Fa-f]{8})(?:\(ppu_context\*\))?|_Z13func_([0-9A-Fa-f]{8})P11ppu_context)$')
LIBSRE = re.compile(r'^(?:libsre_func_([0-9A-Fa-f]{8})(?:\(ppu_context\*\))?|_Z\d+libsre_func_([0-9A-Fa-f]{8})P11ppu_context)$')
GROUPS = [
    ('generated game code', re.compile(r'^func_[0-9A-Fa-f]{8}|^_Z13func_[0-9A-Fa-f]{8}')),
    ('libsre lifted code', re.compile(r'^libsre_func_|^lle_sre|^libsre')),
    ('lwmutex / lwcond / lwsync HLE', re.compile(r'lwmutex|lwcond|lw_mutex|sys_lwcond|ps3_lwmutex|startup_lw|condition_wait|nid2a6d9d51|nid1573dc3f|nid1bc200f4', re.I)),
    ('memory access (ppu_native_*, vm_*, lean/fast stores)', re.compile(r'ppu_native_|^vm_(read|write)|lean_|fast_store|fast_pointer|read_scalar|write_scalar|ps3_flat_guest|watch_check|PpuMemory|ppu_memory', re.I)),
    ('atomic domain / reservations', re.compile(r'ps3_atomic|atomic_domain|reservation|stwcx|stdcx|ppu_stwcx|ppu_stdcx|load_reserve|store_conditional')),
    ('renderer (RSX, Vulkan, NVK)', re.compile(r'rsx|vulkan|^vk|nvk|nouveau|mesa|deko|gpu|shader|spirv|glsl', re.I)),
    ('SPU runner / lifted SPU code', re.compile(r'spu_|ps3_spu|jobmod_|ps3_lifted', re.I)),
    ('startup HLE / syscalls / dispatch', re.compile(r'startup|dispatch|hle|syscall|lv2|ps3_indirect|DRAIN', re.I)),
    ('kernel / libc / libnx waits', re.compile(r'^svc|^__nx|^__libnx|^_?_?(pthread|mutex|cond)|futex|waitprocess|^memcpy|^memset|^malloc|^free|^printf|^fwrite|^write|^_write|^nanosleep|^usleep|^sleep|libc', re.I)),
]


def read_symbols(path):
    """[(address, size, name)] of the ELF64 function-like symbols, sorted by address."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        sys.exit('%s is not an ELF64 file' % path)
    little = data[5] == 1
    e = '<' if little else '>'
    shoff, = struct.unpack_from(e + 'Q', data, 0x28)
    shentsize, shnum = struct.unpack_from(e + 'HH', data, 0x3A)
    sections = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(e + 'IIQQQQIIQQ', data, o)
        sections.append((typ, off, size, link, entsize))
    symbols = []
    for typ, off, size, link, entsize in sections:
        if typ not in (2, 11):                       # SHT_SYMTAB, SHT_DYNSYM
            continue
        s_typ, s_off, s_size, _l, _e = sections[link]
        strtab = data[s_off:s_off + s_size]
        for k in range(size // 24):
            name_off, info, other, shndx, value, sz = struct.unpack_from(e + 'IBBHQQ', data, off + k * 24)
            kind = info & 0xF
            if shndx == 0 or kind not in (0, 2) or not value or not name_off:
                continue
            end = strtab.find(b'\0', name_off)
            name = strtab[name_off:end].decode('ascii', 'replace')
            if kind == 0 and not (name.startswith('func_') or sz):
                continue
            symbols.append((value, sz, name))
    text = None
    phoff, = struct.unpack_from(e + 'Q', data, 0x20)
    phentsize, phnum = struct.unpack_from(e + 'HH', data, 0x36)
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, _ms, _al = struct.unpack_from(e + 'IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1 and (p_flags & 1) and text is None:
            body = data[p_offset:p_offset + p_filesz]
            text = (p_vaddr, memoryview(body[:len(body) // 4 * 4]).cast('I') if little and len(body) >= 4 else None)
    symbols.sort()
    # drop duplicates at one address, keep the longest name-wise first
    unique, seen = [], set()
    for a, sz, n in symbols:
        if (a, n) in seen:
            continue
        seen.add((a, n)); unique.append((a, sz, n))
    read_symbols.text = text
    return unique


class Symbolizer:
    BUCKET_BITS = 16

    def __init__(self, elf, refs):
        self.symbols = read_symbols(elf)
        self.text = read_symbols.text
        self.addresses = [s[0] for s in self.symbols]
        by_name = {}
        for a, sz, n in self.symbols:
            by_name.setdefault(n, a)
        bases = {}
        for name, run in refs.items():
            if name in by_name:
                bases[name] = run - by_name[name]
        if not bases:
            print('WARNING: none of the REF functions %s is in the ELF; assuming load base 0' % sorted(refs), file=sys.stderr)
            self.base = 0
        else:
            self.base = bases['ps3_profile_start'] if 'ps3_profile_start' in bases else bases[sorted(bases)[0]]
        self.bases = sorted(set(bases.values()) | {self.base})
        self.ref_bases = dict(bases)
        self.region = {}                       # 64 KB bucket of run-time addresses -> load base
        self.cache = {}
        self.demangled = {}

    def is_call(self, elf_address):
        """True when the instruction at elf_address - 4 is a bl or blr, i.e. elf_address is a return address."""
        if not self.text or self.text[1] is None:
            return False
        vaddr, words = self.text
        index = (elf_address - 4 - vaddr) >> 2
        if index < 0 or index >= len(words) or (elf_address & 3):
            return False
        w = words[index]
        return (w & 0xFC000000) == 0x94000000 or (w & 0xFFFFFC1F) == 0xD63F0000

    def fit(self, returns):
        """returns: Counter run-time return address -> samples. Chooses the load base per 64 KB region by the call-instruction test."""
        if not returns or not self.text or self.text[1] is None:
            return
        total = sum(returns.values())
        def vote():
            votes = collections.defaultdict(collections.Counter); lost = collections.Counter()
            for a, w in returns.items():
                hit = [b for b in self.bases if self.is_call(a - b)]
                for b in hit:
                    votes[a >> self.BUCKET_BITS][b] += w
                if not hit:
                    lost[a] = w
            return votes, lost
        votes, lost = vote()
        for _round in range(4):                # a base no REF names: search a shift of up to +-1 MB that explains most of the heavy unexplained return addresses
            heavy = [(a, w) for a, w in lost.most_common(80) if w >= total * 0.001]
            if len(heavy) < 3:
                break
            weight = sum(w for _a, w in heavy); best = (0, 0)
            for shift in range(-0x100000, 0x100000, 4):
                score = sum(w for a, w in heavy if self.is_call(a - self.base - shift))
                if score > best[0]:
                    best = (score, shift)
            if best[0] < 0.5 * weight or not best[1] or (self.base + best[1]) in self.bases:
                break
            self.bases = sorted(set(self.bases) | {self.base + best[1]})
            print('NOTE: no REF explains %d heavy return addresses; a load base shifted by %+#x does (%.0f %% of their samples): added' %
                  (len(heavy), best[1], 100.0 * best[0] / weight), file=sys.stderr)
            votes, lost = vote()
        self.region = {bucket: counter.most_common(1)[0][0] for bucket, counter in votes.items()}
        if len(self.bases) > 1:
            print('NOTE: %d load bases in use: %s' % (len(self.bases), ', '.join(hex(b) for b in self.bases)), file=sys.stderr)
        self.cache = {}

    def base_for(self, address):
        if not self.region:
            return self.base
        bucket = address >> self.BUCKET_BITS
        if bucket in self.region:
            return self.region[bucket]
        for distance in range(1, 33):
            for candidate in (bucket - distance, bucket + distance):
                if candidate in self.region:
                    return self.region[candidate]
        return self.base

    def name(self, address, call_return=False):
        key = (address, call_return)
        if key in self.cache:
            return self.cache[key]
        pc = address - self.base_for(address) - (4 if call_return else 0)
        i = bisect.bisect_right(self.addresses, pc) - 1
        result = '0x%x' % address
        if i >= 0:
            a, sz, n = self.symbols[i]
            if pc - a < (sz if sz else 0x4000):
                result = self.pretty(n)
        self.cache[key] = result
        return result

    def pretty(self, name):
        if not name.startswith('_Z'):
            return name
        if name not in self.demangled:
            tool = shutil.which('c++filt')
            if tool:
                self.demangled[name] = subprocess.run([tool, name], capture_output=True, text=True).stdout.strip() or name
            else:
                self.demangled[name] = name
        return self.demangled[name]


SPU_ENTRY = re.compile(r'worker_main|spu', re.I)
WAITING = re.compile(r'^svc(Wait|Sleep|Arbitrate)|nanosleep|^__syscall_nanosleep|futex|condvarWait|^mutexLock|wait_impl')
LIFTED_SPU = re.compile(r'jobmod_|bink_|^spu_lifted_|_lifted_rc|_lifted_rows')


def classify(name):
    for group, pattern in GROUPS:
        if pattern.search(name):
            return group
    return 'other'


def parse(paths):
    out = {'hz': 0, 'main': 0, 'other': 0, 'chain': 0, 'dropped': 0, 'interval': 0, 'refs': {}, 'threads': {}, 'pc': collections.Counter(), 'lr': collections.Counter(),
           'stack': collections.Counter(), 'opc': collections.defaultdict(collections.Counter), 'ostack': collections.defaultdict(collections.Counter)}
    for path in paths:
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p:
                    continue
                if p[0] == 'PS3PROFILE':
                    kv = dict(x.split('=') for x in p[3:] if '=' in x)
                    out['hz'] = int(kv.get('hz', 0)); out['main'] += int(kv.get('main_samples', 0)); out['other'] += int(kv.get('other_samples', 0))
                    out['chain'] += int(kv.get('with_chain', 0)); out['dropped'] += int(kv.get('dropped', 0)); out['interval'] += int(kv.get('interval', 0))
                elif p[0] == 'REF':
                    out['refs'][p[1]] = int(p[2], 16)
                elif p[0] == 'THREAD':
                    kv = dict(x.split('=') for x in p[2:] if '=' in x)
                    t = out['threads'].setdefault(int(p[1]), {'main': int(kv['main']), 'entry': int(kv['entry'], 16), 'samples': 0, 'arg': int(kv.get('arg', '0x0'), 16)})
                    t['samples'] += int(kv['samples'])
                elif p[0] == 'PC':
                    out['pc'][int(p[2], 16)] += int(p[1])
                elif p[0] == 'LR':
                    out['lr'][int(p[2], 16)] += int(p[1])
                elif p[0] == 'STACK':
                    out['stack'][tuple(int(x, 16) for x in p[2:])] += int(p[1])
                elif p[0] == 'OPC':
                    out['opc'][int(p[1])][int(p[3], 16)] += int(p[2])
                elif p[0] == 'OSTACK':
                    out['ostack'][int(p[1])][tuple(int(x, 16) for x in p[3:])] += int(p[2])
    return out


def table(title, rows, total, top, extra=None):
    print('\n== %s' % title)
    for count, name in rows[:top]:
        print('%6.2f %%  %8d  %s' % (100.0 * count / total if total else 0, count, name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dumps', nargs='+')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    d = parse(a.dumps)
    sym = Symbolizer(a.elf, d['refs'])
    returns = collections.Counter()
    for r, c in d['lr'].items():
        returns[r] += c
    for key, c in d['stack'].items():
        for r in key[1:]:
            if r:
                returns[r] += c
    for slot_stacks in d['ostack'].values():
        for key, c in slot_stacks.items():
            for r in key[1:]:
                if r:
                    returns[r] += c
    sym.fit(returns)
    total = sum(d['pc'].values())
    print('dumps: %d, interval %d s, main-thread samples %d (about %.1f s of CPU at %d Hz), other-thread samples %d, with frame chain %d, dropped %d, load base 0x%x%s' %
          (len(a.dumps), d['interval'], d['main'], d['main'] / d['hz'] if d['hz'] else 0, d['hz'], d['other'], d['chain'], d['dropped'], sym.base,
           (' (+%d more regions: %s)' % (len(sym.bases) - 1, ', '.join(hex(b) for b in sym.bases if b != sym.base))) if len(sym.bases) > 1 else ''))
    if not total:
        print('no main-thread samples'); return

    # 1. self time by function
    by_function = collections.Counter()
    for pc, count in d['pc'].items():
        by_function[sym.name(pc)] += count
    rows = [(c, '%-34s [%s]' % (sym.pretty(n), classify(n))) for n, c in by_function.most_common()]
    table('main thread: top %d functions by self samples' % a.top, rows, total, a.top)

    # 2. groups
    by_group = collections.Counter()
    for n, c in by_function.items():
        by_group[classify(n)] += c
    table('main thread: by group (self time)', [(c, g) for g, c in by_group.most_common()], total, 30)

    # 3. innermost generated game function on the frame chain
    inclusive = collections.Counter(); no_game = 0
    for key, count in d['stack'].items():
        pc, rets = key[0], key[1:]
        frames = [sym.name(pc)] + [sym.name(r, True) for r in rets if r]
        game = next((f for f in frames if GAME.match(f)), None)
        if game:
            inclusive[game] += count
        else:
            no_game += count
    if d['chain']:
        rows = [(c, '%s  (guest 0x%s)' % (n, (GAME.match(n).group(1) or GAME.match(n).group(2)).upper())) for n, c in inclusive.most_common()]
        table('main thread: innermost generated game function on the host frame chain (guest call stack), top %d' % a.top, rows, total, a.top)
        print('   (samples with no generated game function on the chain, or no chain read: %d = %.1f %%)' % (no_game + total - sum(d['stack'].values()), 100.0 * (no_game + total - sum(d['stack'].values())) / total))
    else:
        print('\n== no frame chains in the dump (the thread was read without a chain, or the build keeps no frame records): section 3 skipped')

    # 4. who calls the helpers
    for label in ('memory access (ppu_native_*, vm_*, lean/fast stores)', 'lwmutex / lwcond / lwsync HLE', 'atomic domain / reservations'):
        callers = collections.Counter(); seen = 0
        for key, count in d['stack'].items():
            pc, rets = key[0], key[1:]
            if classify(sym.name(pc)) != label:
                continue
            seen += count
            frames = [sym.name(r, True) for r in rets if r]
            callers[next((f for f in frames if GAME.match(f) or LIBSRE.match(f)), '(no generated function on the chain)')] += count
        if seen:
            rows = [(c, n) for n, c in callers.most_common()]
            table('callers of: %s (%d samples, innermost generated function above them)' % (label, seen), rows, seen, 15)

    # 5. other threads
    if d['opc']:
        print('\n== other threads (pc only, coarse): busiest first')
        order = sorted(d['opc'], key=lambda s: -sum(d['opc'][s].values()))
        for slot in order[:12]:
            counter = d['opc'][slot]; n = sum(counter.values())
            entry = d['threads'].get(slot, {}).get('entry', 0)
            print('  thread slot %d: %d samples, entry %s' % (slot, n, sym.pretty(sym.name(entry)) if entry else '?'))
            grouped = collections.Counter()
            for pc, c in counter.items():
                grouped[sym.name(pc)] += c
            for name, c in grouped.most_common(4):
                print('      %5.1f %%  %s [%s]' % (100.0 * c / n, sym.pretty(name), classify(name)))
    spu_section(d, sym, a.top)


def spu_section(d, sym, top):
    """6. the SPU worker threads: per thread and together, from the frame chains (OSTACK) or, in older dumps, the pc alone (OPC)."""
    slots = sorted(set(d['ostack']) | set(d['opc']))
    spu = [s for s in slots if d['threads'].get(s, {}).get('entry') and SPU_ENTRY.search(sym.name(d['threads'][s]['entry']))]
    if not spu:
        return
    print('\n== SPU worker threads (entry matches worker_main / spu): %d threads%s' % (len(spu), '' if d['ostack'] else '  [this dump has no frame chains of other threads: pc only]'))
    all_self, running_self, by_group, lifted, helper_callers, wait_in = (collections.Counter() for _ in range(6))
    total = running = 0
    for slot in spu:
        info = d['threads'][slot]
        stacks = d['ostack'].get(slot) or {(pc,): c for pc, c in d['opc'][slot].items()}
        n = sum(stacks.values()); waiting = 0; mine = collections.Counter()
        for key, c in stacks.items():
            pc_name = sym.name(key[0])
            frames = [pc_name] + [sym.name(r, True) for r in key[1:] if r]
            total += c
            all_self[pc_name] += c
            if WAITING.search(pc_name):
                waiting += c
                wait_in[next((f for f in frames[1:] if not WAITING.search(f) and not f.startswith('0x')), '(no caller on the chain)')] += c
                continue
            running += c; mine[pc_name] += c; running_self[pc_name] += c; by_group[classify(pc_name)] += c
            lifted[next((f for f in frames if LIFTED_SPU.search(f)), '(no lifted SPU function on the chain)')] += c
            group = classify(pc_name)
            if group not in ('SPU runner / lifted SPU code',) and not LIFTED_SPU.search(pc_name):
                helper_callers['%s  <-  %s' % (sym.pretty(pc_name), next((f for f in frames[1:] if LIFTED_SPU.search(f) or classify(f) == 'SPU runner / lifted SPU code'), '(no SPU function above it)'))] += c
        print('  slot %3d  entry %-28s arg 0x%-10x %6d samples, %5.1f %% parked/sleeping; running: %s' % (slot, sym.pretty(sym.name(info['entry']))[:28], info.get('arg', 0), n,
              100.0 * waiting / n if n else 0, ', '.join('%s %d' % (sym.pretty(k)[:34], v) for k, v in mine.most_common(3)) or '-'))
    print('  all SPU threads together: %d samples, %.1f %% running, %.1f %% parked/sleeping' % (total, 100.0 * running / total if total else 0, 100.0 * (total - running) / total if total else 0))
    if wait_in:
        table('SPU threads: what the parked/sleeping samples wait in (innermost non-kernel function)', [(c, sym.pretty(n)) for n, c in wait_in.most_common()], total - running, 10)
    if running:
        table('SPU threads, running samples: self time by function', [(c, '%-40s [%s]' % (sym.pretty(n)[:40], classify(n))) for n, c in running_self.most_common()], running, top)
        table('SPU threads, running samples: by group', [(c, g) for g, c in by_group.most_common()], running, 12)
        table('SPU threads, running samples: innermost LIFTED SPU function on the chain', [(c, sym.pretty(n)) for n, c in lifted.most_common()], running, top)
        table('SPU threads, running samples: runtime helpers (DMA, channels, atomic domain, kernel) and the SPU function that called them', [(c, n) for n, c in helper_callers.most_common()], running, top)


if __name__ == '__main__':
    main()
