/*
 * plugin_abi.h — the contract between toonui and marketplace UI plugins (.so).
 *
 * Plugins are dlopen()ed into toonui and read the global `settings` struct
 * directly. Any field toonui inserts in front of the ones a plugin uses
 * shifts their offsets, and an old plugin then silently reads the wrong
 * memory (this happened with the Hue fields in 1.0.4: the Sonos plugin read
 * hue_host as its Sonos host). So every plugin MUST declare the settings
 * fields it touches, and the loader refuses it unless each field still has
 * the same offset and size in this toonui build.
 *
 * In a plugin, once, at file scope in one .c file:
 *
 *     #include "plugin_abi.h"
 *     FT_PLUGIN_DECLARE_ABI(
 *         FT_SETTINGS_FIELD(language),        // implied by tr() in i18n.h
 *         FT_SETTINGS_FIELD(home_light_mode),
 *         FT_SETTINGS_FIELD(sonos_host));
 *
 * or, for a plugin that never touches `settings` (and does not use tr()):
 *
 *     FT_PLUGIN_DECLARE_ABI_NO_SETTINGS();
 *
 * Rules:
 *   - List EVERY settings field the plugin reads or writes, including the
 *     ones used through inline helpers (tr() uses `language`).
 *   - Only fields in the host's export table (plugin_loader.c) can be used.
 *     Adding a field there is cheap; do it in the same change.
 *   - Bump FT_PLUGIN_ABI_VERSION when anything else plugins depend on
 *     changes incompatibly: an exported function's signature or semantics,
 *     a struct passed to plugins, or lv_conf.h options that change LVGL
 *     struct sizes. Appending new settings fields needs NO bump.
 */
#ifndef FREETOON_PLUGIN_ABI_H
#define FREETOON_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "settings.h"

#define FT_PLUGIN_ABI_MAGIC    0x46545041u   /* "FTPA" */
#define FT_PLUGIN_ABI_VERSION  1u

/* LVGL major/minor; patch releases keep the ABI. */
#define FT_PLUGIN_LVGL_VERSION ((uint32_t)((LVGL_VERSION_MAJOR << 8) | LVGL_VERSION_MINOR))

typedef struct {
    const char * name;
    uint32_t     offset;
    uint32_t     size;
} ft_plugin_field_t;

typedef struct {
    uint32_t                  magic;          /* FT_PLUGIN_ABI_MAGIC */
    uint32_t                  abi_version;    /* FT_PLUGIN_ABI_VERSION */
    uint32_t                  lvgl_version;   /* FT_PLUGIN_LVGL_VERSION */
    uint32_t                  n_fields;
    const ft_plugin_field_t * fields;
} ft_plugin_abi_t;

/* Name of the symbol the loader looks up. */
#define FT_PLUGIN_ABI_SYMBOL "freetoon_plugin_abi"

#define FT_SETTINGS_FIELD(f) \
    { #f, (uint32_t)offsetof(settings_t, f), (uint32_t)sizeof(((settings_t *)0)->f) }

#define FT_PLUGIN_DECLARE_ABI(...)                                            \
    static const ft_plugin_field_t ft_plugin_fields_[] = { __VA_ARGS__ };     \
    __attribute__((visibility("default"), used))                              \
    const ft_plugin_abi_t freetoon_plugin_abi = {                             \
        FT_PLUGIN_ABI_MAGIC, FT_PLUGIN_ABI_VERSION, FT_PLUGIN_LVGL_VERSION,   \
        (uint32_t)(sizeof ft_plugin_fields_ / sizeof ft_plugin_fields_[0]),   \
        ft_plugin_fields_ }

#define FT_PLUGIN_DECLARE_ABI_NO_SETTINGS()                                   \
    __attribute__((visibility("default"), used))                              \
    const ft_plugin_abi_t freetoon_plugin_abi = {                             \
        FT_PLUGIN_ABI_MAGIC, FT_PLUGIN_ABI_VERSION, FT_PLUGIN_LVGL_VERSION,   \
        0, NULL }

#endif
