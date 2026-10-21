#!/bin/bash
# Run as root on bazzite: give the RTX 4060 back to the NVIDIA driver.
set -e
modprobe -r nouveau
echo -n "" > /sys/module/firmware_class/parameters/path
modprobe nvidia_drm modeset=1
systemctl start nvidia-persistenced nvidia-powerd cardwired 2>/dev/null || true
nvidia-smi -L
