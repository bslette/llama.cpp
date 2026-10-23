#!/system/bin/sh
# mctest.sh in.webm out.yuv [codec] (root on Android): run mcdec with SELinux
# permissive for the run only (a shell-domain client cannot share buffers with
# the vendor codec), restoring enforcing afterwards.
was=$(getenforce)
setenforce 0
/data/local/tmp/c2/mcdec "$@"
rc=$?
[ "$was" = Enforcing ] && setenforce 1
exit $rc
