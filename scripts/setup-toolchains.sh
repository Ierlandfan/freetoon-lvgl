#!/bin/sh
# Fetch the three cross toolchains freetoon builds with, into $TC_ROOT
# (default ~/toolchains). Idempotent: anything already present is left alone,
# so it's safe to re-run.
#
#     ./scripts/setup-toolchains.sh          # into ~/toolchains
#     TC_ROOT=/opt/tc ./scripts/setup-toolchains.sh
#
# These used to live in /tmp/qt_rebuild, which a reboot wipes — and then nothing
# can be built or released until they're fetched again. Hence a persistent
# default, a script that documents exactly which ones, and a local tarball cache.
set -e

TC_ROOT="${TC_ROOT:-$HOME/toolchains}"
CACHE="$TC_ROOT/cache"
mkdir -p "$TC_ROOT" "$CACHE"

say() { printf '\n== %s\n' "$1"; }

# fetch <url> <cached-filename>  — cache-first, so a re-run (or a dead upstream)
# doesn't re-download 100 MB.
fetch() {
    if [ -s "$CACHE/$2" ]; then
        echo "   cached: $2"
        return 0
    fi
    echo "   downloading: $2"
    curl -fSL --retry 3 -o "$CACHE/$2.part" "$1"
    mv "$CACHE/$2.part" "$CACHE/$2"
}

# --- Toon 2: Linaro armhf ---------------------------------------------------
# The docs used to name gcc-linaro-7.5.0-2019.12. Linaro has since taken
# releases.linaro.org offline and that exact tarball is simply gone from the
# internet, so we use 7.4.1-2019.02 from the dotsrc Armbian mirror instead.
#
# This substitution is SAFE, and not on faith: `make TARGET=toon2 abi-check`
# passes, and the resulting binary imports exactly the same glibc symbol
# versions (GLIBC_2.4, GLIBC_2.7) as the toonui shipped in v0.9.54. abi-check is
# the real guarantee here — re-run it if you ever change toolchain.
LINARO_TAR=gcc-linaro-7.4.1-2019.02-x86_64_arm-linux-gnueabihf.tar.xz
LINARO_URL=https://mirrors.dotsrc.org/armbian-dl/_toolchain/$LINARO_TAR

if [ -x "$TC_ROOT/linaro/bin/arm-linux-gnueabihf-gcc" ]; then
    say "Toon 2 (Linaro armhf): already present"
else
    say "Toon 2 (Linaro armhf)"
    fetch "$LINARO_URL" "$LINARO_TAR"
    # The mirror's .asc is an MD5, not a signature — same-host, so it proves the
    # download isn't corrupt, nothing more. Check it if we have it.
    if curl -fsSL -o "$CACHE/$LINARO_TAR.md5" "$LINARO_URL.asc" 2>/dev/null; then
        ( cd "$CACHE" && md5sum -c "$LINARO_TAR.md5" ) || {
            echo "   MD5 MISMATCH — refusing to use it" >&2; exit 1; }
    fi
    tar xf "$CACHE/$LINARO_TAR" -C "$TC_ROOT"
    mv "$TC_ROOT/${LINARO_TAR%.tar.xz}" "$TC_ROOT/linaro"
fi

# --- Toon 1: Bootlin armv5 soft-float ---------------------------------------
# arm926ej-s, --with-float=soft. Builds against its own glibc 2.26 headers but
# links dynamically, so the device's glibc 2.21 resolves it at runtime — as long
# as nothing newer than 2.21 is pulled in, which is what abi-check enforces.
BOOTLIN_TAR=armv5-eabi--glibc--stable-2018.02-2.tar.bz2
BOOTLIN_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/$BOOTLIN_TAR

if [ -x "$TC_ROOT/toon1-toolchain/bin/arm-linux-gcc" ]; then
    say "Toon 1 (Bootlin armv5): already present"
else
    say "Toon 1 (Bootlin armv5)"
    fetch "$BOOTLIN_URL" "$BOOTLIN_TAR"
    mkdir -p "$TC_ROOT/toon1-toolchain"
    tar xf "$CACHE/$BOOTLIN_TAR" -C "$TC_ROOT/toon1-toolchain" --strip-components=1
fi

# --- WASM: emsdk + a python it will accept ----------------------------------
# emscripten requires python >= 3.10 and resolves `python3` from PATH. Debian's
# system python3 here is 3.9, which makes emcc abort. We do NOT install a python
# (that's the system's business, and this repo doesn't own /usr/local) — we look
# for one that already exists and shim it onto PATH ahead of the system's.
say "WASM (emsdk)"
PY=""
for c in python3.13 python3.12 python3.11 python3.10 \
         /usr/local/bin/python3.13 /usr/local/bin/python3.12 \
         /usr/local/bin/python3.11 /usr/local/bin/python3.10; do
    p=$(command -v "$c" 2>/dev/null) || continue
    if "$p" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then
        PY="$p"; break
    fi
done
if [ -z "$PY" ]; then
    cat >&2 <<EOF
   NO python >= 3.10 found — emscripten will not run.
   The ARM builds are fine; only the WASM/PWA bundle needs this.
   Install a python >= 3.10 (e.g. python3.11) and re-run this script.
EOF
else
    echo "   python: $PY ($("$PY" --version 2>&1))"
    mkdir -p "$TC_ROOT/pyshim"
    ln -sf "$PY" "$TC_ROOT/pyshim/python3"
    ln -sf "$PY" "$TC_ROOT/pyshim/python"

    if [ -f "$TC_ROOT/emsdk/emsdk_env.sh" ]; then
        echo "   emsdk: already present"
    else
        git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$TC_ROOT/emsdk"
        ( cd "$TC_ROOT/emsdk" \
          && PATH="$TC_ROOT/pyshim:$PATH" EMSDK_PYTHON="$PY" ./emsdk install latest \
          && PATH="$TC_ROOT/pyshim:$PATH" EMSDK_PYTHON="$PY" ./emsdk activate latest )
    fi
fi

say "Done. TC_ROOT=$TC_ROOT"
cat <<EOF

ARM builds need nothing further — the Makefiles use absolute paths:
    make -C lvgl_ui_recovered/src TARGET=toon2
    make -C lvgl_ui_recovered/src TARGET=toon2 abi-check     # always run this

For the WASM bundle, source the env first:
    . scripts/build-env.sh && web/build.sh
(web/build.sh sources it for you if emcc isn't already on PATH.)

Cached tarballs are kept in $CACHE — keep them; the Linaro one is no longer
downloadable from its original home.
EOF
