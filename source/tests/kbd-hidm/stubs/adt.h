#include "types.h"
extern void *adt;
int adt_path_offset_trace(const void *,const char *,int *);
int adt_get_reg(const void *,int *,const char *,int,u64 *,u64 *);
int adt_first_child_offset(const void *,int);
#define ADT_GETPROP(a,n,p,v) ((void)(a),(void)(n),(void)(p),*(v)=0,0)
const void *adt_getprop(const void *,int,const char *,u32 *);
bool adt_is_compatible(const void *,int,const char *);
