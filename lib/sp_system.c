/* sp_system.c -- system()/backtick support in libspinel_rt.a.
 * See sp_system.h.
 *
 * Self-contained (libc + OS process API only); does not include
 * spinel_rt.h, so it carries its own sp_bool/TRUE/FALSE locally. */
#include "sp_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#if defined(_WIN32)
#include <spawn.h>
#endif

typedef int sp_bool;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

int sp_last_status = 0;
int sp_last_pid = 0;


#if !defined(_WIN32)
extern char **environ;
#endif

static int sp_system_run(const char *prog, int argc, const char *const *argv, char **envp);

int sp_system_args(int argc, const char *const *argv) {
  return sp_system_envp(argc, argv, NULL);
}

/* CRuby's rule for a one-String command line (rb_exec_fillarg): the shell
   runs it when it holds a shell meta character, when its first word sets a
   variable (NAME=value), or when its first word is a word the shell itself
   answers (a reserved word or a special builtin); otherwise its words are
   the program and its arguments, exec'd with no shell. */
int sp_cmdline_shell_p(const char *line) {
  static const char *const sh_words[] = {
    "!", ".", ":", "break", "case", "continue", "do", "done", "elif", "else",
    "esac", "eval", "exec", "exit", "export", "fi", "for", "if", "in",
    "readonly", "return", "set", "shift", "then", "times", "trap", "unset",
    "until", "while", NULL };
  const char *first = NULL;
  size_t first_len = 0;
  int first_done = 0;
  for (const char *p = line; *p; p++) {
    if (*p == ' ' || *p == '\t') {
      if (first && !first_done) { first_len = (size_t)(p - first); first_done = 1; }
      continue;
    }
    if (!first) first = p;
    if (strchr("*?{}[]<>()~&|\\$;'`\"\n#", *p)) return 1;
    if (!first_done && *p == '=') return 1;
  }
  if (!first) return 0;
  if (!first_done) first_len = strlen(first);
  for (int i = 0; sh_words[i]; i++)
    if (strlen(sh_words[i]) == first_len && memcmp(sh_words[i], first, first_len) == 0) return 1;
  return 0;
}

/* The words of a command line the shell does not run, split at spaces and
   tabs: a NULL-terminated argv in one malloc block (free it once), with
   *argc_out set; NULL when out of memory. */
char **sp_cmdline_words(const char *line, int *argc_out) {
  size_t len = strlen(line);
  size_t nw = 0;
  for (const char *p = line; *p; ) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    nw++;
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  char **av = (char **)malloc(sizeof(char *) * (nw + 1) + len + 1);
  if (!av) return NULL;
  char *buf = (char *)(av + nw + 1);
  memcpy(buf, line, len + 1);
  size_t k = 0;
  for (char *p = buf; *p; ) {
    while (*p == ' ' || *p == '\t') *p++ = '\0';
    if (!*p) break;
    av[k++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  av[k] = NULL;
  *argc_out = (int)k;
  return av;
}

/* envp, when not NULL, is the child's whole environment in place of this
   process's (system with an environment Hash, lib/sp_process.c).
   1 when the command exits 0, 0 for another exit, -1 when it cannot run
   at all (no such program, no permission, no fork): CRuby's true, false
   and nil. A command the shell runs always runs; the shell's own exit 127
   for a missing program is a false. */
int sp_system_envp(int argc, const char *const *argv, char **envp) {
  return sp_system_run(NULL, argc, argv, envp);
}

/* system([program, argv0], ...): program runs with argv as its arguments,
   argv[0] being the argv0 it sees; the shell never runs it and its name is
   not split into words, as under CRuby */
int sp_system_prog_envp(const char *prog, int argc, const char *const *argv, char **envp) {
  return sp_system_run(prog, argc, argv, envp);
}

/* Start a child: the shell runs line when line is not NULL, else prog runs
   with argv (execvp, which searches the PATH of envp when envp is set).
   envp NULL keeps this process's environment. out_fd >= 0 becomes the
   child's stdout, and close_fd >= 0 is closed in the child. Answers the
   child's pid; 0 when its exec failed, with *exec_errno set, the child
   reaped and its exit 127 in $? (CRuby's answer for a program that cannot
   run); -1 when no child could be made, with $? -1. */
int sp_system_start(const char *line, const char *prog, char *const *argv, char **envp,
                    int out_fd, int close_fd, int *exec_errno) {
  *exec_errno = 0;
#if defined(_WIN32)
  /* posix_spawn where there is no fork (see sp_process.c) */
  pid_t pid = 0;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (close_fd >= 0) posix_spawn_file_actions_addclose(&fa, close_fd);
  if (out_fd >= 0 && out_fd != 1) {
    posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    posix_spawn_file_actions_addclose(&fa, out_fd);
  }
  int rc;
  if (line) {
    char *av[] = { (char *)"sh", (char *)"-c", (char *)line, NULL };
    rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, av, envp ? envp : environ);
  }
  else rc = posix_spawnp(&pid, prog, &fa, NULL, argv, envp ? envp : environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) { *exec_errno = rc; sp_last_status = 127 << 8; return 0; }
  return (int)pid;
#else
  /* The child writes its exec errno here when the exec fails. Both ends
     are close-on-exec, so a started program closes the write end and the
     parent reads nothing (as Process.spawn tells its failure, sp_process.c). */
  extern int sp_io_make_pipe(int fds[2]);
  int err_pipe[2];
  if (sp_io_make_pipe(err_pipe) < 0) { sp_last_status = -1; return -1; }
  extern void sp_fork_exec_lock(void), sp_fork_exec_unlock(void);
  sp_fork_exec_lock();
  pid_t pid = fork();
  if (pid != 0) sp_fork_exec_unlock();
  if (pid < 0) {
    close(err_pipe[0]); close(err_pipe[1]);
    sp_last_status = -1;
    return -1;
  }
  if (pid == 0) {
    close(err_pipe[0]);
    if (close_fd >= 0) close(close_fd);
    if (out_fd >= 0) {
      if (out_fd != 1) { dup2(out_fd, 1); close(out_fd); }
      else fcntl(1, F_SETFD, 0);
    }
    /* the child's own copy of the pointer: execl and execvp pass it on, and
       execvp searches the PATH it holds, as CRuby looks the command up in
       the child's environment */
    if (envp) environ = envp;
    if (line) execl("/bin/sh", "sh", "-c", line, (char *)NULL);
    else if (prog && *prog) execvp(prog, argv);
    else errno = ENOENT;   /* a line of no words: CRuby's exec of "" */
    int e = errno;
    (void)!write(err_pipe[1], &e, sizeof e);
    _exit(127);
  }
  close(err_pipe[1]);
  int e = 0;
  ssize_t got;
  /* (parked until the child execs or fails: a worker in read(2) never
     reaches a safepoint, and a collection would wait on it) */
  extern void sp_io_wait_fd_readable(int fd);
  sp_io_wait_fd_readable(err_pipe[0]);
  do { got = read(err_pipe[0], &e, sizeof e); } while (got < 0 && errno == EINTR);
  close(err_pipe[0]);
  if (got > 0) {
    /* the child could not exec and is exiting 127: reap it here, so $?
       reads its 127 as under CRuby and no zombie is left */
    int st = 0; pid_t r;
    do { r = waitpid(pid, &st, 0); } while (r < 0 && errno == EINTR);
    if (r == pid) { sp_last_status = st; sp_last_pid = (int)pid; }
    else sp_last_status = 127 << 8;
    *exec_errno = e;
    return 0;
  }
  return (int)pid;
#endif
}

/* prog NULL: argv[0] is the program, and a lone command line goes through
   CRuby's shell rule */
static int sp_system_run(const char *prog, int argc, const char *const *argv, char **envp) {
  if (argc <= 0 || argv == NULL || argv[0] == NULL) {
    sp_last_status = -1;
    return -1;
  }
  int via_shell = !prog && argc == 1 && sp_cmdline_shell_p(argv[0]);
  char **words = NULL;
  if (!prog && argc == 1 && !via_shell) {
    int wn = 0;
    words = sp_cmdline_words(argv[0], &wn);
    if (!words) { sp_last_status = -1; return -1; }
    /* a line of no words still forks, and its child's exec of "" fails,
       so $? is that child's exit 127, as under CRuby */
    if (wn > 0) {
      argc = wn;
      argv = (const char *const *)words;
    }
  }
  fflush(NULL);
  int exec_errno = 0;
  int pid = sp_system_start(via_shell ? argv[0] : NULL, prog ? prog : argv[0],
                            (char *const *)argv, envp, -1, -1, &exec_errno);
  free(words);
  if (pid <= 0) return -1;
  /* Same rule as Process.waitpid2 (#4381): a blocking wait answers for the OS
     worker, and a started green thread is pinned to its worker, so it would
     stall the very thread that may have to drain this child's output before it
     can exit. sp_sched_wait_child polls and hands the scheduler back while any
     other thread is alive, and keeps the blocking wait when none is. */
  int status = 0;
  {
    extern int sp_sched_wait_child(int pid, int *status);
    if (sp_sched_wait_child((int)pid, &status) < 0) { sp_last_status = -1; return FALSE; }
  }
  sp_last_status = status;
  sp_last_pid = (int)pid;
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? TRUE : FALSE;
}
