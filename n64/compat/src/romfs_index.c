/* In-RAM index of the cartridge filesystem, answering stat() for rom:/.
 *
 * DragonFS keeps each directory as a linked list of 256-byte entries in ROM,
 * and finds a path by reading them one at a time: every stat() is one PI DMA
 * per entry passed over. STK and Irrlicht call stat() constantly (existFile,
 * getAbsolutePath, a texture search for every material), and directories
 * such as data/textures hold hundreds of entries: measured in ares, startup
 * spent 90% of its time in these reads.
 *
 * At boot the whole tree is walked once (one DMA per entry) into a sorted
 * table of 64-bit path hashes with each entry's type and size: 8 bytes of
 * hash and 4 of data per entry. stat() on a rom:/ path is then answered from
 * RAM by replaying DragonFS's own path walk (get_next_token and recurse_path
 * in dragonfs.c, ported line for line), quirks included:
 *   - "." and "/" tokens are skipped, ".." pops one level (and stays at the
 *     root at the top); a single trailing "/" belongs to the name before it;
 *   - a directory that is not the last component ("dir/.", "dir//") leaves
 *     the walk on its *first entry*, so stat("rom:/data/.") reports the first
 *     entry of data/, as DragonFS does;
 *   - nothing may follow a file.
 * The one case DragonFS itself leaves undefined (the first entry of an empty
 * directory) goes to the real stat(). Linked with -Wl,--wrap=stat.
 *
 * A false "exists" needs a 64-bit hash collision between a missing path and
 * one of the ~3,000 present: about 1 in 10^15 per lookup.
 *
 * -DSTK_N64_VERIFY_ROMFS_INDEX compares every answer with the real stat()
 * and logs any difference. */
/* debugf() is compiled out under NDEBUG; this file's messages are wanted */
#undef NDEBUG
#include <libdragon.h>
#include <dir.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int __real_stat(const char *path, struct stat *st);

#define INFO_DIR       0x80000000u
#define INFO_VALUE     0x7fffffffu   /* file: size; directory: index in g_dirs */
#define DIR_EMPTY      0xffffffffu

typedef struct __attribute__((packed)) { uint64_t hash; uint32_t info; } entry_t;

static entry_t  *g_entries;
static uint32_t  g_count, g_capacity;
/* per directory: the info of its first entry (DIR_EMPTY if none) */
static uint32_t *g_dirs;
static uint32_t  g_dir_count, g_dir_capacity;
static int       g_ready;

#define FNV_OFFSET 0xcbf29ce484222325ull
#define FNV_PRIME  0x100000001b3ull

static uint64_t fnv(uint64_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        h ^= (uint8_t)s[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* hash of parent/name: the root's hash is FNV_OFFSET */
static uint64_t child_hash(uint64_t parent, const char *name, size_t n)
{
    return fnv(fnv(parent, "/", 1), name, n);
}

static int grow(void **p, uint32_t *capacity, uint32_t need, size_t size)
{
    if (need <= *capacity)
        return 1;
    uint32_t c = *capacity ? *capacity * 2 : 256;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, c * size);
    if (!q)
        return 0;
    *p = q;
    *capacity = c;
    return 1;
}

static uint32_t new_dir(void)
{
    if (!grow((void**)&g_dirs, &g_dir_capacity, g_dir_count + 1,
              sizeof(uint32_t)))
        return DIR_EMPTY;
    g_dirs[g_dir_count] = DIR_EMPTY;
    return g_dir_count++;
}

/* Walks the directory at path (hash h), whose g_dirs slot is dir; path[0,len)
 * is its name without a trailing slash ("rom:" for the root, "rom:/" being
 * what dir_findfirst needs there). The walk position lives in the dir_t, so
 * subdirectories are walked as they come. */
static int walk(const char *path, size_t len, uint64_t h, uint32_t dir)
{
    dir_t d;
    int first = 1;
    if (dir_findfirst(path, &d) != 0)
        return 1;                            /* empty directory */
    do
    {
        const size_t n = strlen(d.d_name);
        if (!grow((void**)&g_entries, &g_capacity, g_count + 1,
                  sizeof(entry_t)))
            return 0;
        const uint64_t eh = child_hash(h, d.d_name, n);
        uint32_t info;
        uint32_t sub = DIR_EMPTY;
        if (d.d_type == DT_DIR)
        {
            sub = new_dir();
            if (sub == DIR_EMPTY)
                return 0;
            info = INFO_DIR | sub;
        }
        else
        {
            /* d_size: libdragon-dfs-dir-size.patch */
            if (d.d_size < 0 || (uint64_t)d.d_size > INFO_VALUE)
                return 0;
            info = (uint32_t)d.d_size;
        }
        g_entries[g_count].hash = eh;
        g_entries[g_count].info = info;
        g_count++;
        if (first)
        {
            g_dirs[dir] = info;
            first = 0;
        }
        if (sub != DIR_EMPTY)
        {
            char sub_path[512];
            if (len + 1 + n + 1 > sizeof(sub_path))
                return 0;
            memcpy(sub_path, path, len);
            sub_path[len] = '/';
            memcpy(sub_path + len + 1, d.d_name, n + 1);
            if (!walk(sub_path, len + 1 + n, eh, sub))
                return 0;
        }
    } while (dir_findnext(path, &d) == 0);
    return 1;
}

static int by_hash(const void *a, const void *b)
{
    const uint64_t x = ((const entry_t*)a)->hash, y = ((const entry_t*)b)->hash;
    return x < y ? -1 : x > y;
}

void stk_n64_romfs_index_init(void)
{
    const uint32_t t0 = TICKS_READ();
    const uint32_t root_dir = new_dir();
    if (root_dir == DIR_EMPTY || !walk("rom:/", 4, FNV_OFFSET, root_dir))
    {
        debugf("[n64] romfs index: out of memory, using DragonFS lookups\n");
        free(g_entries);
        free(g_dirs);
        g_entries = NULL;
        g_dirs = NULL;
        return;
    }
    qsort(g_entries, g_count, sizeof(entry_t), by_hash);
    for (uint32_t i = 1; i < g_count; i++)
        if (g_entries[i].hash == g_entries[i - 1].hash)
        {
            debugf("[n64] romfs index: hash collision, using DragonFS "
                   "lookups\n");
            return;
        }
    g_entries = realloc(g_entries, g_count * sizeof(entry_t));
    g_dirs = realloc(g_dirs, g_dir_count * sizeof(uint32_t));
    g_ready = 1;
    debugf("[n64] romfs index: %lu entries, %lu directories, %lu KB, "
           "%lu ms\n", (unsigned long)g_count, (unsigned long)g_dir_count,
           (unsigned long)(g_count * sizeof(entry_t) +
                           g_dir_count * sizeof(uint32_t) + 1023) / 1024,
           (unsigned long)TICKS_TO_MS(TICKS_READ() - t0));
}

static const entry_t* find(uint64_t h)
{
    uint32_t lo = 0, hi = g_count;
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (g_entries[mid].hash < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g_count && g_entries[lo].hash == h ? &g_entries[lo] : NULL;
}

/* Where the DragonFS walk stands (an entry of its directory stack): on the
 * first entry of a directory (a place to look names up from), or on an entry
 * found as the last component. */
typedef struct
{
    uint64_t hash;        /* first-entry place: the directory's path hash */
    uint32_t info;        /* what stat reports here */
    int      first_entry;
} place_t;

enum { FOUND, MISSING, UNDEFINED };

#define MAX_FILENAME_LEN    243     /* dragonfs.h */
#define MAX_DIRECTORY_DEPTH 100

/* get_next_token() from dragonfs.c, verbatim in behaviour: "/" is a token of
 * its own; a name takes one following "/" with it when the path ends there. */
static const char *next_token(const char *path, char *token)
{
    if (!path)
        return 0;
    if (path[0] != 0)
    {
        if (path[0] == '/')
        {
            token[0] = '/';
            token[1] = 0;
            if (path[1] == 0)
                return 0;
            return path + 1;
        }
        int copied = 0;
        while (path[0] != '/')
        {
            if (path[0] == 0)
            {
                token[0] = 0;
                return 0;
            }
            if (copied < MAX_FILENAME_LEN)
            {
                token[0] = path[0];
                token++;
                copied++;
            }
            path++;
        }
        token[0] = 0;
        if (path[1] == 0)
            return 0;
        return path + 1;
    }
    /* empty string: dragonfs.c leaves the token as it was */
    return 0;
}

/* recurse_path(path, WALK_STAT, ..., TYPE_ANY) from dragonfs.c, with the
 * index in place of find_dirent(). path is what DragonFS's stat receives
 * ("/data/..."). The directory stack starts empty: nothing calls dfs_chdir. */
static int lookup(const char *path, uint32_t *info)
{
    place_t stack[MAX_DIRECTORY_DEPTH];
    int top = 0;
    const place_t root = { FNV_OFFSET, g_dirs[0], 1 };
    char token[MAX_FILENAME_LEN + 1];
    token[0] = 0;
    int ignore = 1;
    const char *cur_path = next_token(path, token);
    if (strcmp(token, "/") == 0)
    {
        top = 0;                        /* clear_directory() */
        ignore = 0;
    }
    while (cur_path || ignore)
    {
        if (!ignore)
            cur_path = next_token(cur_path, token);
        ignore = 0;
        if (strcmp(token, "/") == 0 || strcmp(token, ".") == 0)
            continue;
        if (strcmp(token, "..") == 0)
        {
            if (top > 0)                /* pop_directory() */
                top--;
            continue;
        }
        const place_t *cur = top ? &stack[top - 1] : &root;
        if (!cur->first_entry)
            return UNDEFINED;           /* searching from a found entry */
        if (cur->info == DIR_EMPTY)
            return MISSING;             /* find_dirent(token, 0) */
        const uint64_t h = child_hash(cur->hash, token, strlen(token));
        const entry_t *e = find(h);
        if (!e)
            return MISSING;
        if (top == MAX_DIRECTORY_DEPTH)
            return UNDEFINED;
        place_t next;
        next.hash = h;
        if (e->info & INFO_DIR)
        {
            if (!cur_path)
            {
                next.info = e->info;    /* the directory's own entry */
                next.first_entry = 0;
            }
            else
            {
                next.info = g_dirs[e->info & INFO_VALUE];
                next.first_entry = 1;   /* walk into it */
            }
        }
        else
        {
            if (cur_path)
                return MISSING;         /* something follows a file */
            next.info = e->info;
            next.first_entry = 0;
        }
        stack[top++] = next;
    }
    const place_t *cur = top ? &stack[top - 1] : &root;
    if (cur->info == DIR_EMPTY)
        return UNDEFINED;               /* stat of an empty directory's first
                                           entry: whatever DragonFS reads */
    *info = cur->info;
    return FOUND;
}

static int index_stat(const char *file, struct stat *st, int *handled)
{
    *handled = 0;
    if (!g_ready || !st || strncmp(file, "rom:/", 5) != 0)
        return 0;
    uint32_t info;
    switch (lookup(file + 4, &info))
    {
    case UNDEFINED:
        return 0;
    case MISSING:
        *handled = 1;
        errno = ENOENT;
        return -1;
    default:
        *handled = 1;
        memset(st, 0, sizeof(*st));
        st->st_nlink = 1;
        if (info & INFO_DIR)
        {
            st->st_mode = S_IFDIR;
            st->st_size = 0;
        }
        else
        {
            st->st_mode = S_IFREG;
            st->st_size = (int)info;
        }
        return 0;
    }
}

int __wrap_stat(const char *file, struct stat *st)
{
    int handled;
    const int ret = index_stat(file, st, &handled);
#ifdef STK_N64_VERIFY_ROMFS_INDEX
    if (handled)
    {
        const int my_errno = errno;
        struct stat real;
        const int real_ret = __real_stat(file, &real);
        if (real_ret != ret || (ret == 0 &&
            (real.st_mode != st->st_mode || real.st_size != st->st_size)) ||
            (ret != 0 && errno != my_errno))
        {
            debugf("[n64] romfs index MISMATCH for '%s': index %d mode %lx "
                   "size %ld, DragonFS %d mode %lx size %ld\n", file, ret,
                   (unsigned long)st->st_mode, (long)st->st_size, real_ret,
                   (unsigned long)real.st_mode, (long)real.st_size);
        }
        static unsigned checked;
        if (++checked % 2000 == 0)
            debugf("[n64] romfs index: %u lookups verified\n", checked);
        errno = my_errno;
    }
#endif
    if (handled)
        return ret;
    return __real_stat(file, st);
}
