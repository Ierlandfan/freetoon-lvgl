/*
 * Local implementations of libc symbols the Toon firmware glibc (2.21) does not
 * export, even though the build toolchains do. Reached via -Wl,--wrap=<sym>: the
 * linker redirects every call site — including compiler-generated ones — to
 * __wrap_<sym> here, so no external @GLIBC_2.4 reference to them remains in the
 * binary.
 *
 * Why this exists: v0.9.56 crash-looped to qt-gui on Toon 1 with
 *   relocation error: symbol , version GLIBC_2.4 not defined in file libc.so.6
 * because it newly imported strcasecmp, fputc and pthread_attr_destroy@GLIBC_2.4
 * that the device's stripped firmware glibc lacks — the same class as the
 * getifaddrs/freeifaddrs breakage in v0.9.47. Proven empirically: the v0.9.56
 * Toon 1 binary's imports were v0.9.54's set plus exactly these three; v0.9.54
 * loads on-device, v0.9.56 did not. abi-baseline-toon{1,2}.txt now fails the
 * build if any new on-device dependency sneaks back in.
 *
 * fputc came from efanlamp.c's `fprintf(stderr, "\n")`, which GCC folds to
 * fputc('\n', stderr); strcasecmp from the HA client + vent screen.
 *
 * MUST be compiled with -fno-builtin (the Makefile does): at -O2 GCC folds
 * fwrite(&c,1,1,f) back into fputc — re-emitting the very symbol we remove, an
 * infinite loop. Applies to both Toon 1 (gnueabi) and Toon 2 (gnueabihf); the
 * pure-C impls behave identically to libc on any glibc, so wrapping the Toon 2
 * build too costs nothing and closes the same latent gap there.
 *
 * No target #ifdef: this TU is only ever compiled for the device builds (it is
 * in GLIBC_SRC, which is empty for the sim). Guarding it on TOON1/TOON2 would
 * risk compiling to nothing on a define mismatch, leaving __wrap_* undefined and
 * breaking the link.
 */
#include <stdio.h>
#include <pthread.h>

int __wrap_strcasecmp(const char *a, const char *b)
{
    unsigned char ca, cb;
    do {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    } while (ca && ca == cb);
    return (int)ca - (int)cb;
}

int __wrap_fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? (int)ch : EOF;
}

/* We only ever use default pthread attributes (never a cpuset), for which a
 * glibc pthread_attr_t owns no heap — so destroy is genuinely a no-op here, the
 * same as glibc's own implementation for that case. */
int __wrap_pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}
