/* lib/sp_process.c -- Process.spawn and Process.waitpid2 (CRuby-compatible)
 *
 * Lives in its own TU because the codegen calls sp_process_spawn
 * with pre-resolved positional args (in_fd, out_fd, err_fd, pgroup,
 * rlimit_cpu, rlimit_as, chdir, owned). The codegen turns each option
 * into one of them; the in/out/err redirects are found in opts at run
 * time by sp_process_redirect_value (lib/sp_poly_cold.c), grouped keys
 * ([:out, :err] =>) included; the runtime just needs primitive int
 * / int-fd / int-pgroup / int-rlimit values. The cmd is either a
 * String or an Array (boxed as a PolyArray). args is a PolyArray of
 * extra String args appended after cmd.
 *
 * The TU deliberately does NOT include spinel_rt.h (it pulls in only
 * sp_alloc.h and the low-level headers). spinel_rt.h's static-inline
 * family references sp_class_to_s / sp_sym_to_s which are emitted
 * per-program by the codegen and not present in the runtime archive;
 * including spinel_rt.h from here would force those symbols into the
 * link and break the build.
 *
 * Process.spawn(cmd, *args, opts) -> child pid
 * Process.waitpid2(pid) -> [pid, raw_status]
 *
 * opts is a PolyArray with these slots (the last two are optional):
 *   [0] in_fd        - Integer fd (>= 0), -1 for false/nil/absent. An IO
 *                      was resolved to its fd by the codegen; a String
 *                      path was opened here, by sp_process_open_redirect,
 *                      which the generated program calls before this
 *   [1] out_fd       - same conventions
 *   [2] err_fd       - same conventions
 *   [3] pgroup       - Integer (0=inherit, 1=new, >1=specific pgid)
 *   [4] rlimit_cpu   - Integer (seconds), nil = no limit
 *   [5] rlimit_as    - Integer (bytes), nil = no limit
 *   [6] chdir        - String path or nil
 *   [7] owned        - Integer bit mask, bit 0/1/2 set when [0]/[1]/[2]
 *                      is an fd sp_process_open_redirect opened for this
 *                      spawn; those are closed here once the child has
 *                      its copies, and on every raise this file makes
 *                      before the fork.
 *                      A caller's IO or Integer fd is never in the mask.
 *   [8] env          - the Hash a call gave before the command, or nil.
 *   [9] unsetenv_others - true clears inherited entries; false/nil keeps them.
 *
 * An IO value in opts[0..2] is resolved to its fd by the codegen (which
 * has access to sp_File_fileno); a String value reaches the runtime
 * through sp_process_open_redirect. The [:child, :out|:err|Integer]
 * form is resolved by the codegen (it sees the literal array at parse
 * time).
 */

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#if defined(_WIN32)
#include <spawn.h>
#include <sys/stat.h>
#else
#include <poll.h>
#endif

#include "sp_alloc.h"   /* sp_PolyArray, sp_RbVal, sp_box_*, sp_raise_cls */
#include "sp_process_status.h"   /* sp_ProcessStatus, sp_box_process_status */
#include "sp_system.h"   /* sp_last_status: $? */

#if !defined(_WIN32)
extern char **environ;
#endif

/* Local error-message builder. Returns a static buffer; copy the
   result before another call. Avoids sp_sprintf which would pull
   sp_class_to_s / sp_sym_to_s into the link. */
static SP_TLS char sp_err_buf[512];
static const char *sp_errf_errno(const char *prefix, int err) {
  snprintf(sp_err_buf, sizeof sp_err_buf, "%s - %s", prefix, strerror(err));
  return sp_err_buf;
}
/* CRuby's SystemCallError message for a named path: the strerror text,
   then the path that failed, as `No such file or directory - /bin/nope`. */
static const char *sp_errf_path(int err, const char *path) {
  snprintf(sp_err_buf, sizeof sp_err_buf, "%s - %s", strerror(err), path);
  return sp_err_buf;
}

/* CRuby's TypeError for a [program, argv0] element that is no String */
static const char *sp_errf_conv(sp_RbVal v) {
  const char *k = v.tag == SP_TAG_INT ? "Integer" : v.tag == SP_TAG_FLT ? "Float"
                : v.tag == SP_TAG_NIL ? "nil" : v.tag == SP_TAG_SYM ? "Symbol"
                : v.tag == SP_TAG_BOOL ? (v.v.i ? "true" : "false") : "Object";
  snprintf(sp_err_buf, sizeof sp_err_buf, "no implicit conversion of %s into String", k);
  return sp_err_buf;
}

/* Apply the redirect in the child: dup2 src_fd onto target_fd, close src.
   src_fd < 0 means the caller did not pass that slot in the opts hash
   (the codegen initialises every slot to -1 and overwrites only the ones
   the user set); in that case the fd is left inherited from the parent.
   CRuby semantics: an unset :in/:out/:err slot is inherit, not /dev/null.
   The previous /dev/null behaviour silently swallowed the child's stderr
   and made `Process.spawn("cc -c foo.c")` in verbose mode look like it
   produced no output. */
static int apply_redirect(int target_fd, int src_fd) {
  if (src_fd < 0) return 0;
  if (src_fd != target_fd) { dup2(src_fd, target_fd); return 0; }
  /* already in place (a pipe took a free fd 0): no dup2 clears close-on-exec */
  int fl = fcntl(target_fd, F_GETFD);
  if (fl < 0) return -1;
  return (fl & FD_CLOEXEC) ? fcntl(target_fd, F_SETFD, fl & ~FD_CLOEXEC) : 0;
}

/* Close the source descriptors, once every dup2 above has been made. Not
   inside apply_redirect: `out: f, err: f` names one descriptor twice, and
   closing it after the first redirect left the second dup2 working on a
   closed fd -- silently, so the child's stderr went nowhere (#4176). */
static void close_redirect_srcs(int in_fd, int out_fd, int err_fd) {
  int fds[3]; int n = 0;
  if (in_fd  > 2) fds[n++] = in_fd;
  if (out_fd > 2) fds[n++] = out_fd;
  if (err_fd > 2) fds[n++] = err_fd;
  for (int i = 0; i < n; i++) {
    int dup = 0;
    for (int j = 0; j < i; j++) if (fds[j] == fds[i]) dup = 1;
    if (!dup) close(fds[i]);
  }
}

/* The child gets its redirected descriptors blocking. A pipe's write end
   can be O_NONBLOCK (sp_sock_write), and a program that does not expect it
   fails its write with EAGAIN once the pipe is full. The flag lives on the
   description the child shares, so it is cleared here, before the fork,
   and the parent's own handles on it ask for it again. */
static void give_blocking(int in_fd, int out_fd, int err_fd) {
  int fds[3] = { in_fd, out_fd, err_fd };
  int given = 0;
  for (int i = 0; i < 3; i++) {
    if (fds[i] < 0) continue;
    given = 1;
    int fl = fcntl(fds[i], F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) fcntl(fds[i], F_SETFL, fl & ~O_NONBLOCK);
  }
  if (given) {
    extern void sp_io_child_fds_given(void);
    sp_io_child_fds_given();
  }
}

/* The fds this spawn opened itself, one slot each for in/out/err and -1
   where the slot came from the caller (an IO, an Integer, nothing). Only
   these are the spawn's to close: the child closes its copies after the
   dup2s above, and the parent closes its own once the child has them. */
static void close_owned(const int *owned) {
  for (int i = 0; i < 3; i++) if (owned[i] >= 0) close(owned[i]);
}

/* Raise on the spawn's behalf: the fds it opened are released first, so a
   spawn that never forks does not leave them behind. errno survives the
   closes for the Errno class the raise builds. */
SP_NORETURN void sp_process_spawn_fail(int *owned, const char *cls, const char *msg) {
  int e = errno;
  close_owned(owned);
  errno = e;
  sp_raise_cls(cls, msg);
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A filename redirection, opened here rather than by the generated program:
   read-only for slot 0 (:in), created and truncated for :out and :err. The
   fd is recorded in owned[slot]. A failed open raises the Errno class CRuby
   raises, with CRuby's message, after releasing the fds already opened for
   this spawn. The ladder is sp_file_open_raise's four (sp_cold.c) plus the
   six more errnos open(2) answers for a path; anything else is a bare
   SystemCallError. The message is built in a buffer sized for a path, and
   sp_raise_cls copies it before it can allocate, so a local is enough. */
int sp_process_open_redirect(const char *path, int slot, int *owned) {
  int flags = slot == 0 ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
  int fd = open(path, flags, 0644);
  if (fd < 0) {
    int e = errno;
    const char *cls = e == ENOENT ? "Errno::ENOENT" :
                      e == EACCES ? "Errno::EACCES" :
                      e == EEXIST ? "Errno::EEXIST" :
                      e == EISDIR ? "Errno::EISDIR" :
                      e == ENOTDIR ? "Errno::ENOTDIR" :
                      e == ENAMETOOLONG ? "Errno::ENAMETOOLONG" :
                      e == ELOOP ? "Errno::ELOOP" :
                      e == EROFS ? "Errno::EROFS" :
                      e == EMFILE ? "Errno::EMFILE" :
                      e == ENFILE ? "Errno::ENFILE" : "SystemCallError";
    char msg[PATH_MAX + 64];
    snprintf(msg, sizeof msg, "%s - %s", strerror(e), path);
    errno = e;
    sp_process_spawn_fail(owned, cls, msg);
  }
  owned[slot] = fd;
  return fd;
}

/* An environment Hash's names and values are Strings, a value may be nil,
   and a name holds no '=', as CRuby checks them before it starts a child. */
static void sp_spawn_env_check(sp_RbVal env, int *owned) {
  sp_int n = sp_json_len_fn(env);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal key, val;
    sp_json_hpair_fn(env, i, &key, &val);
    if (!sp_rbval_is_string(key)) sp_process_spawn_fail(owned, "TypeError", sp_errf_conv(key));
    if (!sp_rbval_is_string(val) && val.tag != SP_TAG_NIL) sp_process_spawn_fail(owned, "TypeError", sp_errf_conv(val));
    const char *name = sp_rbval_string_bytes(key);
    if (strchr(name, '=')) {
      snprintf(sp_err_buf, sizeof sp_err_buf, "environment name contains a equal : %s", name);
      sp_process_spawn_fail(owned, "ArgumentError", sp_err_buf);
    }
  }
}

/* The child's environment for a checked Hash (or nil): unless cleared, this
   process's environment less every name the Hash mentions, then each name
   whose value is a String. One block, the entries after the pointers; NULL
   when out of memory. */
static char **sp_spawn_envp(sp_RbVal env, int unsetenv_others) {
  sp_int n = env.tag == SP_TAG_NIL ? 0 : sp_json_len_fn(env);
  size_t nenv = 0, bytes = 0;
  while (!unsetenv_others && environ && environ[nenv]) nenv++;
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal key, val;
    sp_json_hpair_fn(env, i, &key, &val);
    if (sp_rbval_is_string(val)) bytes += strlen(sp_rbval_string_bytes(key)) + strlen(sp_rbval_string_bytes(val)) + 2;
  }
  char **envp = (char **)malloc(sizeof(char *) * (nenv + (size_t)n + 1) + bytes);
  if (!envp) return NULL;
  char *entry = (char *)(envp + nenv + (size_t)n + 1);
  size_t at = 0;
  for (size_t e = 0; e < nenv; e++) {
    int named = 0;
    for (sp_int i = 0; i < n && !named; i++) {
      sp_RbVal key, val;
      sp_json_hpair_fn(env, i, &key, &val);
      const char *name = sp_rbval_string_bytes(key);
      size_t len = strlen(name);
      named = strncmp(environ[e], name, len) == 0 && environ[e][len] == '=';
    }
    if (!named) envp[at++] = environ[e];
  }
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal key, val;
    sp_json_hpair_fn(env, i, &key, &val);
    if (!sp_rbval_is_string(val)) continue;
    envp[at++] = entry;
    entry += sprintf(entry, "%s=%s", sp_rbval_string_bytes(key), sp_rbval_string_bytes(val)) + 1;
  }
  envp[at] = NULL;
  return envp;
}

/* Kernel#system with an environment Hash ahead of the command: the Hash is
   checked and the child's environment built as Process.spawn's is, then the
   command runs as sp_system_args runs it. */
void sp_system_env_check(sp_RbVal env) {
  int owned[3] = { -1, -1, -1 };
  sp_spawn_env_check(env, owned);
}
/* prog, for system([program, argv0], ...), is the program and argv[0] the
   argv0 it sees; NULL when argv[0] is the program. env is nil for none. */
int sp_system_prog_env_args(sp_RbVal env, const char *prog, int argc, const char *const *argv) {
  if (env.tag == SP_TAG_NIL) return sp_system_prog_envp(prog, argc, argv, NULL);
  SP_GC_ROOT_RBVAL(env);
  sp_system_env_check(env);
  char **envp = sp_spawn_envp(env, 0);
  if (!envp) sp_raise_cls("NoMemoryError", "out of memory");
  int ok = sp_system_prog_envp(prog, argc, argv, envp);
  free(envp);
  return ok;
}

/* Extract a resolved Integer fd from a pre-resolved opts slot. The
   codegen turns IO/false into Integer before passing, and a String
   path arrives as the fd sp_process_open_redirect returned; we just
   unbox. -1 means "not set": inherit the parent's, per apply_redirect. */
static int slot_to_fd(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return -1;
  if (v.tag == SP_TAG_BOOL && v.v.i == 0) return -1;
  if (v.tag == SP_TAG_INT) return (int)v.v.i;
  sp_raise_cls("TypeError", "redirect slot must be Integer (codegen bug)");
  return -1;
}

/* The options of a spawn or an exec, read from the array the generated
   program builds (the slots at the top of this file): the descriptors of
   the three standard streams, -1 for one left as it is; the process group;
   the rlimits; the directory; the environment Hash and unsetenv_others. */
typedef struct {
  int in_fd, out_fd, err_fd;
  int pgroup;                /* 0 inherit, 1 new, >1 the pgid */
  int rlimit_cpu_set, rlimit_as_set;
  rlim_t rlimit_cpu_val, rlimit_as_val;
  const char *chdir_to;      /* NULL for none */
  sp_RbVal env;              /* nil for none */
  int unsetenv_others;
} sp_spawn_opts;

/* Read the options array into *o; owned gets the fds the generated program
   opened for this call (slot 7), which the raises below release. */
static void spawn_opts_read(sp_RbVal opts_box, sp_spawn_opts *o, int *owned) {
  if (opts_box.tag != SP_TAG_OBJ ||
      opts_box.cls_id != SP_BUILTIN_POLY_ARRAY) {
    sp_raise_cls("TypeError", "opts must be a PolyArray (codegen bug)");
  }
  sp_PolyArray *opts = (sp_PolyArray *)opts_box.v.p;
  if (opts->len < 8) {
    sp_raise_cls("ArgumentError", "opts array too short (codegen bug)");
  }
  o->in_fd  = slot_to_fd(opts->data[0]);
  o->out_fd = slot_to_fd(opts->data[1]);
  o->err_fd = slot_to_fd(opts->data[2]);
  /* Slot 7: a bit per slot the generated program had this file open for
     it, so those fds are the call's to close; a caller's IO stays open. */
  int owned_mask = opts->data[7].tag == SP_TAG_INT ? (int)opts->data[7].v.i : 0;
  owned[0] = owned_mask & 1 ? o->in_fd : -1;
  owned[1] = owned_mask & 2 ? o->out_fd : -1;
  owned[2] = owned_mask & 4 ? o->err_fd : -1;

  o->pgroup = 0;
  if (opts->data[3].tag == SP_TAG_NIL) o->pgroup = 0;
  else if (opts->data[3].tag == SP_TAG_BOOL && opts->data[3].v.i == 1) o->pgroup = 1;
  else if (opts->data[3].tag == SP_TAG_INT) o->pgroup = (int)opts->data[3].v.i;
  else sp_process_spawn_fail(owned, "TypeError", "pgroup must be true, 0, or Integer");

  o->rlimit_cpu_set = 0;
  o->rlimit_cpu_val = 0;
  if (opts->data[4].tag == SP_TAG_INT) { o->rlimit_cpu_set = 1; o->rlimit_cpu_val = (rlim_t)opts->data[4].v.i; }
  else if (opts->data[4].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "TypeError", "rlimit_cpu must be Integer");

  o->rlimit_as_set = 0;
  o->rlimit_as_val = 0;
#ifdef RLIMIT_AS
  if (opts->data[5].tag == SP_TAG_INT) { o->rlimit_as_set = 1; o->rlimit_as_val = (rlim_t)opts->data[5].v.i; }
  else if (opts->data[5].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "TypeError", "rlimit_as must be Integer");
#else
  /* OpenBSD has no RLIMIT_AS: CRuby does not know the option there */
  if (opts->data[5].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "ArgumentError", "wrong exec option symbol: rlimit_as");
#endif

  sp_RbVal dir = opts->data[6];
  o->chdir_to = sp_rbval_string_bytes(dir);
  if (dir.tag != SP_TAG_NIL && !sp_rbval_is_string(dir))
    sp_process_spawn_fail(owned, "TypeError", "chdir must be a String");

  o->env = opts->len > 8 ? opts->data[8] : sp_box_nil();
  sp_RbVal clear = opts->len > 9 ? opts->data[9] : sp_box_nil();
  if (clear.tag != SP_TAG_NIL && clear.tag != SP_TAG_BOOL)
    sp_process_spawn_fail(owned, "ArgumentError", "expected true or false as unsetenv_others");
  o->unsetenv_others = clear.tag == SP_TAG_BOOL && clear.v.b;
  if (o->env.tag != SP_TAG_NIL) sp_spawn_env_check(o->env, owned);
}

/* Apply the options to this process: the directory, the rlimits, the
   process group, then the redirections, in the order the forked child of a
   spawn has always made them. The sources of the redirections are closed in
   a child (for_exec 0); an exec marks them close-on-exec instead, so a
   failed exec still has them. 0, or -1 with errno set and *which 1 for a
   chdir that failed and 0 for a redirection, as the child reports them. */
static int spawn_opts_apply(const sp_spawn_opts *o, int for_exec, int *which) {
  if (o->chdir_to && chdir(o->chdir_to) != 0) { *which = 1; return -1; }
  if (o->rlimit_cpu_set) {
    struct rlimit rl = { o->rlimit_cpu_val, RLIM_INFINITY };
    setrlimit(RLIMIT_CPU, &rl);
  }
#ifdef RLIMIT_AS
  if (o->rlimit_as_set) {
    struct rlimit rl = { o->rlimit_as_val, RLIM_INFINITY };
    setrlimit(RLIMIT_AS, &rl);
  }
#endif
  if (o->pgroup == 1) setpgid(0, 0);
  else if (o->pgroup > 1) setpgid(0, o->pgroup);
  if (apply_redirect(0, o->in_fd) != 0 || apply_redirect(1, o->out_fd) != 0 ||
      apply_redirect(2, o->err_fd) != 0) { *which = 0; return -1; }
  if (!for_exec) close_redirect_srcs(o->in_fd, o->out_fd, o->err_fd);
  else {
    int fds[3] = { o->in_fd, o->out_fd, o->err_fd };
    for (int i = 0; i < 3; i++) {
      if (fds[i] <= 2) continue;
      int fl = fcntl(fds[i], F_GETFD);
      if (fl >= 0) fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC);
    }
  }
  return 0;
}

/* Become prog: execvp, through the search path of the parent's environment
   when the child's has none (unsetenv_others, with no PATH of its own).
   Returns only when the exec failed, with its errno. */
static void exec_prog(const char *prog, char **argv, const char *search_path) {
  if (search_path && !strchr(prog, '/')) {
    for (const char *p = search_path;;) {
      const char *end = strchr(p, ':');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      char path[PATH_MAX];
      if (len + strlen(prog) + 2 > sizeof path) errno = ENAMETOOLONG;
      else {
        snprintf(path, sizeof path, "%.*s/%s", (int)(len ? len : 1), len ? p : ".", prog);
        execvp(path, argv);
      }
      if (errno != EACCES && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG) return;
      if (!end) { errno = ENOENT; return; }
      p = end + 1;
    }
  }
  execvp(prog, argv);
}

sp_int sp_process_spawn(sp_RbVal cmd, sp_RbVal args_box,
                        sp_RbVal opts_box) {
  SP_GC_ROOT_RBVAL(cmd);
  SP_GC_ROOT_RBVAL(args_box);
  SP_GC_ROOT_RBVAL(opts_box);

  int owned[3] = { -1, -1, -1 };
  sp_spawn_opts o;
  spawn_opts_read(opts_box, &o, owned);
  int in_fd = o.in_fd, out_fd = o.out_fd, err_fd = o.err_fd;
  int pgroup = o.pgroup;
  int rlimit_cpu_set = o.rlimit_cpu_set, rlimit_as_set = o.rlimit_as_set;
  rlim_t rlimit_cpu_val = o.rlimit_cpu_val, rlimit_as_val = o.rlimit_as_val;
  const char *chdir_to = o.chdir_to;
  sp_RbVal env = o.env;
  int unsetenv_others = o.unsetenv_others;

  /* Resolve cmd + args into argv. */
  const char *prog = NULL;
  /* the program name of a split command line, kept past free(argv) for the
     error an exec failure reports (prog points into the split words) */
  char prog_copy[4096];
  char **argv = NULL;
  sp_PolyArray *cmd_arr = NULL;
  sp_PolyArray *args_arr = NULL;
  int extra_from_cmd = 0;
  int extra_from_args = 0;

  int via_shell = 0;
  char **words = NULL;   /* a command line's own words, when no shell runs it */
  if (sp_rbval_is_string(cmd)) {
    prog = sp_rbval_string_bytes(cmd);
    if (args_box.tag == SP_TAG_OBJ &&
        args_box.cls_id == SP_BUILTIN_POLY_ARRAY) {
      args_arr = (sp_PolyArray *)args_box.v.p;
    }
    else if (args_box.tag != SP_TAG_NIL) {
      sp_process_spawn_fail(owned, "TypeError", "args must be a PolyArray of extra args");
    }
    /* One string and no arguments is a command LINE, as for Kernel#system:
       CRuby hands it to the shell when it carries a shell character (or
       starts with a word the shell answers) and splits it into words
       otherwise, exec'd with no shell -- so a missing program is ENOENT,
       not the shell's exit 127 (sp_cmdline_shell_p). */
    if (!args_arr || args_arr->len == 0) {
      via_shell = sp_cmdline_shell_p(prog);
      if (!via_shell) {
        int wn = 0;
        if (!(words = sp_cmdline_words(prog, &wn)))
          sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
        if (wn == 0) { free(words); sp_process_spawn_fail(owned, "Errno::ENOENT", sp_errf_path(ENOENT, prog)); }
        prog = words[0];
        if (strlen(prog) < sizeof prog_copy) prog = strcpy(prog_copy, prog);
      }
    }
  }
  else if (cmd.tag == SP_TAG_OBJ &&
             cmd.cls_id == SP_BUILTIN_POLY_ARRAY) {
    /* [program, argv0]: run program with argv0 as its argv[0], exactly two
       Strings, as CRuby takes them */
    cmd_arr = (sp_PolyArray *)cmd.v.p;
    if (cmd_arr->len != 2) sp_process_spawn_fail(owned, "ArgumentError", "wrong first argument");
    for (int i = 0; i < 2; i++)
      if (!sp_rbval_is_string(cmd_arr->data[i]))
        sp_process_spawn_fail(owned, "TypeError", sp_errf_conv(cmd_arr->data[i]));
    prog = sp_rbval_string_bytes(cmd_arr->data[0]);
    if (args_box.tag == SP_TAG_OBJ &&
        args_box.cls_id == SP_BUILTIN_POLY_ARRAY) {
      args_arr = (sp_PolyArray *)args_box.v.p;
    }
  }
  else {
    sp_process_spawn_fail(owned, "TypeError",
                          "wrong first argument type (expected String or Array)");
  }
  if (args_arr) extra_from_args = (int)args_arr->len;

  if (words) argv = words;   /* one block: the free(argv)s below free it */
  else {
    int total = 1 + extra_from_cmd + extra_from_args + (via_shell ? 2 : 0);
    argv = (char **)malloc(sizeof(char *) * (size_t)(total + 1));
    if (!argv) sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
    int ai = 0;
    if (via_shell) { argv[ai++] = (char *)"/bin/sh"; argv[ai++] = (char *)"-c"; }
    argv[ai++] = cmd_arr ? (char *)sp_rbval_string_bytes(cmd_arr->data[1]) : (char *)prog;
    if (via_shell) prog = "/bin/sh";
    if (args_arr) {
      for (int i = 0; i < args_arr->len; i++) {
        if (!sp_rbval_is_string(args_arr->data[i]))
          sp_process_spawn_fail(owned, "ArgumentError", "spawn args must be Strings");
        argv[ai++] = (char *)sp_rbval_string_bytes(args_arr->data[i]);
      }
    }
    argv[ai] = NULL;
  }

  char **envp = NULL;
  if ((env.tag != SP_TAG_NIL || unsetenv_others) && !(envp = sp_spawn_envp(env, unsetenv_others))) {
    free(argv);
    sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
  }
#if !defined(_WIN32)
  /* A missing child PATH still uses the parent's for command lookup. */
  const char *search_path = unsetenv_others ? getenv("PATH") : NULL;
  if (unsetenv_others) for (char **e = envp; *e; e++)
    if (strncmp(*e, "PATH=", 5) == 0) search_path = NULL;
#endif

#if defined(_WIN32)
  /* posix_spawn, where there is no fork (lib/win32 answers it over
     CreateProcess). The child's setup is the file actions -- the same dup2s and closes the fork
     path makes, in the same order -- and the attributes: chdir, and a new
     process group. There is no posix_spawn counterpart for the rlimits;
     they are not applied on this path. The directory is checked here, so a
     missing one raises naming it, as the fork path's chdir failure does;
     posix_spawn reports an exec failure as its return value. */
  {
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    (void)rlimit_cpu_set; (void)rlimit_cpu_val; (void)rlimit_as_set; (void)rlimit_as_val;
    if (chdir_to) {
      struct stat cst;
      int sr = stat(chdir_to, &cst);
      if (sr != 0 || !S_ISDIR(cst.st_mode)) {
        int e = sr != 0 ? errno : ENOTDIR;
        free(argv);
        free(envp);
        sp_last_status = 127 << 8;   /* as after the fork path's child fails its chdir */
        errno = e;
        sp_process_spawn_fail(owned, e == ENOENT ? "Errno::ENOENT" : e == ENOTDIR ? "Errno::ENOTDIR" :
                              e == EACCES ? "Errno::EACCES" : "SystemCallError", sp_errf_path(e, chdir_to));
      }
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    if (chdir_to) posix_spawn_file_actions_addchdir_np(&fa, chdir_to);
    if (in_fd  >= 0 && in_fd  != 0) posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
    if (out_fd >= 0 && out_fd != 1) posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    if (err_fd >= 0 && err_fd != 2) posix_spawn_file_actions_adddup2(&fa, err_fd, 2);
    { int srcs[3] = { in_fd, out_fd, err_fd };
      for (int i = 0; i < 3; i++) {
        if (srcs[i] <= 2) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) if (srcs[j] == srcs[i]) dup = 1;
        if (!dup) posix_spawn_file_actions_addclose(&fa, srcs[i]);
      } }
    if (pgroup) {
      posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
      posix_spawnattr_setpgroup(&at, pgroup == 1 ? 0 : (pid_t)pgroup);
    }
    fflush(NULL);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, prog, &fa, &at, argv, envp ? envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close_owned(owned);
    free(argv);
    free(envp);
    if (rc != 0) {
      /* $? reads the 127 a failed exec exits with under the fork path */
      sp_last_status = 127 << 8;
      errno = rc;
      sp_raise_cls(rc == ENOENT ? "Errno::ENOENT" : rc == EACCES ? "Errno::EACCES" : "SystemCallError",
                   sp_errf_path(rc, prog));
    }
    return (sp_int)pid;
  }
#else
  /* Pre-exec error pipe: the child writes the exec errno here if execve
     fails, so the parent can raise the matching Errno (CRuby raises
     Errno::ENOENT for "no such file or directory" instead of returning
     a dead pid). Both ends are close-on-exec from creation, so a
     successful execvp closes the write end and the parent sees a
     zero-byte read and no raise. */
  extern int sp_io_make_pipe(int fds[2]);
  extern void sp_fork_exec_lock(void), sp_fork_exec_unlock(void), sp_io_wait_fd_readable(int fd);
  int err_pipe[2];
  if (sp_io_make_pipe(err_pipe) < 0) {
    free(argv);
    free(envp);
    sp_process_spawn_fail(owned, "SystemCallError", sp_errf_errno("pipe failed", errno));
  }

  /* what the parent has buffered for its streams is written before the
     child can write to the same descriptors, as CRuby flushes them */
  fflush(NULL);
  give_blocking(in_fd, out_fd, err_fd);
  /* (test/process_spawn_raise_in_exec_wait.rb: the child reads a byte from
     this fd before it execs, so a test can act while the parent waits) */
  int hold_fd = -1;
  { const char *h = getenv("SPINEL_TEST_SPAWN_HOLD_FD"); char *end;
    long v = h && *h >= '0' && *h <= '9' ? strtol(h, &end, 10) : -1;
    if (v > 2 && v < INT_MAX && *end == '\0') hold_fd = (int)v; }
  extern void sp_sig_hold_begin(void), sp_sig_hold_end(void);   /* lib/sp_cold.c */
  sp_sig_hold_begin();
  sp_fork_exec_lock();
  pid_t pid = fork();
  if (pid != 0) { sp_fork_exec_unlock(); free(envp); }
  if (pid < 0) {
    sp_sig_hold_end();
    free(argv);
    sp_process_spawn_fail(owned, "SystemCallError", sp_errf_errno("fork failed", errno));
  }
  if (pid == 0) {
    /* CHILD. If chdir or execve fails, write the errno and which of
       the two failed to the parent's pipe and exit 127; the parent
       will raise the matching Errno (Errno::ENOENT for "no such
       file") naming the directory or the program, as CRuby does.
       Using a pipe (not relying on the child's exit code alone)
       because the exit code is the same regardless of exec failure
       reason. */
    close(err_pipe[0]);
    /* (IO.pipe's read end is O_NONBLOCK: on EAGAIN, poll and read again) */
    if (hold_fd >= 0) for (char c;;) {
      if (read(hold_fd, &c, 1) >= 0) break;
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) break;
      struct pollfd hp; hp.fd = hold_fd; hp.events = POLLIN; hp.revents = 0;
      (void)poll(&hp, 1, -1);
    }
    { int which = 0;
      if (spawn_opts_apply(&o, 0, &which) != 0) {
        int fail[2] = { errno, which };
        (void)!write(err_pipe[1], fail, sizeof fail);
        _exit(127);
      } }
    if (envp) environ = envp;
    exec_prog(prog, argv, search_path);
    /* exec returned: failure. Send the errno to the parent. */
    int fail[2] = { errno, 0 };
    (void)!write(err_pipe[1], fail, sizeof fail);
    _exit(127);
  }
  /* PARENT. The child has its own copies of the redirections now, so the
     ones this spawn opened are closed here, on the exec-failure path too;
     then close the child's write end and read the errno if any. */
  close(err_pipe[1]);
  close_owned(owned);
  /* (parked until the child execs or fails, as sp_system_start's read). A
     Thread#raise or #kill is held until the spawn returns, so the caller
     has the pid. */
  extern void sp_fiber_defer_inject(void), sp_fiber_undefer_inject(void);   /* lib/sp_fiber.c */
  int fail[2] = { 0, 0 };
  ssize_t got;
  sp_fiber_defer_inject();
  /* (a Thread#raise wakes the park: it goes on until the pipe is readable,
     so the read below does not block a worker) */
  for (;;) {
    sp_io_wait_fd_readable(err_pipe[0]);
    struct pollfd rp; rp.fd = err_pipe[0]; rp.events = POLLIN; rp.revents = 0;
    int pr = poll(&rp, 1, 0);
    if (pr > 0 || (pr < 0 && errno != EINTR)) break;
  }
  /* (with sp_sig_hold set the signal handler returns, so read can fail with EINTR) */
  do { got = SP_SIG_BLOCKING(read(err_pipe[0], fail, sizeof fail)); } while (got < 0 && errno == EINTR);
  sp_fiber_undefer_inject();
  close(err_pipe[0]);
  free(argv);
  if (got > 0) {
    /* The child failed to exec: it wrote its errno and is exiting 127.
       It is reaped here before the raise, as CRuby does, so a failed
       spawn leaves no zombie and no stray pid for a later waitpid2(-1)
       to answer with instead of ECHILD. This is a plain blocking wait,
       not sp_sched_wait_child (#4381, #4528): the child has written its
       errno and is exiting, so it holds nothing this thread must drain,
       the wait takes no lock, and it is cheap: two hundred failed spawns
       take 0.36 to 0.52 s on master and 0.39 to 0.59 s with this wait.
       The polling arm would put a scheduler yield inside a half-finished
       spawn instead. */
    { int st = 0; pid_t r;
      do { r = SP_SIG_BLOCKING(waitpid(pid, &st, 0)); } while (r < 0 && errno == EINTR);
      /* the reaped child is the last one waited for, so $? reads its
         exit 127, as it does under CRuby */
      if (r == pid) { sp_last_status = st; sp_last_pid = (int)pid; } }
    sp_sig_hold_end();
    errno = fail[0];
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" :
                 errno == EACCES ? "Errno::EACCES" :
                 "SystemCallError",
                 sp_errf_path(errno, fail[1] == 1 && chdir_to ? chdir_to : prog));
  }
  sp_sig_hold_end();
  return (sp_int)pid;
#endif
}

/* Kernel#exec / Process.exec: replace the process with the command, read
   as spawn reads it -- one String a command line, through the shell when it
   carries a shell character; [program, argv0]; the arguments Strings -- with
   spawn's options (spawn_opts_read): the redirections of the standard
   streams, chdir:, pgroup:, the rlimits, and the environment Hash ahead of
   the command. The options are applied to this process, which becomes the
   command. What the program has buffered for its streams is not written, as
   under CRuby. Returns only by raising the Errno the exec, or a chdir of
   its, failed with; the environment is then as it was, and the streams and
   the directory stay as the options made them, as CRuby leaves them. */
void sp_process_exec(sp_RbVal cmd, sp_RbVal args_box, sp_RbVal opts_box) {
  SP_GC_ROOT_RBVAL(cmd);
  SP_GC_ROOT_RBVAL(args_box);
  SP_GC_ROOT_RBVAL(opts_box);
  int owned[3] = { -1, -1, -1 };
  sp_spawn_opts o;
  spawn_opts_read(opts_box, &o, owned);
  sp_PolyArray *args = (args_box.tag == SP_TAG_OBJ && args_box.cls_id == SP_BUILTIN_POLY_ARRAY)
                       ? (sp_PolyArray *)args_box.v.p : NULL;
  int na = args ? (int)args->len : 0;
  const char *prog = NULL, *argv0 = NULL;
  char prog_copy[4096];   /* as in sp_process_spawn: prog past free(argv) */
  if (sp_rbval_is_string(cmd)) prog = argv0 = sp_rbval_string_bytes(cmd);
  else if (cmd.tag == SP_TAG_OBJ && cmd.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *pa = (sp_PolyArray *)cmd.v.p;
    if (pa->len != 2) sp_process_spawn_fail(owned, "ArgumentError", "wrong first argument");
    for (int i = 0; i < 2; i++)
      if (!sp_rbval_is_string(pa->data[i])) sp_process_spawn_fail(owned, "TypeError", sp_errf_conv(pa->data[i]));
    prog = sp_rbval_string_bytes(pa->data[0]); argv0 = sp_rbval_string_bytes(pa->data[1]);
  }
  else sp_process_spawn_fail(owned, "TypeError", "wrong first argument type (expected String or Array)");
  for (int i = 0; i < na; i++)
    if (!sp_rbval_is_string(args->data[i])) sp_process_spawn_fail(owned, "TypeError", sp_errf_conv(args->data[i]));
  /* a command line as Process.spawn reads one (sp_cmdline_shell_p) */
  int line = sp_rbval_is_string(cmd) && na == 0;
  int via_shell = line && sp_cmdline_shell_p(prog);
  char **argv;
  if (line && !via_shell) {
    int wn = 0;
    if (!(argv = sp_cmdline_words(prog, &wn))) sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
    if (wn == 0) { free(argv); sp_process_spawn_fail(owned, "Errno::ENOENT", sp_errf_path(ENOENT, prog)); }
    prog = argv[0];
    if (strlen(prog) < sizeof prog_copy) prog = strcpy(prog_copy, prog);
  }
  else {
    argv = (char **)malloc(sizeof(char *) * (size_t)(na + 4));
    if (!argv) sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
    int ai = 0;
    if (via_shell) { argv[ai++] = (char *)"/bin/sh"; argv[ai++] = (char *)"-c"; }
    argv[ai++] = (char *)argv0;
    for (int i = 0; i < na; i++) argv[ai++] = (char *)sp_rbval_string_bytes(args->data[i]);
    argv[ai] = NULL;
  }
  /* (the error of a failed exec names the command line, not the shell) */
  const char *target = via_shell ? "/bin/sh" : prog;
  char **envp = NULL;
  if ((o.env.tag != SP_TAG_NIL || o.unsetenv_others) && !(envp = sp_spawn_envp(o.env, o.unsetenv_others))) {
    free(argv);
    sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
  }
  /* a missing PATH of the command's environment still has the parent's for
     the lookup, as in a spawn */
  const char *search_path = o.unsetenv_others ? getenv("PATH") : NULL;
  if (o.unsetenv_others) for (char **e = envp; *e; e++)
    if (strncmp(*e, "PATH=", 5) == 0) search_path = NULL;
  char **env_was = environ;   /* (a failure gives the environment back) */
  give_blocking(o.in_fd, o.out_fd, o.err_fd);
  int which = 0;
  int applied = spawn_opts_apply(&o, 1, &which);
  if (applied == 0) {
    if (envp) environ = envp;
    exec_prog(target, argv, search_path);
  }
  int e = errno;
  environ = env_was;
  const char *at = applied != 0 && which == 1 ? o.chdir_to : prog;
  free(argv);
  free(envp);
  errno = e;
  sp_process_spawn_fail(owned, e == ENOENT ? "Errno::ENOENT" : e == EACCES ? "Errno::EACCES" :
                        e == ENOTDIR ? "Errno::ENOTDIR" : "SystemCallError", sp_errf_path(e, at));
}

/* The wait itself lives in the scheduler (sp_sched_wait_child): a blocking
   waitpid answers for the OS worker, and a started green thread is pinned to
   its worker, so blocking here stalls the thread that may have to drain this
   child's output before it can exit (#4381). */
/* The wait of each form: the pid reaped, 0 for a child still running
   under WNOHANG, and the errors raised as CRuby raises them. $? and $$'s
   last pid follow the child this wait reaped, as after Kernel#system and a
   backtick; before, a waitpid2 left $? at whatever the last system call or
   backtick stored. */
static pid_t process_wait_reap(sp_int pid, int flags, int *status) {
  extern int sp_sched_wait_child_flags(int pid, int *status, int flags);   /* see the note at the top on this TU's includes */
  extern void sp_sig_after_wait(void);                                     /* lib/sp_cold.c */
  *status = 0;
  pid_t r = (pid_t)sp_sched_wait_child_flags((int)pid, status, flags);
  if (r < 0) {
    if (errno == ECHILD) {
      sp_raise_cls("Errno::ECHILD", "No child processes");
    }
    sp_raise_cls("SystemCallError", sp_errf_errno("waitpid failed", errno));
  }
  if (r > 0) {
    sp_last_status = *status;
    sp_last_pid = (int)r;
  }
  /* a signal recorded while the wait went on runs now that $? is stored, as
     CRuby runs it on the way out of its wait (lib/sp_cold.c) */
  sp_sig_after_wait();
  return r;
}

/* [pid, status]: the status is a Process::Status instance wrapping (pid,
   status), not a raw int -- .signaled? / .termsig dispatch on the boxed
   cls_id. */
static sp_PolyArray *process_wait_pair(pid_t r, int status) {
  sp_PolyArray *pa = sp_PolyArray_new(); SP_GC_ROOT(pa);   /* the status object below is an allocation */
  sp_PolyArray_push(pa, sp_box_int((sp_int)r));
  sp_PolyArray_push(pa, sp_box_process_status(sp_process_status_new((sp_int)r, (sp_int)status)));
  return pa;
}

/* Process.wait / waitpid: the same wait as waitpid2, answering only the
   pid; the status goes to $? */
sp_int sp_process_waitpid(sp_int pid) {
  int status;
  return (sp_int)process_wait_reap(pid, 0, &status);
}

/* Process.wait, waitpid, wait2 and waitpid2 with the flags of waitpid(2)
   (Process::WNOHANG, Process::WUNTRACED): the pid, or [pid, status] when
   pair is set, boxed; nil when WNOHANG finds the child still running. */
sp_RbVal sp_process_wait_flags(sp_int pid, sp_int flags, int pair) {
  int status;
  pid_t r = process_wait_reap(pid, (int)flags, &status);
  if (r == 0) return sp_box_nil();
  if (!pair) return sp_box_int((sp_int)r);
  return sp_box_poly_array(process_wait_pair(r, status));
}

sp_PolyArray *sp_process_waitpid2(sp_int pid) {
  int status;
  pid_t r = process_wait_reap(pid, 0, &status);
  return process_wait_pair(r, status);
}

/* Process.daemon(nochdir, noclose): the process carries on as a child of
   itself in a session of its own, detached from the terminal, as CRuby's
   daemon(3) does. The parent leaves at once with status 0 and runs none of
   the program's exit handlers. Unless nochdir, the child moves to "/";
   unless noclose, its stdin, stdout and stderr read and write /dev/null.
   Answers 0. The flags arrive already checked (sp_process_daemon_flag). */
/* Where there is no fork, CRuby's Process.daemon raises NotImplementedError
   before it looks at its arguments; the call checks this first. */
void sp_process_daemon_available(void) {
#if defined(__wasi__) || defined(_WIN32)
  sp_raise_cls("NotImplementedError", "daemon() function is unimplemented on this machine");
#endif
}
sp_int sp_process_daemon(int nochdir, int noclose) {
#if defined(__wasi__) || defined(_WIN32)
  (void)nochdir; (void)noclose;
  sp_process_daemon_available();
  return 0;
#else
  /* see the note at the top on this TU's includes */
  extern int sp_sched_threads_started(void);
  extern const char *sp_errno_class_name(int e);
  /* the worker pool, its monitor and the collector's sweepers are threads a
     fork leaves behind, and the child's scheduler would wait on them (the
     SPINEL_ALLOC_REPORT reader is one too, but only a debugging report
     waits on it, and the child just goes without) */
  if (sp_sched_threads_started())
    sp_raise_cls("NotImplementedError",
                 "Process.daemon after a Thread has started: spinel's threads do not survive a fork");
  /* what the program has buffered is written once, by the parent, before
     the child's streams move to /dev/null */
  fflush(NULL);
  pid_t pid = fork();
  if (pid > 0) _exit(0);
  if (pid == 0) {
    extern void sp_sig_fork_child(void);   /* lib/sp_cold.c */
    sp_sig_fork_child();
    /* as CRuby's own daemon path goes: a setsid failure only means the
       process already leads its group, the redirect is skipped when
       /dev/null will not open, and a failed chdir raises after it */
    (void)setsid();
    int err = nochdir ? 0 : chdir("/");
    int e = errno;
    if (!noclose) {
      int fd = open("/dev/null", O_RDWR);
      if (fd >= 0) {
        (void)dup2(fd, 0);
        (void)dup2(fd, 1);
        (void)dup2(fd, 2);
        if (fd > 2) close(fd);
      }
    }
    if (err == 0) return 0;
    errno = e;
  }
  /* CRuby's rb_sys_fail("daemon"): `Resource temporarily unavailable - daemon` */
  int e = errno;
  sp_raise_cls(sp_errno_class_name(e), sp_errf_path(e, "daemon"));
  return 0;
#endif
}
