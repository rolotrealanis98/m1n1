#!/bin/bash
# Host-only build of the actual Mu facade/NvmExpressDxe/registration/probe
# sources plus the actual frozen resident S1 controller model. No hardware.
#   build.sh <outdir> <facade-flag: 1|0>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
pkg=$(cd "$here/.." && pwd)
S=${S1_SRC:-/Users/rolo/Work/agents/win-el2/storage/s1-mu-windows-bindfix-20261109/source}
B=$S/third_party/mu/MU_BASECORE
R=$(cd "$here/../../source/src/hv/win" && pwd)
out=$1
flag=$2
mkdir -p "$out"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
MUCF=(-std=gnu11 -fshort-wchar -g -O1 -fno-builtin -Wno-everything $SAN
      -include "$here/autogen.h"
      -I"$B/MdePkg/Include" -I"$B/MdePkg/Include/AArch64" -I"$B/MdeModulePkg/Include"
      -I"$S/platforms/j716s/MacBookPro16M5Pkg/Include"
      -I"$S/third_party/mu/Silicon/Apple/AppleSiliconPkg/Include")
if [ "$flag" = 1 ]; then MUCF+=(-DJ716S_SYNTHETIC_NVME_FACADE_ID=1); fi
MU_SRC=(
  "$B/MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.c"
  "$B/MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceIo.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpress.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressHci.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressPassthru.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressBlockIo.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDiskInfo.c"
  "$B/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressMediaSanitize.c"
  "$B/MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.c"
  "$B/MdeModulePkg/Library/IoMmuLibNull/IoMmuLibNull.c"
  "$S/platforms/j716s/MacBookPro16M5Pkg/Library/J716sHandoffLib/J716sHandoffLib.c"
)
objs=()
for f in "${MU_SRC[@]}"; do
  o=$out/$(basename "$f" .c).o
  clang "${MUCF[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
# Probe driver is #included by the harness to reach its STATIC functions.
clang "${MUCF[@]}" -I"$S/platforms/j716s/MacBookPro16M5Pkg/Drivers/J716sSyntheticNvmeDxe" \
  -DS1_FACADE_FLAG="$flag" -DS1_WIN_TAKEOVER -c "$here/e2e.c" -o "$out/e2e.o"
clang "${MUCF[@]}" -c "$here/fake_uefi.c" -o "$out/fake_uefi.o"
RCF=(-std=gnu11 -DVNVME_HOST_TEST -Wall -Wextra -Werror -O1 -g $SAN -I"$R" -I"$R/s1")
for f in hv_nvme.c hv_pci.c hv_nvme_queue.c ram_disk.c; do
  clang "${RCF[@]}" -c "$R/s1/$f" -o "$out/r_$f.o"
  objs+=("$out/r_$f.o")
done
clang "${RCF[@]}" -c "$here/resident_bridge.c" -o "$out/resident_bridge.o"
clang "${RCF[@]}" -c "$here/win_takeover.c" -o "$out/win_takeover.o"
clang $SAN "${objs[@]}" "$out/e2e.o" "$out/fake_uefi.o" "$out/resident_bridge.o" "$out/win_takeover.o" -o "$out/e2e"
echo "built $out/e2e (facade flag $flag)"
