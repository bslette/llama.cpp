#!/bin/bash
# Full build of the board's 6.18.21-nx+ kernel (Image, modules; slot b keeps its DTB) from ~/nouveau-l2/linux
# with the running config (nx-config) plus ext4 quota support (QFMT_V2, so Android's /data,
# which has the ext4 quota feature, mounts read-write). Output: ~/nouveau-l2/kfull,
# modules installed under ~/nouveau-l2/kfull-mods.
set -eo pipefail
CL=$HOME/android/lineage-23.2/kernel/platform/kernel-6.12/prebuilts/clang/host/linux-x86/clang-r536225/bin
BT=$HOME/android/lineage-24.0/kernel/platform/kernel-6.18/prebuilts/build-tools
export PATH=$CL:$BT/linux-x86/bin:$PATH BISON_PKGDATADIR=$BT/common/bison M4=$BT/linux-x86/bin/m4
cd $HOME/nouveau-l2/linux
git log --oneline -1
O=$HOME/nouveau-l2/kfull
rm -rf $O; mkdir -p $O && cp $HOME/nouveau-l2/nx-config $O/.config
MK="make -j$(nproc) O=$O ARCH=arm64 LLVM=1"
scripts/config --file $O/.config -e QUOTA -e QUOTA_TREE -e QFMT_V2
$MK olddefconfig
grep -E "^CONFIG_(QUOTA|QUOTA_TREE|QFMT_V2)=" $O/.config
$MK Image modules 2>&1 | grep -vE "^  (CC|LD|AR|AS|HOSTCC|DTC|CPP|SYNC|GEN|UPD|CALL|CHK|WRAP|SHIPPED|MKCAP|POLICY|COPY|TEST|OBJCOPY|GZIP|LDS|VDSO|SYSHDR|SYSTBL|HDRINST|HOSTLD|KSYMS|MODPOST|SORTTAB|CHKSHA1|NM|PAD|ZOFFSET|UNROLL)" | tail -20
cat $O/include/config/kernel.release
rm -rf $HOME/nouveau-l2/kfull-mods
$MK INSTALL_MOD_PATH=$HOME/nouveau-l2/kfull-mods INSTALL_MOD_STRIP=1 modules_install >/dev/null
ls $HOME/nouveau-l2/kfull-mods/lib/modules
modinfo $(find $HOME/nouveau-l2/kfull-mods -name nouveau.ko) | grep -E "vermagic|uvmm_sysmem_vol"
ls -la $O/arch/arm64/boot/Image
echo KBUILD-DONE
