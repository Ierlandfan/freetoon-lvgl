/*
 * camera.h — Toon 1 live video tile glue.
 *
 * Always-warm pipeline: vpu_stream is forked once at toonui startup with
 * --warm and keeps the TCP socket + VPU decoder hot, decoding silently
 * in the background. Tile tap (or HA trigger) signals SIGUSR1 to start
 * blitting at the next I-frame; closing signals SIGUSR2 to stop. This
 * gets us sub-second show latency from a cold "Camera" tap.
 *
 * The rect is read from settings (video_x/y/w/h, all in panel pixels),
 * with sensible defaults so it works out of the box. If those settings
 * change while the warm child is running, the next video_open() kills
 * and respawns it with the new rect (vpu_stream takes --rect on argv).
 */
#ifndef VIDEO_H
#define VIDEO_H

#include "lvgl/lvgl.h"

/* Add an icon-only camera button to `parent`, stacked directly above
 * `anchor` (the home-screen "+" setpoint button). Created only when video
 * is enabled (no-op otherwise). Tap opens the video overlay. */
void video_install_button(lv_obj_t * parent, lv_obj_t * anchor);

/* Lifecycle.
 *   video_init()     -- called once during toonui startup, spawns the
 *                        warm vpu_stream child. No-op if camera disabled.
 *   video_shutdown() -- called on clean toonui exit, SIGTERMs the child.
 *   video_open()     -- tile tap / HA trigger, show video.
 *   video_close()    -- overlay tap / HA hide, stop blitting (child stays warm).
 */
void video_init(void);
void video_shutdown(void);
void video_open(void);
void video_close(void);

#endif
