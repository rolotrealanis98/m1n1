#!/bin/sh
# In-tree copy of input/hid-backend-20261110/tests, pointed at this tree's sources (kbd-hw).
set -eu
cd "$(dirname "$0")"
O=${FB_OUT:-$PWD/out}; mkdir -p "$O"
S=../../src
TP="$S/hidm/tp/mtp_tp.c $S/hidm/tp/ptp.c $S/hidm/tp/apple_mt.c"
CFLAGS='-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
clang $CFLAGS -I$S test_backend.c $S/mtp_kbd.c $S/hidm/service/hidm_service.c $TP -o $O/test_backend
$O/test_backend
cp $S/hv_win_hidm.c $S/hv_win_hidm.h $O/
clang $CFLAGS -I$O -Istubs -I$S test_glue.c $S/mtp_kbd.c $S/hidm/service/hidm_service.c $TP -o $O/test_glue
$O/test_glue
clang $CFLAGS upstream/tests/test_hidm.c upstream/service/hidm_service.c upstream/service/hidm_script.c upstream/driver/hidm_core.c -o $O/test_service
$O/test_service
mkdir -p $O/rt; cp ../../config.h $O/; cp $S/rtkit.c $S/rtkit.h $S/asc.c $S/asc.h $O/rt/
clang $CFLAGS -Wno-multichar -Wno-sign-compare -Wno-unused-value -Istubs -I$O/rt test_rtkit.c $O/rt/rtkit.c $O/rt/asc.c -o $O/test_rtkit
clang $CFLAGS -Wno-multichar -Wno-sign-compare -Wno-unused-value -Istubs -I$O/rt test_rtkit_host.c $O/rt/rtkit.c $O/rt/asc.c -o $O/test_rtkit_host
$O/test_rtkit_host
$O/test_rtkit
clang $CFLAGS -Wno-multichar -Wno-sign-compare -Wno-unused-value -Istubs -I$O/rt test_rtkit_host.c $O/rt/rtkit.c $O/rt/asc.c -o $O/test_rtkit_host
$O/test_rtkit_host
cp $S/mtp_kbd_resident.c $O/
clang $CFLAGS -I$O -Istubs -I$O/rt -I$S test_adapter.c $S/mtp_kbd.c $S/hidm/service/hidm_service.c $TP -o $O/test_adapter
$O/test_adapter
clang $CFLAGS -I$S test_kbd_tp.c $S/mtp_kbd.c $S/hidm/service/hidm_service.c $TP -o $O/test_kbd_tp
$O/test_kbd_tp
cp $S/mtp_dma.c $O/
clang $CFLAGS -Wno-format -I$O -Istubs-tp -Istubs -I$S test_mtp_dma.c -o $O/test_mtp_dma
$O/test_mtp_dma
cp $S/mtp_tp_resident.c $S/mtp_tp_resident.h $O/
clang $CFLAGS -Wno-format -I$O -Istubs-tp -Istubs -I$S test_tp_resident.c $S/mtp_kbd.c $S/hidm/service/hidm_service.c $TP -o $O/test_tp_resident
$O/test_tp_resident
PYTHONDONTWRITEBYTECODE=1 python3 -I test_launcher.py
echo "kbd-hidm: all groups PASS"
