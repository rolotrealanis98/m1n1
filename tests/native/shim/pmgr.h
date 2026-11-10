/* Host shim: PMGR reset is recorded by the fake; on post-M4 ANS it is forbidden by policy. */
#ifndef PMGR_H
#define PMGR_H
#include "types.h"
#include "utils.h"
#define PMGR_DIE_ID GENMASK(31, 28)
int pmgr_reset(int die, const char *name);
#endif
