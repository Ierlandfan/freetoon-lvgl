#ifndef TOON_BOOTPICK_H
#define TOON_BOOTPICK_H

/* Boot picker mode — invoked when toonui is started with --bootpick by
 * /mnt/data/ui_launcher.sh. Renders a 10-second countdown screen offering
 * "Keep freetoon" vs "Switch to stock qt-gui". Returns:
 *   0   the user wants (or defaulted to) freetoon
 *   99  the user wants (or defaulted to) qt-gui
 *
 * The default is read from /mnt/data/ui_choice; the picker draws the
 * "current" button highlighted and counts down from 10 to 0 before
 * returning that default's rc.
 *
 * If boot_picker_enabled is 0 in toonui.cfg the function short-circuits
 * — no LVGL init, no screen draw — and returns the appropriate rc
 * immediately, so users who don't want the 10 s delay don't pay for it.
 *
 * Side effects: writes /mnt/data/ui_choice with the final selection so
 * the launcher's fallback path is consistent with what the user actually
 * picked. */
int bootpick_run(void);

#endif
