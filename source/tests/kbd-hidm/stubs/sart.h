#ifndef TEST_SART_H
#define TEST_SART_H
#include "types.h"
typedef struct sart_dev sart_dev_t;
bool sart_add_allowed_region(sart_dev_t *,void *,size_t);
bool sart_remove_allowed_region(sart_dev_t *,void *,size_t);
#endif
