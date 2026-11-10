/* sp_syslog.c -- the system log for the `syslog` spin package (Path B
   carried C), linked on demand when `require "syslog"` appears.

   Four calls over syslog(3) -- openlog, closelog, syslog and setlogmask --
   and the value of each LOG_ constant of the platform, which the Ruby of
   the package reads once as it loads. The state a program sees (opened?,
   ident, options, facility, mask) lives in the Ruby; here is only what
   the C library needs: openlog(3) keeps the ident pointer it is given,
   not a copy, so the ident is copied and kept until the next open or the
   close.

   wasm32-wasi and Windows have no syslog(3): there the calls keep nothing
   and a message goes nowhere, the mask is remembered, and the constants
   are the BSD values every other platform shares. */
#include "spinel/runtime.h"
#include <stdlib.h>
#include <string.h>

#if defined(__wasi__) || defined(_WIN32)
#define SP_SYSLOG_NONE 1
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20
#define LOG_KERN (0 << 3)
#define LOG_USER (1 << 3)
#define LOG_MAIL (2 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_AUTH (4 << 3)
#define LOG_SYSLOG (5 << 3)
#define LOG_LPR (6 << 3)
#define LOG_NEWS (7 << 3)
#define LOG_UUCP (8 << 3)
#define LOG_CRON (9 << 3)
#define LOG_AUTHPRIV (10 << 3)
#define LOG_FTP (11 << 3)
#define LOG_LOCAL0 (16 << 3)
#define LOG_LOCAL1 (17 << 3)
#define LOG_LOCAL2 (18 << 3)
#define LOG_LOCAL3 (19 << 3)
#define LOG_LOCAL4 (20 << 3)
#define LOG_LOCAL5 (21 << 3)
#define LOG_LOCAL6 (22 << 3)
#define LOG_LOCAL7 (23 << 3)
static int sp_syslog_mask = 0xff;
#else
#include <syslog.h>
#endif

static char *sp_syslog_ident;   /* the copy openlog(3) keeps a pointer to */

void sp_syslog_open(const char *ident, sp_int options, sp_int facility) {
  char *copy = strdup(ident);
  if (!copy) sp_oom_die();
#ifndef SP_SYSLOG_NONE
  openlog(copy, (int)options, (int)facility);
#else
  (void)options; (void)facility;
#endif
  free(sp_syslog_ident);   /* after the switch: the library no longer reads it */
  sp_syslog_ident = copy;
}

void sp_syslog_close(void) {
#ifndef SP_SYSLOG_NONE
  closelog();
#endif
  free(sp_syslog_ident);
  sp_syslog_ident = NULL;
}

/* The message is already formatted by the Ruby: it is the whole text, so a
   percent sign in it is not a conversion. */
void sp_syslog_log(sp_int priority, const char *msg) {
#ifndef SP_SYSLOG_NONE
  syslog((int)priority, "%s", msg);
#else
  (void)priority; (void)msg;
#endif
}

/* setlogmask(3): sets the mask and answers the one before; 0 only asks. */
sp_int sp_syslog_setmask(sp_int mask) {
#ifndef SP_SYSLOG_NONE
  return (sp_int)setlogmask((int)mask);
#else
  int old = sp_syslog_mask;
  if (mask) sp_syslog_mask = (int)mask;
  return (sp_int)old;
#endif
}

/* The platform's value of a LOG_ constant, -1 for a name that is none. */
sp_int sp_syslog_const(const char *name) {
  static const struct { const char *name; int value; } k[] = {
    { "LOG_EMERG", LOG_EMERG }, { "LOG_ALERT", LOG_ALERT }, { "LOG_CRIT", LOG_CRIT }, { "LOG_ERR", LOG_ERR },
    { "LOG_WARNING", LOG_WARNING }, { "LOG_NOTICE", LOG_NOTICE }, { "LOG_INFO", LOG_INFO }, { "LOG_DEBUG", LOG_DEBUG },
    { "LOG_PID", LOG_PID }, { "LOG_CONS", LOG_CONS }, { "LOG_ODELAY", LOG_ODELAY }, { "LOG_NDELAY", LOG_NDELAY },
    { "LOG_NOWAIT", LOG_NOWAIT }, { "LOG_PERROR", LOG_PERROR },
    { "LOG_KERN", LOG_KERN }, { "LOG_USER", LOG_USER }, { "LOG_MAIL", LOG_MAIL }, { "LOG_DAEMON", LOG_DAEMON },
    { "LOG_AUTH", LOG_AUTH }, { "LOG_SYSLOG", LOG_SYSLOG }, { "LOG_LPR", LOG_LPR }, { "LOG_NEWS", LOG_NEWS },
    { "LOG_UUCP", LOG_UUCP }, { "LOG_CRON", LOG_CRON }, { "LOG_AUTHPRIV", LOG_AUTHPRIV }, { "LOG_FTP", LOG_FTP },
    { "LOG_LOCAL0", LOG_LOCAL0 }, { "LOG_LOCAL1", LOG_LOCAL1 }, { "LOG_LOCAL2", LOG_LOCAL2 }, { "LOG_LOCAL3", LOG_LOCAL3 },
    { "LOG_LOCAL4", LOG_LOCAL4 }, { "LOG_LOCAL5", LOG_LOCAL5 }, { "LOG_LOCAL6", LOG_LOCAL6 }, { "LOG_LOCAL7", LOG_LOCAL7 },
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(k[i].name, name)) return (sp_int)k[i].value;
  return -1;
}
