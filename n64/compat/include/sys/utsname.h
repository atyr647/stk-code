/* POSIX uname() for the N64 (n64/compat/src/posix_extra.c) */
#ifndef STK_N64_SYS_UTSNAME_H
#define STK_N64_SYS_UTSNAME_H
#ifdef __cplusplus
extern "C" {
#endif
struct utsname { char sysname[65], nodename[65], release[65], version[65],
                 machine[65]; };
int uname(struct utsname *buf);
#ifdef __cplusplus
}
#endif
#endif
