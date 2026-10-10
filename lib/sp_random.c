/* sp_random.c -- the shared Kernel-level PRNG stream and the Random
   instance methods (see sp_random.h). */
#include <time.h>
#include <string.h>
#include "sp_random.h"
#include "sp_crypto.h"   /* sp_crypto_entropy: the one secure source */
#include "sp_alloc.h"
#include <math.h>    /* isnan/isinf for the EDOM domain checks */   /* sp_str_alloc / sp_str_set_len / sp_float_to_s / sp_raise_cls / sp_gc_alloc */
#include "sp_format.h"  /* sp_Range_inspect */
#include "sp_str.h"     /* sp_sprintf (defined in the generated TU) */
#include "sp_range.h"   /* sp_range_int_only */

/* PCG-XSH-RR constants (64-bit state, 32-bit output). */
#define SP_PCG_MULT 6364136223846793005ULL
#define SP_PCG_INC  1442695040888963407ULL

uint32_t sp_pcg32_adv(uint64_t *state) {
  uint64_t old = *state;
  *state = old * SP_PCG_MULT + SP_PCG_INC;
  /* output: xorshift-high folds the state, then a random rotate */
  uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
  uint32_t rot = (uint32_t)(old >> 59u);
  return (xs >> rot) | (xs << ((32 - rot) & 31));
}

/* The PCG init sequence: zero the state, step, add the seed, then mix.
   The extra mixing rounds keep nearby seeds from producing correlated
   early output. */
void sp_pcg_seed(uint64_t *state, uint64_t seed) {
  *state = 0;
  (void)sp_pcg32_adv(state);
  *state += seed;
  for (int i = 0; i < 10; i++) (void)sp_pcg32_adv(state);
}

/* Per-worker (SP_TLS) in the threaded build: the generator has no
   internal lock, so a shared stream would race across workers. */
static SP_TLS uint64_t sp_krand_state;
static SP_TLS int sp_krand_seeded;

void sp_krand_srand(uint64_t seed) {
  sp_pcg_seed(&sp_krand_state, seed);
  sp_krand_seeded = 1;
}

uint64_t sp_krand_next(void) {
  if (!sp_krand_seeded) {
    /* Auto-seed on first draw, mirroring CRuby's startup seeding so
       unseeded rand/shuffle/sample vary per run. time() alone repeats
       within a second; mix in clock() and the per-worker state address. */
    sp_krand_srand((uint64_t)time(NULL) ^ ((uint64_t)clock() << 24) ^
                   (uint64_t)(uintptr_t)&sp_krand_state);
  }
  uint64_t hi = sp_pcg32_adv(&sp_krand_state);
  return (hi << 32) | sp_pcg32_adv(&sp_krand_state);
}

sp_int sp_krand_below(sp_int n) {
  if (n <= 0) return 0;
  /* uniform in [0, n) without modulo bias: reject draws below the
     wrap-around threshold (0 for powers of two, so the loop is rare) */
  uint64_t umax = (uint64_t)n;
  uint64_t threshold = (uint64_t)(-(int64_t)umax) % umax;
  uint64_t r;
  do {
    r = sp_krand_next();
  } while (r < threshold);
  return (sp_int)(r % umax);
}

sp_float sp_krand_float(void) {
  return (sp_float)(sp_krand_next() >> 11) / (sp_float)(1ULL << 53);
}

/* ---- Random instance methods ---- */

/* The default stream is a static, not a GC allocation -- but the class-method
   forms (`Random.bytes`, `Random.rand`) hand it to the same entry points an
   instance uses, and those root their receiver, which puts a .bss address on
   the mark path. Lay a 0xfd skip byte directly before it so sp_gc_mark's
   tag-byte protocol bails out instead of reading whatever precedes it as a GC
   header: under SPINEL_GC_STRESS=1 that read reached a bogus scan hook and
   crashed inside the mark walk. Same shape, and the same reason, as the root
   fiber in sp_fiber.c: the guard array is exactly one alignment unit, so its
   last byte always directly precedes the struct with no padding in between. */
static SP_TLS struct { char guard[_Alignof(sp_Random)]; sp_Random r; } sp_random_default_box
    = { .guard = { [_Alignof(sp_Random) - 1] = (char)0xfd } };
#define sp_random_default (sp_random_default_box.r)
uint64_t sp_random_next(sp_Random *r) {SP_GC_ROOT(r);
  if (r == &sp_random_default) return sp_krand_next();
  uint64_t hi = sp_pcg32_adv(&r->state);
  return (hi << 32) | sp_pcg32_adv(&r->state);
}
sp_Random *sp_Random_new(sp_int seed) {
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  sp_pcg_seed(&r->state, (uint64_t)seed);
  r->seed = seed;
  return r;
}
/* Random#dup / #clone: a distinct generator that continues from the same state. */
sp_Random *sp_Random_dup(sp_Random *r) {
  if (!r) return NULL;   /* nil.dup is nil */
  SP_GC_ROOT(r);
  sp_Random *d = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  *d = *r;
  return d;
}
/* Random.new(Float): CRuby truncates the float to an integer seed. A plain
   (sp_int) cast is UB when the truncated value is out of range, so seed from
   the in-range truncation when it fits and from the raw float bits otherwise
   -- always deterministic (same float -> same stream), which is the actual
   contract (the exact sequence is not MT19937 anyway). */
sp_Random *sp_Random_new_float(sp_float f) {
  if (!isfinite(f)) (void)sp_float_to_i_checked(f);
  uint64_t s;
  if (f >= -9.2233720368547758e18 && f < 9.2233720368547758e18) {
    s = (uint64_t)(int64_t)f;
  }
  else {
    uint64_t bits; memcpy(&bits, &f, sizeof bits);
    s = bits;
  }
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  sp_pcg_seed(&r->state, s);
  r->seed = (sp_int)s;
  return r;
}
/* Random#seed: the seed the instance was constructed from. */
sp_int sp_Random_seed(sp_Random *r) { return r ? r->seed : 0; }
/* Random#== compares by internal state (two same-seed, same-position instances
   are equal; advancing one makes them differ). */
sp_bool sp_Random_eq(sp_Random *a, sp_Random *b) {
  if (a == b) return TRUE;
  if (!a || !b) return FALSE;
  return (a->state == b->state && a->seed == b->seed) ? TRUE : FALSE;
}
/* Random.new with no seed: OS-entropy-ish auto seed. time() alone made two
   instances created in the same second identical; mix in a per-thread counter
   and the object address so every instance gets its own stream. */
sp_Random *sp_Random_new_auto(void) {
  static SP_TLS uint64_t sp_random_auto_ctr = 0;
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  uint64_t e = (((uint64_t)time(NULL)) << 20) ^ (++sp_random_auto_ctr * 0x9E3779B97F4A7C15ULL) ^
               (uint64_t)(uintptr_t)r;
  sp_pcg_seed(&r->state, e);
  r->seed = (sp_int)(e >> 1);
  return r;
}
/* Random#rand(Range): an integer in the (int-endpoint) range, empty raises. */
sp_int sp_Random_rand_range(sp_Random *r, sp_Range rg) {SP_GC_ROOT(r);
  sp_range_int_only(rg, "Random#rand");   /* (1..2.5): CRuby answers a Float */
  /* an open side has no span: CRuby's Errno::EDOM */
  if (rg.nobeg || rg.noend)
    sp_raise_cls("Errno::EDOM", "Numerical argument out of domain");
  sp_int lo = rg.first, hi = rg.excl ? rg.last - 1 : rg.last;
  if (hi < lo) sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %s", sp_Range_inspect(&rg)));
  if (!r) return lo;
  return lo + (sp_int)(sp_random_next(r) % ((uint64_t)(hi - lo) + 1));
}
sp_int sp_Random_rand_int(sp_Random *r, sp_int n) {SP_GC_ROOT(r);
  if (n <= 0) sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %lld", (long long)n));
  if (!r) return 0;
  return (sp_int)(sp_random_next(r) % (uint64_t)n);
}
sp_float sp_Random_rand_float(sp_Random *r) {SP_GC_ROOT(r);
  if (!r) return 0.0;
  return (sp_float)(sp_random_next(r) >> 11) / (sp_float)(1ULL << 53);
}
/* Random#rand(Float bound): a random Float in [0, bound). A non-positive bound
   raises ArgumentError like the Integer form (MRI validates both). */
sp_float sp_Random_rand_float_bound(sp_Random *r, sp_float bound) {SP_GC_ROOT(r);
  /* CRuby 4: a non-finite bound is Errno::EDOM; a 0.0 bound draws in [0,1) (#3049) */
  if (isnan(bound) || isinf(bound))
    sp_raise_cls("Errno::EDOM", "Numerical argument out of domain");
  if (bound == 0.0) return sp_Random_rand_float(r);
  if (bound < 0) sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %s", sp_float_to_s(bound)));
  return sp_Random_rand_float(r) * bound;
}
/* Class-method forms (`Random.rand` / `Random.bytes`) share the default
   instance, mirroring CRuby's Random::DEFAULT. Its draws redirect to the
   shared Kernel stream (see sp_random_next), which lazily self-seeds and
   is per-worker in the threaded build. */
sp_Random *sp_random_default_get(void) {
  return &sp_random_default;
}
/* Random#bytes(n) -- n random bytes as a String, tagged ASCII-8BIT as CRuby
   does. sp_str_set_len alone was not enough: #length counts UTF-8 units, and a
   short draw is valid UTF-8 by chance often enough that Random.bytes(8).length
   answered less than 8 about three times in a thousand. That is #3474, which
   fixed urandom eight lines below and left this one. */
const char *sp_Random_bytes(sp_Random *r, sp_int n) {SP_GC_ROOT(r);
  if (n < 0) sp_raise_cls("ArgumentError", "negative string size (or size too big)");
  char *b = sp_str_alloc((size_t)n);
  for (sp_int i = 0; i < n; i++) b[i] = (char)(sp_random_next(r) & 0xff);
  b[n] = 0;
  sp_str_set_len(b, (size_t)n);
  sp_str_mark_binary(b);
  return b;
}
/* Random#rand(Float range): a Float in [lo, hi) (or [lo, hi] for an inclusive
   range, though the float boundary is effectively open). */
sp_float sp_Random_rand_float_range(sp_Random *r, sp_float lo, sp_float hi) {SP_GC_ROOT(r);
  if (isnan(lo) || isinf(lo) || isnan(hi) || isinf(hi))
    sp_raise_cls("Errno::EDOM", "Numerical argument out of domain");
  return lo + sp_Random_rand_float(r) * (hi - lo);
}
/* Random.new_seed: a fresh nonneg seed drawn from the default stream. */
sp_int sp_Random_new_seed(void) {
  sp_int s = (sp_int)(sp_random_next(sp_random_default_get()) >> 1);
  return s < 0 ? -s : s;
}
/* Random.urandom(n): n random bytes as a String (spinel has no real OS entropy
   source wired up, so this draws from a freshly time-seeded stream). */
const char *sp_Random_urandom(sp_int n) {
  if (n < 0) sp_raise_cls("ArgumentError", "negative string size");
  /* CRuby's Random.urandom IS the OS entropy source, and this was a PCG seeded
     from time()/clock() -- a deterministic-per-run stand-in, from before the
     tree had a real source. It does now: sp_crypto_entropy is the one place
     that decides what counts as secure, and it fails closed. A caller of this
     name is minting something that has to be unguessable. */
  char *b = sp_str_alloc((size_t)n);
  { sp_int off = 0;
    while (off < n) {
      int chunk = (int)((n - off) > SPC_RANDOM_MAX ? SPC_RANDOM_MAX : (n - off));
      if (!sp_crypto_entropy((unsigned char *)b + off, chunk))
        sp_raise_cls("RuntimeError", "Random.urandom: no secure random source available");
      off += chunk;
    }
  }
  b[n] = 0;
  sp_str_set_len(b, (size_t)n);
  /* CRuby hands back ASCII-8BIT, so #length is the byte count. Without the
     mark, a short draw that happens to spell valid UTF-8 (about 1.5% of
     4-byte draws) counted code points and answered short (#3474). */
  sp_str_mark_binary(b);
  return b;
}
/* Random#inspect / #to_s: CRuby's default object rendering (the seed is not
   part of it; the address matches CRuby's zero-padded 16-digit form). */
const char *sp_Random_inspect(sp_Random *r) {SP_GC_ROOT(r);
  /* and its instance variables, as CRuby lists an object's */
  void *t = sp_ivtbl_inspect_fn ? sp_ivtbl_get(r) : NULL;
  if (t) return sp_sprintf("#<Random:0x%016llx%s>", (unsigned long long)(uintptr_t)r, sp_ivtbl_inspect_fn(t));
  return sp_sprintf("#<Random:0x%016llx>", (unsigned long long)(uintptr_t)r);
}
/* Kernel#srand: seed the shared Kernel stream and remember the previous
   seed, which srand returns (CRuby returns the prior seed, not the new
   one). Every rand form -- bare/int/range, shuffle, sample, the Random
   default instance -- draws from that one stream, so a single srand makes
   them all reproducible. */
static SP_TLS sp_int sp_kernel_seed = 0;
sp_int sp_kernel_srand(sp_int seed) {
  sp_int prev = sp_kernel_seed;
  sp_kernel_seed = seed;
  sp_krand_srand((uint64_t)seed);
  return prev;
}
/* Random#rand(Bignum bound): a uniform Bigint in [0, bound). Composed from
   32-bit chunks of the instance stream; accumulating two extra chunks past
   the bound keeps the modulo bias around 2^-64. A non-positive bound raises
   ArgumentError like the sp_int form (#3058). */
typedef struct sp_Bigint sp_Bigint;
extern sp_Bigint *sp_bigint_new_int(int64_t v);
extern sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
extern sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
extern sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
extern int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
extern const char *sp_bigint_to_s(sp_Bigint *b);
sp_Bigint *sp_bigint_rand(sp_Random *r, sp_Bigint *bound) {SP_GC_ROOT(r);
  SP_GC_ROOT(bound);
  sp_Bigint *zero = sp_bigint_new_int(0);
  SP_GC_ROOT(zero);
  if (sp_bigint_cmp(bound, zero) <= 0)
    sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %s", sp_bigint_to_s(bound)));
  sp_Bigint *acc = zero;
  SP_GC_ROOT(acc);
  sp_Bigint *b32 = sp_bigint_new_int((int64_t)1 << 32);
  SP_GC_ROOT(b32);
  int extra = 2;
  while (extra > 0) {
    sp_Bigint *chunk = sp_bigint_new_int((int64_t)(sp_random_next(r) & 0xffffffffULL));
    SP_GC_ROOT(chunk);
    acc = sp_bigint_add(sp_bigint_mul(acc, b32), chunk);
    if (sp_bigint_cmp(acc, bound) >= 0) extra--;
  }
  return sp_bigint_mod(acc, bound);
}
