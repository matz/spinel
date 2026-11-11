/* Windows: no user database. getpwuid/getpwnam answer the one user there
   is to ask about -- the current one, with %USERPROFILE% as the home -- and
   NULL for any other name; a walk (getpwent) answers that user alone. */
#ifndef SP_WIN32_PWD_H
#define SP_WIN32_PWD_H
#include "sp_win32.h"
struct passwd {
  char *pw_name, *pw_passwd;
  uid_t pw_uid; gid_t pw_gid;
  char *pw_gecos, *pw_dir, *pw_shell;
};
#ifdef __cplusplus
extern "C" {
#endif
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
void setpwent(void);
void endpwent(void);
struct passwd *getpwent(void);
#ifdef __cplusplus
}
#endif
#endif
