#!/bin/sh
# freetoon build environment — SOURCE this, don't execute it:
#
#     . scripts/build-env.sh
#
# The ARM builds don't actually need this: the Makefiles reach the cross
# compilers by absolute path under $TC_ROOT, so `make TARGET=toon2` works in a
# cold shell. It's the WASM build that needs an environment (emcc on PATH, and a
# python new enough for it), and this is what supplies it.
#
# Run scripts/setup-toolchains.sh once if $TC_ROOT isn't populated yet.

TC_ROOT="${TC_ROOT:-$HOME/toolchains}"
export TC_ROOT

_ft_missing=""
for _d in "$TC_ROOT/linaro" "$TC_ROOT/toon1-toolchain"; do
    [ -d "$_d" ] || _ft_missing="$_ft_missing $_d"
done
if [ -n "$_ft_missing" ]; then
    echo "build-env: missing:$_ft_missing" >&2
    echo "build-env: run scripts/setup-toolchains.sh" >&2
fi

# --- Emscripten -------------------------------------------------------------
# Deliberately NOT `. emsdk_env.sh`: that script is bash-specific, and this file
# gets sourced from web/build.sh, which runs under /bin/sh (dash on Debian) —
# there it silently sets nothing and you get "emcc not found". All it actually
# does is export EMSDK and prepend two PATH entries, so do that portably.
if [ -d "$TC_ROOT/emsdk/upstream/emscripten" ]; then
    EMSDK="$TC_ROOT/emsdk"
    export EMSDK
    PATH="$EMSDK:$EMSDK/upstream/emscripten:$PATH"

    # emcc reads its config from $EMSDK/.emscripten (written by `emsdk activate`).
    if [ -f "$EMSDK/.emscripten" ]; then
        EM_CONFIG="$EMSDK/.emscripten"
        export EM_CONFIG
    fi

    # emsdk ships a node; only fall back to it if the system has none.
    if ! command -v node >/dev/null 2>&1; then
        _ft_node=$(ls -d "$EMSDK"/node/*/bin 2>/dev/null | head -1)
        [ -n "$_ft_node" ] && PATH="$_ft_node:$PATH"
    fi

    # LAST, so it wins: emscripten requires python >= 3.10 and resolves `python3`
    # from PATH. Debian's system python3 here is 3.9, which makes emcc abort with
    #   AssertionError: emscripten requires python 3.10 or above (... 3.9.2)
    # Setting EMSDK_PYTHON alone does NOT help — the wrappers still shell out to
    # `python3` — so the shim has to be ahead of /usr/bin on PATH.
    if [ -x "$TC_ROOT/pyshim/python3" ]; then
        PATH="$TC_ROOT/pyshim:$PATH"
        EMSDK_PYTHON=$(readlink -f "$TC_ROOT/pyshim/python3" 2>/dev/null || echo "$TC_ROOT/pyshim/python3")
        export EMSDK_PYTHON
    fi
    export PATH
else
    echo "build-env: no emsdk at $TC_ROOT/emsdk — WASM builds unavailable" >&2
    echo "build-env: run scripts/setup-toolchains.sh" >&2
fi

unset _ft_missing _d _ft_node
