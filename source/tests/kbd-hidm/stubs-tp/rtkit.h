#include "types.h"
typedef struct rtkit_dev rtkit_dev_t;
void rtkit_set_boot_timeout(u64 us);
void rtkit_mtp_host_buffers(rtkit_dev_t *rtk, bool (*alloc)(size_t sz, void **cpu, u64 *iova));
