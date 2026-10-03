/* sp_process_status.h -- Process::Status value type.
 *
 * Process.waitpid2 returns [pid, status] where status is a
 * Process::Status instance. This file provides the boxed runtime
 * type plus the small set of predicates and accessors CRuby
 * exposes on Process::Status.
 *
 * Layout: a single int (the raw waitpid(2) status word) plus the
 * pid of the child the status is from. Heap-allocated because
 * the per-instance id-tag is needed for the typecheck on
 * `result[1].signaled?` and the like; the struct itself is small
 * (16 bytes) so the allocation is cheap.
 */
#ifndef SP_PROCESS_STATUS_H
#define SP_PROCESS_STATUS_H

#include <stdint.h>
#include "sp_types.h"   /* sp_int */

typedef struct sp_ProcessStatus_s {
  sp_int pid;
  sp_int status;   /* the raw waitpid(2) status word */
} sp_ProcessStatus;

/* Boxed new: takes pid + raw status word. Returns a sp_ProcessStatus *
   on the GC heap. Raises TypeError on a non-Integer pid or negative
   status. */
sp_ProcessStatus *sp_process_status_new(sp_int pid, sp_int status);

/* `$?`: the status of the last child waited for (Kernel#system, a
   backtick, Process.wait*), or NULL -- nil -- before any. A new object on
   each read, carrying the raw status word as it was stored. */
sp_ProcessStatus *sp_last_process_status(void);

/* A reader's receiver: NULL is nil, which has none of the readers, so it
   raises NoMethodError ("undefined method 'exitstatus' for nil"). */
sp_ProcessStatus *sp_process_status_recv(sp_ProcessStatus *p, const char *meth);

/* to_s / inspect of a status, or of nil ("" / "nil") for NULL */
const char *sp_process_status_str(sp_ProcessStatus *p, int is_inspect);

/* Predicates. All return 0 or 1 except success_p, which is tri-state: -1
   means CRuby's nil (the process did not exit normally). */
int sp_process_status_exited_p(sp_int s);
int sp_process_status_signaled_p(sp_int s);
int sp_process_status_coredump_p(sp_int s);
int sp_process_status_success_p(sp_int s);

/* Accessors. CRuby semantics: exited? -> exitstatus, signaled? -> termsig;
   the un-applicable accessor answers nil, carried as SP_INT_NIL -- the
   nullable-Integer sentinel the rest of the runtime uses, so `p st.termsig`
   renders nil and `st.termsig.nil?` is true. */
sp_int sp_process_status_pid(sp_int s);
sp_int sp_process_status_exitstatus(sp_int s);
sp_int sp_process_status_termsig(sp_int s);

/* Render the status to a string for to_s / inspect. The result lives
   in a static buffer; the runtime copies it to a GC-heap string for
   return. */
const char *sp_process_status_to_s(sp_int pid, sp_int s, int is_inspect);

/* Equality: two Process::Status values are equal iff their status
   words are equal. pid is not compared. */
int sp_process_status_eq(sp_int a, sp_int b);

#endif
