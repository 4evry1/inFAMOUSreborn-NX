#include "bink_lifted.h"
#include "generated/bink_lifted_meta.h"
extern const ps3_spu_lifted_row ps3_bink_lifted_rows[];
extern const unsigned ps3_bink_lifted_row_count;
/* image_id 3: the id the Windows port registers this image under (spu_workloads.c, spu_begin_image(3)); the native
 * executor only needs it nonzero-consistent between core.image_id and the bound image. */
const ps3_spu_lifted_module ps3_bink_lifted_module = {
    "inFAMOUS-Bink-spu_0001_at_00673E80", 3, PS3_BINK_LIFTED_ENTRY_PC,
    PS3_BINK_LIFTED_CODE_BEGIN, PS3_BINK_LIFTED_CODE_END, PS3_BINK_LIFTED_CODE_HASH, PS3_BINK_LIFTED_CODE_HASH_FAST,
    ps3_bink_lifted_rows, PS3_BINK_LIFTED_FUNCTIONS
};
