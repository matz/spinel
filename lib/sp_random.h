/* sp_random.h -- the shared Kernel-level PRNG stream (lib/sp_random.c).
 *
 * PCG-XSH-RR, the generator mruby's mruby-random uses: much better
 * statistical quality than libc rand(), no cryptographic claims. One
 * stream is shared by every runtime TU -- Kernel#rand/#srand, the array
 * shuffle/sample paths (generated TU and libspinel_rt.a alike) and the
 * Random default instance all draw from it, so a single `srand(n)`
 * makes every consumer reproducible. */
#ifndef SP_RANDOM_H
#define SP_RANDOM_H

#include <stdint.h>
#include "sp_types.h"

uint32_t sp_pcg32_adv(uint64_t *state);   /* one generator step */
void sp_pcg_seed(uint64_t *state, uint64_t seed);
uint64_t sp_krand_next(void);             /* 64-bit draw, lazily self-seeded */
void sp_krand_srand(uint64_t seed);
sp_int sp_krand_below(sp_int n);        /* uniform [0, n); 0 when n <= 0 */
sp_float sp_krand_float(void);           /* uniform [0, 1) */

/* Random -- per-instance PRNG. CRuby uses MT19937; spinel uses PCG-XSH-RR
   (above), so the *sequence* differs from MRI -- MT19937 is not part of
   the Ruby spec -- but each Random object keeps its own reproducible
   stream from its seed. The default instance is a window onto the shared
   Kernel stream, so srand() governs it too. */
typedef struct { uint64_t state; sp_int seed; } sp_Random;
/* The default instance is private to sp_random.c: it is a guarded static
   there (see the comment on its box) and is reached only through
   sp_random_default_get below. */
uint64_t sp_random_next(sp_Random *r);
sp_Random *sp_Random_new(sp_int seed);
sp_Random *sp_Random_new_float(sp_float f);
sp_Random *sp_Random_new_auto(void);
sp_Random *sp_Random_dup(sp_Random *r);
sp_int sp_Random_seed(sp_Random *r);
sp_bool sp_Random_eq(sp_Random *a, sp_Random *b);
sp_int sp_Random_rand_range(sp_Random *r, sp_Range rg);
int sp_Random_try_rand_range(sp_Random *r, sp_Range rg, sp_int *out);
sp_int sp_Random_rand_int(sp_Random *r, sp_int n);
struct sp_Bigint;
struct sp_Bigint *sp_bigint_rand(sp_Random *r, struct sp_Bigint *bound);
sp_float sp_Random_rand_float(sp_Random *r);
sp_float sp_Random_rand_float_bound(sp_Random *r, sp_float bound);
sp_float sp_Random_rand_float_range(sp_Random *r, sp_float lo, sp_float hi);
sp_float sp_Random_rand_float_range_excl(sp_Random *r, sp_float lo, sp_float hi, int excl);
sp_Random *sp_random_default_get(void);
const char *sp_Random_bytes(sp_Random *r, sp_int n);
sp_int sp_Random_new_seed(void);
const char *sp_Random_urandom(sp_int n);
const char *sp_Random_inspect(sp_Random *r);
sp_int sp_kernel_srand(sp_int seed);

#endif
