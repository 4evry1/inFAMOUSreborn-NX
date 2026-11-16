#!/usr/bin/env bash
# Step 5: generate the lifted SPU modules the NRO runs natively. Everything is derived from YOUR game and firmware and is
# written to git-ignored folders inside switch/native/spu_lifted/.
#
#   switch/scripts/50-generate-spu.sh        (after 20-generate-sources.sh and 40-lift-firmware.sh)
#
#   bink      the Bink movie decoder (EBOOT SPU program at 0x673E80)          -> native/spu_lifted/bink/generated/
#   firmware  the SPURS task-set and system-service policy modules of libsre -> native/spu_lifted/jobs/extra/lift_input/
#   jobs      the 21 WWS job modules, the Edge zlib task and the policy modules -> native/spu_lifted/jobs/generated/
# (the SPURS kernel is generated during the NRO build, from the libsre image of step 4)
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

need_file "$EBOOT_ELF" "EBOOT.elf (run 10-extract-disc.sh)"
need_dir "$REPO_ROOT/infamous-recomp/spu" "regenerated SPU sources (run 20-generate-sources.sh)"
need_file "$LIBSRE_DIR/prx/libsre_image.bin" "lifted libsre image (run 40-lift-firmware.sh)"
SPU="$SWITCH_DIR/native/spu_lifted"

say "Bink decoder"
rm -rf "$SPU/bink/generated"
"$PYTHON" "$SPU/bink/gen_bink_lifted.py" --rc "$REPO_ROOT/infamous-recomp/spu/spu_0001_at_00673E80/spu_recomp_rc.c" \
    --elf "$EBOOT_ELF" --elf-offset 0x673E80 --out "$SPU/bink/generated"

say "libsre policy modules"
rm -rf "$SPU/jobs/extra/lift_input"
"$PYTHON" "$SWITCH_DIR/tools/gen_firmware_spu_inputs.py" --libsre-image "$LIBSRE_DIR/prx/libsre_image.bin"

say "WWS job modules"
rm -rf "$SPU/jobs/generated"
"$PYTHON" "$SPU/jobs/gen_jobmods_lifted.py" --spu "$REPO_ROOT/infamous-recomp/spu" --elf "$EBOOT_ELF" --out "$SPU/jobs/generated"
say "SPU modules ready"
