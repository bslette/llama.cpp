#!/bin/bash
# Build nouveau.ko for the board's 6.18.21-nx+ kernel (config from /proc/config.gz)
set -eo pipefail
CL=$HOME/android/lineage-23.2/kernel/platform/kernel-6.12/prebuilts/clang/host/linux-x86/clang-r536225/bin
BT=$HOME/android/lineage-24.0/kernel/platform/kernel-6.18/prebuilts/build-tools
export PATH=$CL:$BT/linux-x86/bin:$PATH BISON_PKGDATADIR=$BT/common/bison M4=$BT/linux-x86/bin/m4
cd $HOME/nouveau-l2/linux
O=$HOME/nouveau-l2/kout
mkdir -p $O && cp $HOME/nouveau-l2/nx-config $O/.config
MK="make -j$(nproc) O=$O ARCH=arm64 LLVM=1"
$MK olddefconfig
$MK modules_prepare
$MK M=drivers/gpu/drm/nouveau KBUILD_MODPOST_WARN=1 modules 2>&1 | grep -vE "^  (CC|LD|AR)" | tail -15
$O/scripts/mod/modpost -h >/dev/null 2>&1 || true
modinfo $HOME/nouveau-l2/linux/drivers/gpu/drm/nouveau/nouveau.ko | grep -E "vermagic|uvmm_sysmem_vol"
echo BUILD-DONE
