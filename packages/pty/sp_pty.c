/* sp_pty.c -- PTY.spawn for the `pty` spin package (Path B carried C),
   linked on demand when `require "pty"` appears.

   A child on a pseudo-terminal: the master is opened here
   (posix_openpt/grantpt/unlockpt, the portable calls rather than glibc's
   openpty), and the child is made a session leader with the slave as its
   controlling terminal and as stdin, stdout and stderr, then exec'd. This
   is what CRuby's ext/pty does; the Ruby side (pty.rb) wraps the
   descriptors as IOs.

   THE CHILD DOES NOTHING THAT ALLOCATES. The parent may be a threaded
   program, and after fork only async-signal-safe calls are sound in the
   child: the argument vector, the environment and the program's path (the
   PATH search) are all settled here, in the parent, before the fork, and
   the child only moves descriptors and calls execve, as CRuby's
   rb_exec_async_signal_safe does. A file execve cannot run (ENOEXEC) runs
   through /bin/sh, as CRuby and execvp do.

   THE CHILD STARTS WITH NO BLOCKED SIGNALS AND SIGPIPE AT ITS DEFAULT,
   whatever the forking thread had: a mask survives exec, and a shell on the
   terminal that inherited a blocked SIGINT would never see the ^C its line
   discipline sends. CRuby's children start this way too.

   Exec failure is reported through a close-on-exec pipe, as lib/sp_process.c
   does, so PTY.spawn raises Errno::ENOENT for a program that does not exist
   at the call, instead of returning the pid of a child that exits 127.

   OWNERSHIP: a descriptor handed to a function here is that function's to
   close if it raises (sp_pty_dup closes the master, sp_pty_spawn_child the
   master and the writer), so a failed spawn leaves nothing open. Every
   descriptor of ours sits above 2: a program that closed stdin would
   otherwise get the master, or the error pipe, as 0, and the child's dup2
   onto 0..2 would replace it.

   Every symbol carries the full package prefix (see packages/zlib/sp_zlib.c
   for why the reserved-identifier list makes that matter). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* posix_openpt, ptsname_r on glibc */
#endif
#include "spinel/runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* "Errno::ENOENT" and friends for the errnos a spawn meets; the parent
   otherwise, as lib/sp_process.c raises them. */
static const char *sp_pty_errno_class(int e) {
  switch (e) {
    case ENOENT: return "Errno::ENOENT";
    case EACCES: return "Errno::EACCES";
    case ENOEXEC: return "Errno::ENOEXEC";
    case EMFILE: return "Errno::EMFILE";
    case ENFILE: return "Errno::ENFILE";
    case EAGAIN: return "Errno::EAGAIN";
    case ENOMEM: return "Errno::ENOMEM";
    default: return "SystemCallError";
  }
}

/* Close the descriptors a failing call owns (-1 for none). */
static void sp_pty_close2(int a, int b) {
  if (a >= 0) close(a);
  if (b >= 0) close(b);
}

/* CRuby's message: the strerror text, then " - " and what it was about. */
static SP_NORETURN void sp_pty_raise_errno(int e, const char *what) {
  char msg[512];
  if (what) snprintf(msg, sizeof msg, "%s - %s", strerror(e), what);
  else snprintf(msg, sizeof msg, "%s", strerror(e));
  sp_raise_cls(sp_pty_errno_class(e), msg);
}

/* `fd`, moved above 2 if it is a standard slot, close-on-exec either way;
   -1 (errno set, `fd` closed) if the move fails. */
static int sp_pty_above_std(int fd) {
  if (fd > 2) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    return fd;
  }
  int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  int e = errno;
  close(fd);
  errno = e;
  return moved;
}

/* A new master, close-on-exec and above 2, its slave unlocked. */
sp_int sp_pty_open_master(void) {
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0) sp_pty_raise_errno(errno, "/dev/ptmx");
  if (grantpt(m) < 0 || unlockpt(m) < 0) {
    int e = errno;
    close(m);
    sp_pty_raise_errno(e, "/dev/ptmx");
  }
  m = sp_pty_above_std(m);
  if (m < 0) sp_pty_raise_errno(errno, "/dev/ptmx");
  return (sp_int)m;
}

/* A second descriptor on the master for the writer CRuby hands back beside
   the reader, close-on-exec and above 2 like the first. Made before the
   child starts; if it fails, the master is closed here. */
sp_int sp_pty_dup(sp_int fd) {
  int d = fcntl((int)fd, F_DUPFD_CLOEXEC, 3);
  if (d < 0) {
    int e = errno;
    close((int)fd);
    sp_pty_raise_errno(e, NULL);
  }
  return (sp_int)d;
}

/* Is `v` a boxed String? */
static int sp_pty_is_str(sp_RbVal v) { return v.tag == SP_TAG_STR; }
/* A String the program holds as the shared handle (--share-strings)
   arrives boxed as that handle: it reads as the String it holds, the
   handle's first field, as a spawn's arguments do (sp_spawn_str). */
static sp_RbVal sp_pty_str(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p) {
    v.tag = SP_TAG_STR;
    v.v.s = *(const char **)v.v.p;
  }
  return v;
}
/* the command's argument i, and the environment's pair k, read so */
static sp_RbVal sp_pty_arg(sp_RbVal args, sp_int i) { return sp_pty_str(sp_json_aref_fn(args, i)); }
static void sp_pty_env_pair(sp_RbVal env, sp_int k, sp_RbVal *key, sp_RbVal *val) {
  sp_json_hpair_fn(env, k, key, val);
  *key = sp_pty_str(*key);
  *val = sp_pty_str(*val);
}

/* Does an `env` key name this `NAME=value` entry of environ? */
static int sp_pty_env_names(const char *entry, const char *name) {
  size_t n = strlen(name);
  return strncmp(entry, name, n) == 0 && entry[n] == '=';
}

/* The executable `prog` names, found as execvp would find it, written to
   `out`: as given when it holds a '/', else the first match on `path`. 0 on
   success, else the errno execvp would leave (EACCES when only files that
   cannot be executed matched, ENOENT when none did). */
static int sp_pty_find_exe(const char *prog, const char *path, char *out, size_t n) {
  if (strchr(prog, '/')) {
    if (strlen(prog) >= n) return ENAMETOOLONG;
    strcpy(out, prog);
    return 0;
  }
  int seen_eacces = 0;
  const char *p = path;
  for (;;) {
    const char *colon = strchr(p, ':');
    size_t dl = colon ? (size_t)(colon - p) : strlen(p);
    /* an empty entry is the current directory */
    int w = dl == 0 ? snprintf(out, n, "%s", prog) : snprintf(out, n, "%.*s/%s", (int)dl, p, prog);
    struct stat st;
    if (w > 0 && (size_t)w < n && stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
      if (access(out, X_OK) == 0) return 0;
      seen_eacces = 1;
    }
    if (!colon) break;
    p = colon + 1;
  }
  return seen_eacces ? EACCES : ENOENT;
}

/* The shell PTY.spawn runs when it is given no command, as CRuby picks it:
   $SHELL, else the password entry of $USER (or the login name), else
   /bin/sh. */
static const char *sp_pty_login_shell(void) {
  const char *sh = getenv("SHELL");
  if (sh) return sh;
  const char *user = getenv("USER");
  if (!user) user = getlogin();
  struct passwd *pw = user ? getpwnam(user) : NULL;
  return pw && pw->pw_shell ? pw->pw_shell : "/bin/sh";
}

/* Run `args` (an Array of Strings; one String with a shell character runs
   through /bin/sh -c, as Process.spawn reads it) on the slave of `master`,
   with `env` (a Hash of String => String or nil, or nil) laid over the
   current environment, in the directory `chdir` (a String, or nil for this
   one); no command runs the login shell, as in CRuby.
   Answers the child's pid. `master` and `writer` are
   closed here if it raises. */
sp_int sp_pty_spawn_child(sp_int master, sp_int writer, sp_RbVal env, sp_RbVal args, sp_RbVal chdir_to) {
  chdir_to = sp_pty_str(chdir_to);
  int m = (int)master, wfd = (int)writer;

  /* every argument checked before anything is allocated or opened */
  if (args.tag != SP_TAG_OBJ || sp_json_kind_fn(args) != 1) {
    sp_pty_close2(m, wfd);
    sp_raise_cls("TypeError", "PTY.spawn takes the command as Strings");
  }
  sp_int na = sp_json_len_fn(args);
  for (sp_int i = 0; i < na; i++)
    if (!sp_pty_is_str(sp_pty_arg(args, i))) {
      sp_pty_close2(m, wfd);
      sp_raise_cls("TypeError", "no implicit conversion into String (command argument)");
    }
  sp_int ne = 0;
  if (env.tag != SP_TAG_NIL) {
    if (env.tag != SP_TAG_OBJ || sp_json_kind_fn(env) != 2) {
      sp_pty_close2(m, wfd);
      sp_raise_cls("TypeError", "PTY.spawn takes the environment as a Hash");
    }
    ne = sp_json_len_fn(env);
    for (sp_int k = 0; k < ne; k++) {
      sp_RbVal key, val;
      sp_pty_env_pair(env, k, &key, &val);
      if (!sp_pty_is_str(key) || !(val.tag == SP_TAG_NIL || sp_pty_is_str(val))) {
        sp_pty_close2(m, wfd);
        sp_raise_cls("TypeError", "no implicit conversion into String (environment)");
      }
    }
  }
  if (chdir_to.tag != SP_TAG_NIL && !sp_pty_is_str(chdir_to)) {
    sp_pty_close2(m, wfd);
    sp_raise_cls("TypeError", "no implicit conversion into String (chdir)");
  }
  const char *dir = chdir_to.tag == SP_TAG_NIL ? NULL : chdir_to.v.s;

  /* the program's path, searched on the PATH the child will have */
  const char *first = na > 0 ? sp_pty_arg(args, 0).v.s : sp_pty_login_shell();
  int via_shell = na <= 1 && strpbrk(first, " \t\n*?{}[]<>()~&|\\$;'`\"#=%") != NULL;
  const char *prog = via_shell ? "/bin/sh" : first;
  const char *path = getenv("PATH");
  for (sp_int k = 0; k < ne; k++) {
    sp_RbVal key, val;
    sp_pty_env_pair(env, k, &key, &val);
    if (strcmp(key.v.s, "PATH") == 0) path = val.tag == SP_TAG_NIL ? NULL : val.v.s;
  }
  char exe[4096];
  int fe = sp_pty_find_exe(prog, path ? path : "/usr/bin:/bin", exe, sizeof exe);
  if (fe) {
    sp_pty_close2(m, wfd);
    sp_pty_raise_errno(fe, first);
  }

  /* argv, and the /bin/sh one for a file execve cannot run */
  char **argv = malloc(sizeof(char *) * (size_t)(na + 4));
  char **sh_argv = malloc(sizeof(char *) * (size_t)(na + 5));
  if (!argv || !sh_argv) sp_oom_die();
  int ai = 0;
  if (via_shell) { argv[ai++] = (char *)"/bin/sh"; argv[ai++] = (char *)"-c"; }
  if (na == 0) argv[ai++] = (char *)first;
  for (sp_int i = 0; i < na; i++) argv[ai++] = (char *)sp_pty_arg(args, i).v.s;
  argv[ai] = NULL;
  sh_argv[0] = (char *)"sh";
  sh_argv[1] = exe;
  for (int i = 1; i <= ai; i++) sh_argv[i + 1] = argv[i];

  /* envp: environ less every name `env` mentions, then the ones it sets */
  char **envp = environ;
  char **built = NULL;
  char **owned = NULL;
  int nowned = 0;
  if (ne > 0) {
    sp_int nenv = 0;
    while (environ && environ[nenv]) nenv++;
    built = malloc(sizeof(char *) * (size_t)(nenv + ne + 1));
    owned = malloc(sizeof(char *) * (size_t)(ne + 1));
    if (!built || !owned) sp_oom_die();
    int ei = 0;
    for (sp_int i = 0; i < nenv; i++) {
      int shadowed = 0;
      for (sp_int k = 0; k < ne && !shadowed; k++) {
        sp_RbVal key, val;
        sp_pty_env_pair(env, k, &key, &val);
        shadowed = sp_pty_env_names(environ[i], key.v.s);
      }
      if (!shadowed) built[ei++] = environ[i];
    }
    for (sp_int k = 0; k < ne; k++) {
      sp_RbVal key, val;
      sp_pty_env_pair(env, k, &key, &val);
      if (val.tag == SP_TAG_NIL) continue;
      size_t len = strlen(key.v.s) + strlen(val.v.s) + 2;
      char *entry = malloc(len);
      if (!entry) sp_oom_die();
      snprintf(entry, len, "%s=%s", key.v.s, val.v.s);
      owned[nowned++] = entry;
      built[ei++] = entry;
    }
    built[ei] = NULL;
    envp = built;
  }

  /* everything the parent owns from here, released on every way out */
  int err_pipe[2] = { -1, -1 };
  int fail = 0;
  const char *fail_what = NULL;
  pid_t pid = -1;
  char slave[256];
#ifdef __linux__
  if (ptsname_r(m, slave, sizeof slave) != 0) { fail = errno; fail_what = "ptsname"; }
#else
  const char *pn = ptsname(m);
  if (pn) { strncpy(slave, pn, sizeof slave - 1); slave[sizeof slave - 1] = 0; }
  else { fail = errno; fail_what = "ptsname"; }
#endif
  if (!fail && pipe(err_pipe) < 0) { fail = errno; fail_what = "pipe"; err_pipe[0] = err_pipe[1] = -1; }
  if (!fail) {
    err_pipe[0] = sp_pty_above_std(err_pipe[0]);
    if (err_pipe[0] < 0) {
      fail = errno; fail_what = "pipe";
      close(err_pipe[1]);
      err_pipe[1] = -1;
    }
    else {
      err_pipe[1] = sp_pty_above_std(err_pipe[1]);
      if (err_pipe[1] < 0) { fail = errno; fail_what = "pipe"; }
    }
  }

  if (!fail) {
    /* what the parent has buffered is written before the child can write to
       the same descriptors, as lib/sp_process.c and CRuby flush */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
      /* CHILD: async-signal-safe calls only, until exec */
      sigset_t none;
      sigemptyset(&none);
      sigprocmask(SIG_SETMASK, &none, NULL);
      signal(SIGPIPE, SIG_DFL);
      setsid();
      int s = open(slave, O_RDWR);
      if (s < 0) {
        int failed[2] = { errno, 0 };
        (void)!write(err_pipe[1], failed, sizeof failed);
        _exit(127);
      }
#ifdef TIOCSCTTY
      ioctl(s, TIOCSCTTY, 0);
#endif
      dup2(s, 0);
      dup2(s, 1);
      dup2(s, 2);
      if (s > 2) close(s);
      if (dir && chdir(dir) != 0) {
        int failed[2] = { errno, 1 };
        (void)!write(err_pipe[1], failed, sizeof failed);
        _exit(127);
      }
      execve(exe, argv, envp);
      if (errno == ENOEXEC) execve("/bin/sh", sh_argv, envp);
      int failed[2] = { errno, 0 };
      (void)!write(err_pipe[1], failed, sizeof failed);
      _exit(127);
    }
    if (pid < 0) { fail = errno; fail_what = "fork"; }
  }

  free(argv);
  free(sh_argv);
  free(built);
  for (int i = 0; i < nowned; i++) free(owned[i]);
  free(owned);
  if (err_pipe[1] >= 0) close(err_pipe[1]);

  if (!fail) {
    /* the child's errno, and 1 when it was the chdir that failed */
    int failed[2] = { 0, 0 };
    ssize_t got;
    do { got = read(err_pipe[0], failed, sizeof failed); } while (got < 0 && errno == EINTR);
    if (got > 0) {
      /* the child could not exec: reap it before the raise, as CRuby does */
      int st = 0;
      pid_t r;
      do { r = waitpid(pid, &st, 0); } while (r < 0 && errno == EINTR);
      fail = failed[0];
      fail_what = failed[1] ? "chdir" : first;
    }
  }
  if (err_pipe[0] >= 0) close(err_pipe[0]);
  if (fail) {
    sp_pty_close2(m, wfd);
    sp_pty_raise_errno(fail, fail_what);
  }
  return (sp_int)pid;
}
