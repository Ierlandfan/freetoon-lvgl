/*
 * ui_cmd.h — external command channel for toonui.
 *
 * Listens on a UNIX socket (/tmp/toonui.cmd) for one-line commands from
 * out-of-process triggers (currently: the doorbell_daemon translating
 * Home Assistant webhooks into "show"/"hide"). Commands are processed
 * on the LVGL thread via a small periodic poll so we don't have to
 * touch LVGL state from the listener thread.
 *
 * Supported commands (one per accepted connection, '\n' terminated):
 *    show   -> camera_open()
 *    hide   -> camera_close()
 */
#ifndef UI_CMD_H
#define UI_CMD_H

/* Spin up the listener thread + LVGL drain timer. Safe to call once at
 * startup; no-op on non-TOON1 targets (the only consumer is camera.c). */
void ui_cmd_start(void);

#endif
