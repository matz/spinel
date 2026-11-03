/* sp_fiber_ctx.h -- portable coroutine context switch for sp_fiber.c.
 *
 * Replaces POSIX <ucontext.h> (getcontext/makecontext/swapcontext), which
 * OpenBSD removed in 5.7 (W^X / ASLR hardening) and which is deprecated on
 * macOS. On x86_64 and aarch64 we use a tiny cooperative register switch
 * written as file-scope inline asm (defined in sp_fiber.c): it lands in .text
 * (W^X-safe, unlike libco's executable data blob), does no syscall (unlike
 * swapcontext's sigprocmask, so it's faster), and only saves the callee-saved
 * registers + stack pointer. The "context" is therefore just the saved stack
 * pointer; all other state lives on the coroutine's own stack. Other
 * architectures fall back to <ucontext.h>.
 *
 *   sp_ctx_swap(from, to)               save the current context into *from,
 *                                       then switch to *to.
 *   sp_ctx_make(ctx, base, size, entry) prime a fresh context so the first swap
 *                                       into it begins executing entry() on the
 *                                       stack [base, base+size). entry must
 *                                       never return.
 */
#ifndef SP_FIBER_CTX_H
#define SP_FIBER_CTX_H

#include <stddef.h>
#include <stdint.h>

/* Windows takes the ucontext fallback, which lib/win32 provides: its x64
   ABI saves more registers, and a switch there also moves the stack bounds
   the thread's TEB holds. */
#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(_WIN32)
  #define SP_FIBER_ASM 1
#else
  #define SP_FIBER_ASM 0
#endif

/* AddressSanitizer keeps its own record of the stack the thread runs on.
   The asm switch moves the stack pointer without telling it, so the first
   exception after a switch (a longjmp) found the stack out of its bounds,
   left the frames it skipped poisoned, and later calls on those addresses
   were reported as stack overflows and uses after scope. Under
   -fsanitize=address (both the runtime and the program are built with it,
   as with ThreadSanitizer's SP_TSAN) a context also carries the bounds of
   its stack, and sp_ctx_swap announces each switch (sp_fiber.c). */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define SP_ASAN 1
#endif

#if SP_FIBER_ASM

/* The whole machine context is the saved stack pointer; the callee-saved
   registers are pushed onto / popped from the coroutine's own stack by
   sp_ctx_swap (see the file-scope asm in sp_fiber.c). */
#ifdef SP_ASAN
typedef struct sp_fiber_ctx { void *sp; const void *asan_lo; size_t asan_size; } sp_fiber_ctx;
#else
typedef struct sp_fiber_ctx { void *sp; } sp_fiber_ctx;
#endif

#if defined(__aarch64__)
/* The first code a fresh context runs (sp_fiber.c's asm): branches to the
   entry in x19 with lr and fp zero */
void sp_ctx_start(void);
#endif
static inline void sp_ctx_make(sp_fiber_ctx *ctx, void *base, size_t size,
                               void (*entry)(void)) {
  uintptr_t top = ((uintptr_t)base + size) & ~(uintptr_t)15; /* 16B-align the top */
#if defined(__x86_64__)
  /* SysV AMD64: entry's first instruction needs rsp%16 == 8 (as if reached by
     `call`). sp_ctx_swap pops 6 callee-saved regs then `ret`s into the slot
     just above them: [entry][rbp][rbx][r12][r13][r14][r15<-sp]. */
  void **s = (void **)(top - 8);
  *--s = (void *)entry;                  /* `ret` target, at top-16 */
  for (int i = 0; i < 6; i++) *--s = 0;  /* rbp, rbx, r12, r13, r14, r15 */
  ctx->sp = s;                           /* top-64 */
#else /* __aarch64__ */
  /* Mirror the aarch64 sp_ctx_swap frame: 160 bytes holding x19..x30 (12) then
     d8..d15 (8). On the first swap the ldp's load entry into x19 (slot 0) and
     sp_ctx_start into lr (x30, slot 11), and `ret` goes to sp_ctx_start, which
     enters entry with lr and fp zero: entry's frame then ends the stack for an
     unwinder, as the zero above the entry's slot does on x86_64. */
  void **s = (void **)(top - 160);
  for (int i = 0; i < 20; i++) s[i] = 0;
  s[0] = (void *)entry;                  /* x19 */
  s[11] = (void *)sp_ctx_start;          /* x30 (lr) */
  ctx->sp = s;
#endif
#ifdef SP_ASAN
  ctx->asan_lo = base; ctx->asan_size = size;
#endif
}

#else  /* portable fallback: POSIX ucontext */

#include <ucontext.h>
typedef struct sp_fiber_ctx { ucontext_t uc; } sp_fiber_ctx;

static inline void sp_ctx_make(sp_fiber_ctx *ctx, void *base, size_t size,
                               void (*entry)(void)) {
  getcontext(&ctx->uc);
  ctx->uc.uc_stack.ss_sp = base;
  ctx->uc.uc_stack.ss_size = size;
  ctx->uc.uc_link = NULL;               /* entry never returns (trampoline switches) */
  makecontext(&ctx->uc, entry, 0);
}

#endif

/* Defined as file-scope asm in sp_fiber.c (asm backend) or a swapcontext
   wrapper (fallback). */
void sp_ctx_swap(sp_fiber_ctx *from, sp_fiber_ctx *to);
#if SP_FIBER_ASM && defined(SP_ASAN)
/* The first code a fresh context runs (sp_fiber.c) */
void sp_ctx_entered(void);
#endif

#endif /* SP_FIBER_CTX_H */
