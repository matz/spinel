/* lib/sp_process.c -- Process.spawn and Process.waitpid2 (CRuby-compatible)
 *
 * Lives in its own TU because the codegen calls sp_process_spawn
 * with pre-resolved positional args (in_fd, out_fd, err_fd, pgroup,
 * rlimit_cpu, rlimit_as, chdir, owned). All opts-hash unpacking happens
 * at compile time in the codegen; the runtime just needs primitive int
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
 * opts is a PolyArray of 8 elements in this order:
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

#include "sp_alloc.h"   /* sp_PolyArray, sp_RbVal, sp_box_*, sp_raise_cls */
#include "sp_process_status.h"   /* sp_ProcessStatus, sp_box_process_status */
#include "sp_system.h"   /* sp_last_status: $? */

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

/* Apply the redirect in the child: dup2 src_fd onto target_fd, close src.
   src_fd < 0 means the caller did not pass that slot in the opts hash
   (the codegen initialises every slot to -1 and overwrites only the ones
   the user set); in that case the fd is left inherited from the parent.
   CRuby semantics: an unset :in/:out/:err slot is inherit, not /dev/null.
   The previous /dev/null behaviour silently swallowed the child's stderr
   and made `Process.spawn("cc -c foo.c")` in verbose mode look like it
   produced no output. */
static void apply_redirect(int target_fd, int src_fd) {
  if (src_fd < 0) return;
  if (src_fd != target_fd) dup2(src_fd, target_fd);
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

sp_int sp_process_spawn(sp_RbVal cmd, sp_RbVal args_box,
                        sp_RbVal opts_box) {
  SP_GC_ROOT_RBVAL(cmd);
  SP_GC_ROOT_RBVAL(args_box);
  SP_GC_ROOT_RBVAL(opts_box);

  /* opts_box must be a PolyArray of 8 elements. */
  if (opts_box.tag != SP_TAG_OBJ ||
      opts_box.cls_id != SP_BUILTIN_POLY_ARRAY) {
    sp_raise_cls("TypeError", "opts must be a PolyArray (codegen bug)");
  }
  sp_PolyArray *opts = (sp_PolyArray *)opts_box.v.p;
  if (opts->len < 8) {
    sp_raise_cls("ArgumentError", "opts array too short (codegen bug)");
  }
  int in_fd  = slot_to_fd(opts->data[0]);
  int out_fd = slot_to_fd(opts->data[1]);
  int err_fd = slot_to_fd(opts->data[2]);
  /* Slot 7: a bit per slot the generated program had this file open for
     it, so those fds are the spawn's to close; a caller's IO stays open. */
  int owned_mask = opts->data[7].tag == SP_TAG_INT ? (int)opts->data[7].v.i : 0;
  int owned[3] = { owned_mask & 1 ? in_fd : -1,
                   owned_mask & 2 ? out_fd : -1,
                   owned_mask & 4 ? err_fd : -1 };

  int pgroup = 0;
  if (opts->data[3].tag == SP_TAG_NIL) pgroup = 0;
  else if (opts->data[3].tag == SP_TAG_BOOL && opts->data[3].v.i == 1) pgroup = 1;
  else if (opts->data[3].tag == SP_TAG_INT) pgroup = (int)opts->data[3].v.i;
  else sp_process_spawn_fail(owned, "TypeError", "pgroup must be true, 0, or Integer");

  int rlimit_cpu_set = 0;
  rlim_t rlimit_cpu_val = 0;
  if (opts->data[4].tag == SP_TAG_INT) { rlimit_cpu_set = 1; rlimit_cpu_val = (rlim_t)opts->data[4].v.i; }
  else if (opts->data[4].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "TypeError", "rlimit_cpu must be Integer");

  int rlimit_as_set = 0;
  rlim_t rlimit_as_val = 0;
  if (opts->data[5].tag == SP_TAG_INT) { rlimit_as_set = 1; rlimit_as_val = (rlim_t)opts->data[5].v.i; }
  else if (opts->data[5].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "TypeError", "rlimit_as must be Integer");

  const char *chdir_to = NULL;
  if (opts->data[6].tag == SP_TAG_STR) chdir_to = opts->data[6].v.s;
  else if (opts->data[6].tag != SP_TAG_NIL)
    sp_process_spawn_fail(owned, "TypeError", "chdir must be a String");

  /* Resolve cmd + args into argv. */
  const char *prog = NULL;
  char **argv = NULL;
  sp_PolyArray *cmd_arr = NULL;
  sp_PolyArray *args_arr = NULL;
  int extra_from_cmd = 0;
  int extra_from_args = 0;

  int via_shell = 0;
  if (cmd.tag == SP_TAG_STR) {
    prog = cmd.v.s;
    if (args_box.tag == SP_TAG_OBJ &&
        args_box.cls_id == SP_BUILTIN_POLY_ARRAY) {
      args_arr = (sp_PolyArray *)args_box.v.p;
    }
    else if (args_box.tag != SP_TAG_NIL) {
      sp_process_spawn_fail(owned, "TypeError", "args must be a PolyArray of extra args");
    }
    /* One string and no arguments is a command LINE, as for Kernel#system:
       CRuby hands it to the shell when it carries a shell character and
       splits it into words otherwise, which the shell also does; it was
       exec'd as a program name, so `spawn("sleep 2")` was ENOENT. */
    if (!args_arr || args_arr->len == 0)
      via_shell = strpbrk(prog, " \t\n*?{}[]<>()~&|\\$;'`\"#=%") != NULL;
  }
  else if (cmd.tag == SP_TAG_OBJ &&
             cmd.cls_id == SP_BUILTIN_POLY_ARRAY) {
    cmd_arr = (sp_PolyArray *)cmd.v.p;
    if (cmd_arr->len < 1) sp_process_spawn_fail(owned, "ArgumentError", "empty command array");
    if (cmd_arr->data[0].tag != SP_TAG_STR)
      sp_process_spawn_fail(owned, "ArgumentError", "command[0] must be a String");
    prog = cmd_arr->data[0].v.s;
    extra_from_cmd = cmd_arr->len - 1;
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

  int total = 1 + extra_from_cmd + extra_from_args + (via_shell ? 2 : 0);
  argv = (char **)malloc(sizeof(char *) * (size_t)(total + 1));
  if (!argv) sp_process_spawn_fail(owned, "NoMemoryError", "out of memory");
  int ai = 0;
  if (via_shell) { argv[ai++] = (char *)"/bin/sh"; argv[ai++] = (char *)"-c"; }
  argv[ai++] = (char *)prog;
  if (via_shell) prog = "/bin/sh";
  if (cmd_arr) {
    for (int i = 1; i < cmd_arr->len; i++) {
      if (cmd_arr->data[i].tag != SP_TAG_STR)
        sp_process_spawn_fail(owned, "ArgumentError", "command array element must be a String");
      argv[ai++] = (char *)cmd_arr->data[i].v.s;
    }
  }
  if (args_arr) {
    for (int i = 0; i < args_arr->len; i++) {
      if (args_arr->data[i].tag != SP_TAG_STR)
        sp_process_spawn_fail(owned, "ArgumentError", "spawn args must be Strings");
      argv[ai++] = (char *)args_arr->data[i].v.s;
    }
  }
  argv[ai] = NULL;

  /* Pre-exec error pipe: the child writes the exec errno here if execve
     fails, so the parent can raise the matching Errno (CRuby raises
     Errno::ENOENT for "no such file or directory" instead of returning
     a dead pid). FD_CLOEXEC on the write end so a successful execvp
     closes it; the parent then sees a zero-byte read and no raise. */
  int err_pipe[2];
  if (pipe(err_pipe) < 0) {
    free(argv);
    sp_process_spawn_fail(owned, "SystemCallError", sp_errf_errno("pipe failed", errno));
  }

  /* what the parent has buffered for its streams is written before the
     child can write to the same descriptors, as CRuby flushes them */
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
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
    if (fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC) < 0) { _exit(126); }
    if (chdir_to) {
      if (chdir(chdir_to) != 0) {
        int fail[2] = { errno, 1 };
        (void)!write(err_pipe[1], fail, sizeof fail);
        _exit(127);
      }
    }
    if (rlimit_cpu_set) {
      struct rlimit rl = { rlimit_cpu_val, RLIM_INFINITY };
      setrlimit(RLIMIT_CPU, &rl);
    }
    if (rlimit_as_set) {
      struct rlimit rl = { rlimit_as_val, RLIM_INFINITY };
      setrlimit(RLIMIT_AS, &rl);
    }
    if (pgroup == 1) {
      setpgid(0, 0);
    }
    else if (pgroup > 1) {
      setpgid(0, pgroup);
    }
    apply_redirect(0, in_fd);
    apply_redirect(1, out_fd);
    apply_redirect(2, err_fd);
    close_redirect_srcs(in_fd, out_fd, err_fd);
    execvp(prog, argv);
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
  int fail[2] = { 0, 0 };
  ssize_t got = read(err_pipe[0], fail, sizeof fail);
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
      do { r = waitpid(pid, &st, 0); } while (r < 0 && errno == EINTR);
      /* the reaped child is the last one waited for, so $? reads its
         exit 127, as it does under CRuby */
      if (r == pid) { sp_last_status = st; sp_last_pid = (int)pid; } }
    errno = fail[0];
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" :
                 errno == EACCES ? "Errno::EACCES" :
                 "SystemCallError",
                 sp_errf_path(errno, fail[1] == 1 && chdir_to ? chdir_to : prog));
  }
  return (sp_int)pid;
}

/* The wait itself lives in the scheduler (sp_sched_wait_child): a blocking
   waitpid answers for the OS worker, and a started green thread is pinned to
   its worker, so blocking here stalls the thread that may have to drain this
   child's output before it can exit (#4381). */
/* Process.wait / waitpid: the same wait as waitpid2, answering only the
   pid; the status goes to $? */
sp_int sp_process_waitpid(sp_int pid) {
  extern int sp_sched_wait_child(int pid, int *status);
  int status = 0;
  pid_t r = (pid_t)sp_sched_wait_child((int)pid, &status);
  if (r < 0) {
    if (errno == ECHILD) {
      sp_raise_cls("Errno::ECHILD", "No child processes");
    }
    sp_raise_cls("SystemCallError", sp_errf_errno("waitpid failed", errno));
  }
  sp_last_status = status;
  sp_last_pid = (int)r;
  return (sp_int)r;
}

sp_PolyArray *sp_process_waitpid2(sp_int pid) {
  extern int sp_sched_wait_child(int pid, int *status);   /* see the note at the top on this TU's includes */
  int status = 0;
  pid_t r = (pid_t)sp_sched_wait_child((int)pid, &status);
  if (r < 0) {
    if (errno == ECHILD) {
      sp_raise_cls("Errno::ECHILD", "No child processes");
    }
    sp_raise_cls("SystemCallError", sp_errf_errno("waitpid failed", errno));
  }
  /* $? follows the child this wait reaped, as it does after Kernel#system
     and a backtick; before this a waitpid2 left $? at whatever the last
     system call or backtick stored. */
  sp_last_status = status;
  sp_last_pid = (int)r;
  sp_PolyArray *pa = sp_PolyArray_new(); SP_GC_ROOT(pa);   /* the status object below is an allocation */
  sp_PolyArray_push(pa, sp_box_int((sp_int)r));
  /* Second element is a Process::Status instance wrapping (pid, status),
     not a raw int -- .signaled? / .termsig dispatch on the boxed cls_id. */
  sp_PolyArray_push(pa, sp_box_process_status(sp_process_status_new((sp_int)r, (sp_int)status)));
  return pa;
}
