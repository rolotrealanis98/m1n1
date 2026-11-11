/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
void wdt_reboot(void);
void wdt_kick(void);
/* nightly-7b: reprogram the alarm of a watchdog left armed (m1n1.wdt=keep); returns the old alarm, 0 if not armed */
u32 wdt_set_alarm(u32 ticks);

#endif
