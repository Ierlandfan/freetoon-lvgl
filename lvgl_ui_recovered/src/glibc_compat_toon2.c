/*
 * Downgrade fcntl@GLIBC_2.28 → fcntl@GLIBC_2.4 for Toon 2 builds.
 *
 * The Debian arm-linux-gnueabihf cross-compiler (glibc 2.31 headers) generates
 * a dynamic reference to fcntl@GLIBC_2.28 which does not exist on the Toon 2's
 * glibc 2.21. The .symver trick in glibc_compat_toon2.h is silently ignored when
 * injected via -include (a known GCC limitation); this explicit .c file compiles
 * the .symver directive in-line in the TU that actually references the symbol, which
 * is the only form that reliably takes effect.
 *
 * Build: add glibc_compat_toon2.c to APP_SRC and -Wl,--wrap=fcntl to LDFLAGS.
 * The linker redirects every fcntl() call site to __wrap_fcntl (defined here),
 * which in turn calls the GLIBC_2.4-versioned entry point.
 */

#include <features.h>
#include <stdarg.h>

/* Pin the extern reference to the old glibc 2.4 entry point. This directive
 * is in-line in the TU that references the symbol, so GAS reliably emits the
 * versioned reference into the object's dynamic-symbol table. */
__asm__(".symver __toon2_fcntl_old, fcntl@GLIBC_2.4");
extern int __toon2_fcntl_old(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return __toon2_fcntl_old(fd, cmd, arg);
}
