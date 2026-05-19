# Cross-compile LVGL hello-world for Toon (ARMv7 hardfloat, glibc 2.21)

CROSS      := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT    := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC         := $(CROSS)gcc
LD         := $(CROSS)gcc

OUT        := toonui
BUILDDIR   := ../build

# LVGL needs LV_CONF_INCLUDE_SIMPLE so it picks up lv_conf.h from current dir
CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
           -I. -Ilvgl -Ilv_drivers -pthread \
           -MMD -MP
LDFLAGS := --sysroot=$(SYSROOT) -pthread -lm -lrt

# Sources
LVGL_SRC      := $(shell find lvgl/src -name '*.c')
LVDRV_SRC     := lv_drivers/display/fbdev.c lv_drivers/indev/evdev.c
APP_SRC       := main.c boxtalk.c ui_stack.c screen_home.c screen_thermostat.c \
                 screen_dim.c screen_settings.c screen_schedule.c \
                 screen_stats.c screen_forecast.c screen_inbox.c \
                 screen_heater_advanced.c \
                 screen_vent_remote.c screen_vent_advanced.c \
                 screen_lights.c \
                 stats.c homewizard.c weather.c http.c inbox.c rrd_push.c \
                 wastecollection.c ventilation.c homeassistant.c \
                 notify.c healthcheck.c pwa_server.c packages.c mqtt_client.c \
                 schedule.c icons.c backlight.c settings.c bootpick.c \
                 lv_font_montserrat_96_custom.c
ALL_SRC       := $(APP_SRC) $(LVGL_SRC) $(LVDRV_SRC)
OBJ           := $(patsubst %.c,$(BUILDDIR)/%.o,$(ALL_SRC))

.PHONY: all clean
all: $(BUILDDIR)/$(OUT)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(OUT): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) $^ -o $@ $(LDFLAGS)

# Pull in header-dep files (.d) emitted by -MMD so changes to .h trigger rebuild.
-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILDDIR)
