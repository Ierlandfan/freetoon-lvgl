/*
 * Toon 1 stat/fstat/lstat wrappers.
 *
 * These pair with -Wl,--wrap=stat,fstat,lstat in the Makefile. Every call to
 * stat() in the binary gets renamed by the linker to __wrap_stat(); we then
 * forward to __xstat() which DOES exist on Toon's glibc 2.21. Same for
 * fstat/lstat.
 *
 * _STAT_VER_LINUX is 3 on ARM EABI (gnueabi) — that's the version of
 * struct stat that both Toon's glibc 2.21 and buildroot's glibc 2.38
 * emit. The struct layout has been ABI-stable on armel since glibc 2.4.
 */
#ifdef TOON1

#include <sys/stat.h>

#ifndef _STAT_VER
#define _STAT_VER 3   /* _STAT_VER_LINUX for arm-linux-gnueabi */
#endif

extern int __xstat(int ver, const char *path, struct stat *buf);
extern int __fxstat(int ver, int fd, struct stat *buf);
extern int __lxstat(int ver, const char *path, struct stat *buf);

int __wrap_stat(const char *path, struct stat *buf)  { return __xstat(_STAT_VER, path, buf); }
int __wrap_fstat(int fd, struct stat *buf)            { return __fxstat(_STAT_VER, fd, buf); }
int __wrap_lstat(const char *path, struct stat *buf) { return __lxstat(_STAT_VER, path, buf); }

/* __libc_start_main: Scrt1.o (buildroot's CRT) emits an UNVERSIONED reference
 * to it, which the linker resolves to @@GLIBC_2.34 — not present on Toon.
 * --wrap=__libc_start_main routes the CRT's call to __wrap___libc_start_main
 * here, which forwards to the @GLIBC_2.4 version explicitly. */
__asm__(".symver __libc_start_main_v4, __libc_start_main@GLIBC_2.4");
extern int __libc_start_main_v4(int (*main)(int, char **, char **),
    int argc, char **argv,
    void (*init)(void), void (*fini)(void),
    void (*rtld_fini)(void), void *stack_end);

int __wrap___libc_start_main(int (*main)(int, char **, char **),
    int argc, char **argv,
    void (*init)(void), void (*fini)(void),
    void (*rtld_fini)(void), void *stack_end)
{
    return __libc_start_main_v4(main, argc, argv, init, fini, rtld_fini, stack_end);
}

#endif
