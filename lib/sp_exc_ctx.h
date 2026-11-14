#ifndef SP_EXC_CTX_H
#define SP_EXC_CTX_H
/* sp_exc_ctx.h -- the per-fiber exception/catch handler context (#1474) and
 * the proc-return home node whose chain it carries.
 *
 * Types only, plus the three context operations that touch nothing else
 * (sp_exc_ctx_new/free/mark, lib/sp_exc.c). The handler stacks themselves are
 * defined in lib/sp_exc.c and declared in spinel_rt.h. */
#include "sp_setjmp.h"
#include "sp_types.h"   /* sp_int */
#include "sp_gc.h"      /* sp_RbVal */
#include "sp_inspect.h" /* sp_poly_recur_frame */
#ifdef SP_CEXT
#include "sp_cext.h"
#endif

/* Depth of the begin/rescue/ensure handler stack and of the catch/throw
   stack. The stacks themselves are defined in lib/sp_exc.c, so the depth is
   the one the runtime archive was built with: -DSP_EXC_STACK_MAX=<n> (or
   SP_CATCH_STACK_MAX) goes to the archive's build, and a program compiled
   with another value fails to link (sp_exc_stacks_<exc>_<catch>). */
#ifndef SP_EXC_STACK_MAX
#define SP_EXC_STACK_MAX 64
#endif
#ifndef SP_CATCH_STACK_MAX
#define SP_CATCH_STACK_MAX 64
#endif
#define SP_EXC_DEPTHS_SYM_(e, c) sp_exc_stacks_##e##_##c
#define SP_EXC_DEPTHS_SYM(e, c) SP_EXC_DEPTHS_SYM_(e, c)
extern const int SP_EXC_DEPTHS_SYM(SP_EXC_STACK_MAX, SP_CATCH_STACK_MAX);
/* What an ensure body runs for while the unwind it interrupts is held */
enum { SP_UNWIND_NONE, SP_UNWIND_PROCRET, SP_UNWIND_THROW, SP_UNWIND_BREAK, SP_UNWIND_EXIT };

/* A non-lambda proc's home method: a node on that method's C stack, linked
   onto the per-fiber chain sp_proc_ret_head (see the proc-return machinery in
   spinel_rt.h). */
typedef struct sp_proc_home {
  jmp_buf jb;                 /* the home method's setjmp target (on its C stack) */
  sp_RbVal val;               /* the in-flight return value (nil until delivered) */
  int exc_top;                /* sp_exc_top at the method's entry */
  int catch_top;              /* sp_catch_top at the method's entry */
  int brk_top;                /* sp_brk_top at the method's entry: the break scopes a
                                 return longjmps past are dropped at the landing */
  int recur_mark;             /* walk-path depth at the method's entry (see sp_poly_recur_mark) */
  sp_int id;                 /* fresh id captured by the home's returning procs */
  struct sp_proc_home *prev;  /* enclosing home, forming the per-fiber chain */
} sp_proc_home;

/* The live prefix of every handler array, saved into the outgoing fiber's
   context and loaded from the incoming one's at each switch. */
typedef struct {
#ifdef SP_CEXT
  sp_cext_arena_mark *cext_marks;
  sp_cext_arena_context cext_arena;
#endif
  jmp_buf *es; const char **em; const char **ec; void **eo; int en, ecap;
  jmp_buf *cs; const char **ct; unsigned char *ctk; sp_RbVal *cv; int *cet;  int cn, ccap;
  jmp_buf *bs; sp_RbVal *bv; sp_int *bser; int *bet;     int bn, bcap;  /* break scopes */
  sp_proc_home *prhead;  /* this fiber's proc-return chain head (nodes on its C stack) */
  int uk, ut, ue; sp_proc_home *uh;  /* transient unwind state (in flight only while running ensures) */
  void **shand; int rn, rcap;        /* sp_exc_handling prefix [0..sp_rescue_sp) */
  int hooked;                        /* sp_at_exit_inherited */
  void *pcause;                      /* sp_pending_cause */
  void *icause;                      /* sp_inflight_cause: the exception an ensure body runs for */
  sp_poly_recur_frame *rrf; int rrn, rrcap;  /* sp_poly_recur_stack prefix [0..sp_poly_recur_top) */
  int *rrem, *rrcm, *rrbm;           /* the walk-path marks of the exception, catch and break arms */
  int *erm, *ersm, *crm;             /* the GC-root and rescue-stack watermarks of the exception
                                        arms and the GC-root watermark of the catch arms: what a
                                        handler restores sp_gc_nroots / sp_rescue_sp to. They are
                                        per worker like the arms, so without travelling with the
                                        context a green thread that parked inside a begin and
                                        resumed after another fiber's arm sat at the same index
                                        restored the OTHER fiber's watermark: a raise's dead root
                                        stayed on the list and the next collection read a stack
                                        slot that was no longer a string (#4546) */
  int live;                          /* 1 from sp_exc_ctx_load until the next sp_exc_ctx_save:
                                        the fiber is running and its handler state lives in
                                        the worker's globals, so what is here is a stale copy
                                        of its last switch-out. A worker parked at a safepoint
                                        or out in a blocking native call publishes the live
                                        state itself (sp_safepoint_publish_hook); the stale
                                        copy's proc-return chain names nodes on C frames that
                                        have since returned, and a collection that walked it
                                        read whatever the stack holds there now */
} sp_exc_ctx_t;

void *sp_exc_ctx_new(void);
void sp_exc_ctx_free(void *p);
void sp_exc_ctx_mark(void *p);   /* GC: a suspended fiber's carried exc objects */

/* The stacks the two helpers below read, declared (and documented) with the
   rest in spinel_rt.h and defined in lib/sp_exc.c */
extern SP_TLS volatile int sp_exc_top;
extern SP_TLS int sp_poly_recur_mark[SP_EXC_STACK_MAX];
extern SP_TLS void *sp_exc_handling[SP_EXC_STACK_MAX];
extern SP_TLS int sp_rescue_sp;
extern SP_TLS int sp_at_exit_inherited;
#ifdef SP_CEXT
extern SP_TLS sp_cext_arena_mark sp_exc_cextmark[SP_EXC_STACK_MAX];
#endif
/* Called immediately before every longjmp into sp_exc_stack: the landing frame
   is the one on top, and the walk frames pushed since it armed are about to be
   jumped over, so give the path back the depth that frame recorded. */
static inline void sp_poly_recur_unwind(void) {
#ifdef SP_CEXT
  if (sp_exc_top > 0) sp_cext_arena_restore(sp_exc_cextmark[sp_exc_top - 1]);
#endif
  if (sp_exc_top > 0) sp_poly_recur_pop(sp_poly_recur_mark[sp_exc_top - 1]);
}
#define sp_cur_handled() (sp_rescue_sp > 0 ? sp_exc_handling[sp_rescue_sp-1] : NULL)
SP_NORETURN SP_COLD void sp_stack_too_deep(void);   /* lib/sp_exc.c */
/* Guard an exception-frame arm. Every arm stores into sp_exc_rootmark
   [sp_exc_top] (or sp_exc_msg[sp_exc_top]) before it bumps sp_exc_top, so what
   the check must precede is that first indexed store, not the ++; it opens the
   frame-arming sequence at every arm the codegen emits. The depth is not a
   lexical property: a method whose body is a begin/rescue keeps its frame armed
   across the calls that body makes, so ordinary recursion 65 deep reaches the
   end of the array just as 65 nested begins do. Inside a fiber body, and inside
   a thread's, one slot is already spent on the frame the runtime arms around it
   (sp_exc_arm below), so 63 of the body's own is the last that fits. */
static inline void sp_exc_check_depth(void) {
  if (SP_UNLIKELY(sp_exc_top >= SP_EXC_STACK_MAX)) sp_stack_too_deep();
#ifdef SP_CEXT
  sp_exc_cextmark[sp_exc_top] = sp_cext_arena_snapshot();
#endif
  sp_poly_recur_mark[sp_exc_top] = sp_poly_recur_top;
}
#endif
