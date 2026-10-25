set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq git build-essential pkg-config meson ninja-build python3-mako python3-yaml python3-packaging python3-ply \
 rustc cargo bindgen cbindgen flex bison glslang-tools spirv-tools spirv-tools-dev spirv-headers libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev libelf-dev \
  libudev-dev llvm-dev libclang-dev libclang-cpp-dev clang libclc-21-dev libllvmspirvlib-21-dev libpolly-dev > /src/apt.log 2>&1
git config --global --add safe.directory "*"
git -C /src/mesa-main log -1 --format="building %h %s"
[ -d /src/bidp ] || meson setup /src/bidp /src/mesa-main -Dbuildtype=release -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=nouveau -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dglx=disabled -Degl=disabled -Dllvm=enabled -Dbuild-tests=false -Dtools= -Dteflon=false -Dvideo-codecs= -Dmesa-clc=enabled -Dinstall-mesa-clc=false > /src/bidp-meson.log 2>&1 || { tail -30 /src/bidp-meson.log; exit 1; }
ninja -C /src/bidp src/nouveau/vulkan/libvulkan_nouveau.so > /src/bidp.log 2>&1 || { tail -30 /src/bidp.log; exit 1; }
mkdir -p /src/icd-idp && cp /src/bidp/src/nouveau/vulkan/libvulkan_nouveau.so /src/icd-idp/
echo BUILD_EXIT=0
