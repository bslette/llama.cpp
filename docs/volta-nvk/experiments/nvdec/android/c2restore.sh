#!/system/bin/sh
# c2restore.sh (run as root on Android): drop the c2test.sh overlay and restart.
BIN=/vendor/bin/hw/android.hardware.media.c2-default-service
XML=/odm/etc/media_codecs_c2.xml
setprop ctl.stop android-hardware-media-c2-hal
while [ "$(getprop init.svc.android-hardware-media-c2-hal)" = running ]; do sleep 0.2; done
umount $BIN 2>/dev/null
umount $XML 2>/dev/null
umount /mnt/c2bin /mnt/c2etc 2>/dev/null
setprop ctl.start android-hardware-media-c2-hal
sleep 1
killall mediaserver 2>/dev/null
echo "c2 overlay removed: $(getprop init.svc.android-hardware-media-c2-hal), sha $(sha256sum $BIN | cut -c1-8)"
