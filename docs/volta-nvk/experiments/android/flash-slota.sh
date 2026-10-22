#!/bin/bash
# Reprogram the Jetson's slot a (LineageOS) from Linux running on slot b.
# Slot b and the bootloader partitions (QSPI, SMD) are never touched.
#
# usage: flash-slota.sh [-n] [--no-backup | --backup-only] [--reboot] build
#        flash-slota.sh [--reboot] restore <board backup dir>
#   build      write the newest Lineage build: boot_a, vendor_boot_a, recovery_a,
#              vbmeta_a, vbmeta_system_a, and a new super partition table holding
#              product_a, system_a, system_ext_a, vendor_a, odm_a, system_dlkm_a
#              (sized to the new images; the _b logical partitions become empty)
#   restore    write a backup made by an earlier build run back byte for byte
#   -n         stage only: make the images on BUILD_HOST, don't copy or write
#   --no-backup  skip saving slot a on the board first (default: save it to
#              /var/tmp/slota-backup-<time> on the board's NVMe)
#   --backup-only  stage and save slot a on the board, write nothing
#   --reboot   after writing, reboot to cboot fastboot, set slot a active and boot it
#
# Slot a only boots once cboot picks it: from Linux, `systemctl reboot
# --reboot-argument=bootloader` enters cboot fastboot, then `fastboot set_active a;
# fastboot reboot` (what --reboot does). From Android: service call
# android.hardware.boot.IBootControl/default 9 i32 0.
#
# env: BUILD_HOST  host with the Lineage tree (default stealth)
#      TREE        Lineage tree on BUILD_HOST (default ~/android/lineage-24.0)
#      BOARD       ssh host running Linux on the Jetson (default nx-linux)
#      ROOT_PW     root password on BOARD for su (default: password)
set -euo pipefail

BUILD_HOST=${BUILD_HOST:-stealth}
TREE=${TREE:-/home/slettela/android/lineage-24.0}
BOARD=${BOARD:-nx-linux}
ROOT_PW=${ROOT_PW:-password}
POUT=$TREE/out/target/product/galen_tab
LP=$TREE/out/host/linux-x86/bin
HSTAGE=/home/slettela/android/slota-stage
SUPER_DEV_SIZE=10737418240
GROUP_SIZE=5268184832
DYN="product system system_ext vendor odm system_dlkm"
PHYS="boot vendor_boot recovery vbmeta vbmeta_system"

STAGE_ONLY=0 BACKUP=1 BACKUP_ONLY=0 REBOOT=0 MODE= RDIR=
while [ $# -gt 0 ]; do
	case "$1" in
	-n) STAGE_ONLY=1;;
	--no-backup) BACKUP=0;;
	--backup-only) BACKUP_ONLY=1;;
	--reboot) REBOOT=1;;
	build) MODE=build;;
	restore) MODE=restore; RDIR=${2:?restore needs the board backup dir}; shift;;
	*) sed -n '2,26s/^# \{0,1\}//p' "$0"; exit 1;;
	esac
	shift
done
[ -n "$MODE" ] || { sed -n '2,26s/^# \{0,1\}//p' "$0"; exit 1; }

# Run a command as root on the board: su over a pty (root has no ssh login, the user
# has no passwordless sudo).
board_root() {
	ssh "$BOARD" "ROOT_PW=$(printf %q "$ROOT_PW") CMD=$(printf %q "$1") python3 -c '
import pty, os, sys
pid, fd = pty.fork()
if pid == 0:
    os.execvp(\"su\", [\"su\", \"-\", \"root\", \"-c\", os.environ[\"CMD\"]])
out = b\"\"
while b\"assword\" not in out: out += os.read(fd, 1024)
os.write(fd, os.environ[\"ROOT_PW\"].encode() + b\"\n\")
os.read(fd, 16)
while True:
    try: d = os.read(fd, 65536)
    except OSError: break
    if not d: break
    sys.stdout.write(d.decode(errors=\"replace\")); sys.stdout.flush()
_, st = os.waitpid(pid, 0); sys.exit(os.waitstatus_to_exitcode(st))
'" | tr -d '\r'
}

# Board side: write each image (<part>.img -> by-partlabel/<part>), read it back, compare.
WRITE_SH='set -e
cd "$(dirname "$0")"
while read -r sum img; do
	p=${img%.img}; dev=$(readlink -f /dev/disk/by-partlabel/$p)
	size=$(stat -c %s $img); psize=$(blockdev --getsize64 $dev)
	[ $size -le $psize ] || { echo "$img ($size) > $p ($psize)"; exit 1; }
	grep -q "^$dev " /proc/mounts && { echo "$dev is mounted"; exit 1; }
	dd if=$img of=$dev bs=4M conv=fsync status=none
	got=$(head -c $size $dev | sha256sum | cut -d" " -f1)
	[ "$got" = "$sum" ] || { echo "VERIFY FAILED $p"; exit 1; }
	echo "wrote $p ($dev, $size bytes) ok"
done < SHA256SUMS
sync'

# finish DIR [CLEANUP]: write DIR's images, run CLEANUP on the board, then reboot to
# slot a if asked (after the reboot the board is no longer reachable over ssh).
finish() {
	local dir=$1
	board_root "bash $dir/write.sh"
	[ -z "${2:-}" ] || board_root "$2"
	[ $REBOOT = 1 ] && boot_slot_a
	return 0
}

# Reboot into cboot fastboot (Linux "bootloader" reboot reason = PMC SCRATCH0 bit 30,
# cboot's fastboot flag), make slot a active and boot it. Needs the board's USB device
# port on this host.
boot_slot_a() {
	echo "== rebooting to fastboot"
	board_root "systemd-run --on-active=2 systemctl reboot --reboot-argument=bootloader >/dev/null && echo requested"
	local i
	for i in $(seq 36); do
		sleep 5
		fastboot devices 2>/dev/null | grep -q fastboot && break
	done
	fastboot devices 2>/dev/null | grep -q fastboot || { echo "no fastboot device after 3 min"; exit 1; }
	fastboot set_active a && fastboot getvar current-slot && fastboot reboot
}

if [ $MODE = restore ]; then
	board_root "cd $RDIR && sha256sum -c --quiet SHA256SUMS && echo backup intact"
	finish "$RDIR"
	exit
fi

# 1. Stage on BUILD_HOST: the physical images as <part>_a.img, and super.img cut after
#    the last extent of the new table (everything past it stays as it is on the card).
echo "== staging on $BUILD_HOST"
# lpend.py SUFFIX [FILE]: from lpdump text on stdin, the byte offset where the last extent
# of the partitions named *SUFFIX ends (at least FILE's size).
ssh "$BUILD_HOST" "cat > $HSTAGE.lpend.py" <<'EOF'
import os, re, sys
end, name = 0, ""
for line in sys.stdin:
    m = re.match(r"\s*Name: (\S+)", line)
    if m: name = m.group(1)
    m = re.match(r"\s*(\d+) \.\. (\d+) linear super (\d+)", line)
    if m and name.endswith(sys.argv[1]):
        end = max(end, (int(m.group(3)) + int(m.group(2)) - int(m.group(1)) + 1) * 512)
if len(sys.argv) > 2: end = max(end, os.path.getsize(sys.argv[2]))
assert end > 0, "no extents"
print(end)
EOF
ssh "$BUILD_HOST" "set -euo pipefail
rm -rf $HSTAGE; mkdir -p $HSTAGE; cd $HSTAGE
for p in $PHYS; do ln $POUT/\$p.img \${p}_a.img; done
args=()
for p in $DYN; do
	s=\$(stat -c %s $POUT/\$p.img)
	args+=(--partition \${p}_a:readonly:\$s:nvidia_dynamic_partitions_a --image \${p}_a=$POUT/\$p.img)
	args+=(--partition \${p}_b:readonly:0:nvidia_dynamic_partitions_b)
done
$LP/lpmake --metadata-size 65536 --metadata-slots 3 --super-name super \
	--device super:$SUPER_DEV_SIZE --group nvidia_dynamic_partitions_a:$GROUP_SIZE \
	--group nvidia_dynamic_partitions_b:$GROUP_SIZE \"\${args[@]}\" --output super-full.img >/dev/null
end=\$($LP/lpdump super-full.img | python3 $HSTAGE.lpend.py _a)
head -c \$end super-full.img > super.img && rm super-full.img
$LP/lpdump super.img | sed -n '/^Super partition layout/,/^Block device/p' | grep -E '^super:'
# lpmake reads the images as raw after failing the sparse header check: compare each
# partition's bytes in super.img with its image.
$LP/lpdump super.img | python3 -c '
import re, sys
name, ext = \"\", {}
for l in sys.stdin:
    m = re.match(r\"\\s*Name: (\\S+)\", l)
    if m: name = m.group(1)
    m = re.match(r\"\\s*(\\d+) \\.\\. (\\d+) linear super (\\d+)\", l)
    if m: ext.setdefault(name, []).append((int(m.group(3)) * 512, (int(m.group(2)) - int(m.group(1)) + 1) * 512))
for n, es in ext.items():
    img = open(\"$POUT/\" + n[:-2] + \".img\", \"rb\"); sup = open(\"super.img\", \"rb\")
    for off, size in es:
        sup.seek(off)
        while size:
            k = min(size, 1 << 24); assert sup.read(k) == img.read(k), n; size -= k
    assert img.read(1) == b\"\", n + \" image longer than its extents\"
    print(\"super matches\", n)
'
sha256sum *_a.img super.img > SHA256SUMS
ls -la --time-style=+%F\\ %T $POUT/system.img | awk '{print \"build of\", \$6, \$7}'
cat SHA256SUMS"
[ $STAGE_ONLY = 1 ] && exit 0

T=$(date +%Y%m%d-%H%M%S)
BSTAGE=/var/tmp/slota-$T
BBACK=/var/tmp/slota-backup-$T

# 2. Back up what this run overwrites: the physical partitions whole, and super up to
#    the end of whichever table (old slot a or new) reaches further.
if [ $BACKUP = 1 ]; then
	echo "== backing up slot a to $BOARD:$BBACK"
	board_root "head -c 4194304 /dev/disk/by-partlabel/super > /var/tmp/super-head-$T.bin && chmod 644 /var/tmp/super-head-$T.bin"
	ssh "$BOARD" "cat /var/tmp/super-head-$T.bin" | ssh "$BUILD_HOST" "cat > $HSTAGE/old-head.bin"
	end=$(ssh "$BUILD_HOST" "cd $HSTAGE && $LP/lpdump old-head.bin | python3 $HSTAGE.lpend.py _a super.img")
	board_root "set -e; mkdir -p $BBACK; cd $BBACK
for p in $PHYS; do head -c \$(blockdev --getsize64 /dev/disk/by-partlabel/\${p}_a) /dev/disk/by-partlabel/\${p}_a > \${p}_a.img; done
head -c $end /dev/disk/by-partlabel/super > super.img
sha256sum *.img > SHA256SUMS
cat > write.sh <<'EOF'
$WRITE_SH
EOF
du -sh $BBACK"
	echo "restore with: $0 restore $BBACK"
fi
[ $BACKUP_ONLY = 1 ] && exit 0

# 3. Copy to the board (through this host) and write.
echo "== copying to $BOARD:$BSTAGE"
ssh "$BOARD" "mkdir -p $BSTAGE"
ssh "$BUILD_HOST" "tar -C $HSTAGE -cf - SHA256SUMS super.img $(for p in $PHYS; do printf '%s_a.img ' $p; done)" |
	ssh "$BOARD" "tar -C $BSTAGE -xf - && cd $BSTAGE && sha256sum -c --quiet SHA256SUMS && echo copy ok"
printf '%s\n' "$WRITE_SH" | ssh "$BOARD" "cat > $BSTAGE/write.sh"
echo "== writing slot a"
finish "$BSTAGE" "rm -rf $BSTAGE"
echo "slot a written$([ $BACKUP = 1 ] && echo "; backup in $BOARD:$BBACK")"
