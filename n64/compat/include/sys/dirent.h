/* POSIX directory listing for the N64, on libdragon's dir_findfirst/next,
 * which works for every attached filesystem (rom:/ DragonFS, ram:/).
 * newlib's own <dirent.h> is an #error on this target. */
#ifndef STK_N64_SYS_DIRENT_H
#define STK_N64_SYS_DIRENT_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_REG
#define DT_REG 1
#endif
#ifndef DT_DIR
#define DT_DIR 2
#endif
struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct __stk_n64_dir DIR;
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);
#ifdef __cplusplus
}
#endif
#endif
