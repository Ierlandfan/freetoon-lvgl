/*
 * Toon 1 glibc-compat shim.
 *
 * We build with buildroot's glibc 2.38 (the only version in buildroot
 * 2024.02) but the device runs glibc 2.21. Compiler-generated symbol
 * references default to the newest version the build-host glibc has,
 * which then fail to resolve at load time with
 *   "version `GLIBC_2.34' not found".
 *
 * For each affected function the Toon DOES have at an older version,
 * .symver aliases the default reference back to that older one. The
 * linker then emits e.g. pthread_create@GLIBC_2.4 (present in Toon's
 * libpthread.so.0) instead of pthread_create@GLIBC_2.34 (the merged
 * libc.so variant that doesn't exist there).
 *
 * Mapping rationale:
 *   - libpthread merged into libc at 2.34. The old per-symbol versions
 *     at @GLIBC_2.4 still exist in modern libc.so for back-compat.
 *   - __libc_start_main got a new version at 2.34 (CRT change);
 *     @GLIBC_2.4 is still there.
 *   - C23-mode __isoc23_*: alias to __isoc99_*@GLIBC_2.7 (same ABI for
 *     the format-string features we use).
 *   - stat at 2.33: 32-bit stat ABI hasn't changed; @GLIBC_2.4 works.
 *   - fcntl at 2.28: same -- old @GLIBC_2.4 is still callable.
 *   - fmod at 2.38: Debian/glibc's _Float128 update; plain @GLIBC_2.4
 *     is fine for our regular doubles.
 *
 * Forced into every TU via -include in the Makefile.
 */
#ifndef TOON1_GLIBC_COMPAT_H
#define TOON1_GLIBC_COMPAT_H

/* This header is force-included via -include before any system headers, so
 * __GLIBC__ isn't defined yet — pull it in explicitly so the next check
 * actually evaluates. */
#include <features.h>

#if defined(TOON1) && defined(__GLIBC__)

__asm__(".symver pthread_create,pthread_create@GLIBC_2.4");
__asm__(".symver pthread_detach,pthread_detach@GLIBC_2.4");
__asm__(".symver pthread_attr_setstacksize,pthread_attr_setstacksize@GLIBC_2.4");
__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.4");
__asm__(".symver __isoc23_sscanf,__isoc99_sscanf@GLIBC_2.7");
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.4");
__asm__(".symver fcntl,fcntl@GLIBC_2.4");
__asm__(".symver fmod,fmod@GLIBC_2.4");
/* stat/fstat/lstat: no @GLIBC_2.4 alias in modern libc — these moved from
 * being thin wrappers around __xstat/__fxstat/__lxstat into directly-versioned
 * symbols at @GLIBC_2.33. Toon's glibc 2.21 only has the __xstat family.
 * Linker --wrap (see Makefile) routes calls through __wrap_stat etc. in
 * glibc_compat_wrap.c, which call __xstat directly. */

#endif
#endif /* TOON1_GLIBC_COMPAT_H */
