# Running freetoon-lvgl on a Toon 1 — what's needed

Status: **the build target now exists** (`make TARGET=toon1` produces a correct
ARMv5TE soft-float binary — see "How to build", below). What's left before it's
shippable is the **800×480 layout reflow**, which needs a real Toon 1 to tune
against (none on hand). The CPU/ABI/libc/feed unknowns from the original
investigation are all resolved (see "Resolved: confirmed from the live Quby
feed").

## Toon 1 vs Toon 2 hardware
| | Toon 1 | Toon 2 (what we build for) |
|---|---|---|
| SoC | Freescale **i.MX27**, ARM926EJ-S | i.MX6 SoloX, Cortex-A9 |
| CPU arch | **ARMv5TE, ~400 MHz, NO FPU** (soft-float) | ARMv7-A + NEON + VFPv3, ~800 MHz |
| Display | **800 × 480** (imx-fb) | 1024 × 600 |
| RAM | small (~128 MB class) | 512 MB |

Consequence: our binary (armv7-a, **hardfloat + NEON**, laid out for 1024×600)
**will not even start** on a Toon 1 — it would hit illegal instructions, and the
UI wouldn't fit.

## What changes for Toon 1 (and current status)
1. **A soft-float ARMv5 build target** — ✅ **done.** `make TARGET=toon1` uses a
   soft-float `arm926ej-s` toolchain with `-march=armv5te -marm -mfloat-abi=soft`
   (no NEON), links dynamically against the device's `ld-linux.so.3`, and verifies
   the glibc ABI. See "How to build" below.
2. **800×480 layout** — ⏳ **scaffolded, reflow pending hardware.** `main.c` no
   longer hardcodes the geometry; `display.h` switches `DISP_HOR/DISP_VER` on
   `-DTOON1` and exposes `SX()/SY()/SUNI()`. The ~20 screens still carry absolute
   1024×600 coordinates and must be re-flowed for 800×480 — the **bulk of the
   effort**, and not verifiable without a device.
3. **Performance tuning** — ⏳ pending hardware. LVGL is float-heavy and this is a
   ~400 MHz soft-float ARMv5: keep 16-bpp, cut animations/large redraws, lower the
   refresh rate, maybe drop the swipe tileview. Needs measuring on real hardware.

## Checklist for a tester on a real rooted Toon 1
Have them root it (ToonSoftwareCollective "Root-A-Toon-USB-Stick") and run, then
send back the output:
```
uname -a                                  # kernel + arch
cat /proc/cpuinfo                          # confirm ARMv5TE, no vfp/neon in Features
cat /sys/class/graphics/fb0/virtual_size   # fb resolution (expect 800,480)
cat /sys/class/graphics/fb0/bits_per_pixel # 16 or 32 bpp
cat /proc/bus/input/devices                # touch input device (which /dev/input/eventN)
ls -l /dev/fb0 /dev/input/event*
ldd --version 2>&1 | head -1; ls -l /lib/libc*    # glibc version / ABI
readelf -A /bin/busybox | grep -Ei "arch|vfp|abi"  # Tag_CPU_arch, Tag_ABI_VFP_args
cat /etc/issue /etc/os-release 2>/dev/null         # Quby firmware version
free; df -h                                # RAM + storage (where /mnt/data is)
# Is the HCB / BoxTalk stack the same as Toon 2?
netstat -ltnp 2>/dev/null | grep 1337      # BoxTalk broker on 127.0.0.1:1337 ?
ps | grep -E "happ_|hcb|qt-gui|startqt"    # daemon set + how the GUI launches
cat /etc/inittab | grep -iE "qt-gui|gui|flas|toon"   # GUI launch row (where ui_launcher would slot)
```

### What each answer decides
- **cpuinfo / readelf ABI** → exact compiler flags + which toolchain & libc to use.
- **fb resolution / bpp** → the layout target and `lv_conf` color depth.
- **input device** → the evdev node + calibration for touch.
- **port 1337 + happ_thermstat** → whether our BoxTalk/HCB integration works as-is
  or the Toon 1 firmware speaks a different/older protocol (could be the biggest
  unknown — Toon 1 is older Quby firmware).
- **inittab** → how `ui_launcher.sh` takes over the framebuffer (same trick as
  Toon 2, by command).

## Resolved: confirmed from the live Quby feed
Pulled directly off the official `feed.hae.int` (the Toon's own package feed,
over the update VPN) — the `qb2` machine = Toon 1:
- **CPU/ABI: ARMv5TE, soft-float.** The qb2 upgrade script appends
  `arch armv5e 19` to opkg's arch.conf, and the actual `libc6_2.21-r0_armv5e.ipk`
  reports `Tag_CPU_arch: v5TE` with **no `Tag_ABI_VFP_args`** (soft-float) and
  loader `ld-linux.so.3`.
- **libc: glibc 2.21.** Same major version as Toon 2.
- **Kernel: 2.6.36** (Toon 2 is 2.6.32). Matters for the toolchain — see the
  ABI-tag note in the Makefile/build below.
- **Userland: OpenEmbedded `angelica-1.4.0-master-qt5112-ssl102-aws`** (Qt 5.11.2,
  OpenSSL 1.0.2). Feed root:
  `http://feed.hae.int/feeds/qb2/oe/angelica-1.4.0-master-qt5112-ssl102-aws/armv5e`.
- **BoxTalk/HCB stack is the SAME** (biggest risk cleared earlier): identical
  `urn:hcb-hae-com` serviceids; only the base path differs (Toon 1 `/qmf/...`
  vs Toon 2 `/HCBv2/...`).
- For reference, **Toon 2** = ARMv7-A + VFPv3 hardfloat (`ld-linux-armhf`),
  kernel 2.6.32, opkg flavour **`nxt`**.

## How to build
```
cd lvgl_ui_recovered/src
make TARGET=toon1        # → ../build-toon1/toonui-toon1  (ARMv5TE soft-float, 800x480)
make TARGET=toon1 abi-check   # asserts no glibc symbol newer than 2.21 is needed
make                     # Toon 2 (default), unchanged → ../build/toonui
```
Toolchain: a self-contained buildroot `armv5-eabi glibc` cross-compiler
(arm926ej-s, `--with-float=soft`) at `$TC_ROOT/toon1-toolchain`, i.e.
`~/toolchains/toon1-toolchain` by default — fetch it with
`scripts/setup-toolchains.sh` (Bootlin
`armv5-eabi--glibc--stable-2018.02-2`). It compiles with its own glibc 2.26
headers/crt but the result links **dynamically** against the device's standard
`/lib/ld-linux.so.3`, so the Toon 1's own glibc 2.21 resolves it at runtime —
provided no symbol newer than 2.21 is pulled in (`abi-check` enforces this).

One gotcha the Makefile handles automatically: the buildroot toolchain stamps an
`NT_GNU_ABI_TAG` of "Linux 4.1.0" into the binary; the device's `ld-2.21` checks
that against its running **2.6.36** kernel and would abort with *"FATAL: kernel
too old."* The `toon1` build strips that note (`objcopy --remove-section
.note.ABI-tag`) and then strips symbols for the Toon 1's smaller flash.

## What's left (needs a real Toon 1)
1. **800×480 layout reflow.** `display.h` now centralizes the geometry
   (`DISP_HOR`/`DISP_VER` switch on `-DTOON1`) and exposes `SX()/SY()/SUNI()`
   to map the 1024×600 design space onto the panel. New/ported layout code
   should use these; the existing ~20 screens still carry raw 1024×600
   coordinates and must be hand-tuned for 800×480 — there's no magic auto-scale,
   and it can't be verified without hardware. **This is the bulk of the work.**
2. **Performance tuning.** LVGL is float-heavy and this is a ~400 MHz soft-float
   ARMv5: keep 16-bpp, cut animations/large redraws, lower the refresh rate,
   maybe drop the swipe tileview. Measure on-device.
3. **Runtime green-light from the tester checklist** above (fb node + resolution,
   touch evdev node, `/qmf` base path for BoxTalk, daemon set).

## Bottom line
The hard ABI/toolchain/libc question is **solved** — `make TARGET=toon1` yields a
correct, device-loadable ARMv5TE soft-float binary today. The remaining work is
the 800×480 layout pass and on-device perf/runtime validation, both of which
need a real rooted Toon 1 in hand to do properly.
