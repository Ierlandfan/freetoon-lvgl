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

if ! command -v emcc >/dev/null 2>&1; then
    cat <<EOF >&2
emcc not found. Install Emscripten (https://emscripten.org/docs/getting_started/downloads.html):
    git clone https://github.com/emscripten-core/emsdk
    cd emsdk && ./emsdk install latest && ./emsdk activate latest
    source ./emsdk_env.sh
then re-run this script.
EOF
    exit 1
fi

rm -rf build
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release .
emmake make -C build -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Built:"
ls -lh build/index.html build/index.wasm build/index.js 2>/dev/null
echo
echo "Quick local test:    cd build && python3 -m http.server 8080  →  http://localhost:8080/index.html"
echo "Deploy to the Toon:  scp build/index.{html,wasm,js} root@<toon>:/mnt/data/pwa/ui/"
