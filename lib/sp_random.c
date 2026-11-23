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
static SP_TLS sp_int sp_kernel_seed;

void sp_krand_srand(uint64_t seed) {
  sp_pcg_seed(&sp_krand_state, seed);
  sp_krand_seeded = 1;
}

uint64_t sp_krand_next(void) {
  if (!sp_krand_seeded) {
    /* Auto-seed on first draw from OS entropy and record the seed so an
       initial srand() returns the seed that actually drove prior draws,
       mirroring CRuby 4.0. */
    sp_int init = sp_Random_new_seed();
    sp_kernel_seed = init;
    sp_krand_srand((uint64_t)init);
  }
  uint64_t hi = sp_pcg32_adv(&sp_krand_state);
  return (hi << 32) | sp_pcg32_adv(&sp_krand_state);
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
    = { .guard = { [_Alignof(sp_Random) - 1] = (char)SP_STATIC_GUARD } };
#define sp_random_default (sp_random_default_box.r)

static uint64_t sp_random_raw_next(sp_Random *r) {
  if (r == &sp_random_default) return sp_krand_next();
  uint64_t hi = sp_pcg32_adv(&r->state);
  return (hi << 32) | sp_pcg32_adv(&r->state);
}

/* Uniform uint64_t in [0, bound) for bound >= 1 without modulo bias.
   Uses Lemire's nearly divisionless method when 128-bit multiplication is
   available (SP_HAVE_INT128), and standard 64-bit rejection sampling as the
   portable fallback. Degenerate bound <= 1 returns 0 without advancing the
   stream, matching CRuby 4.0. */
static uint64_t sp_random_below_u64(sp_Random *r, uint64_t bound) {
  if (bound <= 1) return 0;
#if SP_HAVE_INT128
  sp_uint128 m = (sp_uint128)sp_random_raw_next(r) * (sp_uint128)bound;
  uint64_t l = (uint64_t)m;
  if (l < bound) {
    uint64_t t = ((uint64_t)0 - bound) % bound;
    while (l < t) {
      m = (sp_uint128)sp_random_raw_next(r) * (sp_uint128)bound;
      l = (uint64_t)m;
    }
  }
  return (uint64_t)(m >> 64);
#else
  uint64_t threshold = ((uint64_t)0 - bound) % bound;
  uint64_t x;
  do {
    x = sp_random_raw_next(r);
  } while (x < threshold);
  return x % bound;
#endif
}

sp_int sp_krand_below(sp_int n) {
  if (n <= 1) return 0;
  return (sp_int)sp_random_below_u64(&sp_random_default, (uint64_t)n);
}

sp_float sp_krand_float(void) {
  return (sp_float)(sp_krand_next() >> 11) / (sp_float)(1ULL << 53);
}

uint64_t sp_random_next(sp_Random *r) {SP_GC_ROOT(r);
  return sp_random_raw_next(r);
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
/* Random.new with no seed: seed from OS entropy via sp_Random_new_seed so
   every instance gets an independent stream and Random.new(r.seed) reproduces
   it, matching CRuby 4.0. */
sp_Random *sp_Random_new_auto(void) {
  sp_int seed = sp_Random_new_seed();
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  sp_pcg_seed(&r->state, (uint64_t)seed);
  r->seed = seed;
  return r;
}
/* Draw a uniform integer in [rg.first, rg.last] (or [rg.first, rg.last) when
   rg.excl). Returns 1 and writes *out, or returns 0 when the range is empty.
   Uses unsigned difference arithmetic so signed overflow/underflow UB and
   full-span (+1 wrap-to-zero) cannot occur on either 32-bit or 64-bit sp_int. */
int sp_Random_try_rand_range(sp_Random *r, sp_Range rg, sp_int *out) {SP_GC_ROOT(r);
  sp_int lo = rg.first, hi = rg.last;
  if (rg.excl) {
    if (lo >= hi) return 0;
    if (!r) { *out = lo; return 1; }
    uint64_t bound = (uint64_t)hi - (uint64_t)lo;
    *out = (sp_int)((uintptr_t)lo + (uintptr_t)sp_random_below_u64(r, bound));
    return 1;
  }
  if (lo > hi) return 0;
  if (!r) { *out = lo; return 1; }
  uint64_t span = (uint64_t)hi - (uint64_t)lo;
  uint64_t delta = (span == UINT64_MAX) ? sp_random_raw_next(r)
                                        : sp_random_below_u64(r, span + 1);
  *out = (sp_int)((uintptr_t)lo + (uintptr_t)delta);
  return 1;
}
/* Random#rand(Range): an integer in the (int-endpoint) range, empty raises. */
sp_int sp_Random_rand_range(sp_Random *r, sp_Range rg) {SP_GC_ROOT(r);
  sp_range_int_only(rg, "Random#rand");   /* (1..2.5): CRuby answers a Float */
  /* an open side has no span: CRuby's Errno::EDOM */
  if (rg.nobeg || rg.noend)
    sp_raise_cls("Errno::EDOM", "Numerical argument out of domain");
  sp_int out;
  if (!sp_Random_try_rand_range(r, rg, &out))
    sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %s", sp_Range_inspect(&rg)));
  return out;
}
sp_int sp_Random_rand_int(sp_Random *r, sp_int n) {SP_GC_ROOT(r);
  if (n <= 0) sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %lld", (long long)n));
  if (!r) return 0;
  return (sp_int)sp_random_below_u64(r, (uint64_t)n);
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
   fixed sp_Random_urandom below and left this one. */
const char *sp_Random_bytes(sp_Random *r, sp_int n) {SP_GC_ROOT(r);
  if (n < 0) sp_raise_cls("ArgumentError", "negative string size (or size too big)");
  char *b = sp_str_alloc((size_t)n);
  for (sp_int i = 0; i < n; i++) b[i] = (char)(sp_random_next(r) & 0xff);
  b[n] = 0;
  sp_str_set_len(b, (size_t)n);
  sp_str_mark_binary(b);
  return b;
}
/* Random#rand(Float range): a Float in [lo, hi] (or [lo, hi) when excl).
   Validates finite bounds and non-empty ordering, scales large spans via
   halved endpoints so -Float::MAX..Float::MAX never overflows to Inf, and
   clamps rounding at the endpoints to preserve the interval invariant. */
sp_float sp_Random_rand_float_range_excl(sp_Random *r, sp_float lo, sp_float hi, int excl) {SP_GC_ROOT(r);
  if (!isfinite(lo) || !isfinite(hi))
    sp_raise_cls("Errno::EDOM", "Numerical argument out of domain");
  if (hi < lo || (excl && hi == lo)) {
    const char *sep = excl ? "..." : "..";
    const char *lo_s = sp_float_to_s(lo);
    SP_GC_ROOT_STR(lo_s);
    const char *hi_s = sp_float_to_s(hi);
    SP_GC_ROOT_STR(hi_s);
    sp_raise_cls("ArgumentError", sp_sprintf("invalid argument - %s%s%s", lo_s, sep, hi_s));
  }
  if (lo == hi) return lo + 0.0;
  if (!r) return lo;
  sp_float u = sp_Random_rand_float(r);
  sp_float span = hi - lo;
  sp_float val;
  if (isfinite(span)) {
    val = lo + u * span;
  }
  else {
    sp_float half_lo = lo * 0.5;
    sp_float half_hi = hi * 0.5;
    sp_float half_val = half_lo + u * (half_hi - half_lo);
    if (half_val < half_lo) half_val = half_lo;
    if (half_val > half_hi) half_val = half_hi;
    val = half_val * 2.0;
  }
  if (val < lo) val = lo;
  if (excl) {
    if (val >= hi) val = nextafter(hi, lo);
  }
  else if (val > hi) {
    val = hi;
  }
  return val;
}
sp_float sp_Random_rand_float_range(sp_Random *r, sp_float lo, sp_float hi) {
  return sp_Random_rand_float_range_excl(r, lo, hi, 0);
}
/* Random.new_seed: a fresh positive seed drawn from OS entropy
   (sp_crypto_entropy), independent of the default PRNG stream. */
sp_int sp_Random_new_seed(void) {
  static SP_TLS uint64_t sp_seed_fallback_ctr = 0;
  uint64_t raw = 0;
  if (!sp_crypto_entropy((unsigned char *)&raw, (int)sizeof(raw))) {
    uint64_t s = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 24) ^
                 (++sp_seed_fallback_ctr * 0x9E3779B97F4A7C15ULL) ^
                 (uint64_t)(uintptr_t)&raw;
    sp_pcg_seed(&s, s);
    raw = ((uint64_t)sp_pcg32_adv(&s) << 32) | sp_pcg32_adv(&s);
  }
  sp_int seed = (sp_int)(raw & (uint64_t)INTPTR_MAX);
  return seed == 0 ? 1 : seed;
}
/* Random.urandom(n): n bytes from the OS entropy source (sp_crypto_entropy) as
   a String. Raises RuntimeError when sp_crypto_entropy has no secure source. */
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
   them all reproducible. An unseeded stream initializes its prior seed from
   OS entropy so the first srand() returns a non-zero seed, matching CRuby 4.0. */
sp_int sp_kernel_srand(sp_int seed) {
  if (!sp_krand_seeded) {
    sp_kernel_seed = sp_Random_new_seed();
  }
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
