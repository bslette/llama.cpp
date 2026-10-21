#!/bin/bash
# Run as root on bazzite: show what keeps the NVIDIA modules loaded.
echo "== modules"; lsmod | grep -E "^nvidia"
echo "== open device files"; fuser -v /dev/nvidia* /dev/dri/card0 /dev/dri/renderD129 2>&1
echo "== processes with nvidia mappings"; grep -l nvidia /proc/[0-9]*/maps 2>/dev/null | cut -d/ -f3 | while read p; do echo "$p $(cat /proc/$p/comm) $(stat -c %U /proc/$p)"; done
echo "== consoles"; for v in /sys/class/vtconsole/vtcon*; do echo "$v $(cat $v/name) bind=$(cat $v/bind)"; done
echo "== services"; systemctl list-units --no-pager --state=running | grep -iE "nvidia|display|gdm|sddm|steam|switcheroo|persist"
