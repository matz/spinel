/* sp_win32_ctx.c -- the coroutine context switch for Windows x64, behind
   <ucontext.h> (ucontext.h here), which the runtime's coroutine fallback
   calls (lib/sp_fiber_ctx.h).

   sp_fiber.c's x86_64 switch is written for the System V ABI; Windows x64
   passes the arguments in rcx/rdx, keeps rdi, rsi and xmm6-xmm15 across a
   call as well, and leaves 32 bytes of home space above a callee's return
   address. And Windows keeps the running stack's bounds in the thread's TEB
   -- StackBase, StackLimit, DeallocationStack -- which the unwinder checks
   frames against and the memory manager consults to grow a stack on its
   guard page. So the switch saves and restores those three with the
   registers: each coroutine stack is, to Windows, the thread's stack while
   it runs, and grows the way a thread's does (sp_win32.c maps a MAP_STACK
   region like one: committed at the top, a guard page below).

   The frame sp_w32_swapcontext leaves on the stack it switches away from, from the
   saved stack pointer up:
     +0   DeallocationStack   gs:[0x1478]
     +8   StackLimit          gs:[0x10]
     +16  StackBase           gs:[0x08]
     +24  xmm6 .. xmm15       10 x 16 bytes
     +184 r15 r14 r13 r12 rsi rdi rbx rbp
     +248 return address */
#if defined(_WIN64) && defined(__x86_64__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ucontext.h>

__asm__(
  ".text\n"
  ".globl sp_w32_swapcontext\n"
  ".def sp_w32_swapcontext; .scl 2; .type 32; .endef\n"
  "sp_w32_swapcontext:\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %rdi\n  pushq %rsi\n"
  "  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
  "  subq $160, %rsp\n"
  "  movups %xmm6,    0(%rsp)\n  movups %xmm7,   16(%rsp)\n"
  "  movups %xmm8,   32(%rsp)\n  movups %xmm9,   48(%rsp)\n"
  "  movups %xmm10,  64(%rsp)\n  movups %xmm11,  80(%rsp)\n"
  "  movups %xmm12,  96(%rsp)\n  movups %xmm13, 112(%rsp)\n"
  "  movups %xmm14, 128(%rsp)\n  movups %xmm15, 144(%rsp)\n"
  "  pushq %gs:0x08\n"           /* StackBase */
  "  pushq %gs:0x10\n"           /* StackLimit */
  "  pushq %gs:0x1478\n"         /* DeallocationStack */
  "  movq %rsp, (%rcx)\n"        /* from->sp = rsp */
  "  movq (%rdx), %rsp\n"        /* rsp = to->sp   */
  "  popq %gs:0x1478\n"
  "  popq %gs:0x10\n"
  "  popq %gs:0x08\n"
  "  movups    0(%rsp), %xmm6\n  movups   16(%rsp), %xmm7\n"
  "  movups   32(%rsp), %xmm8\n  movups   48(%rsp), %xmm9\n"
  "  movups   64(%rsp), %xmm10\n  movups   80(%rsp), %xmm11\n"
  "  movups   96(%rsp), %xmm12\n  movups  112(%rsp), %xmm13\n"
  "  movups  128(%rsp), %xmm14\n  movups  144(%rsp), %xmm15\n"
  "  addq $160, %rsp\n"
  "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
  "  popq %rsi\n  popq %rdi\n  popq %rbx\n  popq %rbp\n"
  "  xorl %eax, %eax\n"          /* swapcontext answers 0 */
  "  ret\n"
);

/* A context makecontext can prime: the stack it runs on is the caller's to
   set in uc_stack afterwards, as POSIX has it. */
int sp_w32_getcontext(ucontext_t *uc) {
  if (!uc) { errno = EINVAL; return -1; }
  memset(uc, 0, sizeof *uc);
  return 0;
}

/* Prime a context so the first switch into it starts fn() on the stack
   uc_stack names. The TEB values it will run under: the top as StackBase,
   the reservation's start as DeallocationStack, and as StackLimit the
   bottom of the committed run at the top (the guard page sits just below
   it; the memory manager moves both as the stack grows). fn takes no
   arguments here and must not return. */
void sp_w32_makecontext(ucontext_t *uc, void (*fn)(void), int argc, ...) {
  (void)argc;
  char *base = (char *)uc->uc_stack.ss_sp;
  uintptr_t top = ((uintptr_t)base + uc->uc_stack.ss_size) & ~(uintptr_t)15;
  void *dealloc = base, *limit = base;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery((void *)(top - 1), &mbi, sizeof mbi)) {
    dealloc = mbi.AllocationBase;
    limit = (void *)top;
    if (mbi.State == MEM_COMMIT) {
      /* VirtualQuery's BaseAddress is the page of the address asked about,
         not the start of the run it belongs to: walk the reservation from
         its start to the run holding the top, whose BaseAddress is then the
         bottom of the committed top. With the top page alone as StackLimit,
         every frame deeper than one page (the rest of the committed top,
         which no guard page fault ever reports to the memory manager) was
         outside the stack to RtlUnwindEx, and a C++ exception thrown there
         -- DirectWrite throws and catches its own -- ended the process with
         STATUS_BAD_STACK (0xC0000028). */
      char *q = (char *)dealloc;
      MEMORY_BASIC_INFORMATION r;
      while (q < (char *)top && VirtualQuery(q, &r, sizeof r)) {
        if ((char *)r.BaseAddress + r.RegionSize >= (char *)top) {
          if (r.State == MEM_COMMIT && !(r.Protect & PAGE_GUARD)) limit = r.BaseAddress;
          break;
        }
        q = (char *)r.BaseAddress + r.RegionSize;
      }
    }
  }
  /* fn's first instruction sees rsp % 16 == 8, as after a call, with its
     32 bytes of home space above the return slot */
  uintptr_t ret_slot = top - 48;
  void **s = (void **)(ret_slot - 248);
  memset(s, 0, 256);
  s[0] = dealloc;
  s[1] = limit;
  s[2] = (void *)top;
  s[31] = (void *)fn;   /* +248 */
  uc->sp = s;
}
#endif
