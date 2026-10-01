/* sp_fiber.h -- Fiber runtime surface.
 *
 * The sp_Fiber struct (a cooperative coroutine) is shared between the generated
 * translation unit and lib/sp_fiber.c, which holds the function bodies. The
 * context switch lives in sp_fiber_ctx.h (a portable asm switch on x86_64 /
 * aarch64, ucontext elsewhere) -- no <ucontext.h> here, so OpenBSD compiles.
 * `storage` is an opaque pointer to fiber-local variable storage. */
#ifndef SP_FIBER_H
#define SP_FIBER_H

#include "sp_gc.h"   /* sp_RbVal */
#include "sp_fiber_ctx.h"


/* A green thread's whole C stack: the build-time default, 256 KB (half of
   CRuby's machine stack for a fiber). The mapping is virtual space until a
   page is touched, so what a fiber costs is the depth it actually reaches,
   not this number; what it bounds is how deep a call chain, or how large a
   frame, a fiber can run (#4496). The size a program runs with is
   sp_fiber_stack_size, settable at run time: SPINEL_FIBER_STACK=<bytes>
   (K/M suffixes) in the environment wins, else the hint the generated
   program gives (an unoptimised build asks for more, its frames being many
   times -O2's), else this default. */
#ifndef SP_FIBER_STACK_SIZE
#define SP_FIBER_STACK_SIZE (256*1024)
#endif
extern size_t sp_fiber_stack_size;
/* the generated program's request, honoured unless the environment says otherwise */
void sp_fiber_stack_hint(size_t bytes);
/* The PROT_NONE guard below it. One page catches a frame that grows past the
   stack a page at a time, and nothing else: a C compiler that emits no stack
   probes (gcc on Linux, by default) lets a 51 KB frame step straight over a
   4 KB guard into whatever mapping sits below, and the program runs on with
   that mapping corrupted (#4496). The guard is virtual space only, never
   touched, so it is sized to the largest frame a build is likely to emit
   rather than to a page; a frame larger than this still has to be probed. */
#ifndef SP_FIBER_GUARD_SIZE
#define SP_FIBER_GUARD_SIZE (256*1024)
#endif

/* ThreadSanitizer support: the asm context switch swaps stacks without TSan's
   knowledge, so a fiber program reports spurious "unexpected memory mapping"
   or false races. Under -fsanitize=thread we give each fiber a TSan fiber
   handle and call __tsan_switch_to_fiber at every swap so TSan follows the
   cooperative switch. SP_TSAN is set only in that build (a separate archive),
   so the fiber struct carries the two extra fields only there -- the archive
   and the generated TU agree because both are compiled with -fsanitize=thread. */
#ifndef __has_feature           /* gcc lacks __has_feature; clang has it */
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define SP_TSAN 1
#endif

typedef struct sp_Fiber{sp_fiber_ctx ctx;sp_fiber_ctx caller_ctx;char*stack;size_t stack_size;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;void*exc_ctx;int raised;const char*raised_cls;const char*raised_msg;void*raised_obj;int inject;int inject_defer;const char*inj_cls;const char*inj_msg;void*inj_obj;void*storage;void*attrs;/* per-fiber attributes (Fiber.attr_accessor): a table of the fiber's own, never copied into a new fiber as `storage` is */void***saved_roots;int saved_nroots;int saved_roots_cap;struct sp_Fiber*fiber_next;struct sp_Fiber*fiber_prev;struct sp_Fiber*resumer;/* the fiber suspended inside #resume of this one, until it returns: the chain from the running fiber back to the root is what the collector roots; every other suspended fiber lives by reference (#4525) */const char*birth_file;sp_int birth_line;int pass_argc;unsigned owner;/* id + 1 of the thread that made it (#6110); 0: no owner */struct sp_Fiber*return_to;/* where it goes if it ends after a transfer: the innermost fiber still inside a resume (NULL: the thread's main fiber) */unsigned char thread_main;/* a Thread's own fiber (its main fiber) */unsigned char blocking;/* Fiber.new(blocking: true), or inside Fiber.blocking { } *//* how many values the first resume passed (-1: not known), so |a| can tell resume(1, 2) from resume([1, 2]) *//* the Fiber.new site, for #inspect */
#ifdef SP_TSAN
  void *tsan_fiber;                 /* __tsan fiber handle for this coroutine */
  struct sp_Fiber *caller_fiber;    /* who switched into us (the swap-out target) */
#endif
}sp_Fiber;

/* The currently-running fiber; the generated TU reads it directly for
   `Fiber.current` and as the implicit receiver of Fiber operations. Per-worker
   (SP_TLS) in the threaded build so each worker tracks the green thread it runs;
   the generated TU is compiled with the matching -DSP_THREADS. (The scheduler
   root fiber it is initialized to stays a shared static until per-worker root
   fibers land with the workers.) */
extern SP_TLS sp_Fiber *sp_fiber_current;

/* Adopt the calling OS thread's root fiber (its native scheduler stack) as its
   current coroutine, and read that per-worker root back. Worker 0 calls
   sp_fiber_worker_init from sp_sched_init; helper workers from their startup. In
   the single-threaded build the root is a shared static and these are inert. */
void      sp_fiber_worker_init(void);
sp_Fiber *sp_fiber_worker_root(void);
/* Switch from a green thread back to the fiber that ran it, without the
   checks Fiber#transfer makes (that fiber may be one the main thread resumed). */
void      sp_fiber_sched_switch(sp_Fiber *f);
/* Publish the running worker's shadow-stack roots into its current green thread
   (for a stop-the-world collector to mark while the worker is parked). */
void      sp_fiber_publish_current_roots(void);
/* Mark a fiber's published (saved) roots; used by the collector to reach a
   parked worker's root fiber, which is not on the global fiber list. */
void      sp_fiber_mark_roots(sp_Fiber *f);
void      sp_fiber_mark_chain(sp_Fiber *f);   /* f and every resumer waiting on it (#4525) */

/* Public Fiber API (called from the generated TU). */
sp_Fiber *sp_Fiber_new(void (*body)(sp_Fiber *));
sp_RbVal sp_Fiber_resume(sp_Fiber *f, sp_RbVal val);
sp_RbVal sp_Fiber_yield(sp_RbVal val);
unsigned sp_thread_owner_id(void);   /* lib/sp_sched.c */
sp_Fiber *sp_thread_main_fiber(void);   /* lib/sp_sched.c */
/* The fiber a green thread switches back to: the one that ran it on this
   worker, else the worker's root. */
sp_Fiber *sp_sched_home_fiber(void);   /* lib/sp_sched.c */
sp_RbVal sp_Fiber_resume_n(sp_Fiber *f, sp_RbVal val, int argc);
sp_RbVal sp_Fiber_transfer_n(sp_Fiber *f, sp_RbVal val, int argc);
sp_RbVal sp_Fiber_transfer(sp_Fiber *f, sp_RbVal val);
/* Like sp_Fiber_transfer, but captures f's unhandled termination exception into
   the out-params (for the thread scheduler) instead of re-raising it. When
   f's thread stopped inside a Fiber it resumed, `at` is that fiber and the
   switch goes there (NULL: to f). */
sp_RbVal sp_Fiber_transfer_catch(sp_Fiber *f, sp_Fiber *at, sp_RbVal val, int *out_raised,
                                 const char **out_cls, const char **out_msg, void **out_obj);
/* Fiber#raise: inject an exception at the fiber's suspension point (or at entry
   for an unstarted fiber). cls/msg describe a class+message; obj, when non-NULL,
   is a pre-built exception object to raise instead. Returns the next yielded
   value, or re-raises in the caller if the fiber does not handle it. */
sp_RbVal sp_Fiber_raise(sp_Fiber *f, const char *cls, const char *msg, void *obj);
/* Fiber#kill: terminate the fiber (running its ensure blocks); returns it. */
sp_Fiber *sp_Fiber_kill(sp_Fiber *f);
sp_bool sp_Fiber_alive(sp_Fiber *f);
sp_bool sp_Fiber_blocking_p(sp_Fiber *f);   /* #blocking? */
sp_Fiber *sp_Fiber_at(sp_Fiber *f, const char *file, sp_int line);
const char *sp_Fiber_inspect(sp_Fiber *f);   /* #inspect / #to_s */
sp_RbVal sp_Fiber_storage_get(sp_Fiber *f, sp_sym k);
void sp_Fiber_storage_set(sp_Fiber *f, sp_sym k, sp_RbVal v);
void sp_Fiber_storage_put(sp_Fiber *f, sp_sym k, sp_RbVal v);
sp_int sp_Fiber_storage_len(sp_Fiber *f);
sp_sym sp_Fiber_storage_key(sp_Fiber *f, sp_int i);
sp_RbVal sp_Fiber_storage_val(sp_Fiber *f, sp_int i);
void sp_Fiber_storage_clear(sp_Fiber *f);
void sp_Fiber_storage_empty(sp_Fiber *f);
sp_RbVal sp_Fiber_attr_get(sp_Fiber *f, sp_sym k);
void sp_Fiber_attr_set(sp_Fiber *f, sp_sym k, sp_RbVal v);
/* Reached from sp_re_mark_globals in the generated TU during a GC pass. */
void sp_mark_fiber_root_storage(void);

/* Thread #kill / #raise: the scheduler queues a pending inject on a target
   fiber, then fires it when that thread next runs (in its own context). */
void sp_fiber_set_raise_inject(sp_Fiber *f, const char *cls, const char *msg, void *obj);
void sp_fiber_set_kill_inject(sp_Fiber *f);
/* ConditionVariable waits defer async delivery until their mutex is reacquired. */
void sp_fiber_defer_inject(void);
void sp_fiber_undefer_inject(void);
void sp_fiber_fire_inject_if_pending(void);
int  sp_fiber_inject_pending(sp_Fiber *f);   /* lock-free acquire peek */
SP_NORETURN void sp_fiber_raise_kill_self(void);

/* A C stack that ran out is CRuby's SystemStackError, and a program that
   rescues it goes on running. The raise cannot live here: the exception
   stack is thread-local state in the generated translation unit, which this
   archive is compiled without. The generated TU installs this hook, and the
   fault handler calls it instead of reporting and dying -- on the alternate
   signal stack, so the raise runs on memory the overflow did not touch.
   NULL (nothing installed, or no handler armed) keeps the old report. */
extern void (*sp_stack_overflow_raise_fn)(void);
void sp_sig_unblock(int a, int b);
void sp_stack_guard_init(void);
/* Run the program body on a stack this library maps (see sp_main_stack_run);
   sp_main_stack_hint asks for a size, SPINEL_MAIN_STACK in the environment
   wins over it. */
void sp_main_stack_hint(size_t bytes);
void sp_main_stack_run(void (*body)(void));

/* The running thread's own stack, for the fault handler's overflow test: a
   fault just below `lo` is this stack growing past its end. Recorded per
   thread when the handler is armed. */
extern SP_TLS char *sp_thread_stack_lo;
extern SP_TLS char *sp_thread_stack_hi;

#endif
