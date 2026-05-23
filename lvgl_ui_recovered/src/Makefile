# Cross-compile the LVGL UI for a Toon.
#
#   make                 # Toon 2 (default): i.MX6 SoloX, ARMv7-A hardfloat+NEON, 1024x600
#   make TARGET=toon1    # Toon 1: i.MX27 ARM926EJ-S, ARMv5TE soft-float, 800x480
#
# Both Toons run glibc 2.21 (kernel 2.6.32 on Toon 2, 2.6.36 on Toon 1) and the
# same HCB/BoxTalk stack. The Toon 1 binary is built with a self-contained
# buildroot armv5 soft-float toolchain (glibc 2.26 headers/crt) but linked
# dynamically against the standard /lib/ld-linux.so.3 the device ships — so the
# device's own glibc 2.21 resolves it at runtime. `make abi-check` verifies the
# binary references no glibc symbol newer than 2.21 (would fail to load on-device).

# Device builds use main.c + the fbdev/evdev drivers; the headless `sim` builds
# swap in sim.c (offscreen framebuffer → PPM) and drop the hardware drivers.
ENTRY      := main.c
LVDRV_SRC  := lv_drivers/display/fbdev.c lv_drivers/indev/evdev.c
EXTRA      :=
# (EXTRA is set per-target in the branches below; it needs BUILDDIR.)

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS      := /tmp/qt_rebuild/toon1-toolchain/bin/arm-linux-
  SYSROOT    := /tmp/qt_rebuild/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  SYSROOTFLAG:= --sysroot=$(SYSROOT)
  ARCHFLAGS  := -march=armv5te -marm -mfloat-abi=soft
  TARGET_DEF := -DTOON1
  OUT        := toonui-toon1
  BUILDDIR   := ../build-toon1
  EXTRA      := $(BUILDDIR)/fbvnc_input $(BUILDDIR)/wastefetch
  WF_POST    := $(CROSS)objcopy --remove-section .note.ABI-tag $(BUILDDIR)/wastefetch && $(CROSS)strip $(BUILDDIR)/wastefetch
  # The buildroot toolchain stamps an NT_GNU_ABI_TAG of "Linux 4.1.0" into the
  # binary. The device's ld-2.21 checks that note against its running kernel
  # (2.6.36) and aborts with "FATAL: kernel too old" — so drop the note. Then
  # strip, for the Toon 1's small flash. (Symbols ARE only glibc 2.21; see
  # abi-check.)
  POSTLINK   := $(CROSS)objcopy --remove-section .note.ABI-tag $(BUILDDIR)/$(OUT) && $(CROSS)strip $(BUILDDIR)/$(OUT)
else ifeq ($(TARGET),toon2)
  CROSS      := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
  SYSROOT    := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
  SYSROOTFLAG:= --sysroot=$(SYSROOT)
  ARCHFLAGS  :=
  TARGET_DEF :=
  OUT        := toonui
  BUILDDIR   := ../build
  EXTRA      := $(BUILDDIR)/fbvnc_input $(BUILDDIR)/wastefetch
  WF_POST    := :
  POSTLINK   := :
else ifeq ($(TARGET),sim)
  # Headless host build — render UI to a PPM, no device/X/SDL. 1024x600.
  CROSS      :=
  SYSROOTFLAG:=
  ARCHFLAGS  :=
  TARGET_DEF := -DSIM
  OUT        := toonui-sim
  BUILDDIR   := ../build-sim
  POSTLINK   := :
  ENTRY      := sim.c
  EXTRA      :=
else ifeq ($(TARGET),sim1)
  # Headless host build at the Toon 1 geometry (800x480).
  CROSS      :=
  SYSROOTFLAG:=
  ARCHFLAGS  :=
  TARGET_DEF := -DSIM -DTOON1
  OUT        := toonui-sim1
  BUILDDIR   := ../build-sim1
  POSTLINK   := :
  ENTRY      := sim.c
  EXTRA      :=
else
  $(error Unknown TARGET '$(TARGET)' — use toon1, toon2, sim or sim1)
endif

CC         := $(CROSS)gcc
LD         := $(CROSS)gcc

# Build-time version stamp — `git describe --tags` from the repo root.
# Falls back to "dev" when there's no git directory (release tarball, etc.).
# Update-check polls GitHub and compares against this.
BUILD_VERSION := $(shell cd ../.. && git describe --tags --always --dirty 2>/dev/null || echo dev)

# LVGL needs LV_CONF_INCLUDE_SIMPLE so it picks up lv_conf.h from current dir
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE $(TARGET_DEF) \
           -DBUILD_VERSION=\"$(BUILD_VERSION)\" \
           -I. -Ilvgl -Ilv_drivers -pthread \
           -MMD -MP
LDFLAGS := $(SYSROOTFLAG) $(ARCHFLAGS) -pthread -lm -lrt

# Sources
LVGL_SRC      := $(shell find lvgl/src -name '*.c')
APP_SRC       := $(ENTRY) boxtalk.c ui_stack.c screen_home.c screen_thermostat.c \
                 screen_dim.c screen_settings.c screen_schedule.c \
                 screen_stats.c screen_forecast.c screen_inbox.c \
                 screen_heater_advanced.c \
                 screen_vent_remote.c screen_vent_advanced.c \
                 screen_lights.c screen_marketplace.c screen_zwave.c screen_wifi.c \
                 screen_adapters.c screen_domoticz.c \
                 stats.c airhist.c homewizard.c meteradapter.c domoticz.c weather.c otgw.c http.c inbox.c rrd_push.c \
                 wastecollection.c ventilation.c homeassistant.c \
                 notify.c healthcheck.c pwa_server.c packages.c mqtt_client.c \
                 schedule.c icons.c backlight.c settings.c bootpick.c update_check.c \
                 tile_slots.c client_link.c news.c doorbell.c \
                 lv_font_montserrat_96_custom.c
ALL_SRC       := $(APP_SRC) $(LVGL_SRC) $(LVDRV_SRC)
OBJ           := $(patsubst %.c,$(BUILDDIR)/%.o,$(ALL_SRC))

# Make doesn't track -D flag changes, so changing BUILD_VERSION alone won't
# recompile the files that bake it in (update_check.c, screen_home.c) — that
# produced mismatched version stamps in a binary. Stamp the value into a file
# that's rewritten only when it changes, and make those objects depend on it.
VERFILE := $(BUILDDIR)/.buildver
$(shell mkdir -p $(BUILDDIR); [ "$$(cat $(VERFILE) 2>/dev/null)" = "$(BUILD_VERSION)" ] || printf '%s' "$(BUILD_VERSION)" > $(VERFILE))

.PHONY: all clean abi-check
all: $(BUILDDIR)/$(OUT) $(EXTRA)

# Recompile the version-bearing objects whenever BUILD_VERSION changes
# (placed after `all` so it doesn't hijack the default goal).
$(BUILDDIR)/update_check.o $(BUILDDIR)/screen_home.o: $(VERFILE)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(OUT): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) $^ -o $@ $(LDFLAGS)
	@$(POSTLINK)

# Guard rail for the Toon 1 build: the binary is compiled against a newer glibc
# (2.26) but must load against the device's glibc 2.21. Fail loudly if anything
# pulled in a symbol version > 2.21 (those would be unresolvable on-device).
abi-check: $(BUILDDIR)/$(OUT)
	@echo "ABI check: glibc symbol versions required by $(OUT)"
	@bad=$$($(CROSS)readelf -V $(BUILDDIR)/$(OUT) 2>/dev/null \
	        | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV \
	        | awk -F. '$$2+0 > 21 {print}'); \
	if [ -n "$$bad" ]; then \
	  echo "  FAIL — needs newer-than-2.21 symbols:"; echo "$$bad" | sed 's/^/    /'; \
	  echo "  (would fail to load on the Toon's glibc 2.21 — avoid those APIs)"; exit 1; \
	else echo "  OK — nothing newer than GLIBC_2.21 required"; fi

# Standalone VNC touch-injection bridge (no LVGL deps). Shipped alongside
# toonui so VNC gives full control on the multi-touch screen.
$(BUILDDIR)/fbvnc_input: fbvnc_input.c
	@mkdir -p $(dir $@)
	$(CC) --sysroot=$(SYSROOT) -O2 -Wall -Wextra -o $@ $<
	$(CROSS)strip $@

# wastefetch — QuickJS host that runs the ToonSoftwareCollective provider
# scripts (full stock-app mimic). QuickJS is vendored under quickjs/.
QJS_OBJ := $(addprefix $(BUILDDIR)/quickjs/,quickjs.o cutils.o libregexp.o libunicode.o libbf.o)
$(BUILDDIR)/quickjs/%.o: quickjs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -w -DCONFIG_VERSION='"2024-01-13"' -D_GNU_SOURCE -Iquickjs -c $< -o $@
$(BUILDDIR)/wastefetch: wastefetch.c $(QJS_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Iquickjs -o $@ wastefetch.c $(QJS_OBJ) -lm -lpthread
	@$(WF_POST)

# Pull in header-dep files (.d) emitted by -MMD so changes to .h trigger rebuild.
-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILDDIR)
