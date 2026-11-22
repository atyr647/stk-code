/* BSD <err.h> for the N64 (n64/compat/src/posix_extra.c) */
#ifndef STK_N64_ERR_H
#define STK_N64_ERR_H
#ifdef __cplusplus
extern "C" {
#endif
void err(int eval, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void errx(int eval, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warnx(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#endif
