/*
 * Toon 1 camera tile -- always-warm video pipeline.
 *
 * Goal: tap on the home-screen tile (or HA "doorbell" trigger) shows live
 * video in well under a second, instead of the ~10 s cold-start path we
 * had originally (TCP connect + ffmpeg analysis + VPU init + keyframe
 * wait). To get there, vpu_stream is spawned once at toonui startup with
 * --warm: it keeps the TCP socket and VPU decoder hot, decoding every
 * frame, but skips the eMMA PrP + framebuffer blit. SIGUSR1 makes it
 * start displaying (at the next I-frame, so the first visible frame is
 * clean); SIGUSR2 returns it to hidden.
 *
 * Lifecycle:
 *   - video_init()  [once, at toonui startup]:
 *       fork+exec /root/vpu/vpu_stream --warm --rect X Y W H 5000
 *       or --rtp PORT when video_rtp is configured
 *       (X,Y,W,H from settings, computed once and remembered so we can
 *       detect settings edits later)
 *   - video_open()  [tile tap or HA trigger]:
 *       if rect changed since spawn -> kill + video_init() again
 *       SIGUSR1  -> vpu_stream starts blitting at next I-VOP
 *       fbdev_set_cutout()  -> LVGL stops drawing the video rect
 *       create transparent click target -> tap closes
 *   - video_close()  [overlay tap or HA "hide" trigger]:
 *       SIGUSR2  -> vpu_stream stops blitting (decoder stays warm)
 *       fbdev_clear_cutout()
 *       delete overlay + invalidate so LVGL repaints over the last frame
 *   - video_shutdown()  [toonui exit]:
 *       SIGTERM the child so it doesn't outlive us
 */
#include "video.h"

/* Toon 1 only — the live video pipeline depends on the i.MX27 VPU/eMMA
 * PrP and the /root/vpu/vpu_stream helper, neither of which exists on
 * Toon 2. On non-TOON1 targets we just compile empty stubs so screen_home
 * can call video_install_button unconditionally. */
#ifndef TOON1
void video_init(void)     {}
void video_shutdown(void) {}
void video_open(void)     {}
void video_close(void)    {}
void video_install_button(lv_obj_t * parent, lv_obj_t * anchor) { (void)parent; (void)anchor; }
#else

#include "display.h"
#include "settings.h"
#include "fbdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#define VPU_STREAM_BIN     "/root/vpu/vpu_stream"
#define DEFAULT_PORT_STR   "5000"

static pid_t        s_pid     = -1;
static lv_obj_t *   s_overlay = NULL;
static int          s_x, s_y, s_w, s_h;
/* Rect we last spawned vpu_stream with; if the resolved rect changes
 * (because the user edited settings), we need to kill+respawn since
 * vpu_stream takes the rect on the command line. */
static int          s_spawn_x = -1, s_spawn_y = -1;
static int          s_spawn_w = -1, s_spawn_h = -1;
static int          s_spawn_rtp = -1;
static int          s_spawn_overlay = -1;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Resolve the rect from settings: size is a % of the configured source
 * dimensions. PP can now upscale, so 125% is allowed for 640x360 -> 800x450.
 * video_src_w/h should match what the OPi side is sending so the aspect ratio
 * is preserved. Pos -1 on either axis = centre on that axis. */
static void resolve_rect(void)
{
    int pct = clamp(settings.video_size_pct ? settings.video_size_pct : 100, 25, 125);
    int sw  = settings.video_src_w ? settings.video_src_w : 640;
    int sh  = settings.video_src_h ? settings.video_src_h : 480;
    s_w = sw * pct / 100;
    s_h = sh * pct / 100;

    s_x = (settings.video_x < 0) ? (DISP_HOR - s_w) / 2 : settings.video_x;
    s_y = (settings.video_y < 0) ? (DISP_VER - s_h) / 2 : settings.video_y;

    /* Clamp inside panel */
    if (s_x < 0) s_x = 0;
    if (s_y < 0) s_y = 0;
    if (s_x + s_w > DISP_HOR) s_x = DISP_HOR - s_w;
    if (s_y + s_h > DISP_VER) s_y = DISP_VER - s_h;
}

/* fork+exec vpu_stream --warm with the current rect. Sets s_pid and
 * remembers the spawn-time rect. Returns 0 on success, -1 on error. */
static int spawn_warm(void)
{
    char xs[12], ys[12], ws[12], hs[12], port[12];
    snprintf(xs, sizeof xs, "%d", s_x);
    snprintf(ys, sizeof ys, "%d", s_y);
    snprintf(ws, sizeof ws, "%d", s_w);
    snprintf(hs, sizeof hs, "%d", s_h);
    snprintf(port, sizeof port, "%d", settings.video_rtp);

    pid_t p = fork();
    if (p == 0) {
        /* Build argv: --warm [--overlay] --rect X Y W H {--rtp PORT | TCPPORT}.
         * --overlay makes vpu_stream render to the fb1 graphic-window
         * (DISP0 FG) plane instead of into fb0 -- the LCDC composites the
         * video over the LVGL UI in hardware, so no fbdev cutout is needed. */
        char *av[16];
        int n = 0;
        setpgid(0, 0);
        av[n++] = "vpu_stream";
        av[n++] = "--warm";
        if (settings.video_overlay) av[n++] = "--overlay";
        av[n++] = "--rect"; av[n++] = xs; av[n++] = ys; av[n++] = ws; av[n++] = hs;
        if (settings.video_rtp > 0) { av[n++] = "--rtp"; av[n++] = port; }
        else                         { av[n++] = DEFAULT_PORT_STR; }
        av[n] = NULL;
        execv(VPU_STREAM_BIN, av);
        _exit(127);
    } else if (p < 0) {
        fprintf(stderr, "[video] fork failed: %s\n", strerror(errno));
        return -1;
    }
    s_pid = p;
    s_spawn_x = s_x; s_spawn_y = s_y;
    s_spawn_w = s_w; s_spawn_h = s_h;
    s_spawn_rtp = settings.video_rtp;
    s_spawn_overlay = settings.video_overlay;
    printf("[video] warm-spawned vpu_stream pid=%d rect=(%d,%d)+%dx%d %s=%d%s\n",
           s_pid, s_x, s_y, s_w, s_h,
           settings.video_rtp > 0 ? "rtp" : "tcp",
           settings.video_rtp > 0 ? settings.video_rtp : atoi(DEFAULT_PORT_STR),
           settings.video_overlay ? " overlay" : "");
    return 0;
}

/* Check whether the warm child has exited unexpectedly (crash, OOM, etc).
 * If so, reset s_pid so the next open() respawns it. */
static void reap_if_gone(void)
{
    if (s_pid <= 0) return;
    int status;
    pid_t r = waitpid(s_pid, &status, WNOHANG);
    if (r == s_pid) {
        fprintf(stderr, "[video] warm vpu_stream pid=%d exited (status=0x%x); will respawn\n",
                s_pid, status);
        s_pid = -1;
    }
}

/* Lifecycle timer: spawns the initial warm child once /dev/mxc_vpu
 * appears, then respawns it whenever it exits. 2 s tick = at most
 * 2 s reconnect latency, cheap (one waitpid + one access() syscall
 * per tick when idle). */
static void video_init_poll_cb(lv_timer_t * t)
{
    (void)t;
    reap_if_gone();
    if (s_pid > 0) return;
    if (access("/dev/mxc_vpu", R_OK | W_OK) != 0)
        return;
    resolve_rect();
    if (spawn_warm() < 0) {
        fprintf(stderr, "[video] warm-spawn failed; will retry next tick\n");
        return;
    }
    /* If the user had the overlay up when the child died, re-arm the
     * show signal on the new child. vpu_stream installs SIGUSR1 at
     * the very top of main() so the old fork+exec race window is gone. */
    if (s_overlay != NULL && s_pid > 0) {
        if (kill(s_pid, SIGUSR1) == 0)
            printf("[video] post-respawn: re-armed show on pid=%d\n", s_pid);
    }
}
void video_init(void)
{
    if (!settings.video_enabled) return;
    /* Lifecycle timer -- spawns the initial warm child once
     * /dev/mxc_vpu appears, then respawns it whenever it exits. */
    lv_timer_create(video_init_poll_cb, 2000, NULL);
}

void video_shutdown(void)
{
    if (s_pid > 0) {
        kill(s_pid, SIGTERM);
        waitpid(s_pid, NULL, 0);   /* block: we're exiting */
        s_pid = -1;
    }
}

static void on_overlay_tap(lv_event_t * e) { (void)e; video_close(); }

void video_open(void)
{
    if (!settings.video_enabled) return;

    resolve_rect();
    reap_if_gone();

    /* Settings edit since last spawn -> respawn with the new rect.
     * vpu_stream takes --rect on argv, no runtime change channel. */
    if (s_pid > 0 && (s_x != s_spawn_x || s_y != s_spawn_y ||
                      s_w != s_spawn_w || s_h != s_spawn_h ||
                      settings.video_rtp != s_spawn_rtp ||
                      settings.video_overlay != s_spawn_overlay)) {
        printf("[video] settings changed (%d,%d)+%dx%d rtp=%d ov=%d -> (%d,%d)+%dx%d rtp=%d ov=%d; respawning warm child\n",
               s_spawn_x, s_spawn_y, s_spawn_w, s_spawn_h, s_spawn_rtp, s_spawn_overlay,
               s_x, s_y, s_w, s_h, settings.video_rtp, settings.video_overlay);
        kill(s_pid, SIGTERM);
        waitpid(s_pid, NULL, 0);
        s_pid = -1;
    }

    if (s_pid <= 0 && spawn_warm() < 0) return;

    /* Tell the warm child to start blitting at the next I-VOP. */
    if (kill(s_pid, SIGUSR1) < 0) {
        fprintf(stderr, "[video] SIGUSR1 to pid=%d failed: %s\n", s_pid, strerror(errno));
        return;
    }

    /* Overlay mode: vpu_stream drives the fb1 (DISP0 FG) hardware plane and
     * enables the LCDC graphic window itself, so the UI is NOT clobbered --
     * no fbdev cutout needed. Direct-to-fb0 mode still needs the cutout so
     * LVGL stops repainting the video rect. */
    if (!settings.video_overlay)
        fbdev_set_cutout(s_x, s_y, s_x + s_w - 1, s_y + s_h - 1);

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_pos(s_overlay, s_x, s_y);
    lv_obj_set_size(s_overlay, s_w, s_h);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, on_overlay_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_invalidate(lv_scr_act());

    printf("[video] show rect=(%d,%d)+%dx%d (warm pid=%d, awaiting I-VOP)\n",
           s_x, s_y, s_w, s_h, s_pid);
}

void video_close(void)
{
    /* Don't kill the child -- just tell it to stop blitting. The decoder
     * stays warm so the next open() shows video in well under a second. */
    if (s_pid > 0) {
        if (kill(s_pid, SIGUSR2) < 0)
            fprintf(stderr, "[video] SIGUSR2 to pid=%d failed: %s\n", s_pid, strerror(errno));
    }
    /* Overlay mode never set a cutout (the FG plane composites in hardware);
     * SIGUSR2 makes vpu_stream disable the graphic window so the UI shows
     * fully again. Only the direct-to-fb0 path needs the cutout cleared. */
    if (!settings.video_overlay)
        fbdev_clear_cutout();
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    lv_obj_invalidate(lv_scr_act());
    printf("[video] hidden (vpu_stream still warm)\n");
}

static void on_tile_tap(lv_event_t * e) { (void)e; video_open(); }

/* Camera home tile — installed only when settings.video_enabled is on.
 * Sized to roughly match the existing right-column tiles (Energy/Family/
 * Water style) and slotted in below them, but in the bottom-LEFT where
 * the v1 button used to sit (clear of the thermostat panel and the
 * weather row). Adjusts to the panel via SX/SY. */
void video_install_button(lv_obj_t * parent, lv_obj_t * anchor)
{
    /* Only present when video is enabled. */
    if (!settings.video_enabled) return;

    /* Icon-only camera button, styled like the +/- setpoint buttons and
       stacked directly above the "+" (anchor). No title/subtitle text. */
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 84, 56);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x335577), 0);   /* COL_TILE_ACCENT */
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 12);
    lv_obj_add_event_cb(btn, on_tile_tap, LV_EVENT_CLICKED, NULL);
    /* OUT_TOP_MID + a small gap so it sits just above the "+" button. */
    if (anchor) lv_obj_align_to(btn, anchor, LV_ALIGN_OUT_TOP_MID, 0, -8);

    lv_obj_t * glyph = lv_label_create(btn);
    lv_label_set_text(glyph, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_color(glyph, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(glyph, &lv_font_montserrat_28, 0);
    lv_obj_center(glyph);
}
#endif  /*TOON1*/
