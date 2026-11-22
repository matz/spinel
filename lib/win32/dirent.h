/* Windows: <dirent.h> over FindFirstFileExW, with UTF-8 names and d_type
   (MinGW's answers names in the ANSI code page and has no d_type). dirfd
   and fdopendir answer ENOTSUP: a directory listing has no descriptor. */
#ifndef SP_WIN32_DIRENT_H
#define SP_WIN32_DIRENT_H
#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
  unsigned long long d_ino;
  unsigned short d_reclen;
  unsigned short d_namlen;
  unsigned char d_type;
  char d_name[260 * 3];
};

/* MinGW's <limits.h> defines NAME_MAX only under _POSIX_. A file name's
   longest UTF-8 spelling is what d_name holds, less its terminator. */
#ifndef NAME_MAX
#define NAME_MAX (260 * 3 - 1)
#endif
typedef struct sp_w32_dir DIR;

#ifdef __cplusplus
extern "C" {
#endif
DIR *sp_w32_opendir(const char *path);
DIR *sp_w32_fdopendir(int fd);
struct dirent *sp_w32_readdir(DIR *d);
int sp_w32_closedir(DIR *d);
void sp_w32_rewinddir(DIR *d);
long sp_w32_telldir(DIR *d);
void sp_w32_seekdir(DIR *d, long pos);
int sp_w32_dirfd(DIR *d);
#define opendir(p)    sp_w32_opendir(p)
#define fdopendir(fd) sp_w32_fdopendir(fd)
#define readdir(d)    sp_w32_readdir(d)
#define closedir(d)   sp_w32_closedir(d)
#define rewinddir(d)  sp_w32_rewinddir(d)
#define telldir(d)    sp_w32_telldir(d)
#define seekdir(d, p) sp_w32_seekdir((d), (p))
#define dirfd(d)      sp_w32_dirfd(d)
#ifdef __cplusplus
}
#endif
#endif
