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

/* video.c (excluded — Toon-1 i.MX27 VPU pipeline). The camera button is only
 * installed when settings.video_enabled, which is 0 in the WASM slave, so these
 * never run; they exist only to satisfy the linker (C links by symbol name). */
void video_install_button(void *parent, void *anchor) { (void)parent; (void)anchor; }
void video_init(void)     { }
void video_shutdown(void) { }
void video_open(void)     { }
void video_close(void)    { }
