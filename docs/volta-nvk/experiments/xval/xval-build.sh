#!/bin/bash
# Runs inside ubuntu:24.04 (glibc old enough for every host). Builds base/head/exp
# into /src/out/<tree>, without AVX2 so the binaries also run on oldcrustylx.
set -eo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq build-essential cmake ninja-build glslc libvulkan-dev spirv-headers git python3 >/dev/null
G=/src/sdk/1.4.363.0/x86_64/bin/glslc
$G --version | head -1
for t in base head exp; do
  cd /src/$t
  rm -rf build-xval
  cmake -G Ninja -B build-xval -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=OFF \
    -DGGML_NATIVE=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_BMI2=OFF -DCMAKE_BUILD_RPATH='$ORIGIN' -DVulkan_GLSLC_EXECUTABLE=$G > /src/cmake-$t.log 2>&1
  grep -iE "integer_dot|coopmat|bfloat" /src/cmake-$t.log || true
  grep -q "GL_EXT_integer_dot_product supported" /src/cmake-$t.log || { echo "NO-INTEGER-DOT $t"; exit 1; }
  cmake --build build-xval -j16 --target test-backend-ops llama-bench > /src/build-$t.log 2>&1 || { grep -E "rror" /src/build-$t.log | head; echo "BUILD-FAILED $t"; exit 1; }
  rm -rf /src/out/$t && mkdir -p /src/out/$t && cp -a build-xval/bin/* /src/out/$t/
  echo "built $t: $(git log --oneline -1)"
done
echo XVAL-BUILD-DONE
