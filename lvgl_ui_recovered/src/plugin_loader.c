/*
 * plugin_loader.c — Dynamic Shared Object (.so) Plugin Screen Loader for FreeToon.
 *
 * Allows marketplace applications to provide their own compiled LVGL detail
 * screen (.so plugin) without requiring any app-specific code in toonui.
 */

#include "plugin_loader.h"
#include "plugin_abi.h"
#include "screens.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#define MAX_CACHED_PLUGINS 8
static struct {
    char id[64];
    void * handle;
    lv_obj_t * (*create_fn)(void);
} s_plugins[MAX_CACHED_PLUGINS];
static int s_plugin_count = 0;

/* settings_t fields plugins may use (see plugin_abi.h). A plugin naming a
 * field that is not here, or whose offset/size differ from this build, is
 * refused. Add a field here when a plugin needs it. */
static const ft_plugin_field_t s_exported_fields[] = {
    FT_SETTINGS_FIELD(language),
    FT_SETTINGS_FIELD(home_light_mode),
    FT_SETTINGS_FIELD(sonos_enabled),
    FT_SETTINGS_FIELD(sonos_host),
    FT_SETTINGS_FIELD(sonos_port),
    FT_SETTINGS_FIELD(sonos_zone),
};

/* Returns NULL if the plugin's ABI matches this build, else a short reason. */
static const char * plugin_abi_mismatch(void * h, char * why, size_t whysz) {
    const ft_plugin_abi_t * abi = (const ft_plugin_abi_t *)dlsym(h, FT_PLUGIN_ABI_SYMBOL);
    if (!abi)
        return "no ABI declaration (built for an older FreeToon)";
    if (abi->magic != FT_PLUGIN_ABI_MAGIC)
        return "bad ABI magic";
    if (abi->abi_version != FT_PLUGIN_ABI_VERSION) {
        snprintf(why, whysz, "plugin ABI %u, toonui ABI %u",
                 (unsigned)abi->abi_version, (unsigned)FT_PLUGIN_ABI_VERSION);
        return why;
    }
    if (abi->lvgl_version != FT_PLUGIN_LVGL_VERSION) {
        snprintf(why, whysz, "built for LVGL %u.%u, toonui has %u.%u",
                 (unsigned)(abi->lvgl_version >> 8), (unsigned)(abi->lvgl_version & 0xff),
                 (unsigned)LVGL_VERSION_MAJOR, (unsigned)LVGL_VERSION_MINOR);
        return why;
    }
    if (abi->n_fields > 128 || (abi->n_fields && !abi->fields))
        return "malformed field table";

    const size_t n_exp = sizeof s_exported_fields / sizeof s_exported_fields[0];
    for (uint32_t i = 0; i < abi->n_fields; i++) {
        const ft_plugin_field_t * pf = &abi->fields[i];
        const ft_plugin_field_t * hf = NULL;
        if (!pf->name) return "malformed field table";
        for (size_t j = 0; j < n_exp; j++) {
            if (strcmp(s_exported_fields[j].name, pf->name) == 0) { hf = &s_exported_fields[j]; break; }
        }
        if (!hf) {
            snprintf(why, whysz, "uses settings.%.40s, which toonui does not export", pf->name);
            return why;
        }
        if (hf->offset != pf->offset || hf->size != pf->size) {
            snprintf(why, whysz, "settings.%.40s moved (plugin %u+%u, toonui %u+%u)", pf->name,
                     (unsigned)pf->offset, (unsigned)pf->size,
                     (unsigned)hf->offset, (unsigned)hf->size);
            return why;
        }
    }
    return NULL;
}

static void show_plugin_refused(const char * id) {
    char msg[192];
    snprintf(msg, sizeof msg,
             tr("De app '%s' is niet compatibel met deze FreeToon-versie.\n"
                "Werk de app bij via de Marketplace.",
                "The app '%s' is not compatible with this FreeToon version.\n"
                "Update it from the Marketplace."), id);
    lv_obj_t * mb = lv_msgbox_create(NULL, tr("App bijwerken", "Update app"), msg, NULL, true);
    lv_obj_center(mb);
}

int plugin_screen_open(const integration_meta_t * meta) {
    if (!meta || !meta->id[0]) return 0;

    /* Check if already loaded in memory to prevent dlclose-on-thread crashes */
    for (int i = 0; i < s_plugin_count; i++) {
        if (strcmp(s_plugins[i].id, meta->id) == 0 && s_plugins[i].create_fn) {
            lv_obj_t * scr = s_plugins[i].create_fn();
            if (scr) {
                ui_push(scr);
                return 1;
            }
            return 0;
        }
    }

    struct utsname uts;
    const char * arch = "toon2";
    if (uname(&uts) == 0 && strncmp(uts.machine, "armv5", 5) == 0) {
        arch = "toon1";
    }

    char path[256];
    struct stat st;

    /* 1. Try arch-specific naming: <id>_ui.<arch>.so (e.g. sonos_ui.toon2.so) */
    snprintf(path, sizeof path, "/mnt/data/integrations/%s/%s_ui.%s.so",
             meta->id, meta->id, arch);
    if (stat(path, &st) != 0) {
        /* 2. Try generic <id>_ui.so */
        snprintf(path, sizeof path, "/mnt/data/integrations/%s/%s_ui.so",
                 meta->id, meta->id);
    }
    if (stat(path, &st) != 0) {
        /* 3. Try ui.so */
        snprintf(path, sizeof path, "/mnt/data/integrations/%s/ui.so",
                 meta->id);
    }
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[plugin] no UI library found for '%s'\n", meta->id);
        return 0;
    }

    fprintf(stderr, "[plugin] loading %s ...\n", path);
    void * h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[plugin] dlopen failed for %s: %s\n", path, dlerror());
        return 0;
    }

    /* Refuse plugins compiled against a different settings_t layout / ABI
     * before running any of their code paths that read `settings`. */
    char why[160];
    const char * bad = plugin_abi_mismatch(h, why, sizeof why);
    if (bad) {
        fprintf(stderr, "[plugin] refusing %s: %s\n", path, bad);
        dlclose(h);
        show_plugin_refused(meta->id);
        return 0;
    }

    /* Look for entry function */
    lv_obj_t * (*create_fn)(void) = (lv_obj_t * (*)(void))dlsym(h, "screen_create");
    if (!create_fn) {
        char alt_sym[64];
        snprintf(alt_sym, sizeof alt_sym, "%s_screen_create", meta->id);
        create_fn = (lv_obj_t * (*)(void))dlsym(h, alt_sym);
    }
    if (!create_fn) {
        fprintf(stderr, "[plugin] no screen_create symbol in %s: %s\n", path, dlerror());
        dlclose(h);
        return 0;
    }

    lv_obj_t * scr = create_fn();
    if (!scr) {
        fprintf(stderr, "[plugin] %s screen_create returned NULL\n", meta->id);
        dlclose(h);
        return 0;
    }

    if (s_plugin_count < MAX_CACHED_PLUGINS) {
        snprintf(s_plugins[s_plugin_count].id, sizeof(s_plugins[s_plugin_count].id), "%s", meta->id);
        s_plugins[s_plugin_count].handle = h;
        s_plugins[s_plugin_count].create_fn = create_fn;
        s_plugin_count++;
    }

    ui_push(scr);
    return 1;
}
