#!/usr/bin/env bash
# Host speed of the lifted inFAMOUS Bink decoder against the native SPU interpreter (no sanitizers, -O2).
#
# usage: bench_bink_lifted.sh <fresh out dir> [interpreter-seconds=30] [events=20000]
#
# Same build and checks as verify_bink_lifted.sh (it IS verify_bink_lifted.sh), then two speed measurements, both on
# state that is bit-identical on both sides (the differential passes first, and the final states are compared again):
#   BENCH_BINK       <events> synthetic receive-event round trips of the real decoder (stop, bridge, DMA, event): the
#                    per-event path the console takes, dominated by fixed costs because the zero-filled job is empty;
#   FUZZ_REPLAY_*    the heaviest function-level trials found by the fuzz (>= 20000 interpreter instructions each),
#                    replayed from identical state until the interpreter has used <seconds> in total: sustained lifted-vs-interpreter throughput on
#                    the decoder's own code. "lifted_equiv_mips" = interpreter instruction count / lifted wall time.
# The synthetic job is empty because no real Bink frame job is available to the host test (see the report).
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
out="${1:?usage: bench_bink_lifted.sh <fresh out dir> [interpreter-seconds] [events]}"
seconds="${2:-30}"
events="${3:-20000}"
exec bash "$here/verify_bink_lifted.sh" "$out" --fuzz 100000 --fuzz-replay "$seconds" --bench "$events"
