#!/system/bin/sh
# k4final.sh (root): 4K correctness + throughput. Writes k4final.log.
cd /data/local/tmp/c2
exec > k4final.log 2>&1
sh c2test.sh | tail -1
setenforce 0
echo "== correctness (md5 of decoded I420)"
for f in k4_h264.mp4 k4_hevc.mp4 k4_vp9.webm hv_1920x1080.mp4 hv_1920x2160.mp4 hv_3840x1080.mp4 hv_4096x2160.mp4; do
	./mcdec $f o.yuv >/dev/null 2>&1; echo "$f $(md5sum o.yuv | cut -c1-32)"
done
rm -f o.yuv
echo "== decode to a surface, unpaced"
for f in k4_h264.mp4 k4_hevc.mp4 k4_vp9.webm; do
	echo "$f: $(MCDEC_SURFACE=1 ./mcdec $f x | grep -E 'frames .* fps')"
done
echo "== decode to a surface, paced at 60 fps"
for f in k4_h264.mp4 k4_hevc.mp4 k4_vp9.webm; do
	echo "$f: $(MCDEC_SURFACE=1 MCDEC_REALTIME=1 ./mcdec $f x | grep -E 'fps|latency' | tr '\n' ' ')"
done
echo "== encode 4K (ByteBuffer NV12 input)"
MCENC_BITRATE=40000000 MCENC_FPS=30 ./mcenc src4k_30.yuv 3840x2160 30 e4k.h264 | tail -1
setenforce 1
echo "== screenrecord at 3840x2160"
screenrecord --size 3840x2160 --time-limit 4 --bit-rate 40000000 sr4k.mp4 2>&1 | tail -1
chmod 644 e4k.h264 sr4k.mp4
logcat -d -s C2NvencEnc | tail -3
echo K4FDONE
