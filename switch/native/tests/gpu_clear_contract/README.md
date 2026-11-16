# CPU adapter contract regression

Run `python3 native/tests/gpu_clear_contract/run.py` from the project root.
This compiles the **actual production adapter** against the unmodified deko3d
header extracted from the pinned SDK image
`sha256:d222e34188568076e65c73f328039b3f997c2397d5d0d1d5011be752e9538d26`
at `/opt/devkitpro/libnx/include/deko3d.h`. `run.py` verifies the header SHA256.
The separate counter header is a CPU-only clock stub with fixed ticks.

The narrow C ABI spy records the production copy arguments and commands. It
does not draw, emulate GPU command execution, or copy image data. A successful
adapter return copies its untouched 0xa5 sentinel; the test explicitly checks
that sentinel so it cannot manufacture a pixel success claim.

The effective row layout uses the pinned deko v0.5.0 interpretation from
`source/dk_image.cpp` lines 777-783: nonzero `rowLength` is a byte pitch; zero
means width times bytes per block. At 64x32 the original `{addr,width,height}`
call fails with pitch 64 versus 256 required bytes, final end 2240 versus 8192.
Four sizes cover minimum, rectangular, diagnostic, and maximum requests.

Success paths must show queue-init completion, two clears and clear completion,
command reset, then barrier/copy and copy completion. Each wait must retain its
five second timeout. Six forked cases inject a fence timeout or queue error at
each of the three stages. Shared counters and output prove SIGABRT with no
destruction/readback and no later-stage work after the failed stage. Core dumps
are disabled in the test process.

These are CPU boundary-call and lifetime-policy checks. They establish neither
GPU completion/cache correctness nor valid pixel output or Switch hardware
behavior.
