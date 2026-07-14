# Cross toolchains — persistent by default (they used to sit in /tmp, which a
# reboot wipes). Fetch with ../scripts/setup-toolchains.sh; override with TC_ROOT.
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= host
ifeq ($(TARGET),toon2)
  CC := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-gcc
  SR := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  AR :=
  OUT := boxtalk_mirror
else ifeq ($(TARGET),toon1)
  CC := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-gcc
  SR := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  AR := -march=armv5te -marm -mfloat-abi=soft
  OUT := boxtalk_mirror-toon1
else
  CC := gcc
  SR :=
  AR :=
  OUT := boxtalk_mirror-host
endif
CFLAGS := $(SR) $(AR) -O2 -Wall -Wextra
$(OUT): boxtalk_mirror.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f boxtalk_mirror boxtalk_mirror-toon1 boxtalk_mirror-host
