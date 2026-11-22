/* ram:/ -- a small writable filesystem in RDRAM for the N64.
 *
 * STK writes its configuration, player profiles, highscores and log under
 * the user's config/data directories and expects to read them back during the
 * same run. The cartridge (rom:/, DragonFS) is read-only, so those
 * directories point here (see n64_main.cpp). Contents last until power-off;
 * persisting the few files that matter to EEPROM/SRAM is a separate step.
 *
 * Plugged in through libdragon's attach_filesystem(), so ordinary
 * open/read/write/stat/mkdir/dir_findfirst calls work on "ram:/..." paths.
 */
#include <system.h>
#include <dir.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct node {
    char *path;              /* normalised: "a/b/c", "" is the root */
    int is_dir;
    unsigned char *data;
    int size, cap;
    struct node *next;
} node_t;

typedef struct { node_t *n; int pos; int append; } handle_t;

static node_t *nodes;

static char *normalise(const char *p)
{
    char *out = malloc(strlen(p) + 1), *o = out;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len == 1 && p[0] == '.') { p += len; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            /* drop the last component */
            while (o > out && o[-1] != '/') o--;
            if (o > out) o--;
            p += len; continue;
        }
        if (o > out) *o++ = '/';
        memcpy(o, p, len); o += len; p += len;
    }
    *o = 0;
    return out;
}

static node_t *find(const char *norm)
{
    for (node_t *n = nodes; n; n = n->next)
        if (!strcmp(n->path, norm)) return n;
    return NULL;
}

static int parent_exists(const char *norm)
{
    const char *slash = strrchr(norm, '/');
    if (!slash) return 1;                           /* parent is root */
    char *parent = strndup(norm, slash - norm);
    node_t *p = find(parent);
    free(parent);
    return p && p->is_dir;
}

static node_t *add(const char *norm, int is_dir)
{
    node_t *n = calloc(1, sizeof *n);
    n->path = strdup(norm);
    n->is_dir = is_dir;
    n->next = nodes;
    nodes = n;
    return n;
}

static void *ram_open(char *name, int flags)
{
    char *norm = normalise(name);
    node_t *n = find(norm);
    if (n && n->is_dir) { free(norm); errno = EISDIR; return NULL; }
    if (!n) {
        if (!(flags & O_CREAT)) { free(norm); errno = ENOENT; return NULL; }
        if (!parent_exists(norm)) { free(norm); errno = ENOENT; return NULL; }
        n = add(norm, 0);
    }
    free(norm);
    if (flags & O_TRUNC) n->size = 0;
    handle_t *h = calloc(1, sizeof *h);
    h->n = n;
    h->append = (flags & O_APPEND) != 0;
    return h;
}

static int ram_read(void *f, uint8_t *ptr, int len)
{
    handle_t *h = f;
    int left = h->n->size - h->pos;
    if (len > left) len = left;
    if (len <= 0) return 0;
    memcpy(ptr, h->n->data + h->pos, len);
    h->pos += len;
    return len;
}

static int ram_write(void *f, uint8_t *ptr, int len)
{
    handle_t *h = f;
    node_t *n = h->n;
    if (h->append) h->pos = n->size;
    int end = h->pos + len;
    if (end > n->cap) {
        int cap = n->cap ? n->cap : 256;
        while (cap < end) cap *= 2;
        unsigned char *d = realloc(n->data, cap);
        if (!d) { errno = ENOSPC; return -1; }
        n->data = d; n->cap = cap;
    }
    if (h->pos > n->size) memset(n->data + n->size, 0, h->pos - n->size);
    memcpy(n->data + h->pos, ptr, len);
    h->pos = end;
    if (end > n->size) n->size = end;
    return len;
}

static int ram_lseek(void *f, int off, int whence)
{
    handle_t *h = f;
    int base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? h->pos : h->n->size;
    if (base + off < 0) { errno = EINVAL; return -1; }
    h->pos = base + off;
    return h->pos;
}

static int ram_close(void *f) { free(f); return 0; }

static void fill_stat(node_t *n, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = n->is_dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = n->is_dir ? 0 : n->size;
}

static int ram_fstat(void *f, struct stat *st)
{ fill_stat(((handle_t *)f)->n, st); return 0; }

static int ram_stat(char *name, struct stat *st)
{
    char *norm = normalise(name);
    node_t *n = find(norm);
    int root = !*norm;
    free(norm);
    if (root) { memset(st, 0, sizeof *st); st->st_mode = S_IFDIR | 0777; return 0; }
    if (!n) { errno = ENOENT; return -1; }
    fill_stat(n, st);
    return 0;
}

static int ram_unlink(char *name)
{
    char *norm = normalise(name);
    for (node_t **pp = &nodes; *pp; pp = &(*pp)->next) {
        node_t *n = *pp;
        if (!strcmp(n->path, norm)) {
            *pp = n->next;
            free(n->path); free(n->data); free(n); free(norm);
            return 0;
        }
    }
    free(norm);
    errno = ENOENT;
    return -1;
}

static int ram_mkdir(char *path, mode_t mode)
{
    (void)mode;
    char *norm = normalise(path);
    if (!*norm || find(norm)) { free(norm); errno = EEXIST; return -1; }
    if (!parent_exists(norm)) { free(norm); errno = ENOENT; return -1; }
    add(norm, 1);
    free(norm);
    return 0;
}

static int ram_ftruncate(void *f, int length)
{
    handle_t *h = f;
    if (length < h->n->size) h->n->size = length;
    return 0;
}

/* Directory walking: d_cookie is the index of the next child to report. */
static int child_at(const char *dir, int index, dir_t *out)
{
    size_t dl = strlen(dir);
    int i = 0;
    for (node_t *n = nodes; n; n = n->next) {
        const char *rest;
        if (dl == 0) rest = n->path;
        else if (!strncmp(n->path, dir, dl) && n->path[dl] == '/') rest = n->path + dl + 1;
        else continue;
        if (!*rest || strchr(rest, '/')) continue;       /* direct children only */
        if (i++ == index) {
            strncpy(out->d_name, rest, sizeof out->d_name - 1);
            out->d_name[sizeof out->d_name - 1] = 0;
            out->d_type = n->is_dir ? DT_DIR : DT_REG;
            out->d_size = n->is_dir ? 0 : n->size;
            out->d_cookie = index + 1;
            return 0;
        }
    }
    return -1;
}

static int ram_findnext2(const char *path, dir_t *dir)
{
    char *norm = normalise(path);
    int r = child_at(norm, dir->d_cookie, dir);
    free(norm);
    return r;
}

static int ram_findfirst(char *path, dir_t *dir)
{
    dir->d_cookie = 0;
    return ram_findnext2(path, dir);
}

static filesystem_t ram_fs = {
    .open = ram_open, .fstat = ram_fstat, .stat = ram_stat,
    .lseek = ram_lseek, .read = ram_read, .write = ram_write,
    .close = ram_close, .unlink = ram_unlink, .findfirst = ram_findfirst,
    .findnext2 = ram_findnext2, .ftruncate = ram_ftruncate,
    .mkdir = ram_mkdir,
};

void stk_n64_ramfs_init(void)
{
    attach_filesystem("ram:/", &ram_fs);
}
