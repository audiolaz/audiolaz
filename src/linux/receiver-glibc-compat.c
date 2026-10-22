/* src/linux/receiver-glibc-compat.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#if defined(__x86_64__)
#define AZ_GLIBC_BASE "GLIBC_2.2.5"
#elif defined(__aarch64__)
#define AZ_GLIBC_BASE "GLIBC_2.17"
#else
#define AZ_GLIBC_BASE "GLIBC_2.4"
#endif
__asm__(".symver az_strtol_old,strtol@" AZ_GLIBC_BASE);
__asm__(".symver az_strtoul_old,strtoul@" AZ_GLIBC_BASE);
long az_strtol_old(const char *, char **, int);
unsigned long az_strtoul_old(const char *, char **, int);
long __isoc23_strtol(const char *s, char **e, int b) { return az_strtol_old(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return az_strtoul_old(s, e, b); }
/* strtoull too (rt_watch.h: a thread's schedstat, nanoseconds: 64 bits on a 32-bit ARM as well) */
__asm__(".symver az_strtoull_old,strtoull@" AZ_GLIBC_BASE);
unsigned long long az_strtoull_old(const char *, char **, int);
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return az_strtoull_old(s, e, b); }
