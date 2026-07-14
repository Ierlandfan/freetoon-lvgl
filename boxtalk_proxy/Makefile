# boxtalk_proxy — standalone authenticated LAN gateway to 127.0.0.1:1337.
# Self-contained C, no deps. Cross-compile for either Toon, or build for host.
#
#   make TARGET=toon2   -> boxtalk_proxy        (i.MX6 armhf, glibc <=2.21)
#   make TARGET=toon1   -> boxtalk_proxy-toon1  (i.MX27 armv5, soft-float)
#   make                -> boxtalk_proxy-host   (local test build)

# Cross toolchains — persistent by default (they used to sit in /tmp, which a
# reboot wipes). Fetch with ../scripts/setup-toolchains.sh; override with TC_ROOT.
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= host

ifeq ($(TARGET),toon2)
  CC  := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-gcc
  SR  := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  AR  :=
  OUT := boxtalk_proxy
else ifeq ($(TARGET),toon1)
  CC  := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-gcc
  SR  := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  AR  := -march=armv5te -marm -mfloat-abi=soft
  OUT := boxtalk_proxy-toon1
else
  CC  := gcc
  SR  :=
  AR  :=
  OUT := boxtalk_proxy-host
endif

CFLAGS := $(SR) $(AR) -O2 -Wall -Wextra

$(OUT): boxtalk_proxy.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f boxtalk_proxy boxtalk_proxy-toon1 boxtalk_proxy-host
