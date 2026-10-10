/* sp_process_status.c -- Process::Status runtime.
 *
 * The runtime is deliberately tiny: a heap-allocated struct holding
 * pid + raw status, plus the W*() bit-field accessors from
 * <sys/wait.h> wrapped as sp_int-returning helpers. Spinel's
 * boxing layer (sp_box_obj) carries the type tag (cls_id =
 * SP_BUILTIN_PROCESS_STATUS) so the codegen can dispatch
 * `result[1].signaled?` to the right accessor at the call site.
 *
 * This TU is compiled into libspinel_rt.a; it does NOT need
 * spinel_rt.h. It does include sp_alloc.h for sp_gc_alloc.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include "sp_alloc.h"   /* sp_RbVal, sp_gc_alloc, sp_raise_cls */
#include "sp_process_status.h"

/* The W*() macros must be handed an int LVALUE, not a cast expression. macOS
   still defines them through the legacy `union wait` bridge --
   `#define _W_INT(w) (*(int *)&(w))` -- so the macro takes the ADDRESS of its
   argument, and `WIFEXITED((int)s)` fails to compile there with "cannot take
   the address of an rvalue". glibc casts instead of dereferencing, which is
   why this built on Linux. Each accessor below copies its sp_int argument
   into `int st` first and passes that. */

/* CRuby answers nil for exitstatus on a signaled status and for termsig on
   one that exited: an sp_oint. -1 is a real Integer here -- `p st.termsig`
   printed -1 rather than nil. */

/* ---- allocator ---- */

sp_ProcessStatus *sp_process_status_recv(sp_ProcessStatus *p, const char *meth) {
  extern void sp_nil_recv(const char *meth);   /* sp_str.c */
  if (!p) sp_nil_recv(meth);
  return p;
}

sp_ProcessStatus *sp_last_process_status(void) {
  extern int sp_last_status, sp_last_pid;   /* sp_system.c */
  if (!sp_last_pid) return NULL;
  sp_ProcessStatus *p = (sp_ProcessStatus *)sp_gc_alloc(sizeof(sp_ProcessStatus), NULL, NULL);
  if (!p) sp_raise_cls("NoMemoryError", "out of memory");
  p->pid = sp_last_pid;
  p->status = sp_last_status;
  return p;
}

sp_ProcessStatus *sp_process_status_new(sp_int pid, sp_int status) {
  if (status < 0 || status > 0xFFFF) {
    sp_raise_cls("ArgumentError", "invalid status word");
  }
  sp_ProcessStatus *p = (sp_ProcessStatus *)sp_gc_alloc(sizeof(sp_ProcessStatus), NULL, NULL);
  if (!p) sp_raise_cls("NoMemoryError", "out of memory");
  p->pid = pid;
  p->status = status;
  return p;
}

/* sp_box_process_status is declared in sp_alloc.h (with a forward
   struct decl, so sp_alloc.h does not need to include
   sp_process_status.h). The impl lives here so a TU that links
   against the runtime (e.g. sp_process.c) does not need to
   include sp_process_status.h to box a Status. */
sp_RbVal sp_box_process_status(sp_ProcessStatus *p) {
  return sp_box_obj(p, SP_BUILTIN_PROCESS_STATUS);
}

/* ---- predicates ---- */

int sp_process_status_exited_p(sp_int s) {
  int st = (int)s;
  return WIFEXITED(st) ? 1 : 0;
}

int sp_process_status_signaled_p(sp_int s) {
  int st = (int)s;
  return WIFSIGNALED(st) ? 1 : 0;
}

int sp_process_status_coredump_p(sp_int s) {
  int st = (int)s;
  if (!WIFSIGNALED(st)) return 0;
  return WCOREDUMP(st) ? 1 : 0;
}

/* Tri-state, because CRuby's is: true when the process exited with status 0,
   false when it exited with anything else, and NIL when it did not exit
   normally at all. -1 is that nil; the call site boxes it. */
int sp_process_status_success_p(sp_int s) {
  int st = (int)s;
  if (!WIFEXITED(st)) return -1;
  return WEXITSTATUS(st) == 0 ? 1 : 0;
}

/* ---- accessors ---- */

sp_int sp_process_status_pid(sp_int s) {
  /* The runtime only sees the status word (the pid travels alongside
     in the boxed struct). Returning the lower byte of the status is
     wrong -- the pid is a separate field. The pid is in the boxed
     struct, accessible via sp_ProcessStatus*.pid. To keep the
     signature simple the accessor takes the status word; the pid
     access is plumbed via a separate sp_process_status_pid_of helper
     that takes the boxed struct. */
  /* This path is unused: the codegen path uses
     sp_process_status_pid_of for the boxed struct. */
  (void)s;
  return 0;
}

sp_oint sp_process_status_exitstatus(sp_int s) {
  int st = (int)s;
  if (!WIFEXITED(st)) return sp_oint_nil();
  return sp_oint_of((sp_int)WEXITSTATUS(st));
}

sp_oint sp_process_status_termsig(sp_int s) {
  int st = (int)s;
  if (!WIFSIGNALED(st)) return sp_oint_nil();
  return sp_oint_of((sp_int)WTERMSIG(st));
}

/* ---- boxed-struct pid access ---- */
/* The codegen needs the pid of a boxed Process::Status. The runtime
   accessor signatures above take the raw status word, so the pid
   accessor goes through a separate helper that takes the boxed
   struct directly. */

sp_int sp_process_status_pid_of(sp_ProcessStatus *st) {
  return st->pid;
}

/* ---- equality ---- */

int sp_process_status_eq(sp_int a, sp_int b) {
  return a == b;
}

/* ---- to_s / inspect ----
 *
 * CRuby renders:
 *   exited normally:    "exit 0"            (or "exited" prefix for inspect)
 *   signaled:           "signal 9 (SIGKILL)" (or "signaled" prefix for inspect)
 *   unknown:            ""? (CRuby raises; we return a generic string)
 *
 * The output goes into a static buffer; the codegen copies it via
 * sp_str_dup_external when boxing the result as a String.
 */

static const char *sp_signal_name(int sig) {
  /* A short list. CRuby maps every signal; we cover the common ones
     so dvtm's signal detection reads naturally in log output. */
  switch (sig) {
    case  1: return "SIGHUP";
    case  2: return "SIGINT";
    case  3: return "SIGQUIT";
    case  4: return "SIGILL";
    case  6: return "SIGABRT";
    case  8: return "SIGFPE";
    case  9: return "SIGKILL";
    case 11: return "SIGSEGV";
    case 13: return "SIGPIPE";
    case 15: return "SIGTERM";
    case 24: return "SIGXCPU";
    default: return NULL;
  }
}

static SP_TLS char sp_status_buf[96];

const char *sp_process_status_to_s(sp_int pid, sp_int s, int is_inspect) {
  /* CRuby's pst_message: "pid N", then how the child ended */
  const char *prefix = is_inspect ? "#<Process::Status: " : "";
  const char *suffix = is_inspect ? ">" : "";
  int st = (int)s;
  if (WIFEXITED(st)) {
    snprintf(sp_status_buf, sizeof sp_status_buf,
             "%spid %lld exit %d%s", prefix, (long long)pid, WEXITSTATUS(st), suffix);
  }
  else if (WIFSIGNALED(st)) {
    int sig = WTERMSIG(st);
    const char *name = sp_signal_name(sig);
    const char *core = WCOREDUMP(st) ? " (core dumped)" : "";
    if (name) {
      snprintf(sp_status_buf, sizeof sp_status_buf,
               "%spid %lld %s (signal %d)%s%s", prefix, (long long)pid, name, sig, core, suffix);
    }
    else {
      snprintf(sp_status_buf, sizeof sp_status_buf,
               "%spid %lld signal %d%s%s", prefix, (long long)pid, sig, core, suffix);
    }
  }
  else {
    snprintf(sp_status_buf, sizeof sp_status_buf,
             "%spid %lld status 0x%llx%s", prefix, (long long)pid, (unsigned long long)s, suffix);
  }
  return sp_status_buf;
}

/* A fresh String each call: the rendering's static buffer is shared, and
   `[$?.to_s, $?.inspect]` read the second rendering twice. */
const char *sp_process_status_str(sp_ProcessStatus *p, int is_inspect) {
  if (!p) return sp_str_dup_external(is_inspect ? "nil" : "");
  return sp_str_dup_external(sp_process_status_to_s(p->pid, p->status, is_inspect));
}
