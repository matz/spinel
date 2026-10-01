/* sp_fiber.c -- Fiber runtime bodies (POSIX ucontext).
 * See sp_fiber.h. The collector roots, sp_gc_alloc, and sp_raise_cls live
 * in the generated TU and are reached by name; fiber-local storage is
 * self-contained here. */
/* pthread_getattr_np (sp_thread_stack_bounds, below) is a GNU extension:
   glibc declares it only under _GNU_SOURCE, and clang 16+ (and gcc 14+)
   make the implicit declaration an error rather than a warning. Before
   the first include, as sp_cold.c does for statx. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sp_fiber.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* OpenBSD requires the stack a coroutine runs on to be MAP_STACK, else the
   first push faults. Linux defines MAP_STACK as a (currently) no-op; keep it
   portable. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0   /* macOS, the BSDs: no overcommit accounting to opt out of */
#endif
#ifndef MAP_STACK
#define MAP_STACK 0
#endif

/* A PROT_NONE guard page below the usable stack: the coroutine stack grows down
   from base+size toward base (see sp_ctx_make), so an overflow runs into the
   guard and faults cleanly instead of silently corrupting whatever mapping sits
   below. f->stack points at the mmap base (guard + stack); the usable region the
   context switch runs on starts sp_fiber_guard() bytes in. Sized to one page
   (queried so 16K/64K-page arm64 hosts stay aligned). */
size_t sp_fiber_stack_size = 0;   /* 0: not decided yet (see sp_fiber_stack_bytes) */
static int sp_fiber_stack_from_env = 0;
static size_t sp_fiber_stack_parse(const char *e) {
  char *end = NULL;
  unsigned long long v = strtoull(e, &end, 10);
  if (end == e) return 0;
  if (*end == 'k' || *end == 'K') v <<= 10;
  else if (*end == 'm' || *end == 'M') v <<= 20;
  return (size_t)v;
}
/* the size the next fiber's stack is mapped with, decided once */
static size_t sp_fiber_stack_bytes(void) {
  if (!sp_fiber_stack_size) {
    const char *e = getenv("SPINEL_FIBER_STACK");
    size_t v = e && *e ? sp_fiber_stack_parse(e) : 0;
    if (v) sp_fiber_stack_from_env = 1;
    sp_fiber_stack_size = v ? v : (size_t)SP_FIBER_STACK_SIZE;
  }
  /* whole pages, at least one */
  long p = sysconf(_SC_PAGESIZE); size_t page = p > 0 ? (size_t)p : 4096;
  size_t sz = (sp_fiber_stack_size + page - 1) / page * page;
  return sz < page ? page : sz;
}
void sp_fiber_stack_hint(size_t bytes) {
  (void)sp_fiber_stack_bytes();   /* reads the environment first */
  if (!sp_fiber_stack_from_env && bytes) sp_fiber_stack_size = bytes;
}

static size_t sp_fiber_guard(void) {
  static size_t g = 0;
  if (!g) {
    long p = sysconf(_SC_PAGESIZE);
    size_t page = p > 0 ? (size_t)p : 4096;
    /* SP_FIBER_GUARD_SIZE rounded up to whole pages (see sp_fiber.h) */
    g = ((size_t)SP_FIBER_GUARD_SIZE + page - 1) / page * page;
    if (g < page) g = page;
  }
  return g;
}


/* ---- Portable coroutine context switch (replaces swapcontext) ----
   sp_ctx_swap saves the callee-saved registers onto the *current* stack, stows
   the resulting stack pointer in *from, loads *to's stack pointer, and pops the
   callee-saved registers (the very first switch into a coroutine pops the zeroed
   slots sp_ctx_make laid out and `ret`s into the trampoline). It lives in .text
   (W^X-safe) and issues no syscall. See sp_fiber_ctx.h for sp_ctx_make and the
   stack layout it must mirror. */
#if SP_FIBER_ASM
#if defined(__APPLE__)
  #define SP_CTX_SYM "_sp_ctx_swap"
#else
  #define SP_CTX_SYM "sp_ctx_swap"
#endif
#if defined(__x86_64__)
__asm__(
  ".text\n"
  ".globl " SP_CTX_SYM "\n"
  SP_CTX_SYM ":\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
  "  movq %rsp, (%rdi)\n"        /* from->sp = rsp                      */
  "  movq (%rsi), %rsp\n"        /* rsp = to->sp                        */
  "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
  "  ret\n"
);
#elif defined(__aarch64__)
__asm__(
  ".text\n"
  ".globl " SP_CTX_SYM "\n"
  SP_CTX_SYM ":\n"
  /* save x19..x30 (12*8) + d8..d15 (8*8) = 160 bytes onto the current stack */
  "  stp x19, x20, [sp, #-160]!\n"
  "  stp x21, x22, [sp, #16]\n"
  "  stp x23, x24, [sp, #32]\n"
  "  stp x25, x26, [sp, #48]\n"
  "  stp x27, x28, [sp, #64]\n"
  "  stp x29, x30, [sp, #80]\n"
  "  stp d8,  d9,  [sp, #96]\n"
  "  stp d10, d11, [sp, #112]\n"
  "  stp d12, d13, [sp, #128]\n"
  "  stp d14, d15, [sp, #144]\n"
  "  mov x2, sp\n"
  "  str x2, [x0]\n"             /* from->sp = sp */
  "  ldr x2, [x1]\n"            /* sp = to->sp   */
  "  mov sp, x2\n"
  "  ldp d14, d15, [sp, #144]\n"
  "  ldp d12, d13, [sp, #128]\n"
  "  ldp d10, d11, [sp, #112]\n"
  "  ldp d8,  d9,  [sp, #96]\n"
  "  ldp x29, x30, [sp, #80]\n"
  "  ldp x27, x28, [sp, #64]\n"
  "  ldp x25, x26, [sp, #48]\n"
  "  ldp x23, x24, [sp, #32]\n"
  "  ldp x21, x22, [sp, #16]\n"
  "  ldp x19, x20, [sp], #160\n"
  "  ret\n"
);
#endif
#else  /* ucontext fallback */
void sp_ctx_swap(sp_fiber_ctx *from, sp_fiber_ctx *to) { swapcontext(&from->uc, &to->uc); }
#endif

/* ---- Reached by name in the generated TU ---- */
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);

/* Per-fiber exception/catch handler context (#1474): defined in the generated
   TU (spinel_rt.h), reached here by name. A fiber's begin/rescue handlers and
   catch tags are saved/restored around each context switch so a raise/throw
   can't longjmp into a suspended fiber's stack frame, and an unhandled fiber
   raise is re-raised in the resumer's context rather than crossing stacks. */
#include <setjmp.h>
void *sp_exc_ctx_new(void);
void sp_exc_ctx_free(void *p);
void sp_exc_ctx_save(void *p);
void sp_exc_ctx_load(void *p);
void sp_exc_ctx_mark(void *p);
void sp_exc_arm(jmp_buf b);
void sp_exc_disarm(void);
const char *sp_exc_cur_cls(void);
const char *sp_exc_cur_msg(void);
void *sp_exc_cur_obj(void);
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);

static inline sp_RbVal sp_box_nil(void) { sp_RbVal r; r.tag = SP_TAG_NIL; r.cls_id = 0; r.v.i = 0; return r; }
static void sp_raise(const char *msg) {SP_GC_ROOT_STR(msg); sp_raise_cls("RuntimeError", msg); }

/* Fiber-local variable storage (Fiber#[] / Fiber#[]=). A small sym->RbVal
   map, GC-allocated so the collector marks its values via the scan hook;
   self-contained here rather than borrowing the runtime's sym_poly_hash so
   the runtime sym_poly_hash stays collector-private to the generated TU. */
typedef struct { sp_sym *keys; sp_RbVal *vals; sp_int len, cap; } sp_FiberStore;
static void sp_FiberStore_scan(void *p) { sp_FiberStore *s = (sp_FiberStore *)p; for (sp_int i = 0; i < s->len; i++) sp_mark_rbval(s->vals[i]); }
static void sp_FiberStore_fin(void *p) { sp_FiberStore *s = (sp_FiberStore *)p; free(s->keys); free(s->vals); }
static sp_FiberStore *sp_FiberStore_new(void) {
  sp_FiberStore *s = (sp_FiberStore *)sp_gc_alloc(sizeof(sp_FiberStore), sp_FiberStore_fin, sp_FiberStore_scan);
  s->cap = 8; s->len = 0;
  s->keys = (sp_sym *)malloc(sizeof(sp_sym) * s->cap);
  s->vals = (sp_RbVal *)malloc(sizeof(sp_RbVal) * s->cap);
  if (!s->keys || !s->vals) {
    /* If one side succeeded, release it now rather than waiting for the
       finalizer -- and NULL both so that finalizer's free stays a no-op. */
    free(s->keys); free(s->vals);
    s->keys = NULL; s->vals = NULL;
    sp_raise_cls("NoMemoryError", "failed to allocate fiber storage");
  }
  return s;
}
static sp_RbVal sp_FiberStore_get(sp_FiberStore *s, sp_sym k) {
  for (sp_int i = 0; i < s->len; i++) if (s->keys[i] == k) return s->vals[i];
  return sp_box_nil();
}
static void sp_FiberStore_set(sp_FiberStore *s, sp_sym k, sp_RbVal v) {SP_GC_ROOT_RBVAL(v);SP_GC_ROOT(s); sp_gc_wb((void*)s);
  for (sp_int i = 0; i < s->len; i++) if (s->keys[i] == k) { s->vals[i] = v; return; }
  if (s->len == s->cap) {
    /* Grow each array into a temp and commit only on success, so a failed
       realloc leaves the store intact (the old buffer survives) instead of
       NULL-deref'ing on the write below or double-freeing a moved buffer. */
    sp_int nc = s->cap * 2;
    sp_sym *nk = (sp_sym *)realloc(s->keys, sizeof(sp_sym) * nc);
    if (!nk) sp_raise_cls("NoMemoryError", "failed to grow fiber storage");
    s->keys = nk;
    sp_RbVal *nv = (sp_RbVal *)realloc(s->vals, sizeof(sp_RbVal) * nc);
    if (!nv) sp_raise_cls("NoMemoryError", "failed to grow fiber storage");
    s->vals = nv; s->cap = nc;
  }
  s->keys[s->len] = k; s->vals[s->len] = v; s->len++;
}
/* drop key k, keeping the order of the rest */
static void sp_FiberStore_delete(sp_FiberStore *s, sp_sym k) {
  for (sp_int i = 0; i < s->len; i++) {
    if (s->keys[i] != k) continue;
    for (sp_int j = i + 1; j < s->len; j++) {
      s->keys[j - 1] = s->keys[j];
      s->vals[j - 1] = s->vals[j];
    }
    s->len--;
    return;
  }
}
static sp_FiberStore *sp_FiberStore_dup(sp_FiberStore *o) {SP_GC_ROOT(o);
  sp_FiberStore *s = sp_FiberStore_new();
  for (sp_int i = 0; i < o->len; i++) sp_FiberStore_set(s, o->keys[i], o->vals[i]);
  return s;
}

/* ThreadSanitizer fiber instrumentation (see sp_fiber.h SP_TSAN). Each fiber
   gets a __tsan fiber handle; before every context switch we tell TSan which
   fiber will run next so its happens-before tracking follows the cooperative
   switch instead of choking on the asm stack swap. No-ops when not building
   under -fsanitize=thread. */
#ifdef SP_TSAN
extern void *__tsan_get_current_fiber(void);
extern void *__tsan_create_fiber(unsigned flags);
extern void  __tsan_destroy_fiber(void *fiber);
extern void  __tsan_switch_to_fiber(void *fiber, unsigned flags);
#define SP_TSAN_SET_CALLER(f, who) ((f)->caller_fiber = (who))
#define SP_TSAN_SWITCH(to)         do { sp_Fiber *_st = (to); if (_st && _st->tsan_fiber) __tsan_switch_to_fiber(_st->tsan_fiber, 0); } while (0)
#else
#define SP_TSAN_SET_CALLER(f, who) ((void)0)
#define SP_TSAN_SWITCH(to)         ((void)0)
#endif

/* The root fiber is a static, not a GC allocation -- but `Fiber.current`
   hands it to user code, where a rooted local makes the collector mark
   it. Lay a 0xfd skip byte directly before it so sp_gc_mark's tag-byte
   protocol bails out instead of treating .bss as a GC header. The guard
   array is exactly one alignment unit, so its last byte always directly
   precedes `root` (no compiler padding can slip in between). */
/* In the threaded build the root fiber is per-worker (SP_TLS): each OS worker's
   native scheduler stack is its own root coroutine, so the green thread running
   on a worker transfers back to *that* worker's root. The 0xfd guard byte still
   directly precedes `root` in each thread's TLS block, so sp_gc_mark's tag-byte
   protocol skips it there too. sp_fiber_current cannot be statically initialized
   to &sp_fiber_root once the root is thread-local (that address is not a
   constant), so each worker sets it via sp_fiber_worker_init before running any
   fiber; the single-threaded build keeps the constant static init unchanged
   (byte-identical). */
static SP_TLS struct { char guard[_Alignof(sp_Fiber)]; sp_Fiber root; } sp_fiber_root_box
    = { .guard = { [_Alignof(sp_Fiber) - 1] = (char)0xfd }, .root = {0} };
#define sp_fiber_root (sp_fiber_root_box.root)
#ifdef SP_THREADS
SP_TLS sp_Fiber *sp_fiber_current = NULL;              /* set by sp_fiber_worker_init */
#else
SP_TLS sp_Fiber *sp_fiber_current = &sp_fiber_root;    /* extern: read by the generated TU */
#endif

/* Adopt the calling OS thread's root fiber as its current coroutine. Called once
   per worker before it runs any green thread (worker 0 from sp_sched_init; helper
   workers from their startup). A no-op-shaped reset in the single-threaded build
   (sp_fiber_current already equals &sp_fiber_root). */
/* A fault inside the current fiber's guard is a stack overflow, and it is
   reported as one: without this the program died in ___chkstk_darwin on macOS
   or in a corrupted neighbour mapping on Linux, and nothing named the fiber
   stack (#4496). The handler runs on a per-thread alternate stack, since the
   fiber's own is what just ran out, writes the line with write(2) only, then
   restores the default disposition and returns so the faulting instruction
   re-executes and the process dies the ordinary way (a core, the signal in
   the exit status). Any other fault is left to the default just the same.
   Installed once, only when nothing else has claimed the signal (the GC
   verifier's fault reporter, for one). */
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#ifdef SP_THREADS
#include <pthread.h>    /* only where the build has it: see sp_thread_stack_bounds */
#endif
/* Unblock signal a (and b, when nonzero) for the calling thread: a handler
   that leaves by a jump instead of a return has to, or the signal stays
   blocked (see sp_trap_call in spinel_rt.h). */
void sp_sig_unblock(int a, int b) {
  sigset_t m; sigemptyset(&m); sigaddset(&m, a); if (b) sigaddset(&m, b);
#ifdef SP_THREADS
  pthread_sigmask(SIG_UNBLOCK, &m, 0);
#else
  sigprocmask(SIG_UNBLOCK, &m, 0);   /* one thread: the same mask */
#endif
}
static void sp_fiber_fault_write(const char *s) { size_t n = strlen(s); while (n) { ssize_t w = write(2, s, n); if (w <= 0) break; s += w; n -= (size_t)w; } }
static void sp_fiber_fault_write_num(size_t v) { char b[24]; int i = (int)sizeof b; b[--i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); sp_fiber_fault_write(b + i); }
static void sp_fiber_fault_handler(int sig, siginfo_t *si, void *uctx) {
  (void)uctx;
  sp_Fiber *f = sp_fiber_current;
  char *a = si ? (char *)si->si_addr : NULL;
  /* The thread's own stack ran past its end: the fault lands in the OS guard
     just below it. Read as an ordinary segfault this killed the process where
     CRuby raises SystemStackError and lets a rescue continue. The raise is
     the generated TU's (it owns the exception stack). */
  if (sp_stack_overflow_raise_fn && a && sp_thread_stack_lo &&
      a < sp_thread_stack_lo && a >= sp_thread_stack_lo - (ptrdiff_t)(1 << 20) &&
      (!f || f == &sp_fiber_root || !f->stack)) {
    /* unblock the signal first: this raise does not return, and the mask it
       was entered with would otherwise stay blocked in the frame we jump to
       (on Linux longjmp does not restore it) */
    sp_sig_unblock(SIGSEGV, SIGBUS);
#ifdef __APPLE__
    /* _longjmp (sp_types.h) also leaves the kernel's on-alt-stack flag set,
       so the next overflow's signal has nowhere to go and the process dies
       of SIGILL. Clear it the way Darwin's longjmp does. */
    { extern int __sigreturn(void *, int, uintptr_t);
      __sigreturn(NULL, 0x80000000 /* UC_RESET_ALT_STACK */, 0); }
#endif
    sp_stack_overflow_raise_fn();   /* returns only if no handler is armed */
    /* Nothing to rescue it: report it as the uncaught exception it is and
       leave, rather than letting the default disposition kill the process on
       a signal with nothing said. Writing a fixed string and _exit are all
       this handler may do -- the allocator is not safe to re-enter from a
       signal, and the stack a formatter would want is the one that just ran
       out. */
    /* what the program already printed is still in stdout's buffer, and
       _exit does not flush it: a run that printed a hundred lines and then
       overflowed would otherwise show none of them. The at_exit hooks do NOT
       run here -- they are Ruby code, and the stack they would run on is the
       one that just ran out. */
    fflush(stdout);
    sp_fiber_fault_write("stack level too deep (SystemStackError)\n");
    _exit(1);
  }
  if (f && f != &sp_fiber_root && f->stack && a >= f->stack && a < f->stack + sp_fiber_guard()) {
    sp_fiber_fault_write("spinel: fiber stack overflow: a green thread, Fiber or Enumerator body ran past the ");
    sp_fiber_fault_write_num(f->stack_size / 1024);
    sp_fiber_fault_write(" KB C stack the fiber runs on (a deep call chain, a large local, or an unoptimised build "
                         "whose frames are far larger than -O2's); SPINEL_FIBER_STACK=<bytes> in the environment gives it more\n");
  }
  signal(sig, SIG_DFL);
}
void (*sp_stack_overflow_raise_fn)(void) = 0;
SP_TLS char *sp_thread_stack_lo = 0;
SP_TLS char *sp_thread_stack_hi = 0;

/* The running thread's stack bounds. A fault just below the low end is this
   stack growing past it -- the OS puts its own guard page there, which is
   what the fault is.

   With threads, pthread answers for every stack including the main one (on
   macOS the address it returns is the HIGH end; on Linux the attr pair gives
   base and size). A program built without them has one stack, the process's
   own: arming happens at startup, so a local here is within a frame or two of
   its high end, and the soft RLIMIT_STACK is its size. That is asked of
   <sys/resource.h> rather than of pthread, because a target may have no
   pthread at all -- a pack of an unthreaded program is built with whatever
   compiler its recipient has, and <pthread.h> there may not even compile. */
static void sp_thread_stack_bounds(char *here) {
#ifdef SP_THREADS
  (void)here;
  pthread_t self = pthread_self();
#if defined(__APPLE__)
  char *hi = (char *)pthread_get_stackaddr_np(self);
  size_t sz = pthread_get_stacksize_np(self);
  sp_thread_stack_hi = hi;
  sp_thread_stack_lo = hi - sz;
#else
  pthread_attr_t at;
  void *base = 0; size_t sz = 0;
  if (pthread_getattr_np(self, &at) == 0) {
    if (pthread_attr_getstack(&at, &base, &sz) != 0) { base = 0; sz = 0; }
    pthread_attr_destroy(&at);
  }
  sp_thread_stack_lo = (char *)base;
  sp_thread_stack_hi = (char *)base + sz;
#endif
#else
  struct rlimit rl;
  size_t sz = 8u << 20;   /* the common default, when the limit says nothing */
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
      rl.rlim_cur > (rlim_t)(64u << 10))
    sz = (size_t)rl.rlim_cur;
  /* `here` is a frame or two below the true top, not at it, and the low end is
     derived by subtracting the size -- so under-estimating the top would put
     the computed low end BELOW the real one, and the real guard page would sit
     above the band the handler tests rather than in it. Over-estimate instead:
     64 KB of startup frames is far more than the few hundred bytes actually
     used, and the computed low end then sits just inside the stack, which is
     where the handler's window wants it. */
  sp_thread_stack_hi = here + (64u << 10);
  sp_thread_stack_lo = sp_thread_stack_hi - sz;
#endif
}

static SP_TLS int sp_fiber_fault_armed = 0;
/* this thread's sigaltstack buffer; external, so the store is not optimized away */
SP_TLS void *sp_fiber_altstack;
static void sp_fiber_fault_arm(void) {
  if (sp_fiber_fault_armed) return;
  sp_fiber_fault_armed = 1;
  { char here; sp_thread_stack_bounds(&here); }
  /* the alternate stack is per OS thread; workers live as long as the process.
     Its only other reference is the kernel's, so it is kept here too: a leak
     checker (valgrind) reported the buffer as definitely lost. */
  size_t asz = 64 * 1024;
  void *as = malloc(asz);
  sp_fiber_altstack = as;
  if (as) { stack_t ss; ss.ss_sp = as; ss.ss_size = asz; ss.ss_flags = 0; sigaltstack(&ss, NULL); }
  static int installed = 0;   /* process-wide: read racily, a second install is harmless */
  if (installed) return;
  int sigs[2] = { SIGSEGV, SIGBUS };
  for (int i = 0; i < 2; i++) {
    struct sigaction cur;
    if (sigaction(sigs[i], NULL, &cur) != 0) continue;
    if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler != SIG_DFL) continue;
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction) continue;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sp_fiber_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(sigs[i], &sa, NULL);
  }
  installed = 1;
}
/* Arm the fault handler for a program that never creates a fiber: the C
   stack it overflows is the thread's own, and the handler is what turns that
   into SystemStackError. Fiber creation and worker startup arm it too, so
   this is a no-op once either has run. */
void sp_stack_guard_init(void) { sp_fiber_fault_arm(); }

/* ---- the stack the program body runs on -------------------------------
   The OS gives a process one stack and decides how big: 8 MB on macOS, and
   a recursion that is merely deep -- generated code reaches depths written
   code does not -- runs out of it long before it has done anything wrong.
   The body runs on a stack mapped here instead, so its depth is this
   compiler's choice rather than the loader's.

   This deliberately does NOT go through sp_Fiber. A fiber would bring its
   bookkeeping -- state, roots, exception context, a resumer to hand back to
   -- and an overflow raised past all that leaves it mid-flight. The body
   needs none of it: it is entered once, it returns once, and nothing
   transfers to it. So it takes the context primitives on their own, and the
   root fiber stays what it already is, the implicit running coroutine with
   no mmap'd stack of its own.

   The guard is the same PROT_NONE page a fiber's stack gets, and the raise
   that reads it is the one already in this file: sp_thread_stack_lo/hi are
   pointed at the new stack, so the handler's own test needs no change. */
static size_t sp_main_stack_size = 0;
static int sp_main_stack_from_env = 0;
void sp_main_stack_hint(size_t bytes) {
  if (!sp_main_stack_from_env && bytes) sp_main_stack_size = bytes;
}
static size_t sp_main_stack_bytes(void) {
  if (!sp_main_stack_size) {
    const char *e = getenv("SPINEL_MAIN_STACK");
    size_t v = e && *e ? sp_fiber_stack_parse(e) : 0;
    if (v) sp_main_stack_from_env = 1;
    /* A 64-bit address space is wide, but the reservation is not free
       everywhere: with overcommit disabled the mapping is charged against
       commit even though MAP_NORESERVE asks otherwise, so the default is a
       size that is defensible to pay for outright rather than the largest
       one the address space would hold. 32-bit space is scarcer still. */
    sp_main_stack_size = v ? v
                          : (sizeof(void *) < 8 ? (size_t)24 << 20
                                                : (size_t)64 << 20);
  }
  long p = sysconf(_SC_PAGESIZE); size_t page = p > 0 ? (size_t)p : 4096;
  size_t sz = (sp_main_stack_size + page - 1) / page * page;
  /* a stack too small to hold the startup frames is worse than none: floor it
     at a size that certainly works, whatever the environment asked for */
  if (sz < (size_t)(1u << 20)) sz = (size_t)1u << 20;
  return sz;
}

static char *sp_main_stack_base = 0;   /* mmap base (guard + stack), or NULL */
static size_t sp_main_stack_map = 0;
static void (*sp_main_stack_body)(void) = 0;
static sp_fiber_ctx sp_main_stack_caller;
static sp_fiber_ctx sp_main_stack_ctx;

static void sp_main_stack_entry(void) {
  sp_main_stack_body();
  /* back to main, on the stack the loader gave it, to return from there */
  sp_ctx_swap(&sp_main_stack_ctx, &sp_main_stack_caller);
}

/* Run `body` on a stack of this compiler's choosing. If one cannot be had --
   any size, down to the floor -- run it where we are: a program must not fail
   to start because it could not reserve a big stack. */
void sp_main_stack_run(void (*body)(void)) {
  size_t guard = sp_fiber_guard();
  size_t want = sp_main_stack_bytes();
  char *base = MAP_FAILED;
  for (;;) {
    base = (char *)mmap(NULL, guard + want, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, -1, 0);
    if (base != MAP_FAILED) break;
    if (want <= (size_t)(1u << 20)) break;    /* the floor: give up on mapping */
    want /= 2;                                 /* halve and ask again */
  }
  if (base == MAP_FAILED) { body(); return; }
  mprotect(base, guard, PROT_NONE);
  sp_main_stack_base = base; sp_main_stack_map = guard + want;
  sp_main_stack_body = body;
  /* Arm FIRST: arming reads the running thread's own bounds, and would
     overwrite the ones set here if it ran after them. Then say where the
     stack really is -- the handler reads these to tell a stack that ran out
     from a bad pointer, and the guard page below `lo` is the mprotect'd one
     above. */
  sp_stack_guard_init();
  sp_thread_stack_lo = base + guard;
  sp_thread_stack_hi = base + guard + want;
  sp_ctx_make(&sp_main_stack_ctx, base + guard, want, sp_main_stack_entry);
  sp_ctx_swap(&sp_main_stack_caller, &sp_main_stack_ctx);
  /* the body returned; nothing runs on that stack any more */
  sp_thread_stack_lo = 0; sp_thread_stack_hi = 0;
  munmap(sp_main_stack_base, sp_main_stack_map);
  sp_main_stack_base = 0; sp_main_stack_map = 0;
}

void sp_fiber_worker_init(void) {
  sp_fiber_current = &sp_fiber_root;
  sp_fiber_fault_arm();
#ifdef SP_TSAN
  /* Register this worker's root fiber on its own OS thread, so every worker --
     including a helper spawned mid-run that only ever runs fibers created by
     another thread -- has a valid tsan handle to switch back to. Without this,
     switching to the root (SP_TSAN_SWITCH) is a silent no-op and TSan loses the
     thread's current-fiber tracking, faulting on the next real switch. */
  if (!sp_fiber_root.tsan_fiber) sp_fiber_root.tsan_fiber = __tsan_get_current_fiber();
#endif
}
sp_Fiber *sp_fiber_worker_root(void) { return &sp_fiber_root; }
static sp_Fiber *sp_fiber_list_head = NULL;

static void sp_fiber_save_roots(sp_Fiber*f){if(f->stack)sp_gc_wb((void*)f);/* the snapshot may name young objects, and only a scanned fiber follows it now: an old fiber has to be remembered (#4525) */if(f->saved_roots_cap<sp_gc_nroots){int nc=sp_gc_nroots>64?sp_gc_nroots*2:64;void***nx=(void***)realloc(f->saved_roots,sizeof(void**)*nc);if(!nx)return;f->saved_roots=nx;f->saved_roots_cap=nc;}/* both segments: the array, then whatever spilled past it (sp_gc.h) */
int n1=sp_gc_nroots<SP_GC_STACK_MAX?sp_gc_nroots:SP_GC_STACK_MAX;if(n1>0)memcpy(f->saved_roots,sp_gc_roots,sizeof(void**)*n1);if(sp_gc_nroots>n1)memcpy(f->saved_roots+n1,sp_gc_roots_ext,sizeof(void**)*(sp_gc_nroots-n1));f->saved_nroots=sp_gc_nroots;}
static void sp_fiber_restore_roots(sp_Fiber*f){int n=f->saved_nroots;int n1=n<SP_GC_STACK_MAX?n:SP_GC_STACK_MAX;if(n1>0)memcpy(sp_gc_roots,f->saved_roots,sizeof(void**)*n1);if(n>n1){if(!sp_gc_roots_ext_reserve(n))sp_oom_die();memcpy(sp_gc_roots_ext,f->saved_roots+n1,sizeof(void**)*(n-n1));}sp_gc_nroots=n;}
/* Snapshot the calling worker's live shadow-stack roots into the green thread it
   is running, so a stop-the-world collector can mark them while this worker is
   parked at a safepoint. The collector reaches them via the suspended-fibers GC
   hook (every fiber but the collector's own current is marked from saved_roots). */
void sp_fiber_publish_current_roots(void){if(sp_fiber_current)sp_fiber_save_roots(sp_fiber_current);}
/* The global fiber list is mutated by Fiber.new from any worker (list_add) and
   walked by the collector during stop-the-world (all mutators parked, so the
   walk needs no lock). A small mutex serializes concurrent list_add/remove; the
   critical section is a few pointer writes with no safepoint poll inside, so a
   worker never parks holding it and the collector never waits on it. */
#ifdef SP_THREADS
#include <pthread.h>
static pthread_mutex_t sp_fiber_list_lock = PTHREAD_MUTEX_INITIALIZER;
#define FIBER_LIST_LOCK()   pthread_mutex_lock(&sp_fiber_list_lock)
#define FIBER_LIST_UNLOCK() pthread_mutex_unlock(&sp_fiber_list_lock)
#else
#define FIBER_LIST_LOCK()   ((void)0)
#define FIBER_LIST_UNLOCK() ((void)0)
#endif
static void sp_fiber_list_add(sp_Fiber*f){FIBER_LIST_LOCK();f->fiber_prev=NULL;f->fiber_next=sp_fiber_list_head;if(sp_fiber_list_head)sp_fiber_list_head->fiber_prev=f;sp_fiber_list_head=f;FIBER_LIST_UNLOCK();}
static void sp_fiber_list_remove(sp_Fiber*f){FIBER_LIST_LOCK();if(f->fiber_prev)f->fiber_prev->fiber_next=f->fiber_next;else if(sp_fiber_list_head==f)sp_fiber_list_head=f->fiber_next;if(f->fiber_next)f->fiber_next->fiber_prev=f->fiber_prev;f->fiber_prev=NULL;f->fiber_next=NULL;FIBER_LIST_UNLOCK();}
/* Mark a fiber's published (saved) roots and pending-exception objects. Public
   so the thread scheduler can mark a parked worker's per-worker root fiber, which
   is not on sp_fiber_list_head (only Fiber.new fibers are) and so would otherwise
   be missed when a *different* worker is the stop-the-world collector. */
void sp_fiber_mark_roots(sp_Fiber*f){SP_GC_ROOT(f);int i;for(i=0;i<f->saved_nroots;i++){void**e=f->saved_roots[i];if((uintptr_t)e&(uintptr_t)3){sp_gc_mark_root_entry(e);}
else{void*obj=*e;if(obj)sp_gc_mark(obj);}}if(f->exc_ctx)sp_exc_ctx_mark(f->exc_ctx);if(f->raised_obj)sp_gc_mark(f->raised_obj);if(f->inj_obj)sp_gc_mark(f->inj_obj);}
/* The fibers whose C stacks are live: the running one (its roots are the
   collector's own) and the chain of resumers waiting for it, each suspended
   inside #resume with a frame that continues when the resumed fiber yields.
   Those are roots. Every other suspended fiber is an object like any other:
   it lives while something refers to it, and its saved roots are marked when
   it is (sp_Fiber_scan). Rooting the whole fiber list kept an object alive
   through the fiber it owned (`@loop = Fiber.new { loop { tick } }`, whose
   block captures self), so neither was ever collected (#4525). */
/* The fiber the collection runs on: its saved roots are stale (the live ones
   are the root array) and no scanner may read them -- a parallel mark helper
   on another worker has its own sp_fiber_current, so the test cannot be TLS.
   Set by the fibers hook, which runs before any object is scanned. */
static sp_Fiber *sp_fiber_collecting = NULL;
void sp_fiber_mark_chain(sp_Fiber*f){for(sp_Fiber*g=f;g;g=g->resumer){if(g==sp_fiber_current||g==sp_fiber_collecting)continue;if(g->stack)sp_gc_mark(g);/* a root fiber is a worker's native stack, not a heap object */sp_fiber_mark_roots(g);}}
static void sp_mark_suspended_fibers(void){sp_fiber_collecting=sp_fiber_current;if(sp_fiber_current!=&sp_fiber_root)sp_fiber_mark_roots(&sp_fiber_root);if(sp_fiber_current)sp_fiber_mark_chain(sp_fiber_current->resumer);
  /* and the fiber a transferred one returns to, suspended in its transfer */
  if(sp_fiber_current&&sp_fiber_current->return_to)sp_fiber_mark_chain(sp_fiber_current->return_to);}
static void sp_fiber_install_gc_hook(void){if(!sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook=sp_mark_suspended_fibers;}
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,sp_fiber_guard()+f->stack_size);if(f->saved_roots)free(f->saved_roots);if(f->exc_ctx)sp_exc_ctx_free(f->exc_ctx);
#ifdef SP_TSAN
  if(f->tsan_fiber)__tsan_destroy_fiber(f->tsan_fiber);
#endif
  sp_fiber_list_remove(f);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);if(f->storage)sp_gc_mark(f->storage);if(f->attrs)sp_gc_mark(f->attrs);if(f->return_to)sp_gc_mark(f->return_to);
  /* a reachable suspended fiber keeps what its stack holds (its published
     roots); the running one's roots are live, a dead one's are gone */
  if(f!=sp_fiber_collecting&&f!=sp_fiber_current&&f->state!=3)sp_fiber_mark_roots(f);}
sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_fiber_fault_arm();sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);{size_t _g=sp_fiber_guard();f->stack_size=sp_fiber_stack_bytes();
/* MAP_NORESERVE: the stack is virtual space until touched, and a strict-overcommit host must not charge every fiber the whole of it (#4496) */
f->stack=(char*)mmap(NULL,_g+f->stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK|MAP_NORESERVE,-1,0);if(f->stack==MAP_FAILED){f->stack=NULL;sp_raise_cls("FiberError","failed to allocate fiber stack");}mprotect(f->stack,_g,PROT_NONE);}f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->exc_ctx=sp_exc_ctx_new();f->raised=0;f->raised_cls=NULL;f->raised_msg=NULL;f->raised_obj=NULL;f->inject=0;f->inject_defer=0;f->inj_cls=NULL;f->inj_msg=NULL;f->inj_obj=NULL;f->storage=NULL;f->attrs=NULL;f->birth_file=NULL;f->birth_line=0;f->pass_argc=-1;f->owner=sp_thread_owner_id();f->return_to=NULL;f->thread_main=0;f->blocking=0;f->saved_roots=NULL;f->saved_nroots=0;f->saved_roots_cap=0;f->fiber_next=NULL;f->fiber_prev=NULL;sp_fiber_list_add(f);sp_fiber_install_gc_hook();if(sp_fiber_current&&sp_fiber_current->storage){sp_Fiber*volatile _froot=f;int _pushed=0;if(sp_gc_nroots<SP_GC_STACK_MAX){sp_gc_roots[sp_gc_nroots++]=(void**)&_froot;_pushed=1;}sp_FiberStore*_fst=sp_FiberStore_dup((sp_FiberStore*)sp_fiber_current->storage);sp_gc_wb((void*)f);f->storage=_fst;if(_pushed)sp_gc_nroots--;}
#ifdef SP_TSAN
  if(!sp_fiber_root.tsan_fiber)sp_fiber_root.tsan_fiber=__tsan_get_current_fiber();
  f->tsan_fiber=__tsan_create_fiber(0);f->caller_fiber=NULL;
#endif
  return f;}
sp_RbVal sp_Fiber_storage_get(sp_Fiber*f,sp_sym k){if(!f->storage)return sp_box_nil();return sp_FiberStore_get((sp_FiberStore*)f->storage,k);}
/* store v under k as it is, nil included */
void sp_Fiber_storage_put(sp_Fiber*f,sp_sym k,sp_RbVal v){SP_GC_ROOT_RBVAL(v);SP_GC_ROOT(f); sp_gc_wb((void*)f);if(!f->storage){f->storage=sp_FiberStore_new();sp_gc_wb((void*)f);}sp_FiberStore_set((sp_FiberStore*)f->storage,k,v);}
/* Fiber[k] = v: nil removes the key (CRuby) */
void sp_Fiber_storage_set(sp_Fiber*f,sp_sym k,sp_RbVal v){
  if(v.tag==SP_TAG_NIL){
    if(f->storage)sp_FiberStore_delete((sp_FiberStore*)f->storage,k);
    return;
  }
  sp_Fiber_storage_put(f,k,v);
}
/* the whole store, for Fiber#storage / #storage= (the Hash side is in
   spinel_rt.h) */
sp_int sp_Fiber_storage_len(sp_Fiber*f){
  return f->storage?((sp_FiberStore*)f->storage)->len:0;
}
/* entry i: keep i below sp_Fiber_storage_len, which is 0 when there is no store */
sp_sym sp_Fiber_storage_key(sp_Fiber*f,sp_int i){
  return ((sp_FiberStore*)f->storage)->keys[i];
}
sp_RbVal sp_Fiber_storage_val(sp_Fiber*f,sp_int i){
  return ((sp_FiberStore*)f->storage)->vals[i];
}
/* no store at all (#storage answers nil), or an empty one */
void sp_Fiber_storage_clear(sp_Fiber*f){
  sp_gc_wb((void*)f);
  f->storage=NULL;
}
void sp_Fiber_storage_empty(sp_Fiber*f){
  SP_GC_ROOT(f);
  sp_FiberStore*s=sp_FiberStore_new();
  sp_gc_wb((void*)f);
  f->storage=s;
}
/* A `Fiber.attr_accessor` attribute: the fiber's own table, which -- unlike
   `storage` -- a new fiber does not inherit (an attribute on a fresh fiber is
   nil, as an ivar on a fresh object is). */
sp_RbVal sp_Fiber_attr_get(sp_Fiber*f,sp_sym k){if(!f||!f->attrs)return sp_box_nil();return sp_FiberStore_get((sp_FiberStore*)f->attrs,k);}
void sp_Fiber_attr_set(sp_Fiber*f,sp_sym k,sp_RbVal v){if(!f)return;SP_GC_ROOT_RBVAL(v);SP_GC_ROOT(f); sp_gc_wb((void*)f);if(!f->attrs){f->attrs=sp_FiberStore_new();sp_gc_wb((void*)f);}sp_FiberStore_set((sp_FiberStore*)f->attrs,k,v);}   /* the store, not the head, is what the barrier has to follow: sp_FiberStore_new can collect */
/* Internal class name of the Fiber#kill signal. It is raised to unwind the
   fiber (running ensure blocks) but is excluded from every user rescue clause by
   the codegen (emit_begin), so only ensures run; the trampoline below recognizes
   it and terminates without propagating. Must stay in sync with the literal the
   codegen emits in src/codegen_stmt.c. */
#define SP_FIBER_KILL_CLS "FiberKillSignal"
/* the kill signal's message when it is aimed at a main fiber: fibers pass it
   on up instead of ending there (see sp_Fiber_kill) */
#define SP_FIBER_KILL_MAIN "main"

/* The inject slot is written by another OS thread (Thread#kill/#raise under the
   scheduler lock) but read lock-free by the target's own paths (the trampoline,
   Fiber.yield, sp_fiber_fire_inject_if_pending after a park). The flag alone is
   not enough: a second inject can arrive while the target is mid-consume, so the
   payload fields (inj_cls/inj_msg/inj_obj) would race with the consumer's reads
   and clears. Guard the whole slot with a spinlock folded into the high bit of
   the inject int: publishers and the consumer CAS the bit in, touch the payload,
   and release-store the new kind (which also drops the bit). The critical
   section is a handful of plain stores, so contention is a few spins at worst;
   the lock holder never takes the scheduler lock, so there is no ordering cycle
   with publishers that hold it. In the single-threaded build the atomics
   compile to plain ops on a never-contended slot. */
#define SP_INJECT_LOCK_BIT 0x40000000
#define SP_INJECT_PEEK(f) (SP_ATOMIC_LOAD(&(f)->inject, __ATOMIC_ACQUIRE) & ~SP_INJECT_LOCK_BIT)
/* Acquire the slot; returns the kind bits observed at lock time. */
static int sp_fiber_inject_lock(sp_Fiber*f){
  for(;;){
    int cur=SP_ATOMIC_LOAD(&f->inject,__ATOMIC_RELAXED);
    if(cur&SP_INJECT_LOCK_BIT)continue;   /* holder is mid-publish/consume; spin */
    if(SP_ATOMIC_CAS(&f->inject,&cur,cur|SP_INJECT_LOCK_BIT,0,
                                   __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return cur;
  }
}
static void sp_fiber_inject_unlock(sp_Fiber*f,int kind){SP_ATOMIC_STORE(&f->inject,kind,__ATOMIC_RELEASE);}
/* Publish kind+payload atomically with respect to a concurrent consume. */
static void sp_fiber_inject_publish(sp_Fiber*f,int kind,const char*cls,const char*msg,void*obj){SP_GC_ROOT(f);SP_GC_ROOT_STR(msg);
  sp_fiber_inject_lock(f);
  f->inj_cls=cls;f->inj_msg=msg;f->inj_obj=obj;
  sp_fiber_inject_unlock(f,kind);
}

/* Raise the exception queued by sp_Fiber_raise / sp_Fiber_kill in the fiber's
   own context. Runs at the fiber's suspension point (sp_Fiber_yield) or, for an
   unstarted fiber, at body entry (the trampoline). inject==2 is a kill signal;
   inject==1 is an ordinary raise. Clears the slot (under the inject spinlock)
   first so a rescue/retry does not re-fire it. */
static void sp_fiber_consume_inject(sp_Fiber*f){SP_GC_ROOT(f);int kind=sp_fiber_inject_lock(f);const char*cl=f->inj_cls;const char*ms=f->inj_msg;void*ob=f->inj_obj;f->inj_cls=NULL;f->inj_msg=NULL;f->inj_obj=NULL;sp_fiber_inject_unlock(f,0);if(kind==2)sp_raise_cls(SP_FIBER_KILL_CLS,(&("\xff")[1]));else sp_fiber_reraise(cl,ms,ob);/* kind 1 (Fiber#raise) and 3 (Thread#raise) both re-raise */}

/* Thread #kill / #raise support: the scheduler sets a pending inject on a target
   thread's fiber, then fires it (sp_fiber_fire_inject_if_pending) when that
   thread next runs -- in its own context, so its ensure blocks unwind on its own
   stack. sp_fiber_raise_kill_self raises the kill signal in the running fiber
   (a thread killing itself).
   A thread raise is inject==3, NOT 1: the trampoline must not consume it at
   body entry. A Thread#raise that lands before the body has run (the target
   was picked up but not yet started) would otherwise raise ahead of the body's
   own begin/rescue and escape as an unhandled thread exception; deferring it to
   the thread's next suspension point (sp_sched_block / sleep / Thread.pass /
   yield) delivers it inside the body, where its rescue/ensure can see it. */
void sp_fiber_set_raise_inject(sp_Fiber*f,const char*cls,const char*msg,void*obj){SP_GC_ROOT(f);SP_GC_ROOT_STR(msg);sp_fiber_inject_publish(f,3,cls,msg,obj);}
void sp_fiber_set_kill_inject(sp_Fiber*f){SP_GC_ROOT(f);sp_fiber_inject_publish(f,2,NULL,NULL,NULL);}
void sp_fiber_defer_inject(void){sp_Fiber*f=sp_fiber_current;if(f)f->inject_defer++;}
void sp_fiber_undefer_inject(void){sp_Fiber*f=sp_fiber_current;if(f&&f->inject_defer)f->inject_defer--;}
void sp_fiber_fire_inject_if_pending(void){sp_Fiber*f=sp_fiber_current;if(!f||f->inject_defer)return;if(SP_INJECT_PEEK(f)){sp_fiber_consume_inject(f);return;}
  /* A #kill/#raise of a thread that is inside a Fiber it resumed: it is
     delivered in that fiber, and a kill goes on up to the thread's own. */
  if(f->thread_main||f==&sp_fiber_root)return;
  sp_Fiber*m=sp_thread_main_fiber();if(!m||!SP_INJECT_PEEK(m))return;
  SP_GC_ROOT(m);int kind=sp_fiber_inject_lock(m);const char*cl=m->inj_cls;const char*ms=m->inj_msg;void*ob=m->inj_obj;m->inj_cls=NULL;m->inj_msg=NULL;m->inj_obj=NULL;sp_fiber_inject_unlock(m,0);
  if(kind==2)sp_raise_cls(SP_FIBER_KILL_CLS,(&("\xff" SP_FIBER_KILL_MAIN)[1]));else sp_fiber_reraise(cl,ms,ob);}
/* Lock-free peek for the scheduler's pre-park checks (sp_sched_block etc.). */
int sp_fiber_inject_pending(sp_Fiber*f){return SP_INJECT_PEEK(f)!=0;}
SP_NORETURN void sp_fiber_raise_kill_self(void){sp_raise_cls(SP_FIBER_KILL_CLS,(&("\xff")[1]));}
static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;jmp_buf base;if(setjmp(base)==0){sp_exc_arm(base);{int _inj=SP_INJECT_PEEK(f);if(_inj&&_inj!=3)sp_fiber_consume_inject(f);}/* a thread raise (3) defers to the body's first suspension point */f->body(f);sp_exc_disarm();}
else{const char*_cc=sp_exc_cur_cls();
  int killed=_cc&&!strcmp(_cc,SP_FIBER_KILL_CLS);
  /* a kill aimed at a main fiber goes on up to it, like an exception */
  const char*_km=killed?sp_exc_cur_msg():NULL;
  if(_km&&!strcmp(_km,SP_FIBER_KILL_MAIN)&&f!=sp_thread_main_fiber())killed=0;
  if(killed){
  /* killed: ensures already ran while unwinding; end without propagating,
     and #resume answers nil rather than whatever it last yielded */
  f->yielded_value=sp_box_nil();}
else{f->raised=1;f->raised_cls=_cc;f->raised_msg=sp_exc_cur_msg();f->raised_obj=sp_exc_cur_obj();}}f->state=3;f->saved_nroots=0;/* dead: the snapshot points into unwound frames; never mark it */if(f->transferred){
  /* back to the fiber it returns to, whose transfer answers the block's result */
  sp_Fiber*home=f->return_to&&f->return_to->state!=3?f->return_to:sp_thread_main_fiber();
  if(!home||home==f)home=sp_sched_home_fiber();
  home->resumed_value=f->yielded_value;sp_fiber_current=home;
  SP_TSAN_SWITCH(home);sp_ctx_swap(&f->ctx,&home->ctx);}
else{SP_TSAN_SWITCH(f->caller_fiber);sp_ctx_swap(&f->ctx,&f->caller_ctx);}}
/* A fiber belongs to the thread that made it (CRuby's rule). */
static void sp_fiber_check_thread(sp_Fiber*f){
  if(f->owner&&f->owner!=sp_thread_owner_id())
    sp_raise_cls("FiberError","fiber called across threads");
}
/* f is waiting in its #resume of a fiber on the running chain. */
static int sp_fiber_resuming(sp_Fiber*f){
  for(sp_Fiber*g=sp_fiber_current;g;){
    if(g->resumer){if(g->resumer==f)return 1;g=g->resumer;}
    else g=g->return_to;
  }
  return 0;
}
/* CRuby's refusals for a resume target, in its order. A root fiber has no
   stack and is never resumable: it is current, resuming, or transferring.
   That holds for the main thread's root asked from another thread too, which
   is not this worker's root, so the test is the missing stack. */
static void sp_fiber_check_resume(sp_Fiber*f){
  if(f->state==3)sp_raise_cls("FiberError","attempt to resume a terminated fiber");
  if(f==sp_fiber_current)sp_raise_cls("FiberError","attempt to resume the current fiber");
  if(f->resumer)sp_raise_cls("FiberError","attempt to resume a resumed fiber (double resume)");
  if(sp_fiber_resuming(f))sp_raise_cls("FiberError","attempt to resume a resuming fiber");
  if(f->transferred||f==&sp_fiber_root||!f->stack)sp_raise_cls("FiberError","attempt to resume a transferring fiber");
  if(f->state==1)sp_raise_cls("FiberError","attempt to resume a resumed fiber (double resume)");
}
sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){SP_GC_ROOT_RBVAL(val);SP_GC_ROOT(f);sp_fiber_check_thread(f);sp_fiber_check_resume(f);f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);if(!prev->exc_ctx)prev->exc_ctx=sp_exc_ctx_new();sp_exc_ctx_save(prev->exc_ctx);sp_exc_ctx_load(f->exc_ctx);f->resumer=prev;sp_fiber_current=f;SP_TSAN_SET_CALLER(f,prev);SP_TSAN_SWITCH(f);if(f->state==0){f->state=1;sp_ctx_make(&f->ctx,f->stack+sp_fiber_guard(),f->stack_size,sp_fiber_trampoline);sp_ctx_swap(&f->caller_ctx,&f->ctx);}
else{f->state=1;sp_ctx_swap(&f->caller_ctx,&f->ctx);}f->resumer=NULL;sp_exc_ctx_save(f->exc_ctx);sp_exc_ctx_load(prev->exc_ctx);if(f->state!=3)sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;if(f->raised){f->raised=0;const char*rc=f->raised_cls;const char*rm=f->raised_msg;void*ro=f->raised_obj;f->raised_obj=NULL;sp_fiber_reraise(rc,rm,ro);}return f->yielded_value;}
/* Fiber.yield is only valid inside a fiber entered via #resume. The root fiber
   was never resumed, and a fiber entered via #transfer has no resumer to return
   to (its caller_ctx is unset) -- yielding from either would swap to a garbage
   context, so raise instead (matching CRuby's FiberError). */
sp_RbVal sp_Fiber_yield(sp_RbVal val){SP_GC_ROOT_RBVAL(val);sp_Fiber*f=sp_fiber_current;if(f==&sp_fiber_root||f->transferred){sp_raise_cls("FiberError","attempt to yield on a not resumed fiber");}f->yielded_value=val;f->state=2;SP_TSAN_SWITCH(f->caller_fiber);sp_ctx_swap(&f->ctx,&f->caller_ctx);if(!f->inject_defer&&SP_INJECT_PEEK(f))sp_fiber_consume_inject(f);return f->resumed_value;}
sp_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
/* A main fiber -- the root (no stack of its own) or a Thread's -- is
   blocking; a Fiber.new one only when asked to be. */
sp_bool sp_Fiber_blocking_p(sp_Fiber*f){
  return f->stack==NULL||f->thread_main||f->blocking;
}
sp_Fiber*sp_Fiber_at(sp_Fiber*f,const char*file,sp_int line){if(f){f->birth_file=file;f->birth_line=line;}return f;}
/* Fiber#inspect: CRuby's #<Fiber:0xADDR file:line (status)>. Only the
   running fiber is "resumed"; one waiting in #resume of another is
   "suspended by resuming". */
const char*sp_Fiber_inspect(sp_Fiber*f){
  extern const char*sp_sprintf(const char*fmt,...);
  if(!f)return "nil";
  const char*st="suspended";
  if(f->state==0&&f!=&sp_fiber_root)st="created";
  else if(f->state==3)st="terminated";
  else if(f==sp_fiber_current)st="resumed";
  else for(sp_Fiber*g=sp_fiber_current->resumer;g;g=g->resumer)if(g==f){st="suspended by resuming";break;}
  if(f->birth_file)
    return sp_sprintf("#<Fiber:0x%016llx %s:%lld (%s)>",(unsigned long long)(uintptr_t)f,f->birth_file,(long long)f->birth_line,st);
  return sp_sprintf("#<Fiber:0x%016llx (%s)>",(unsigned long long)(uintptr_t)f,st);
}
/* Fiber#raise: queue an exception, then resume the fiber so its suspension point
   (or body entry) raises it. An unhandled raise propagates to this caller via
   sp_Fiber_resume's re-raise, exactly like an exception raised by the body. */
sp_RbVal sp_Fiber_raise(sp_Fiber*f,const char*cls,const char*msg,void*obj){SP_GC_ROOT(f);SP_GC_ROOT_STR(msg);
  if(f->state==3){sp_raise_cls("FiberError","dead fiber called");}
  /* Never resumed: there is no fiber context to deliver into, so CRuby refuses
     rather than raising the exception somewhere. Delivering it in the CALLER
     instead -- which is what resuming an unstarted fiber with the injection
     queued did -- made a `rescue FiberError` around fiber setup miss, and
     handed the caller the wrong class (#3468). Note Fiber#kill is different
     and stays a no-op here: an unborn fiber has nothing to unwind. */
  if(f->state==0){sp_raise_cls("FiberError","cannot raise exception on unborn fiber");}
  sp_fiber_inject_publish(f,1,cls,msg,obj);   /* same-thread, but keep the slot discipline uniform */
  return sp_Fiber_resume(f,sp_box_nil());
}
/* Fiber#kill: terminate the fiber, running its ensure blocks. A suspended fiber
   is resumed with a kill signal that unwinds it (ensures run, user rescues are
   bypassed) until the trampoline terminates it. An unstarted fiber never ran its
   body, so it is just marked dead. Returns the fiber, matching CRuby. */
sp_Fiber*sp_Fiber_kill(sp_Fiber*f){SP_GC_ROOT(f);
  /* A main fiber: the root (no stack of its own) or a Thread's own fiber. */
  int main_fiber=f->stack==NULL||f->thread_main;
  /* the running thread's: the root only counts on the main thread */
  int ours=f->stack==NULL?sp_thread_main_fiber()==NULL:f==sp_thread_main_fiber();
  /* Killing the main fiber of the program or of a Thread ends that program
     or thread, as in CRuby: the signal unwinds every fiber on the way. */
  if(main_fiber&&ours)
    sp_raise_cls(SP_FIBER_KILL_CLS,(&("\xff" SP_FIBER_KILL_MAIN)[1]));
  /* Another thread's fiber is refused before anything is queued on it,
     with CRuby's messages. */
  if(main_fiber)sp_raise_cls("FiberError","attempt to resume a transferring fiber");
  sp_fiber_check_thread(f);
  /* a fiber killing itself unwinds right here */
  if(f==sp_fiber_current)sp_fiber_raise_kill_self();
  if(f->state==3)return f;             /* already dead: no-op */
  if(f->state==0){f->state=3;return f;}/* unstarted: nothing to unwind */
  sp_fiber_inject_publish(f,2,NULL,NULL,NULL);
  sp_Fiber_resume(f,sp_box_nil());     /* runs ensures; the trampoline terminates it */
  return f;
}
/* Core of #transfer: switch to f and do all the save/restore, but leave f's
   unhandled termination exception PENDING in f->raised for the caller to
   consume. sp_Fiber_transfer re-raises it (Fiber semantics); the thread
   scheduler captures it instead (sp_Fiber_transfer_catch). */
/* CRuby's refusals for a transfer target. The root fiber passes, and so does
   a live fiber already entered by transfer (what the scheduler switches to). */
static void sp_fiber_check_transfer(sp_Fiber*f){
  /* the main thread's root, from another thread: it has no owner to check */
  if(!f->stack&&sp_thread_main_fiber())sp_raise_cls("FiberError","fiber called across threads");
  if(f==&sp_fiber_root||f==sp_fiber_current)return;
  if(f->state==3)sp_raise_cls("FiberError","dead fiber called");
  if(f->transferred)return;
  if(f->state==2)sp_raise_cls("FiberError","attempt to transfer to a yielding fiber");
  /* resuming: waiting in #resume of a fiber on the running chain. One that
     is only suspended in its own transfer may be transferred back to. */
  for(sp_Fiber*g=sp_fiber_current;g;){
    if(g->resumer){if(g->resumer==f)sp_raise_cls("FiberError","attempt to transfer to a resuming fiber");g=g->resumer;}
    else g=g->return_to;
  }
}
/* The switch itself, past CRuby's checks: save prev's (the current fiber's)
   context and run f. Inlined, so a transfer costs what it did before the
   scheduler's switch shared it. The caller roots f. */
static SP_INLINE sp_RbVal sp_fiber_switch(sp_Fiber*f,sp_Fiber*prev,sp_RbVal val){f->resumed_value=val;
  sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);if(!prev->exc_ctx)prev->exc_ctx=sp_exc_ctx_new();sp_exc_ctx_save(prev->exc_ctx);sp_exc_ctx_load(f->exc_ctx);sp_fiber_current=f;SP_TSAN_SWITCH(f);if(f->state==0&&f!=&sp_fiber_root){f->state=1;f->transferred=1;sp_ctx_make(&f->ctx,f->stack+sp_fiber_guard(),f->stack_size,sp_fiber_trampoline);sp_ctx_swap(&prev->ctx,&f->ctx);}
else{/* the root fiber is the implicit running coroutine: it has no mmap'd
   stack/body, so it must never be ctx_make'd. Its context was already
   saved into root.ctx by the first transfer away from it, so transferring
   back just swaps to that saved context. */f->state=1;sp_ctx_swap(&prev->ctx,&f->ctx);}/* Do NOT re-save f's exception context or roots here. When f yielded back it
   already saved its own (its transfer's pre-swap save); the live TLS now holds
   prev's, which f restored before switching to us. Re-saving would clobber f's
   saved state with prev's -- losing a blocked/sleeping thread's roots/handlers
   and freeing its live locals on the next collection -- and, since f may already
   be running on another worker (woken between its switch-out and here), that
   write races that worker's load of f's context. We only restore prev's. */sp_exc_ctx_load(prev->exc_ctx);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return prev->resumed_value;}
static sp_RbVal sp_Fiber_transfer_core(sp_Fiber*f,sp_RbVal val){SP_GC_ROOT(f);sp_fiber_check_thread(f);sp_fiber_check_transfer(f);sp_Fiber*prev=sp_fiber_current;
  /* it returns where its transferrer would: to that fiber if it was resumed */
  if(f!=&sp_fiber_root&&f!=prev)f->return_to=prev->resumer?prev:prev->return_to;
  /* TSan's caller is set by the transfer, not by the scheduler's switch back:
     a resumed fiber's caller is its resumer, which its Fiber.yield goes to */
  SP_TSAN_SET_CALLER(f,prev);
  return sp_fiber_switch(f,prev,val);}
/* A green thread's switch back to the fiber that ran it (sp_sched.c). That
   may be a Fiber the main thread resumed, which a transfer would refuse (it
   is the main thread's, and resumed), and it is not the green thread's to
   return to when its body ends, so it leaves return_to alone, and under TSan
   caller_fiber too. */
void sp_fiber_sched_switch(sp_Fiber*f){SP_GC_ROOT(f);sp_fiber_switch(f,sp_fiber_current,sp_box_nil());}
sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){SP_GC_ROOT_RBVAL(val);SP_GC_ROOT(f);sp_RbVal r=sp_Fiber_transfer_core(f,val);if(f->raised){f->raised=0;const char*rc=f->raised_cls;const char*rm=f->raised_msg;void*ro=f->raised_obj;f->raised_obj=NULL;sp_fiber_reraise(rc,rm,ro);}return r;}
/* resume / transfer that also tell the body how many values were passed */
sp_RbVal sp_Fiber_resume_n(sp_Fiber*f,sp_RbVal val,int argc){SP_GC_ROOT(f);f->pass_argc=argc;sp_RbVal r=sp_Fiber_resume(f,val);f->pass_argc=-1;return r;}
sp_RbVal sp_Fiber_transfer_n(sp_Fiber*f,sp_RbVal val,int argc){SP_GC_ROOT(f);f->pass_argc=argc;sp_RbVal r=sp_Fiber_transfer(f,val);f->pass_argc=-1;return r;}
/* Thread scheduler transfer: on f's unhandled termination exception, hand the
   (cls,msg,obj) back through *out_* and set *out_raised, rather than re-raising
   in the caller (the scheduler stores it on the green thread for #join/#value).
   A non-terminating transfer (f yielded back) leaves *out_raised 0.
   The thread's fiber returns to no fiber: the fibers it transfers to end in
   it, and its own body ends where its yields go, in the fiber that ran it
   (sp_sched_home_fiber). The main thread may run it from inside a Fiber,
   whose chain is not the thread's. When the thread stopped inside a Fiber
   it resumed, `at`, the switch goes there, still with f's bookkeeping. */
sp_RbVal sp_Fiber_transfer_catch(sp_Fiber*f,sp_Fiber*at,sp_RbVal val,int*out_raised,const char**out_cls,const char**out_msg,void**out_obj){SP_GC_ROOT_RBVAL(val);SP_GC_ROOT(f);sp_Fiber*prev=sp_fiber_current;f->return_to=NULL;sp_RbVal r;
  if(at){SP_GC_ROOT(at);r=sp_fiber_switch(at,prev,val);}
  else{sp_fiber_check_thread(f);sp_fiber_check_transfer(f);SP_TSAN_SET_CALLER(f,prev);r=sp_fiber_switch(f,prev,val);}
  *out_raised=f->raised;if(f->raised){f->raised=0;*out_cls=f->raised_cls;*out_msg=f->raised_msg;*out_obj=f->raised_obj;f->raised_obj=NULL;}return r;}

void sp_mark_fiber_root_storage(void){if(sp_fiber_root.storage)sp_gc_mark(sp_fiber_root.storage);if(sp_fiber_root.attrs)sp_gc_mark(sp_fiber_root.attrs);}
