#!/bin/bash
# Run as root on bazzite: hand the RTX 4060 from the NVIDIA driver to nouveau (for NVK).
# The Plasma session (kwin/Xwayland) holds the NVIDIA card; log out of it first.
# cardwired (Bazzite GPU switching) and nvidia-powerd also keep it open.
set -e
systemctl stop cardwired nvidia-powerd nvidia-persistenced 2>/dev/null || true
sleep 1
if fuser -s /dev/nvidia* /dev/dri/card0 2>/dev/null; then
  echo "still in use:"; fuser -v /dev/nvidia* /dev/dri/card0; exit 1
fi
unload() { for m in nvidia_drm nvidia_modeset nvidia_uvm nvidia; do
  if [ -d /sys/module/$m ]; then rmmod $m || return 1; fi; done; }
if ! unload; then
  # nvidia-drm's fbdev may carry the text console; detach it and retry
  echo 0 > /sys/class/vtconsole/vtcon1/bind
  unload
fi
# the image has no AD107 GSP firmware; use linux-firmware's copy staged in /var
echo -n /var/home/slettela/nouveau-fw > /sys/module/firmware_class/parameters/path
modprobe nouveau
sleep 3
dmesg | grep -i nouveau | tail -8
ls -l /dev/dri/by-path
