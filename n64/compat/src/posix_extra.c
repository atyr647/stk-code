/* POSIX pieces newlib/libdragon do not provide on the N64. */
#include <dirent.h>
#include <dir.h>
#include <errno.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <n64sys.h>      /* get_memory_size */
#include <sys/stat.h>

struct __stk_n64_dir {
    char *path;
    dir_t d;
    int state;          /* 0 = not started, 1 = have entry, 2 = finished */
    struct dirent out;
};

DIR *opendir(const char *name)
{
    DIR *d = calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->path = strdup(name);
    if (dir_findfirst(d->path, &d->d) < 0) {
        /* an empty directory and a missing one look alike to findfirst;
         * stat tells them apart */
        struct stat st;
        if (stat(name, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(d->path); free(d);
            errno = ENOENT;
            return NULL;
        }
        d->state = 2;
    } else {
        d->state = 1;
    }
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->state != 1) return NULL;
    strncpy(d->out.d_name, d->d.d_name, sizeof d->out.d_name - 1);
    d->out.d_name[sizeof d->out.d_name - 1] = 0;
    d->out.d_type = d->d.d_type == DT_DIR ? DT_DIR : DT_REG;
    d->out.d_ino = 1;
    if (dir_findnext(d->path, &d->d) < 0) d->state = 2;
    return &d->out;
}

void rewinddir(DIR *d)
{
    d->state = dir_findfirst(d->path, &d->d) < 0 ? 2 : 1;
}

int closedir(DIR *d)
{
    free(d->path);
    free(d);
    return 0;
}

/* The process has one working directory, which starts at the cartridge. */
static char cwd[PATH_MAX] = "rom:/";

char *getcwd(char *buf, size_t size)
{
    if (!buf) { size = strlen(cwd) + 1; buf = malloc(size); }
    if (strlen(cwd) + 1 > size) { errno = ERANGE; return NULL; }
    strcpy(buf, cwd);
    return buf;
}

int chdir(const char *path)
{
    if (strlen(path) + 1 > sizeof cwd) { errno = ENAMETOOLONG; return -1; }
    strcpy(cwd, path);
    return 0;
}

#include <sys/utsname.h>
int uname(struct utsname *u)
{
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "libdragon");
    strcpy(u->nodename, "n64");
    strcpy(u->release, "preview");
    strcpy(u->version, "1");
    strcpy(u->machine, "mips64-vr4300");
    return 0;
}

#include <err.h>
#include <stdarg.h>
#include <stdio.h>
static void vwarn_impl(int with_errno, const char *fmt, va_list ap)
{
    int e = errno;
    fputs("supertuxkart: ", stderr);
    if (fmt) vfprintf(stderr, fmt, ap);
    if (with_errno) fprintf(stderr, ": %s", strerror(e));
    fputc('\n', stderr);
}
void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn_impl(1, fmt, ap); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn_impl(0, fmt, ap); va_end(ap); }
void err(int eval, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn_impl(1, fmt, ap); va_end(ap); exit(eval); }
void errx(int eval, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn_impl(0, fmt, ap); va_end(ap); exit(eval); }

#include <fcntl.h>
#include <stdarg.h>
/* File-descriptor flags: the only fds STK asks about are sockets (which never
 * open on the N64) and files, where O_NONBLOCK is meaningless. */
int fcntl(int fd, int cmd, ...)
{
    (void)fd;
    switch (cmd) {
    case F_GETFL: case F_GETFD: return 0;
    case F_SETFL: case F_SETFD: return 0;
    default: errno = EINVAL; return -1;
    }
}

pid_t getppid(void) { return 1; }

int access(const char *path, int mode)
{
    struct stat st;
    if (stat(path, &st) != 0) { errno = ENOENT; return -1; }
    if ((mode & W_OK) && !strncmp(path, "rom:/", 5)) { errno = EROFS; return -1; }
    return 0;
}

/* realpath: collapse "." / ".." / "//" while keeping the "fs:/" prefix, and
 * require the result to exist (POSIX semantics). */
char *realpath(const char *path, char *resolved)
{
    const char *colon = strstr(path, ":/");
    char prefix[16] = "";
    const char *rest = path;
    if (colon && colon - path < (int)sizeof prefix - 2) {
        memcpy(prefix, path, colon - path + 2);
        prefix[colon - path + 2] = 0;
        rest = colon + 2;
    }
    char tmp[PATH_MAX];
    char *o = tmp;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *e = strchr(rest, '/');
        size_t len = e ? (size_t)(e - rest) : strlen(rest);
        if (len == 1 && rest[0] == '.') { rest += len; continue; }
        if (len == 2 && rest[0] == '.' && rest[1] == '.') {
            while (o > tmp && o[-1] != '/') o--;
            if (o > tmp) o--;
            rest += len; continue;
        }
        if ((size_t)(o - tmp) + len + 2 >= sizeof tmp) { errno = ENAMETOOLONG; return NULL; }
        if (o > tmp) *o++ = '/';
        memcpy(o, rest, len); o += len; rest += len;
    }
    *o = 0;
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s", prefix, tmp);
    struct stat st;
    if (stat(full, &st) != 0) { errno = ENOENT; return NULL; }
    if (!resolved) return strdup(full);
    strcpy(resolved, full);
    return resolved;
}

/* sysconf: STK's hardware statistics ask for the memory size and the CPU
 * count. newlib has no sysconf on bare metal. */
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:          return 4096;
    case _SC_PHYS_PAGES:        return (long)(get_memory_size() / 4096);
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:  return 1;
    default:                    errno = EINVAL; return -1;
    }
}
