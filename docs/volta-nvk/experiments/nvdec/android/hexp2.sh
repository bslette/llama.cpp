#!/system/bin/sh
# hexp2.sh (root): smallest deblock-top multiplier that decodes each size bit-exact. Writes hexp2.log.
cd /data/local/tmp/c2
exec > hexp2.log 2>&1
setenforce 0
for sz in 1920x1080 1920x2160 3840x1080 4096x2160; do
	for m in 1 2 3 4 5 6 7 8 10; do
		setprop vendor.nvdec.hevc.m_dt $m
		./mcdec hv_$sz.mp4 hv.yuv >/dev/null 2>&1
		echo "$sz x$m $(md5sum hv.yuv | cut -c1-32)"
	done
done
setprop vendor.nvdec.hevc.m_dt 4
setenforce 1
rm -f hv.yuv
echo HX2DONE
