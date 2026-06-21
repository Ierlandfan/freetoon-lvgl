/* Downgrade fcntl from GLIBC_2.28 to GLIBC_2.4 for toon2 builds.
 * The Toon 2 runs glibc 2.21; the Debian cross-compiler resolves fcntl
 * to the 2.28 versioned alias which does not exist on device. */
#ifndef GLIBC_COMPAT_TOON2_H
#define GLIBC_COMPAT_TOON2_H
#ifdef __GLIBC__
__asm__(".symver fcntl,fcntl@GLIBC_2.4");
#endif
#endif
