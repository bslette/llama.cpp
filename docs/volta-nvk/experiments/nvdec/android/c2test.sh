#!/system/bin/sh
# c2test.sh (run as root on Android): overlay the Codec2 service and codec list
# staged in /data/local/tmp/c2 with tmpfs bind mounts, then restart the HAL
# and mediaserver so the codec list is rebuilt. Undo with c2restore.sh.
S=/data/local/tmp/c2
BIN=/vendor/bin/hw/android.hardware.media.c2-default-service
XML=/odm/etc/media_codecs_c2.xml
# the running binary pins its bind mount: stop the HAL before replacing it
setprop ctl.stop android-hardware-media-c2-hal
while [ "$(getprop init.svc.android-hardware-media-c2-hal)" = running ]; do sleep 0.2; done
umount $BIN 2>/dev/null
umount $XML 2>/dev/null
umount /mnt/c2bin /mnt/c2etc 2>/dev/null
# labeled tmpfs mounts need permissive mode; enforcing returns before the services start
was=$(getenforce)
setenforce 0
for d in /mnt/c2bin /mnt/c2etc; do mkdir -p $d; done
mount -t tmpfs -o context=u:object_r:mediacodec_exec:s0 c2bin /mnt/c2bin
mount -t tmpfs -o context=u:object_r:vendor_configs_file:s0 c2etc /mnt/c2etc
cp $S/android.hardware.media.c2-default-service /mnt/c2bin/svc
cp $S/media_codecs_c2.xml /mnt/c2etc/media_codecs_c2.xml
chmod 755 /mnt/c2bin/svc
chmod 644 /mnt/c2etc/media_codecs_c2.xml
mount --bind /mnt/c2bin/svc $BIN
mount --bind /mnt/c2etc/media_codecs_c2.xml $XML
[ "$was" = Enforcing ] && setenforce 1
setprop ctl.start android-hardware-media-c2-hal
sleep 1
killall mediaserver 2>/dev/null
sleep 2
echo "c2 overlay active: $(getprop init.svc.android-hardware-media-c2-hal), sha $(sha256sum $BIN | cut -c1-8)"
