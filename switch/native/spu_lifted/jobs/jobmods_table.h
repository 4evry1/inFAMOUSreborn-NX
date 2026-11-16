#ifndef PS3_JOBMODS_TABLE_H
#define PS3_JOBMODS_TABLE_H
/* The lifted job modules compiled into this build (Decision 104): generated/jobmods_table.c, a module is in the table when its
 * PS3_JOBMOD_<name> is defined (evidence/lle-sre-b1b/lle.mk LLE_SPU_JOBMODS). */
#include "../spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const ps3_spu_lifted_module *const ps3_jobmods_table[];
extern const unsigned ps3_jobmods_count;
extern const char *const ps3_jobmods_all_names[];   /* every module the generator knows (0 terminated), compiled in or not */
#ifdef __cplusplus
}
#endif
#endif
