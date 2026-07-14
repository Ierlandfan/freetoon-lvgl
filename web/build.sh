#!/bin/sh
# freetoon-WASM convenience build script.
#
# Requires the Emscripten SDK active in the current shell, i.e. you've sourced
# `emsdk_env.sh` from your emsdk checkout (`emcc --version` should print).
#
# Outputs: build/index.html  +  build/index.js  +  build/index.wasm
# Serve that directory over HTTP (any static server) to load freetoon in a
# browser. To wire it onto the master Toon's pwa_server, drop the three files
# into /mnt/data/pwa/ui/ on the Toon.
set -e
cd "$(dirname "$0")"

# Pull in the toolchain environment ourselves if the caller hasn't. build-env.sh
# both sources emsdk and puts a python >= 3.10 ahead of the system one, which
# emscripten hard-requires (Debian's 3.9 makes emcc abort).
if ! command -v emcc >/dev/null 2>&1 && [ -f ../scripts/build-env.sh ]; then
    . ../scripts/build-env.sh
fi

if ! command -v emcc >/dev/null 2>&1; then
    cat <<EOF >&2
emcc not found. Set the build environment up once:
    ./scripts/setup-toolchains.sh      # fetches emsdk (+ both ARM toolchains)
then re-run this script. To use an emsdk you already have, source its
emsdk_env.sh first — but note emscripten needs python >= 3.10 on PATH.
EOF
    exit 1
fi

JOBS="$(nproc 2>/dev/null || echo 4)"

# Toon 2 layout (1024x600) — the default bundle, served at /ui/.
rm -rf build
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release ${BUILD_VERSION:+-DFT_BUILD_VERSION=$BUILD_VERSION} .
emmake make -C build -j"$JOBS"

# Toon 1 layout (800x480) — preview/test the native small-panel layout in a
# browser. Served at /ui-toon1/ (or open build-toon1/index.html locally).
rm -rf build-toon1
emcmake cmake -B build-toon1 -DCMAKE_BUILD_TYPE=Release -DTOON1=ON ${BUILD_VERSION:+-DFT_BUILD_VERSION=$BUILD_VERSION} .
emmake make -C build-toon1 -j"$JOBS"

echo
echo "Built:"
echo "  Toon 2 (1024x600):"; ls -lh build/index.html build/index.wasm build/index.js 2>/dev/null
echo "  Toon 1 (800x480):";  ls -lh build-toon1/index.html build-toon1/index.wasm build-toon1/index.js 2>/dev/null
echo
echo "Quick local test:    cd build && python3 -m http.server 8080  →  http://localhost:8080/index.html"
echo "Deploy Toon 2 PWA:   scp build/index.{html,wasm,js}       root@<toon>:/mnt/data/pwa/ui/"
echo "Deploy Toon 1 PWA:   scp build-toon1/index.{html,wasm,js} root@<toon>:/mnt/data/pwa/ui-toon1/"
