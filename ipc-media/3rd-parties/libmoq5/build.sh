#!/bin/bash
# Cross-builds libmoq (service tier + picoquic raw QUIC) for RV1106
# and installs headers + static libs into this directory.
# MOQ_CATALOG_INLINE_INIT_DATA: the MOQtail player reads only per-track inline initData.
# Usage: ./build.sh   (needs arm-rockchip830-linux-uclibcgnueabihf-* in PATH)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IPC_MEDIA=$(cd "$HERE/../.." && pwd)
MOQ_ROOT=$(cd "$IPC_MEDIA/.." && pwd)
DEPS_DIR=$MOQ_ROOT/.deps/rv1106
BUILD_DIR=$MOQ_ROOT/build-rv1106

export CMAKE_TOOLCHAIN_FILE=$IPC_MEDIA/cmake/rv1106-toolchain.cmake
export OPENSSL_ROOT_DIR=$IPC_MEDIA/3rd-parties/libopenssl-1.1.1h
export MOQ_DEPS_DIR=$DEPS_DIR
# Built for size (-Os); a publisher over raw QUIC needs no WebTransport adapters or examples
export MOQ_PICOTLS_BUILD_CONFIG=MinSizeRel

"$MOQ_ROOT/scripts/setup_picoquic_deps.sh" > /dev/null

cmake -G Ninja -S "$MOQ_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_INSTALL_PREFIX="$HERE" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \
    -DMOQ_BUILD_TESTS=OFF -DMOQ_BUILD_SIM=OFF -DMOQ_BUILD_EXAMPLES=OFF \
    -DMOQ_BUILD_MSF=ON -DMOQ_BUILD_MEDIA_OBJECT=ON -DMOQ_BUILD_SERVICE=ON \
    -DMOQ_BUILD_ADAPTER_PICOQUIC=ON -DMOQ_BUILD_PQ_THREADED=ON \
    -DMOQ_BUILD_ADAPTER_PICO_WT=OFF -DMOQ_BUILD_PICO_WT_MANAGED=OFF \
    -DMOQ_CATALOG_INLINE_INIT_DATA=ON -DMOQ_BUILD_CMAF_PACKAGER=ON \
    -DMOQ_PICOQUIC_SOURCE_DIR="$DEPS_DIR/picoquic" \
    -DMOQ_PICOTLS_PREFIX="$DEPS_DIR/picotls/build"
cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"

# picotls is not part of the libmoq install; ship it next to the other archives
cp "$DEPS_DIR"/picotls/build/libpicotls-{core,openssl,minicrypto}.a "$HERE/lib/"
