/* Windows: no group database; getgrgid/getgrnam and getgrent answer NULL. */
#ifndef SP_WIN32_GRP_H
#define SP_WIN32_GRP_H
#include "sp_win32.h"
struct group { char *gr_name, *gr_passwd; gid_t gr_gid; char **gr_mem; };
#ifdef __cplusplus
extern "C" {
#endif
struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
void setgrent(void);
void endgrent(void);
struct group *getgrent(void);
#ifdef __cplusplus
}
#endif
#endif
