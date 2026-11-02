#!/bin/sh
# FSR 3.1 libraries for a native Linux build (tools/ffx/README.md has the Windows llvm-mingw recipe). Builds the same
# pinned UniversalScalers-Vulkan commit with the host clang and puts the three static libraries and the bridge header
# into <deps>/lib and <deps>/include, the --deps folder of tools/build_game.py, which then links them.
# Usage: tools/ffx/build_linux.sh <deps> [work dir]   Needs git, cmake, ninja, clang, Vulkan headers.
set -eu
DEPS=$(realpath -m "$1"); WORK=${2:-/tmp/ffx-linux}; HERE=$(dirname "$(realpath "$0")")
COMMIT=c64f093404125e960813f95f1996c14780f0dd69
mkdir -p "$WORK" "$DEPS/lib" "$DEPS/include"; cd "$WORK"
[ -d fsr-vulkan ] || git clone -q https://github.com/FireBurn/UniversalScalers-Vulkan fsr-vulkan
git -C fsr-vulkan checkout -q "$COMMIT"
cmake -S fsr-vulkan -B build-ffx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DFFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD=ON -DFFX_VK_PORTABLE_BUILD_TESTS=OFF \
    -DFFX_VK_PORTABLE_BUILD_PROBE=OFF -DFFX_VK_PORTABLE_BUILD_FSR3_HOST=OFF \
    -DFFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND=OFF -DFFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN=OFF \
    -DFFX_VK_PORTABLE_BUILD_INSTALL=OFF ${VULKAN_INCLUDE:+-DVulkan_INCLUDE_DIR=$VULKAN_INCLUDE}
cmake --build build-ffx --target ffx_vulkan_fsr3_vk_backend_3_1_5 ffx_vulkan_portable
K=fsr-vulkan/upstream/ffx-2.3.0/Kits/FidelityFX
clang++ -O2 -fPIC -std=c++17 -c -include fsr-vulkan/src/ffx_1_1_4_portability.hpp -I $K/api/include \
    -I $K/api/internal -I $K/upscalers/include -I $K/upscalers/fsr3/include -I $K/upscalers/fsr3/internal \
    "$HERE/ffx_fsr3_blobs.cpp" -o ffx_fsr3_blobs.o
rm -f libffx_vulkan_fsr3_blobs.a; ar rcs libffx_vulkan_fsr3_blobs.a ffx_fsr3_blobs.o
cp build-ffx/libffx_vulkan_fsr3_vk_backend_3_1_5.a build-ffx/libffx_vulkan_portable.a libffx_vulkan_fsr3_blobs.a "$DEPS/lib/"
cp fsr-vulkan/include/ffx_vk_fsr3_3_1_5_bridge.h "$DEPS/include/"
echo "FSR 3.1 libraries in $DEPS (rebuild the runtime and totk: the log then says FSR 3.1 is available)"
