/*
 * stubs_wasm.c — no-op replacements for Toon-only functions the WASM build
 * excludes (backlight, etc). Lets the rest of freetoon link without #ifdef-ing
 * every call site. Add new stubs here as the linker surfaces them.
 */
#include <stdint.h>

/* backlight.c (excluded in CMakeLists.txt — Toon LTR-303 / /sys/class/backlight) */
void backlight_set(int level)        { (void)level; }
int  backlight_auto_level(void)      { return 0; }
void backlight_als_start(void)       { /* nothing — no ambient sensor */ }
