/* SPDX-License-Identifier: MIT */
#ifndef HIDM_SCRIPT_H
#define HIDM_SCRIPT_H
#include "hidm_service.h"
typedef struct hm_script {hm_u32 step;hm_u8 leds,feature;int pointer;} hm_script;
hm_u32 hm_script_init(hm_service *,hm_script *,int pointer);
hm_u32 hm_script_tick(hm_service *,hm_script *,hm_u64);
#endif
