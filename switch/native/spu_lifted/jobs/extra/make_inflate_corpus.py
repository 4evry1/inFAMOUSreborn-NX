#!/usr/bin/env python3
"""Real deflate streams for the native-inflate differential test (round 2, 2026-11-13).

usage: make_inflate_corpus.py <out file> <max streams> <game file> [<game file> ...]

Scans the (read-only) game files for zlib streams (header 78 01 / 78 5E / 78 9C / 78 DA) that inflate cleanly to the end of the stream with the
host's zlib, and writes the RAW deflate payload (header and adler32 trailer removed: the Edge task inflates with windowBits -15) with the uncompressed
bytes' length and adler32. Record format (little endian): u32 payload_len, u32 out_len, u32 adler32, payload. Streams larger than 60000 bytes
payload or 262144 bytes output are skipped (they must fit the local store next to each other in the test). Spread over the files by taking at most
`max/len(files)` streams per file, at evenly spaced positions of the file and at all four header kinds.
Reads only; never writes next to the game files."""
import mmap, os, re, struct, sys, zlib

HEADERS = (b'\x78\x9c', b'\x78\xda', b'\x78\x5e', b'\x78\x01')


def candidates(m, limit):
    """offsets of header candidates in mm, in file order, from all four header kinds"""
    out = []
    for h in HEADERS:
        pos = 0
        while True:
            pos = m.find(h, pos)
            if pos < 0 or pos >= limit:
                break
            out.append(pos)
            pos += 1
    out.sort()
    return out


def try_stream(m, pos):
    window = m[pos:pos + 70000]
    d = zlib.decompressobj()
    try:
        out = d.decompress(window, 262144)
    except zlib.error:
        return None
    if not d.eof or d.unconsumed_tail:
        return None
    used = len(window) - len(d.unused_data)
    if used < 8 or used > 60000:
        return None
    payload = window[2:used - 4]
    # the raw payload must inflate raw too, to the same bytes
    r = zlib.decompressobj(-15)
    try:
        raw_out = r.decompress(payload, 262144)
    except zlib.error:
        return None
    if not r.eof or raw_out != out:
        return None
    return payload, out, zlib.adler32(out)


def main():
    out_path, maximum = sys.argv[1], int(sys.argv[2])
    files = sys.argv[3:]
    per_file = max(1, maximum // len(files))
    records = []
    for path in files:
        with open(path, 'rb') as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            cand = candidates(m, len(m))
            step = max(1, len(cand) // (per_file * 6))
            taken = 0
            for pos in cand[::step]:
                if taken >= per_file:
                    break
                r = try_stream(m, pos)
                if r:
                    records.append(r)
                    taken += 1
            print('%s: %d candidates, %d streams taken' % (os.path.basename(path), len(cand), taken), flush=True)
            m.close()
    with open(out_path, 'wb') as f:
        for payload, out, adler in records:
            f.write(struct.pack('<III', len(payload), len(out), adler) + payload)
    print('wrote %d streams, %d payload bytes, %d output bytes' % (len(records), sum(len(r[0]) for r in records), sum(len(r[1]) for r in records)))


if __name__ == '__main__':
    main()
