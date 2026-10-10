/* analyze_share.c -- the share classes --share-strings decides by (#6765).

   See share.h. One walk over the node table gives every node a value (an
   element of the union-find, or none) and unifies what flows together:
   each write with its target, each container store with the container's
   elements, each argument with the parameter it binds, each yield with the
   blocks the method is called with, each return with the method's value,
   each break or next with the call, loop or block it leaves (sh_jumps),
   each throw with its lexical catch or UNKNOWN,
   each pattern variable with the part it matches (sh_pattern).
   A builtin call is read off its builtin-op share row (bop_share); a user
   call off its call plan (cplan_user_fresh). What the walk does not follow
   -- a proc or Method call, a runtime `send`, a builtin with no row -- joins
   UNKNOWN, so a case it misses costs a handle, never a silent copy. Not
   yet followed: the parts a pattern reads off an object's deconstruct, a
   user Enumerable's generator (Fiber.yield), and a String a native class
   keeps (its C copies it).

   A parameter its method only reads and mutates is lent, as the lent slot
   is: binding it does not unify, and its mutation marks each argument's
   class instead. That keeps `grow(buf)` on a local accumulator a
   `const char *` lent by address, as without the flag. */

#include <stdlib.h>
#include <string.h>
#include "analyze_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "share.h"
#include "repr.h"
#include "codegen_internal.h"

/* element-own flags (not merged by a union). SHE_COMPARED: the value is
   compared by identity, or handed to a consumer that keeps or mutates it
   (sh_finalize reads it for a String reopening's receiver) */
enum { SHE_WRITTEN = 1, SHE_IDENTITY = 2, SHE_COMPARED = 4 };
/* a class flag beside share.h's SHF_*: a value of the class leaves a call
   to be read after it (`p(lit.each { |x| x << y })`, `lit.map { }.first`),
   so a container of the class can be reached again (sh_finalize) */
enum { SHF_OUT = 8 };
/* per-node marks: a statement whose value is dropped (SHU_STMT), and a value
   its construct drops that the walk itself still follows -- a loop body's
   last statement, the tail of a block whose iterator keeps none of its
   values (SHU_TAIL). Only the method reads (sh_settle_reads) take the
   second. SHU_SPLIT marks an Array literal a multiple write splits into its
   targets (sh_masgn_plain): no container holds its elements. SHU_PEEK
   marks a container literal handed to a builtin that only reads it and
   keeps none of it (`puts [a, b]`, `p [a, b]` as a statement), and such a
   `p a, b` itself: no name sees its elements again (sh_settle_peeks).
   It also marks transient arguments and receivers whose builtin keeps
   none of their value, including fresh call results needing no handle. */
enum { SHU_STMT = 1, SHU_TAIL = 2, SHU_SPLIT = 4, SHU_PEEK = 8 };
/* per-node answers built from the flows (ShareFacts.into) */
enum { SHI_INTO = 1, SHI_MUTATED = 2, SHI_LITOUT = 4 };

/* The builtin side of a mixed boxed String call can be fresh even when its
   joined user result is not. Keep the exact selected row and call-shape fact
   from the share walk so the later return-tail proof can revalidate it without
   replaying sh_builtin's effects. */
typedef struct {
  int call, share, family, container, recv_type, result_type, plan_ret;
  int argc, ntg;
  const BuiltinOp *row;
  int *arg_types;
  int *targets;
} ShBoxedFreshFact;

typedef struct ShareFacts {
  int n, cap;
  /* key: per element, the keys a Hash's lookups ask for, which its default
     proc is handed (sh_key; -1 none) */
  int *key;
  int *parent, *elem, *nhold, *nmem, *nelem, *hidx, *owner;
  /* per root: the method scope every holder of the class is a plain local
     of (no parameter, no captured local), or -1 none yet, or -2 when some
     member is anything else */
  int *lsc;
  /* a method's return of a value, held back until the classes settle: one
     whose class is still only that method's own locals hands the caller a
     String nobody else names, and joins no class (sh_settle_rets) */
  int *ret_m, *ret_v, nret, cret;
  unsigned char *ret_done;
  unsigned char *unused;   /* per node: SHU_* */
  /* the call and super nodes that reach a user method, whose value is the
     method's then, and per method scope, once settled, whether a caller
     may read its value (sh_settle_reads) */
  int *rsite, nrsite, crsite;
  unsigned char *mread;
  /* per method scope: a String it answers joined its value (sh_settle_rets,
     a multiple return, a dynamic call): one it does not answers only
     Strings no other name holds (share_call_fresh) */
  unsigned char *ret_joined;
  /* the literal blocks each yielding method is given (sh_block_to_method):
     pairs, then once built, per method scope its run (mb_start..mb_start+1)
     of mb_blk; blk_dyn per method scope: a block the walk does not list
     reaches its yields (a block passed as a value, a zsuper's, a dynamic
     call's) */
  int *mb_m, *mb_b, nmb, cmb;
  int *mb_start, *mb_blk;
  unsigned char *blk_dyn;
  /* the blocks a method can run as share_value_fresh asks: a method's
     `&blk` handed on (`m2(&blk)`, an anonymous `&`, a zsuper) as an edge
     from the method to each it reaches (fw_from, fw_to), and blk_unk per
     method scope: a block that is neither a literal nor such a forward
     reaches it; fresh_blk per method scope, once asked: every block it can
     run answers a new String (1), not (0), not asked (-1) */
  int *fw_from, *fw_to, nfw, cfw;
  unsigned char *blk_unk;
  signed char *fresh_blk;
  /* per node, once asked: a `call` on a callable whose every value is a
     new String or no String (1), not (0), not asked (-1); NULL until then */
  signed char *fresh_call;
  /* the methods the default build may lend a parameter's slot
     (an_byref_eligible_scopes), and per method scope, the parameters it
     passes by value (byval), once asked (byval_done) */
  char *byref_elig;
  int own_elig;        /* byref_elig is this build's own, not c->byref_elig */
  unsigned *byval, *byval_done;
  /* the mutation sites, for SPINEL_SHARE_STATS=3: node, value */
  int *mut_n, *mut_v, nmut, cmut;
  /* the flows (sh_flow): per flow, the node that takes the value (site),
     the node whose value it takes, and the kind (ShareFlowKind) */
  int *fl_site, *fl_val, nfl, cfl;
  unsigned char *fl_kind;
  /* per flow, the union-find element that takes the value -- the holder of
     a variable, a parameter, an ivar or a block parameter, or the elements
     of a container -- or -1 when the walk made none (sh_flow) */
  int *fl_dest;
  /* per node, once asked (sh_into_build): SHI_INTO when a flow takes its
     value into a holder whose class the rule shares, SHI_MUTATED when it is
     the receiver of an in-place String change, SHI_LITOUT when it is an
     element of a container literal whose Strings the rule does not hand
     out; NULL until then */
  unsigned char *into;
  /* the calls sh_peek_args recorded: n for one that only reads its
     arguments, -n-1 for one that answers them */
  int *pk, npk, cpk;
  /* per node, a lambda a local holds and only calls (sh_mark_local_lambdas):
     the lambda node itself, and each `f.call(...)` of that local; -1 for
     any other node, or NULL when the program has none (read by
     share_value_fresh) */
  int *lam;
  int *hcount;         /* per root, once built: holders storing a String */
  unsigned char *anchored;   /* per root, once built: a container of the class
                                can be reached again (sh_finalize) */
  unsigned char *kind, *flags, *own;
  /* the holders, and their element */
  ShareHolder *h;
  int *helem, nh, ch;
  /* holder keys: a hash over (kind, a, name) */
  int *bucket, *hnext;
  int nbucket;
  /* each node's value: -2 not yet computed, -1 none */
  int *nval, nnodes;
  /* per node: a blockless builtin call answering a new String Array of
     new Strings (`s.scan(re)`, `s.split`), whose value is a class of its
     own (sh_builtin) */
  unsigned char *fresh_cont;
  /* mixed boxed-String calls whose selected builtin row actually answered
     with no carried identity (`sh_builtin` returned -1), alongside the
     selected row and input signature used to revalidate the proof */
  ShBoxedFreshFact *boxed_fresh;
  int nboxed_fresh, cboxed_fresh;
  /* lazily allocated per-node index into boxed_fresh (stored as index + 1) */
  int *boxed_fresh_index;
  /* `break v` and `next v` (sh_jumps): per node, the value the breaks out
     of a call's block or a loop hand the call or the loop, or the nexts of
     a block hand the block (-1 none); and the nodes the walk that finds
     them reached */
  int *jump;
  int catch_unknown;   /* a throw whose dynamic target the walk cannot name */
  unsigned char *jseen;
  /* per node, a `Fiber.yield` in an Enumerator.new block (the yielder a
     desugared `y << v` is): that Enumerator.new call, or -1 (sh_jumps) */
  int *fgen;
  /* the Hash lookups' containers and keys (sh_lookup_key) */
  int *lk_c, *lk_k, nlk, clk;
  unsigned char *lk_done;
  /* lent bindings: argument value -> parameter holder element, and the
     argument's node */
  int *lend_arg, *lend_par, *lend_node;
  unsigned char *lend_direct, *lend_done;
  int nlend, clend;
  /* the method names a Method, `send` or define_method can reach */
  const char **dyn;
  int ndyn, cdyn, dyn_all, dyn_ivars;
  const char **mconst;
  int nmconst, cmconst;
  int unknown;
  /* the Strings handed to an exception (an exception class's new, raise,
     an exception's super into Exception#initialize), which its message
     answers (sh_exc), or -1 before the first */
  int exc;
  int ostruct;         /* the program names OpenStruct, whose fields a poly
                          receiver's call may read */
  int singleton_accessors; /* some class has singleton readers/writers whose
                               class-side holder edges are not in this graph */
  int closed;          /* unions with UNKNOWN are dropped (the stats' second build) */
  int union_stack_cap;
  int *union_stack;
  /* `k.new(...)` with k a class held in a variable reaches any initialize:
     its arguments join any_new_pos (by position; the 16th and later share
     the last) and any_new_kw (its keywords), and its literal blocks
     any_new_blk, which every initialize takes once after the walk
     (sh_settle_any_new) */
  int any_new_pos[16], any_new_kw, any_new_used;
  /* the values of every user deconstruct (0) and deconstruct_keys (1), a
     pattern over a value that may be any object reads its parts off
     (sh_deconstructed; -2 not made yet, -1 none) */
  int any_dec[2];
  /* does the program make a Lazy (a `lazy` call): -1 not asked yet */
  int any_lazy;
  /* does the program make a proc or a Method a box could hold (a lambda,
     a proc literal, a block taken as &blk, a Method, a to_proc): -1 not
     asked yet */
  int any_callable;
  int *any_new_blk, nany_blk, cany_blk;
  /* the attr readers and writers of every class by name, and the method
     scopes by name, sorted for a binary search (built on first use) */
  struct ShNamed { const char *name; int k, yielder; } *attr_r, *attr_w;
  int nattr_r, nattr_w, named_built;
  /* the variables a literal block or a lambda binds (its parameters and
     block-locals), by name, k the scope they live in, yielder when it is
     Enumerator.new's yielder; built on first use (sh_blk_bound) */
  struct ShNamed *blkp;
  int nblkp, cblkp, blkp_built;
} ShareFacts;

/* ---- the union-find ---- */

static int sh_find(ShareFacts *F, int x) {
  while (F->parent[x] != x) {
    F->parent[x] = F->parent[F->parent[x]];
    x = F->parent[x];
  }
  return x;
}

static int sh_storing_kind(int k) {
  if (k == SHK_SELF) return 1;
  return k == SHK_LOCAL || k == SHK_IVAR || k == SHK_GVAR || k == SHK_CVAR ||
         k == SHK_CONST || k == SHK_ELEM;
}

static int sh_new(ShareFacts *F, int kind) {
  if (F->n >= F->cap) {
    int nc = F->cap ? F->cap * 2 : 1024;
    F->parent = realloc(F->parent, sizeof(int) * (size_t)nc);
    F->elem = realloc(F->elem, sizeof(int) * (size_t)nc);
    F->key = realloc(F->key, sizeof(int) * (size_t)nc);
    F->nhold = realloc(F->nhold, sizeof(int) * (size_t)nc);
    F->nmem = realloc(F->nmem, sizeof(int) * (size_t)nc);
    F->nelem = realloc(F->nelem, sizeof(int) * (size_t)nc);
    F->hidx = realloc(F->hidx, sizeof(int) * (size_t)nc);
    F->owner = realloc(F->owner, sizeof(int) * (size_t)nc);
    F->lsc = realloc(F->lsc, sizeof(int) * (size_t)nc);
    F->kind = realloc(F->kind, (size_t)nc);
    F->flags = realloc(F->flags, (size_t)nc);
    F->own = realloc(F->own, (size_t)nc);
    if (!F->parent || !F->elem || !F->key || !F->nhold || !F->nmem || !F->nelem || !F->hidx || !F->owner ||
        !F->lsc || !F->kind || !F->flags || !F->own) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    F->cap = nc;
  }
  int e = F->n++;
  F->parent[e] = e;
  F->elem[e] = -1;
  F->key[e] = -1;
  F->nhold[e] = sh_storing_kind(kind);
  F->nmem[e] = 1;
  F->nelem[e] = kind == SHK_ELEM;
  F->hidx[e] = -1;
  F->owner[e] = -1;
  F->lsc[e] = -2;
  F->kind[e] = (unsigned char)kind;
  F->flags[e] = 0;
  F->own[e] = 0;
  return e;
}

static void sh_union(ShareFacts *F, int a, int b) {
  if (a < 0 || b < 0) return;
  int sp = 0;
  if (F->union_stack_cap < 64) {
    F->union_stack_cap = 64;
    F->union_stack = realloc(F->union_stack, sizeof(int) * 2 * 64);
    if (!F->union_stack) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->union_stack[sp++] = a; F->union_stack[sp++] = b;
  while (sp > 0) {
    int y = F->union_stack[--sp], x = F->union_stack[--sp];
    int rx = sh_find(F, x), ry = sh_find(F, y);
    if (rx == ry) continue;
    if (F->closed) {
      int ru = sh_find(F, F->unknown);
      if (rx == ru || ry == ru) continue;
    }
    if (F->nmem[rx] < F->nmem[ry]) { int t = rx; rx = ry; ry = t; }
    F->parent[ry] = rx;
    F->flags[rx] |= F->flags[ry];
    F->nhold[rx] += F->nhold[ry];
    F->nmem[rx] += F->nmem[ry];
    F->nelem[rx] += F->nelem[ry];
    if (F->lsc[rx] == -1 || F->lsc[ry] == -2) F->lsc[rx] = F->lsc[ry];
    else if (F->lsc[ry] != -1 && F->lsc[ry] != F->lsc[rx]) F->lsc[rx] = -2;
    for (int part = 0; part < 2; part++) {
      int *slot = part ? F->key : F->elem;
      int ex = slot[rx], ey = slot[ry];
      if (ex < 0) { slot[rx] = ey; continue; }
      if (ey < 0) continue;
      if (sp + 2 > F->union_stack_cap * 2) {
        F->union_stack_cap *= 2;
        F->union_stack = realloc(F->union_stack, sizeof(int) * 2 * (size_t)F->union_stack_cap);
        if (!F->union_stack) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      F->union_stack[sp++] = ex; F->union_stack[sp++] = ey;
    }
  }
}

/* the element of x's containers' elements, made on first use */
static int sh_elem(ShareFacts *F, int x) {
  if (x < 0) return -1;
  int r = sh_find(F, x);
  if (F->elem[r] < 0) {
    int e = sh_new(F, SHK_ELEM);
    r = sh_find(F, x);
    F->elem[r] = e;
    F->owner[e] = r;
  }
  return F->elem[r];
}

/* The same element when x's class has one already, else -1: a flow's
   destination, which the walk must not make (a class that gained an element
   here would change what the seal sees). */
static int sh_elem_peek(ShareFacts *F, int x) {
  return x < 0 ? -1 : F->elem[sh_find(F, x)];
}

/* The keys x's Hashes' default procs are handed, made by a Hash.new block
   that takes one (sh_builtin_new) */
static int sh_key(ShareFacts *F, int x) {
  if (x < 0) return -1;
  int r = sh_find(F, x);
  if (F->key[r] < 0) {
    int e = sh_new(F, SHK_VALUE);
    F->key[sh_find(F, x)] = e;
  }
  return F->key[sh_find(F, x)];
}

/* A lookup of key k in container value c (`h[k]`, `h.dig(k)`): if c's
   class turns out to hold a Hash with a default proc, the proc is handed k
   itself (sh_settle_keys). */
static void sh_lookup_key(ShareFacts *F, int c, int k) {
  if (c < 0 || k < 0) return;
  if (F->nlk >= F->clk) {
    F->clk = F->clk ? F->clk * 2 : 64;
    F->lk_c = realloc(F->lk_c, sizeof(int) * (size_t)F->clk);
    F->lk_k = realloc(F->lk_k, sizeof(int) * (size_t)F->clk);
    F->lk_done = realloc(F->lk_done, (size_t)F->clk);
    if (!F->lk_c || !F->lk_k || !F->lk_done) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->lk_c[F->nlk] = c;
  F->lk_k[F->nlk] = k;
  F->lk_done[F->nlk++] = 0;
}

static void sh_mark_at(ShareFacts *F, int x, unsigned fl, int node) {
  if (x < 0) return;
  if (F->nmut >= F->cmut) {
    F->cmut = F->cmut ? F->cmut * 2 : 64;
    F->mut_n = realloc(F->mut_n, sizeof(int) * (size_t)F->cmut);
    F->mut_v = realloc(F->mut_v, sizeof(int) * (size_t)F->cmut);
  }
  F->mut_n[F->nmut] = node;
  F->mut_v[F->nmut] = x;
  F->nmut++;
  F->flags[sh_find(F, x)] |= (unsigned char)fl;
}

/* A flow (share.h): node `site` takes the value of node v into a holder,
   a container's elements, a method's or a block's value, or changes it in
   place. Recorded for every value; the seal reads only those that are no
   holder's read and whose class the rule shares (share_flow_*). dest is
   the element that takes the value, which the class of a value no holder
   names (a new String joins none) cannot say: the walk joins a value to its
   destination only when the value has a class of its own. */
static void sh_flow(ShareFacts *F, int kind, int site, int v, int dest) {
  if (v < 0 || site < 0) return;
  /* The jump walk also reaches every emitted subtree. Desugaring leaves
     detached nodes behind, including an expanded literal splat's Array. */
  if (F->jseen && !F->jseen[site]) return;
  int e = F->nval[v];
  if (kind != SHFL_ARG && e >= 0 && F->kind[e] == SHK_SELF) F->own[e] |= SHE_IDENTITY;
  if (F->nfl >= F->cfl) {
    F->cfl = F->cfl ? F->cfl * 2 : 64;
    F->fl_site = realloc(F->fl_site, sizeof(int) * (size_t)F->cfl);
    F->fl_val = realloc(F->fl_val, sizeof(int) * (size_t)F->cfl);
    F->fl_kind = realloc(F->fl_kind, (size_t)F->cfl);
    F->fl_dest = realloc(F->fl_dest, sizeof(int) * (size_t)F->cfl);
    if (!F->fl_site || !F->fl_val || !F->fl_kind || !F->fl_dest) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->fl_site[F->nfl] = site;
  F->fl_val[F->nfl] = v;
  F->fl_dest[F->nfl] = dest;
  F->fl_kind[F->nfl++] = (unsigned char)kind;
}

/* ---- holders ---- */

static unsigned sh_key_hash(int kind, int a, const char *name) {
  unsigned h = (unsigned)kind * 2654435761u ^ (unsigned)a * 40503u;
  if (name) h ^= sp_strhash(name);
  return h;
}

static int sh_holder(ShareFacts *F, int kind, int a, int b, const char *name, int node) {
  if (!name && (kind == SHK_IVAR || kind == SHK_GVAR || kind == SHK_CVAR || kind == SHK_CONST)) return -1;
  if (!F->bucket) {
    F->nbucket = 4096;
    F->bucket = malloc(sizeof(int) * (size_t)F->nbucket);
    for (int i = 0; i < F->nbucket; i++) F->bucket[i] = -1;
  }
  unsigned hb = sh_key_hash(kind, a, name) & (unsigned)(F->nbucket - 1);
  for (int i = F->bucket[hb]; i >= 0; i = F->hnext[i]) {
    ShareHolder *h = &F->h[i];
    if (h->kind != kind) continue;
    if (kind == SHK_SELF) { if (h->scope == a) return F->helem[i]; continue; }
    if (kind == SHK_LOCAL ? (h->scope == a && h->local == b) :
        kind == SHK_IVAR ? (h->cid == a && sp_streq(h->name, name)) :
        (kind == SHK_RET || kind == SHK_YIELD || kind == SHK_BLKRET) ? h->scope == a :
        sp_streq(h->name, name))
      return F->helem[i];
  }
  if (F->nh >= F->ch) {
    F->ch = F->ch ? F->ch * 2 : 256;
    F->h = realloc(F->h, sizeof(ShareHolder) * (size_t)F->ch);
    F->helem = realloc(F->helem, sizeof(int) * (size_t)F->ch);
    F->hnext = realloc(F->hnext, sizeof(int) * (size_t)F->ch);
  }
  int i = F->nh++;
  ShareHolder *h = &F->h[i];
  memset(h, 0, sizeof *h);
  h->kind = (unsigned char)kind;
  h->scope = kind == SHK_IVAR ? -1 : a;
  h->local = b;
  h->cid = kind == SHK_IVAR ? a : -1;
  h->name = name ? strdup(name) : NULL;
  h->node = node;
  int e = sh_new(F, kind);
  if (kind == SHK_LOCAL) F->lsc[e] = a;
  F->helem[i] = e;
  F->hidx[e] = i;
  F->hnext[i] = F->bucket[hb];
  F->bucket[hb] = i;
  /* the buckets grow with the holders, so a lookup stays a short chain */
  if (F->nh > 2 * F->nbucket) {
    F->nbucket *= 2;
    F->bucket = realloc(F->bucket, sizeof(int) * (size_t)F->nbucket);
    for (int k = 0; k < F->nbucket; k++) F->bucket[k] = -1;
    for (int k = 0; k < F->nh; k++) {
      ShareHolder *hk = &F->h[k];
      unsigned b2 = sh_key_hash(hk->kind, hk->kind == SHK_IVAR ? hk->cid : hk->scope, hk->name) &
                    (unsigned)(F->nbucket - 1);
      F->hnext[k] = F->bucket[b2];
      F->bucket[b2] = k;
    }
  }
  return e;
}

/* Can a value of type t hold a String, or a container of them? An object's
   Strings sit in its ivars, which are holders of their own. */
static int sh_may_hold(const Compiler *c, TyKind t) {
  /* an object holds its Strings in ivars, which are holders of their own;
     a native class's object keeps the String its binding declares */
  if (ty_is_object(t)) return ty_object_class(t) < c->nclasses && c->classes[ty_object_class(t)].native_share_keeps;
  if (ty_is_obj_array(t)) return 0;
  switch (t) {
  case TY_VOID: case TY_NIL: case TY_INT: case TY_BIGINT: case TY_FLOAT: case TY_SYMBOL:
  case TY_BOOL: case TY_RANGE: case TY_FLOAT_RANGE: case TY_TIME: case TY_COMPLEX:
  case TY_RATIONAL: case TY_MATCHDATA: case TY_REGEX: case TY_EXCEPTION:
  case TY_INT_ARRAY: case TY_FLOAT_ARRAY: case TY_INT_ARRAY_ARRAY: case TY_FLOAT_ARRAY_ARRAY:
  case TY_STR_INT_HASH: case TY_INT_INT_HASH: case TY_PROC: case TY_CURRY: case TY_FIBER:
  case TY_THREAD: case TY_QUEUE: case TY_MUTEX: case TY_CONDVAR: case TY_RANDOM: case TY_DIR:
  case TY_ADDRINFO: case TY_SOCKOPT: case TY_TMS: case TY_PROCESS_STATUS:
  case TY_METHOD: case TY_IO: case TY_ARGF: case TY_CLASS:
    return 0;
  default:
    return 1;
  }
}

static int sh_local_of(ShareFacts *F, Compiler *c, Scope *s, const char *name, int node) {
  if (!s || !name) return -1;
  LocalVar *lv = scope_local(s, name);
  if (!lv || !sh_may_hold(c, lv->type)) return -1;
  int e = sh_holder(F, SHK_LOCAL, (int)(s - c->scopes), (int)(lv - s->locals), NULL, node);
  /* a parameter's String is the caller's, a captured local's a proc's too */
  if (e >= 0 && (lv->is_param || lv->is_block_param || lv->is_cell || lv->cell_outlives || s->def_node < 0))
    F->lsc[sh_find(F, e)] = -2;
  return e;
}

static int sh_local_at(ShareFacts *F, Compiler *c, int node) {
  return sh_local_of(F, c, comp_scope_of(c, node), nt_str(c->nt, node, "name"), node);
}

/* the class whose ivar slot an ivar node names, as the emitters store it */
static int sh_ivar_owner(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  if (s && s->class_id >= 0) return s->class_id;
  return comp_class_index(c, "Toplevel");
}

static int sh_ivar(ShareFacts *F, Compiler *c, int cid, const char *name, int node) {
  if (cid < 0 || !name) return -1;
  int iv = comp_ivar_index(&c->classes[cid], name);
  if (iv >= 0 && !sh_may_hold(c, c->classes[cid].ivar_types[iv])) return -1;
  int nh0 = F->nh;
  int e = sh_holder(F, SHK_IVAR, cid, -1, name, node);
  /* a superclass's ivar of the name is the same slot of the same object:
     one written in A#initialize and changed in B#bang (B < A) */
  if (F->nh > nh0)
    for (int k = c->classes[cid].parent; k >= 0; k = c->classes[k].parent)
      if (comp_ivar_index(&c->classes[k], name) >= 0) {
        sh_union(F, e, sh_ivar(F, c, k, name, node));
        break;
      }
  return e;
}

static int sh_join(ShareFacts *F, int a, int b);

/* The ivar holder an ivar node names. In an instance_eval or instance_exec
   block it is the receiver's (`k.instance_eval { @k = s }` writes k's @k).
   Inside a method the block's own class's slot is joined too, and so is
   each class a boxed receiver can be (or what the walk does not follow,
   when they are more than the list holds). */
static int sh_ivar_at(ShareFacts *F, Compiler *c, int node) {
  const char *name = nt_str(c->nt, node, "name");
  int ie = ie_class_of(c, node);
  Scope *s = comp_scope_of(c, node);
  if (ie >= 0 && (!s || s->class_id < 0)) return sh_ivar(F, c, ie, name, node);
  int e = sh_ivar(F, c, sh_ivar_owner(c, node), name, node);
  if (ie >= 0) return sh_join(F, e, sh_ivar(F, c, ie, name, node));
  if (ie == -1) return e;
  int pk[64];
  int np = ie_poly_classes_at(c, node, pk, 64);
  if (np >= 64) return sh_join(F, e, F->unknown);
  for (int i = 0; i < np; i++) e = sh_join(F, e, sh_ivar(F, c, pk[i], name, node));
  return e;
}

/* A String written into ivar holder l from node vn, whose value is v. The
   holder is one per class but a slot per object: a value the write does
   not make itself -- a call's answer (`k2.v = k1.v`), a member read, a
   yield -- may be the String another object's slot already holds, so the
   class counts as several names (SHF_MULTI). A String of its own (no
   value) and a read of a holder (itself a name the class counts) do not. */
static void sh_ivar_store(ShareFacts *F, Compiler *c, int l, int vn, int v) {
  if (l < 0 || v < 0) return;
  sh_union(F, l, v);
  NodeKind k = vn >= 0 ? nt_kind(c->nt, vn) : NK_NilNode;
  if (k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || k == NK_GlobalVariableReadNode ||
      k == NK_ClassVariableReadNode || k == NK_ConstantReadNode || k == NK_ConstantPathNode)
    return;
  F->flags[sh_find(F, l)] |= SHF_MULTI;
}

/* A global's holder. `alias $b $a` makes $b the global $a, which the
   compiler resolves as it reads the program (comp_resolve_gvar): the
   holder is $a's. */
static int sh_gvar(ShareFacts *F, Compiler *c, const char *name, int node) {
  if (!name) return -1;
  const char *bare = name[0] == '$' ? name + 1 : name;
  const char *to = comp_resolve_gvar(c, bare);
  LocalVar *gv = comp_gvar(c, to);
  if (gv && !sh_may_hold(c, gv->type)) return -1;
  if (to == bare) return sh_holder(F, SHK_GVAR, 0, -1, name, node);
  size_t ln = strlen(to);
  char *full = malloc(ln + 2);
  if (!full) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  full[0] = '$';
  memcpy(full + 1, to, ln + 1);
  int e = sh_holder(F, SHK_GVAR, 0, -1, full, node);
  free(full);
  return e;
}

static int sh_scope_holder(ShareFacts *F, int kind, int mi) {
  return mi < 0 ? -1 : sh_holder(F, kind, mi, -1, NULL, -1);
}

static int sh_method_index(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  return s && s->def_node >= 0 ? (int)(s - c->scopes) : -1;
}

/* A reopening's receiver is a binding, just as an explicit parameter is.
   Its byte-only uses do not require a handle ABI. */
static int sh_self(ShareFacts *F, Compiler *c, int mi) {
  if (!c->share_strings || mi < 0) return -1;
  Scope *s = &c->scopes[mi];
  return !s->is_cmethod && s->class_id >= 0 && s->class_id == comp_class_index(c, "String")
       ? sh_scope_holder(F, SHK_SELF, mi) : -1;
}

static void sh_bind_self(ShareFacts *F, Compiler *c, int call, int mi, int rv) {
  int self = sh_self(F, c, mi);
  if (self < 0) return;
  if (rv < 0) rv = sh_self(F, c, sh_method_index(c, call));
  sh_union(F, self, rv);
  int pf = scope_proc_form_of(c, mi);
  if (pf >= 0) sh_union(F, self, sh_self(F, c, pf));
}

static int sh_holder_read(const NodeTable *nt, int n) {
  NodeKind k = n >= 0 ? nt_kind(nt, n) : NK_NONE;
  return k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode ||
         k == NK_GlobalVariableReadNode || k == NK_ClassVariableReadNode;
}

static void sh_dyn_name(ShareFacts *F, const char *name) {
  if (!name) { F->dyn_all = 1; return; }
  if (F->ndyn >= F->cdyn) {
    F->cdyn = F->cdyn ? F->cdyn * 2 : 16;
    F->dyn = realloc(F->dyn, sizeof(char *) * (size_t)F->cdyn);
  }
  F->dyn[F->ndyn++] = name;
}

static void sh_lend(ShareFacts *F, int arg, int par, int node, int direct) {
  if (arg < 0 || par < 0) return;
  if (F->nlend >= F->clend) {
    F->clend = F->clend ? F->clend * 2 : 64;
    F->lend_arg = realloc(F->lend_arg, sizeof(int) * (size_t)F->clend);
    F->lend_par = realloc(F->lend_par, sizeof(int) * (size_t)F->clend);
    F->lend_node = realloc(F->lend_node, sizeof(int) * (size_t)F->clend);
    F->lend_direct = realloc(F->lend_direct, (size_t)F->clend);
    F->lend_done = realloc(F->lend_done, (size_t)F->clend);
    if (!F->lend_arg || !F->lend_par || !F->lend_node || !F->lend_direct || !F->lend_done) {
      fprintf(stderr, "spinel: out of memory\n");
      exit(1);
    }
  }
  F->lend_node[F->nlend] = node;
  F->lend_arg[F->nlend] = arg;
  F->lend_par[F->nlend] = par;
  F->lend_direct[F->nlend] = (unsigned char)direct;
  F->lend_done[F->nlend] = 0;
  F->nlend++;
}

/* method mi returns value v (see ShareFacts.ret_m) */
static void sh_ret(ShareFacts *F, int mi, int v) {
  if (mi < 0 || v < 0) return;
  if (F->kind[v] == SHK_SELF) F->own[v] |= SHE_IDENTITY;
  if (F->nret >= F->cret) {
    F->cret = F->cret ? F->cret * 2 : 64;
    F->ret_m = realloc(F->ret_m, sizeof(int) * (size_t)F->cret);
    F->ret_v = realloc(F->ret_v, sizeof(int) * (size_t)F->cret);
    F->ret_done = realloc(F->ret_done, (size_t)F->cret);
  }
  F->ret_m[F->nret] = mi;
  F->ret_v[F->nret] = v;
  F->ret_done[F->nret] = 0;
  F->nret++;
}

/* call or super node n reaches a user method (see ShareFacts.rsite) */
static void sh_read_site(ShareFacts *F, int n) {
  if (F->nrsite >= F->crsite) {
    F->crsite = F->crsite ? F->crsite * 2 : 64;
    F->rsite = realloc(F->rsite, sizeof(int) * (size_t)F->crsite);
    if (!F->rsite) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->rsite[F->nrsite++] = n;
}

/* ---- node values ---- */

static int sh_val(ShareFacts *F, Compiler *c, int n);
static int sh_const_read(ShareFacts *F, Compiler *c, int n);
static void sh_mark_unused(ShareFacts *F, const NodeTable *nt, int n, unsigned char bit);

static int sh_join(ShareFacts *F, int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  if (F->kind[a] == SHK_SELF) F->own[a] |= SHE_IDENTITY;
  if (F->kind[b] == SHK_SELF) F->own[b] |= SHE_IDENTITY;
  sh_union(F, a, b);
  return a;
}

static int sh_stmts_val(ShareFacts *F, Compiler *c, int st) {
  if (st < 0) return -1;
  if (nt_kind(c->nt, st) != NK_StatementsNode) return sh_val(F, c, st);
  int n = 0; const int *b = nt_arr(c->nt, st, "body", &n);
  return n > 0 ? sh_val(F, c, b[n - 1]) : -1;
}

/* the value an argument hands over: a splat's elements, or the value */
static int sh_arg_val(ShareFacts *F, Compiler *c, int a) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, a);
  if (k == NK_SplatNode) return sh_elem(F, sh_val(F, c, nt_ref(nt, a, "expression")));
  if (k == NK_AssocSplatNode) return sh_elem(F, sh_val(F, c, nt_ref(nt, a, "value")));
  return sh_val(F, c, a);
}

/* every value a call's arguments hand over, keyword values included */
/* (past cap - 1 values, the rest join the last slot: a value dropped there
   would join no class) */
static void sh_args_put(ShareFacts *F, int *out, int *n, int cap, int v) {
  if (*n < cap) out[(*n)++] = v;
  else out[cap - 1] = sh_join(F, out[cap - 1], v);
}
static int sh_args_vals(ShareFacts *F, Compiler *c, int call, int *out, int cap) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, call, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int n = 0;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_KeywordHashNode) {
      int en = 0; const int *el = nt_arr(nt, argv[i], "elements", &en);
      for (int e = 0; e < en; e++) {
        if (nt_kind(nt, el[e]) == NK_AssocNode) sh_args_put(F, out, &n, cap, sh_val(F, c, nt_ref(nt, el[e], "value")));
        else sh_args_put(F, out, &n, cap, sh_arg_val(F, c, el[e]));
      }
      continue;
    }
    if (k == NK_BlockArgumentNode) continue;
    sh_args_put(F, out, &n, cap, sh_arg_val(F, c, argv[i]));
  }
  return n;
}

/* Each value a call's arguments hand over, keyword values included (not a
   splat's elements, which a container holds), as a flow of kind k at
   site, into the element dest. */
static void sh_args_flows(ShareFacts *F, Compiler *c, int kind, int call, int site, int dest) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, call, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_KeywordHashNode) {
      int en = 0; const int *el = nt_arr(nt, argv[i], "elements", &en);
      for (int e = 0; e < en; e++)
        if (nt_kind(nt, el[e]) == NK_AssocNode) sh_flow(F, kind, site, nt_ref(nt, el[e], "value"), dest);
      continue;
    }
    if (k != NK_BlockArgumentNode && k != NK_SplatNode) sh_flow(F, kind, site, argv[i], dest);
  }
}

/* Bind a target (a write's target node inside a multiple assignment, a
   block's parameter, a for loop's index) to value v. */
static void sh_target(ShareFacts *F, Compiler *c, int t, int v);

static void sh_targets_of(ShareFacts *F, Compiler *c, int t, int v) {
  const NodeTable *nt = c->nt;
  static const char *const fields[] = { "lefts", "rights" };
  int ev = sh_elem(F, v);
  for (int f = 0; f < 2; f++) {
    int n = 0; const int *ts = nt_arr(nt, t, fields[f], &n);
    for (int i = 0; i < n; i++) sh_target(F, c, ts[i], ev);
  }
  int rest = nt_ref(nt, t, "rest");
  if (rest >= 0) sh_target(F, c, rest, v);
}

static void sh_target(ShareFacts *F, Compiler *c, int t, int v) {
  const NodeTable *nt = c->nt;
  if (t < 0) return;
  switch (nt_kind(nt, t)) {
  case NK_LocalVariableTargetNode: case NK_RequiredParameterNode: case NK_OptionalParameterNode:
  case NK_RestParameterNode: case NK_OptionalKeywordParameterNode: case NK_KeywordRestParameterNode:
  sh_param: {
    int l = sh_local_at(F, c, t);
    if (l >= 0) F->own[l] |= SHE_WRITTEN;
    NodeKind k = nt_kind(nt, t);
    /* a rest gathers values: its elements are them */
    if (k == NK_RestParameterNode || k == NK_KeywordRestParameterNode) sh_union(F, sh_elem(F, l), v);
    else sh_union(F, l, v);
    int dv = nt_ref(nt, t, "value");   /* an optional parameter's default */
    if (dv >= 0 && k != NK_LocalVariableTargetNode) sh_union(F, l, sh_val(F, c, dv));
    return;
  }
  case NK_InstanceVariableTargetNode:
    sh_union(F, sh_ivar_at(F, c, t), v);
    return;
  case NK_GlobalVariableTargetNode:
    sh_union(F, sh_gvar(F, c, nt_str(nt, t, "name"), t), v);
    return;
  case NK_ClassVariableTargetNode:
    sh_union(F, sh_holder(F, SHK_CVAR, 0, -1, nt_str(nt, t, "name"), t), v);
    return;
  case NK_ConstantTargetNode:
    sh_union(F, sh_holder(F, SHK_CONST, 0, -1, nt_str(nt, t, "name"), t), v);
    return;
  case NK_IndexTargetNode:
    sh_union(F, sh_elem(F, sh_val(F, c, nt_ref(nt, t, "receiver"))), v);
    return;
  case NK_SplatNode:
    sh_target(F, c, nt_ref(nt, t, "expression"), v);
    return;
  case NK_MultiTargetNode:
    sh_targets_of(F, c, t, v);
    return;
  default:
    if (nt_type(nt, t) && sp_streq(nt_type(nt, t), "RequiredKeywordParameterNode")) goto sh_param;
    /* a call target (`o.x, y = ...`) or anything else: not followed */
    sh_union(F, v, F->unknown);
    return;
  }
}

/* The holder a plain multiple-write target stores into (the kinds sh_target
   binds to a holder of their own), made as sh_target makes it; -1 for any
   other. */
static int sh_target_holder(ShareFacts *F, Compiler *c, int t) {
  const NodeTable *nt = c->nt;
  if (t < 0) return -1;
  switch (nt_kind(nt, t)) {
  case NK_LocalVariableTargetNode: return sh_local_at(F, c, t);
  case NK_InstanceVariableTargetNode: return sh_ivar_at(F, c, t);
  case NK_GlobalVariableTargetNode: return sh_gvar(F, c, nt_str(nt, t, "name"), t);
  case NK_ClassVariableTargetNode: return sh_holder(F, SHK_CVAR, 0, -1, nt_str(nt, t, "name"), t);
  case NK_ConstantTargetNode: return sh_holder(F, SHK_CONST, 0, -1, nt_str(nt, t, "name"), t);
  default: return -1;
  }
}

/* The value a pattern reads an object's parts off: what its deconstruct
   (keys 0) or deconstruct_keys (keys 1) answers, the user method's value
   as a call's is. vt is the matched value's type and cls the class the
   pattern names (`in Box[t]`), or -1. A value that may be any object reads
   them off every user method of the name, joined once per build. The
   pattern reads the method's value as a call does (F->mread). -1 for a
   value no user method deconstructs. */
static int sh_deconstructed(ShareFacts *F, Compiler *c, TyKind vt, int cls, int keys) {
  const char *name = keys ? "deconstruct_keys" : "deconstruct";
  if (cls < 0 && ty_is_object(vt)) cls = ty_object_class(vt);
  if (cls >= 0 && cls < c->nclasses) {
    int defc = -1, mi = comp_method_in_chain(c, cls, name, &defc);
    if (mi >= 0 && F->mread) F->mread[mi] = 1;   /* the pattern reads its value */
    return mi >= 0 ? sh_scope_holder(F, SHK_RET, mi) : -1;
  }
  if (vt != TY_POLY && vt != TY_UNKNOWN) return -1;
  if (F->any_dec[keys] == -2) {
    F->any_dec[keys] = -1;
    for (int mi = 0; mi < c->nscopes; mi++) {
      Scope *m = &c->scopes[mi];
      if (m->def_node >= 0 && m->class_id >= 0 && m->name && sp_streq(m->name, name)) {
        if (F->mread) F->mread[mi] = 1;
        F->any_dec[keys] = sh_join(F, F->any_dec[keys], sh_scope_holder(F, SHK_RET, mi));
      }
    }
  }
  return F->any_dec[keys];
}

/* the type of the parts a pattern reads off a value of type vt (an
   object's: its deconstruct's elements), TY_UNKNOWN when not known */
static TyKind sh_part_type(Compiler *c, TyKind vt, int keys) {
  if (ty_is_object(vt) && !keys) {
    int defc = -1, mi = comp_method_in_chain(c, ty_object_class(vt), "deconstruct", &defc);
    vt = mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
  }
  if (ty_is_array(vt)) return ty_array_elem(vt);
  if (ty_is_hash(vt)) return ty_hash_val(vt);
  return TY_UNKNOWN;
}

/* Bind the variables pattern p names to the parts of v, the value it
   matches, of type vt (`case [s] in [t]` binds t to s itself; `in {name:
   t}` to the Hash's value). An object's parts are those of what its
   deconstruct answers (sh_deconstructed). */
static void sh_pattern(ShareFacts *F, Compiler *c, int p, int v, TyKind vt) {
  const NodeTable *nt = c->nt;
  if (p < 0) return;
  switch (nt_kind(nt, p)) {
  case NK_LocalVariableTargetNode:
    sh_target(F, c, p, v);
    return;
  case NK_CapturePatternNode:
    sh_pattern(F, c, nt_ref(nt, p, "value"), v, vt);
    sh_target(F, c, nt_ref(nt, p, "target"), v);
    return;
  case NK_AlternationPatternNode:
    sh_pattern(F, c, nt_ref(nt, p, "left"), v, vt);
    sh_pattern(F, c, nt_ref(nt, p, "right"), v, vt);
    return;
  case NK_IfNode: case NK_UnlessNode: {   /* a guard: `in [t] if t` */
    int st = nt_ref(nt, p, "statements");
    int bn = 0; const int *bv = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
    if (bn > 0) sh_pattern(F, c, bv[0], v, vt);
    return;
  }
  case NK_SplatNode:   /* `*rest`: a container of the parts */
    sh_pattern(F, c, nt_ref(nt, p, "expression"), v, vt);
    return;
  case NK_AssocSplatNode:   /* `**rest` */
    sh_pattern(F, c, nt_ref(nt, p, "value"), v, vt);
    return;
  case NK_ArrayPatternNode: case NK_HashPatternNode:
  sh_parts: {
    int keys = nt_kind(nt, p) == NK_HashPatternNode;
    int kn = nt_ref(nt, p, "constant");
    const char *cn = kn >= 0 && (nt_kind(nt, kn) == NK_ConstantReadNode || nt_kind(nt, kn) == NK_ConstantPathNode)
                     ? nt_str(nt, kn, "name") : NULL;
    int cls = cn ? comp_class_index(c, cn) : -1;
    int dv = sh_deconstructed(F, c, vt, cls, keys);
    TyKind pt = sh_part_type(c, cls >= 0 ? ty_object(cls) : vt, keys);
    /* an object's parts are its deconstruct's; a value that may be any
       object's, either */
    int obj = ty_is_object(vt) || cls >= 0;
    int srcs[2] = { obj && dv >= 0 ? dv : v, obj ? -1 : dv };
    static const char *const lists[] = { "requireds", "posts", "elements" };
    static const char *const rests[] = { "rest", "left", "right" };
    for (int si = 0; si < 2; si++) {
      if (srcs[si] < 0) continue;
      int ev = sh_elem(F, srcs[si]);
      for (int f = 0; f < 3; f++) {
        int m = 0; const int *ps = nt_arr(nt, p, lists[f], &m);
        for (int i = 0; i < m; i++) {
          int q = ps[i];
          if (nt_kind(nt, q) == NK_AssocNode) q = nt_ref(nt, q, "value");
          sh_pattern(F, c, q, ev, pt);
        }
      }
      for (int f = 0; f < 3; f++) sh_pattern(F, c, nt_ref(nt, p, rests[f]), srcs[si], TY_UNKNOWN);
    }
    return;
  }
  default:
    if (nt_type(nt, p) && sp_streq(nt_type(nt, p), "FindPatternNode")) goto sh_parts;
    /* a value, a pinned value or a constant: binds nothing */
    return;
  }
}

/* A block's parameters, each bound to v (a destructured one to v's
   elements); `deep` also binds each to the elements of v's elements, for an
   iterator that splats an element over several parameters. */
static void sh_block_params(ShareFacts *F, Compiler *c, int blk, int v, int deep) {
  const NodeTable *nt = c->nt;
  if (blk < 0 || v < 0) return;
  int bp = nt_ref(nt, blk, "parameters");
  if (bp < 0) return;
  if (nt_kind(nt, bp) != NK_BlockParametersNode) {
    /* `_1` / `it`: the parameters by name */
    for (int i = 0; ; i++) {
      const char *pn = block_param_name(c, blk, i);
      if (!pn) break;
      sh_union(F, sh_local_of(F, c, comp_scope_of(c, blk), pn, blk), v);
    }
    return;
  }
  int pn = nt_ref(nt, bp, "parameters");
  if (pn < 0) return;
  int nreq = 0; const int *reqs = nt_arr(nt, pn, "requireds", &nreq);
  int nopt = 0; nt_arr(nt, pn, "optionals", &nopt);
  int many = nreq + nopt > 1 || nt_ref(nt, pn, "rest") >= 0;
  for (int i = 0; i < nreq; i++)
    if (nt_kind(nt, reqs[i]) == NK_MultiTargetNode) many = 1;
  int ev = deep && many ? sh_elem(F, v) : -1;
  static const char *const lists[] = { "requireds", "optionals", "posts", "keywords" };
  for (int f = 0; f < 4; f++) {
    int n = 0; const int *ps = nt_arr(nt, pn, lists[f], &n);
    for (int i = 0; i < n; i++) {
      sh_target(F, c, ps[i], v);
      if (ev >= 0) sh_target(F, c, ps[i], ev);
    }
  }
  int rest = nt_ref(nt, pn, "rest");
  if (rest >= 0) sh_target(F, c, rest, v);
  int kwr = nt_ref(nt, pn, "keyword_rest");
  if (kwr >= 0) sh_target(F, c, kwr, v);
}

/* The defaults of lambda n's optional and keyword parameters, each joined
   with its parameter as sh_target joins a block's. A lambda's parameters
   are its ParametersNode itself, not the BlockParametersNode
   sh_block_params reads, so a default reading a variable (`->(v = s) { v }`)
   joined no class with it, and the parameter's binding lifted a copy of s
   into a handle of its own. */
static void sh_lambda_defaults(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int pn = nt_ref(nt, n, "parameters");
  if (pn < 0 || nt_kind(nt, pn) != NK_ParametersNode) return;
  static const char *const lists[] = { "optionals", "keywords" };
  for (int f = 0; f < 2; f++) {
    int np = 0; const int *ps = nt_arr(nt, pn, lists[f], &np);
    for (int i = 0; i < np; i++) {
      int dv = nt_ref(nt, ps[i], "value");
      if (dv < 0) continue;
      int l = sh_local_at(F, c, ps[i]);
      if (l >= 0) F->own[l] |= SHE_WRITTEN;
      sh_union(F, l, sh_val(F, c, dv));
    }
  }
}

/* n's value joined with what the breaks or nexts that leave it hand it
   (sh_jumps) */
static int sh_jumped(ShareFacts *F, int n, int v) {
  return F->jump && n >= 0 && n < F->nnodes ? sh_join(F, v, F->jump[n]) : v;
}

/* a block's value: its body's last value, or any `next v` */
static int sh_block_val(ShareFacts *F, Compiler *c, int blk) {
  return blk >= 0 ? sh_jumped(F, blk, sh_stmts_val(F, c, nt_ref(c->nt, blk, "body"))) : -1;
}

/* The values block blk answers -- its body's last statement's, and each
   `next v`'s that leaves it (not one in a nested block, lambda, method or
   loop) -- as flows at site, the call that stores them in a container
   (map, map!, Array.new and Hash.new blocks), or at the next itself, into
   the container's elements dest. */
static void sh_block_nexts(ShareFacts *F, const NodeTable *nt, int site, int n, int dest) {
  if (n < 0) return;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_WhileNode || k == NK_UntilNode ||
      k == NK_ForNode)
    return;
  if (k == NK_NextNode) {
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode) sh_flow(F, SHFL_BLOCK, n, argv[0], dest);
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++) sh_block_nexts(F, nt, site, nt_ref_at(nt, n, i), dest);
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) sh_block_nexts(F, nt, site, ids[j], dest);
  }
}
static void sh_block_flow(ShareFacts *F, const NodeTable *nt, int site, int blk, int dest) {
  /* a container whose value is dropped (`h.map(&:upcase!)` as a statement)
     keeps its block's values for nobody; map! keeps them in its receiver */
  const char *sn = site >= 0 && site < F->nnodes && (F->unused[site] & SHU_STMT) ? nt_str(nt, site, "name") : NULL;
  if (sn && !is_map_bang_alias(sn) && bop_share_named(BOP_ANY_ARRAY, sn) != BSH_FILL) return;
  int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
  int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn > 0) sh_flow(F, SHFL_BLOCK, site, bb[bn - 1], dest);
  sh_block_nexts(F, nt, site, body, dest);
}

/* the literal name a `send`, `method` or `instance_variable_*` names */
static const char *sh_lit_name(const NodeTable *nt, int a) {
  if (a < 0) return NULL;
  NodeKind k = nt_kind(nt, a);
  if (k == NK_SymbolNode) return nt_str(nt, a, "value") ? nt_str(nt, a, "value") : nt_str(nt, a, "unescaped");
  if (k == NK_StringNode) return nt_str(nt, a, "content");
  return NULL;
}

static int sh_plain_operand(Compiler *c, int n);
/* How lending a parameter argument node a of `call` keeps CRuby's answer:
   SHL_HOLDS by itself, SHL_READ only while the method's value is taken as
   read (F->mread), SHL_UNSOUND not at all. A value no variable holds is
   lent a temporary nothing else names. A variable's C slot is lent by
   address, so it must be a local (or parameter) nothing can rebind while
   the callee runs: no proc that captures it assigns it, the call passes no
   block (the method's yield runs the block, which may assign it), and every
   argument is a read, a literal or scalar arithmetic over those (*plain,
   asked once per call). A global's or an ivar's slot is refused where the
   call can rebind it at the top level or in a class body (#6179), which
   only the shared class avoids. A block of the call that can assign the
   argument variable, a local or an instance variable
   (comp_block_rebinds_arg: `m(s) { s = +"b" }`, `m(@s) { @s = +"b" }`),
   rebinds the lent slot under the parameter while the method runs: its
   appends then land on the new String. No lend holds there; the parameter
   shares its argument's class instead. */
enum { SHL_UNSOUND = -1, SHL_READ = 0, SHL_HOLDS = 1 };
static int sh_lend_holds(Compiler *c, int call, int a, int *plain) {
  const NodeTable *nt = c->nt;
  if (!sh_holder_read(nt, a)) return SHL_HOLDS;
  int blk = nt_ref(nt, call, "block");
  if (comp_block_rebinds_arg(c, blk, a)) return SHL_UNSOUND;
  if (nt_kind(nt, a) != NK_LocalVariableReadNode) return SHL_READ;
  const char *ln = nt_str(nt, a, "name");
  Scope *s = ln ? comp_scope_of(c, a) : NULL;
  LocalVar *lv = s ? scope_local(s, ln) : NULL;
  if (blk >= 0 || !lv || lv->cell_outlives || lv->proc_rebinds) return SHL_READ;
  if (*plain < 0) {
    int args = nt_ref(nt, call, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    *plain = 1;
    for (int i = 0; i < argc && *plain; i++) *plain = sh_plain_operand(c, argv[i]);
  }
  return *plain ? SHL_HOLDS : SHL_READ;
}

/* Bind the arguments of `call` to method mi's parameters. A parameter the
   method may lend (only read and mutated) is bound by sh_lend, the rest by
   a union. An argument the layout places nowhere joins every parameter.
   A lend that does not hold by itself keeps mi's returns joining its value
   (F->mread), as a caller that reads it does; one that cannot hold at all
   (sh_lend_holds) is a union too. */
static void sh_bind(ShareFacts *F, Compiler *c, int call, int mi) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int args = nt_ref(nt, call, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int claimed[64];
  int nclaimed = 0, plain = -1;
  for (int j = 0; j < m->nparams; j++) {
    int p = m->pnames[j] ? sh_local_of(F, c, m, m->pnames[j], m->def_node) : -1;
    int spread = -1;
    int a = arg_layout_param_node(c, m, call, j, &spread);
    if (a >= 0 && nclaimed < 64) claimed[nclaimed++] = a;
    if (p < 0) continue;
    if (a >= 0) {
      int v = sh_val(F, c, a);
      sh_flow(F, SHFL_ARG, call, a, j == m->rest_idx || j == m->kwrest_idx ? sh_elem(F, p) : p);
      if (j == m->rest_idx || j == m->kwrest_idx) sh_union(F, sh_elem(F, p), v);
      /* self has no lendable byte slot: a callee must keep its identity. */
      /* A byte-only parameter can still borrow it; sh_settle_lends joins
         a held receiver to a parameter that mutates or keeps it. */
      else if (v >= 0 && F->kind[v] == SHK_SELF) sh_lend(F, v, p, a, 0);
      else {
        int holds = sh_lend_holds(c, call, a, &plain);
        if (holds == SHL_UNSOUND) sh_union(F, p, v);
        else sh_lend(F, v, p, a, sh_holder_read(nt, a));
        if (holds != SHL_HOLDS) F->mread[mi] = 1;
      }
    }
    else if (spread >= 0) sh_union(F, p, sh_elem(F, sh_val(F, c, spread)));
  }
  /* any value the layout placed nowhere: every parameter, and the elements
     of a rest */
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_BlockArgumentNode) continue;
    int vals[32], nodes[32];
    int nv = 0;
    if (k == NK_KeywordHashNode) {
      int en = 0; const int *el = nt_arr(nt, argv[i], "elements", &en);
      for (int e = 0; e < en && nv < 32; e++) {
        nodes[nv] = nt_kind(nt, el[e]) == NK_AssocNode ? nt_ref(nt, el[e], "value") : el[e];
        vals[nv] = nt_kind(nt, el[e]) == NK_AssocNode ? sh_val(F, c, nodes[nv]) : sh_arg_val(F, c, el[e]);
        nv++;
      }
    }
    else { nodes[0] = argv[i]; vals[0] = sh_arg_val(F, c, argv[i]); nv = 1; }
    for (int q = 0; q < nv; q++) {
      int placed = 0;
      for (int w = 0; w < nclaimed && !placed; w++) placed = claimed[w] == nodes[q];
      if (placed || vals[q] < 0) continue;
      /* the value joins every parameter: the first names the class */
      int dest = -1;
      for (int j = 0; j < m->nparams; j++) {
        int p = m->pnames[j] ? sh_local_of(F, c, m, m->pnames[j], m->def_node) : -1;
        if (p < 0) continue;
        int pe = j == m->rest_idx || j == m->kwrest_idx ? sh_elem(F, p) : p;
        if (dest < 0) dest = pe;
        sh_union(F, pe, vals[q]);
      }
      sh_flow(F, SHFL_ARG, call, nodes[q], dest);
    }
  }
}

/* A block literal handed to user method mi: its parameters take what mi
   yields, its value is what mi's yields answer; a block mi keeps as &blk
   may be called from anywhere. */
/* Method from hands the block it was given on to method to (share_value_fresh) */
static void sh_fwd_edge(ShareFacts *F, int from, int to) {
  if (from < 0 || to < 0) return;
  if (F->nfw >= F->cfw) {
    F->cfw = F->cfw ? F->cfw * 2 : 16;
    F->fw_from = realloc(F->fw_from, sizeof(int) * (size_t)F->cfw);
    F->fw_to = realloc(F->fw_to, sizeof(int) * (size_t)F->cfw);
    if (!F->fw_from || !F->fw_to) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->fw_from[F->nfw] = from;
  F->fw_to[F->nfw++] = to;
}
/* Call n hands method mi a block as a value (blk, a BlockArgumentNode): the
   enclosing method's own &blk (or an anonymous `&`) is a forward edge; any
   other block (`&proc`, `&:sym`, `&method(:m)`) is one the facts do not
   list (blk_unk). */
static void sh_block_value_to(ShareFacts *F, Compiler *c, int n, int blk, int mi) {
  const NodeTable *nt = c->nt;
  int x = nt_ref(nt, blk, "expression");
  int cur = sh_method_index(c, n);
  const char *bp = cur >= 0 ? c->scopes[cur].blk_param : NULL;
  if (cur >= 0 && bp &&
      (x < 0 ? !bp[0] : nt_kind(nt, x) == NK_LocalVariableReadNode && nt_int(nt, x, "depth", 0) == 0 &&
                        nt_str(nt, x, "name") && bp[0] && sp_streq(nt_str(nt, x, "name"), bp)))
    sh_fwd_edge(F, cur, mi);
  else F->blk_unk[mi] = 1;
}

static void sh_block_to_method(ShareFacts *F, Compiler *c, int blk, int mi) {
  Scope *m = &c->scopes[mi];
  /* (a method that keeps its block as &blk records it too: it may hand it
     on, share_value_fresh) */
  int yields = m->yields || m->is_lowered_yield;
  if ((yields || m->blk_param) && F->nmb >= F->cmb) {
    F->cmb = F->cmb ? F->cmb * 2 : 64;
    F->mb_m = realloc(F->mb_m, sizeof(int) * (size_t)F->cmb);
    F->mb_b = realloc(F->mb_b, sizeof(int) * (size_t)F->cmb);
    if (!F->mb_m || !F->mb_b) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  if (yields || m->blk_param) {
    F->mb_m[F->nmb] = mi;
    F->mb_b[F->nmb++] = blk;
  }
  /* a method lowered to take its block as a proc still yields to it */
  if (yields) {
    sh_block_params(F, c, blk, sh_scope_holder(F, SHK_YIELD, mi), 1);
    sh_union(F, sh_block_val(F, c, blk), sh_scope_holder(F, SHK_BLKRET, mi));
  }
  if (m->blk_param || m->is_lowered_yield || m->is_proc_form) {
    sh_block_params(F, c, blk, F->unknown, 1);
    sh_union(F, sh_block_val(F, c, blk), F->unknown);
  }
}

static void sh_named_build(ShareFacts *F, Compiler *c);
static int sh_named_first(const struct ShNamed *a, int n, const char *name);
/* the user methods a call reaches (cplan_targets: the plan's method and, for
   a switch, every member); -1 when they are more than cap or not knowable
   yet (the caller then treats the call as one the walk does not follow,
   never as fewer methods). The answers are held for one build of the facts,
   dropped at its start. */
static int sh_targets_in(ShareFacts *F, Compiler *c, int call, int *out, int cap) {
  (void)F;
  int n = cplan_targets(c, call, out, cap);
  return n == CPT_UNKNOWN ? -1 : n;
}
static int sh_targets(Compiler *c, int call, int *out, int cap) { return sh_targets_in(NULL, c, call, out, cap); }
/* does a call reach a user method (sh_targets != 0), without listing them */
static int sh_has_targets(Compiler *c, int call) {
  const CallPlan *p = cplan_user_fresh(c, call);
  return p->mi >= 0 && p->dispatch != CP_REFUSE;
}

/* the receiver family a builtin's share row is keyed by */
static TyKind sh_family(TyKind rt) {
  if (rt == TY_STRING || rt == TY_STRBUF) return TY_STRING;
  if (ty_is_array(rt) || ty_is_ptr_array(rt) || rt == TY_STR_RANGE || rt == TY_ENUMERATOR) return BOP_ANY_ARRAY;
  if (ty_is_hash(rt)) return BOP_ANY_HASH;
  if (rt == TY_ARGF) return TY_IO;
  if (rt == TY_PROC || rt == TY_METHOD || rt == TY_CURRY) return BOP_CALLABLE;
  return rt;
}

static int sh_named_cmp(const void *a, const void *b) {
  int d = strcmp(((const struct ShNamed *)a)->name, ((const struct ShNamed *)b)->name);
  return d ? d : ((const struct ShNamed *)a)->k - ((const struct ShNamed *)b)->k;
}
/* the indexes by name the walk asks per call, so a call costs a search
   rather than a pass over every class or method */
static void sh_named_build(ShareFacts *F, Compiler *c) {
  if (F->named_built) return;
  F->named_built = 1;
  int nr = 0, nw = 0;
  for (int k = 0; k < c->nclasses; k++) { nr += c->classes[k].nreaders; nw += c->classes[k].nwriters; }
  F->attr_r = malloc(sizeof *F->attr_r * (size_t)(nr + 1));
  F->attr_w = malloc(sizeof *F->attr_w * (size_t)(nw + 1));
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    for (int i = 0; i < ci->nreaders; i++)
      if (ci->readers[i]) F->attr_r[F->nattr_r++] = (struct ShNamed){ ci->readers[i], k };
    for (int i = 0; i < ci->nwriters; i++)
      if (ci->writers[i]) F->attr_w[F->nattr_w++] = (struct ShNamed){ ci->writers[i], k };
  }
  qsort(F->attr_r, (size_t)F->nattr_r, sizeof *F->attr_r, sh_named_cmp);
  qsort(F->attr_w, (size_t)F->nattr_w, sizeof *F->attr_w, sh_named_cmp);
}
/* the first entry of a sorted index whose name is `name` (n when none) */
static int sh_named_first(const struct ShNamed *a, int n, const char *name) {
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(a[mid].name, name) < 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}

/* The ivars the attr readers (or, for `x=`, the writers) of the name on any
   class read or write, joined; -1 when no class has one. */
static int sh_attr_ivars(ShareFacts *F, Compiler *c, const char *name, int node, int *writer) {
  size_t ln = strlen(name);
  *writer = ln > 1 && name[ln - 1] == '=' && name[0] != '=' && name[0] != '!' &&
            name[0] != '<' && name[0] != '>' && name[0] != '[';
  char base[256];
  if (ln >= sizeof base - 2) return -1;
  base[0] = '@';
  memcpy(base + 1, name, ln - (size_t)*writer);
  base[1 + ln - (size_t)*writer] = 0;
  int r = -1;
  sh_named_build(F, c);
  const struct ShNamed *a = *writer ? F->attr_w : F->attr_r;
  int na = *writer ? F->nattr_w : F->nattr_r;
  for (int i = sh_named_first(a, na, base + 1); i < na && sp_streq(a[i].name, base + 1); i++)
    r = sh_join(F, r, sh_ivar(F, c, a[i].k, base, node));
  return r;
}

static int sh_unknown_call(ShareFacts *F, Compiler *c, int n, int blk);

/* The share-row semantics of builtin call n. Answers its value. */
/* An iterator's block over a container's elements. Over a Hash (`hash`), a
   block of two or more plain parameters takes the key first: a key is the
   frozen copy CRuby makes as it is stored, no name for a value. */
static void sh_iter_params(ShareFacts *F, Compiler *c, int blk, int ev, int hash) {
  const NodeTable *nt = c->nt;
  int bp = hash ? nt_ref(nt, blk, "parameters") : -1;
  int pn = bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
  int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
  int nopt = 0; if (pn >= 0) nt_arr(nt, pn, "optionals", &nopt);
  if (nreq < 2 || nopt || nt_ref(nt, pn, "rest") >= 0 || nt_kind(nt, reqs[0]) != NK_RequiredParameterNode) {
    sh_block_params(F, c, blk, ev, 1);
    return;
  }
  for (int i = 1; i < nreq; i++) {
    sh_target(F, c, reqs[i], ev);
    sh_target(F, c, reqs[i], sh_elem(F, ev));
  }
}

/* Call n, a builtin that only reads its arguments (keep 0: BSH_PURE) or
   answers them (keep 1: BSH_ARGS, `p a, b`), recorded until the dropped
   values are known (sh_settle_peeks). */
static void sh_peek_args(ShareFacts *F, int n, int keep) {
  if (F->npk >= F->cpk) {
    F->cpk = F->cpk ? F->cpk * 2 : 64;
    F->pk = realloc(F->pk, sizeof(int) * (size_t)F->cpk);
    if (!F->pk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->pk[F->npk++] = keep ? -n - 1 : n;
}

/* Each recorded call's container literal arguments are SHU_PEEK, and so is
   a `p a, b` whose value is dropped (the Array it answers): no name sees
   their elements again. A call that answers its arguments counts only
   where its own value is dropped. The same marks describe transient call
   results, which need no handle when a builtin only reads them. */
static void sh_settle_peeks(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  /* a method no caller reads drops its body's value, through its arms
     (the walk is done: only the peeks read these marks now) */
  for (int mi = 0; mi < c->nscopes && F->npk > 0; mi++)
    if (c->scopes[mi].def_node >= 0 && !F->mread[mi]) sh_mark_unused(F, nt, c->scopes[mi].body, SHU_TAIL);
  for (int i = 0; i < F->npk; i++) {
    int n = F->pk[i] < 0 ? -F->pk[i] - 1 : F->pk[i];
    /* A builtin's pure/argument peek describes only that dispatch arm. A
       same-named user arm can retain these values, so its flows take
       precedence over the builtin-only no-retention fact. */
    if (sh_has_targets(c, n)) continue;
    if (F->pk[i] < 0) {
      if (!F->unused[n]) continue;
      F->unused[n] |= SHU_PEEK;
    }
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    int recv = nt_ref(nt, n, "receiver");
    const char *name = nt_str(nt, n, "name");
    /* A family's wildcard is not proof that an unlisted operation keeps
       no value. Non-literal peeks require the operation's own row. */
    int named = F->pk[i] < 0 || recv < 0 || bop_share_named(sh_family(c->ntype[recv]), name) ||
                bop_share_named(BOP_ANY_RECV, name);
    if (!named && (c->ntype[recv] == TY_POLY || c->ntype[recv] == TY_UNKNOWN))
      named = bop_share_named(BOP_ANY_ARRAY, name) || bop_share_named(BOP_ANY_HASH, name) ||
              bop_share_named(TY_CLASS, name) || bop_share_named(TY_STRING, name) || bop_share_named(TY_IO, name);
    for (int k = 0; k < argc; k++) {
      NodeKind ak = nt_kind(nt, argv[k]);
      if (ak == NK_ArrayNode || ak == NK_HashNode) F->unused[argv[k]] |= SHU_PEEK;
      else if (named) sh_mark_unused(F, nt, argv[k], SHU_PEEK);
    }
    if (F->pk[i] >= 0 && named) sh_mark_unused(F, nt, recv, SHU_PEEK);
  }
}

/* container: 1 an Array's (or a poly receiver's) row, 2 a Hash's */
static int sh_builtin(ShareFacts *F, Compiler *c, int n, int share, int rv, int blk, int container) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int vals[64];
  int nv = sh_args_vals(F, c, n, vals, 64);
  int lit_blk = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode;
  int bv = lit_blk ? sh_block_val(F, c, blk) : -1;
  /* a String's (or Kernel's) builtin answering a new String Array without
     a block (`s.split`, `s.scan(re)`): its elements are new Strings, held
     as a local's Array holds them, in a class of its own */
  if ((share == BSH_PURE || share == BSH_ITER_FRESH_RECV || (share == BSH_LINE && argc == 0)) && !container && blk < 0 &&
      c->ntype[n] == TY_STR_ARRAY) {
    F->fresh_cont[n] = 1;
    return sh_new(F, SHK_VALUE);
  }
  if (share == BSH_FILL) share = lit_blk ? BSH_ITER_MAP_BANG : BSH_STORE_ALL;
  switch (share) {
  case BSH_PACK:
    if (argc == 2 && nt_kind(nt, argv[1]) == NK_KeywordHashNode) {
      int en = 0, r = -1;
      const int *el = nt_arr(nt, argv[1], "elements", &en);
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, el[e], "key");
        if (nt_kind(nt, key) == NK_SymbolNode && !sp_streq(nt_str(nt, key, "value"), "buffer")) continue;
        int v = nt_kind(nt, el[e]) == NK_AssocSplatNode ? sh_arg_val(F, c, el[e]) : sh_val(F, c, nt_ref(nt, el[e], "value"));
        sh_mark_at(F, v, SHF_MUT | SHF_INDIRECT, n);
        r = sh_join(F, r, v);
      }
      return r;
    }
    /* Without a buffer, pack only reads its arguments. */
    /* fall through */
  case BSH_PURE:
    sh_peek_args(F, n, 0);
    /* a container's block is handed its elements, whatever it answers */
    if (lit_blk && container) sh_block_params(F, c, blk, sh_elem(F, rv), 1);
    return -1;
  case BSH_ITER_FRESH: case BSH_FROZEN:
    return -1;
  case BSH_RECV: case BSH_EMPTY_SELF: case BSH_ITER_FRESH_RECV:
    return rv;
  case BSH_CLAMP:
    /* A Range holds its endpoints in the existing element class. */
    for (int i = 0; i < nv; i++) rv = sh_join(F, rv, argc == 1 ? sh_elem(F, vals[i]) : vals[i]);
    return rv;
  case BSH_ELEM:
    if (container && nv >= 1) sh_lookup_key(F, rv, vals[0]);
    /* `a[i, n]`, `a[r]`: a run of elements */
    if (argc >= 2 || (argc == 1 && nt_kind(nt, argv[0]) == NK_RangeNode))
      return sh_join(F, rv, sh_elem(F, rv));
    return sh_elem(F, rv);
  case BSH_ELEM_N:
    return argc >= 1 ? sh_join(F, rv, sh_elem(F, rv)) : sh_elem(F, rv);
  case BSH_SUB:
    /* values_at runs a Hash's default proc on each key it misses */
    for (int i = 0; container == 2 && i < nv; i++) sh_lookup_key(F, rv, vals[i]);
    return rv;
  case BSH_FLATTEN:
    /* the elements of the receiver's nested containers, at any depth, are
       the answer's: the levels are one */
    sh_union(F, sh_elem(F, rv), sh_elem(F, sh_elem(F, rv)));
    return rv;
  case BSH_FETCH: {
    /* a default block is handed the key it was asked for */
    if (lit_blk && nv >= 1) sh_block_params(F, c, blk, vals[0], 0);
    /* Only a container can answer an element. String assignment copies
       bytes into its receiver and answers the argument, not that receiver. */
    int r = container ? sh_elem(F, rv) : -1;
    if (nv >= 2) r = sh_join(F, r, vals[nv - 1]);
    return sh_join(F, r, bv);
  }
  case BSH_SUBST: case BSH_SUBST_BANG:
    if (lit_blk && nv) sh_block_params(F, c, blk, vals[0], 0);
    if (blk < 0 && argc < 2) return sh_join(F, rv, nv ? vals[0] : -1);
    return share == BSH_SUBST_BANG ? rv : -1;
  case BSH_LINE:
    if (lit_blk && nv && (c->ntype[argv[0]] == TY_NIL || c->ntype[argv[0]] == TY_POLY ||
                          c->ntype[argv[0]] == TY_UNKNOWN)) sh_block_params(F, c, blk, rv, 0);
    return rv;
  case BSH_BLOCK:
    return bv;
  case BSH_LAST:
    return nv > 0 ? vals[nv - 1] : -1;
  case BSH_SUM:
    if (lit_blk) sh_iter_params(F, c, blk, sh_elem(F, rv), container == 2);
    if (nv && (c->ntype[argv[0]] == TY_STRING || c->ntype[argv[0]] == TY_STRBUF)) return vals[0];
    return sh_join(F, sh_join(F, nv ? vals[0] : -1, sh_elem(F, rv)), bv);
  case BSH_QUERY:
    if (lit_blk) sh_iter_params(F, c, blk, sh_elem(F, rv), container == 2);
    return blk < 0 ? rv : -1;
  case BSH_STORE_LAST:
    /* Hash copies String keys; a key of another kind retains its object. */
    if (container == 2 && nv >= 2 && c->ntype[argv[0]] != TY_STRING && c->ntype[argv[0]] != TY_STRBUF) {
      sh_union(F, sh_elem(F, rv), vals[0]);
      sh_flow(F, SHFL_ELEM, nt_ref(nt, n, "receiver"), argv[0], sh_elem_peek(F, rv));
    }
    if (nv > 0) sh_union(F, sh_elem(F, rv), vals[nv - 1]);
    if (nv > 0 && argc > 0 && nt_kind(nt, argv[argc - 1]) != NK_KeywordHashNode)
      sh_flow(F, SHFL_ELEM, nt_ref(nt, n, "receiver"), argv[argc - 1], sh_elem_peek(F, rv));
    /* A used setter result names the stored element, even when the
       argument was fresh and therefore had no holder of its own. */
    return nv > 0 ? (container && !(F->unused[n] & SHU_STMT) ? sh_elem(F, rv) : vals[nv - 1]) : rv;
  case BSH_STORE_ALL:
    for (int i = 0; i < nv; i++) sh_union(F, sh_elem(F, rv), vals[i]);
    sh_args_flows(F, c, SHFL_ELEM, n, nt_ref(nt, n, "receiver"), sh_elem_peek(F, rv));
    return rv;
  case BSH_STORE_TAIL:
    for (int i = 1; i < nv; i++) sh_union(F, sh_elem(F, rv), vals[i]);
    for (int i = 1; i < argc; i++)
      if (nt_kind(nt, argv[i]) != NK_SplatNode && nt_kind(nt, argv[i]) != NK_KeywordHashNode)
        sh_flow(F, SHFL_ELEM, nt_ref(nt, n, "receiver"), argv[i], sh_elem_peek(F, rv));
    return rv;
  case BSH_MERGE:
    /* a receiver that holds no String (`[1].zip([s])`) still answers a
       container of what the arguments hold */
    if (rv < 0 && nv > 0) rv = sh_new(F, SHK_VALUE);
    for (int i = 0; i < nv; i++) {
      sh_union(F, sh_elem(F, rv), sh_elem(F, vals[i]));
      /* zip and product pair elements up: a tuple holds the elements */
      sh_union(F, sh_elem(F, rv), vals[i]);
    }
    if (lit_blk) {
      sh_block_params(F, c, blk, sh_elem(F, rv), 1);
      sh_union(F, sh_elem(F, rv), bv);
      sh_block_flow(F, nt, n, blk, sh_elem_peek(F, rv));
    }
    return rv;
  case BSH_ARGS: {
    if (lit_blk && container) sh_block_params(F, c, blk, sh_elem(F, rv), 1);
    /* one argument is the answer; several, an Array of them, which joins
       them only where something takes it (`p a, b` as a statement keeps
       neither) */
    /* An empty sum can answer its seed; its block still reads the elements. */
    if (lit_blk && container) sh_block_params(F, c, blk, sh_elem(F, rv), 1);
    if (container && nv == 0) return -1;
    sh_peek_args(F, n, 1);
    if (nv == 1) return vals[0];
    if (F->unused[n] & SHU_STMT) return -1;
    int r = sh_new(F, SHK_VALUE);
    for (int i = 0; i < nv; i++) sh_union(F, sh_elem(F, r), vals[i]);
    sh_args_flows(F, c, SHFL_ELEM, n, n, sh_elem_peek(F, r));
    return r;
  }
  case BSH_ARRAY_OF: {
    /* an Array is the answer itself; anything else, wrapped in one */
    if (nv != 1) return -1;
    TyKind at = c->ntype[argv[0]];
    if (ty_is_array(at) || at == TY_POLY || at == TY_UNKNOWN) return sh_join(F, vals[0], -1);
    int r = sh_new(F, SHK_VALUE);
    sh_union(F, sh_elem(F, r), vals[0]);
    sh_flow(F, SHFL_ELEM, n, argv[0], sh_elem_peek(F, r));
    return r;
  }
  case BSH_FILL1:
    if (argc >= 2) {
      int b = sh_val(F, c, argv[1]);
      sh_mark_at(F, b, SHF_MUT | (sh_holder_read(nt, argv[1]) ? 0 : SHF_INDIRECT), n);
      return b;
    }
    return -1;
  case BSH_FILL2:
    if (argc >= 3) {
      int b = sh_val(F, c, argv[2]);
      sh_mark_at(F, b, SHF_MUT | (sh_holder_read(nt, argv[2]) ? 0 : SHF_INDIRECT), n);
      return b;
    }
    return -1;
  case BSH_ITER: case BSH_ITER_SEL: case BSH_ITER_FIND:
    if (lit_blk) sh_iter_params(F, c, blk, sh_elem(F, rv), container == 2);
    return share == BSH_ITER_FIND ? (argc >= 1 ? rv : sh_elem(F, rv)) : rv;
  case BSH_ITER_MAP_BANG:
    if (lit_blk) {
      sh_iter_params(F, c, blk, sh_elem(F, rv), container == 2);
      sh_union(F, sh_elem(F, rv), bv);
      sh_block_flow(F, nt, n, blk, sh_elem_peek(F, rv));
    }
    return rv;
  case BSH_ITER_MAP: {
    if (lit_blk) sh_iter_params(F, c, blk, sh_elem(F, rv), container == 2);
    /* `map(&:to_s)`: a new container of what the name answers on each
       element, which is the element at most; elements that hold no String
       (Symbols, numbers) answer new Strings */
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode &&
        nt_kind(nt, nt_ref(nt, blk, "expression")) == NK_SymbolNode) {
      int r = sh_new(F, SHK_VALUE);
      int recv = nt_ref(nt, n, "receiver");
      TyKind at = recv >= 0 ? c->ntype[recv] : TY_UNKNOWN;
      TyKind et = ty_is_array(at) ? ty_array_elem(at) : TY_UNKNOWN;
      if (et == TY_UNKNOWN || sh_may_hold(c, et)) sh_union(F, sh_elem(F, r), sh_elem(F, rv));
      return r;
    }
    if (!lit_blk) return rv;   /* an Enumerator over the receiver */
    /* a new container of the block's values; a flat_map's or to_h's value
       is itself a container of them */
    int r = sh_new(F, SHK_VALUE);
    sh_union(F, sh_elem(F, r), bv);
    sh_union(F, sh_elem(F, r), sh_elem(F, bv));
    sh_block_flow(F, nt, n, blk, sh_elem_peek(F, r));
    return r;
  }
  case BSH_ITER_SUB:
    /* the block takes runs of elements: a run's elements are the
       receiver's, so the two levels are one */
    sh_union(F, rv, sh_elem(F, rv));
    if (lit_blk) sh_block_params(F, c, blk, rv, 1);
    return rv;
  case BSH_ITER_MEMO0: {
    const char *sym = argc >= 1 ? sh_lit_name(nt, argv[argc - 1]) : NULL;
    int memo = nv >= 1 && !sym ? vals[0] : sh_elem(F, rv);
    if (lit_blk) {
      int bp = nt_ref(nt, blk, "parameters");
      int pn = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
      int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
      for (int i = 0; i < nreq; i++) sh_target(F, c, reqs[i], i == 0 ? memo : sh_elem(F, rv));
      memo = sh_join(F, memo, bv);
    }
    return sym ? -1 : memo;
  }
  case BSH_ITER_MEMO1: {
    int memo = nv >= 1 ? vals[0] : -1;
    if (lit_blk) {
      int bp = nt_ref(nt, blk, "parameters");
      int pn = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
      int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
      for (int i = 0; i < nreq; i++) {
        int src = i == 1 ? memo : sh_elem(F, rv);
        sh_target(F, c, reqs[i], src);
        if (i == 0 && nt_kind(nt, reqs[i]) == NK_MultiTargetNode) sh_target(F, c, reqs[i], sh_elem(F, src));
      }
    }
    return memo;
  }
  case BSH_ITER_SELF:
    /* a fresh receiver (`(+"a").tap { |x| x << y }`) is one String the
       block's parameter and the answer both name, the answer a second name
       when it is used */
    if (rv < 0 && lit_blk && !(F->unused[n] & SHU_STMT)) {
      rv = sh_new(F, SHK_VALUE);
      F->nhold[rv] = 1;
    }
    if (lit_blk) sh_block_params(F, c, blk, rv, 0);
    if (lit_blk) sh_flow(F, SHFL_PARAM, n, nt_ref(nt, n, "receiver"), rv);
    return rv;
  case BSH_ITER_THEN:
    if (blk < 0) return rv;  /* the Enumerator retains its receiver */
    if (lit_blk) sh_block_params(F, c, blk, rv, 0);
    if (lit_blk) sh_flow(F, SHFL_PARAM, n, nt_ref(nt, n, "receiver"), rv);
    return bv;
  case BSH_UNKNOWN:
    sh_union(F, rv, F->unknown);
    return sh_unknown_call(F, c, n, blk);
  case BSH_CALL:
    return sh_unknown_call(F, c, n, blk);
  case BSH_METHOD_REF:
    /* the method it names is called from wherever the Method goes; a
       define_method body is called with what the walk does not see */
    if (is_method_obj_call(c, n)) {
      int mi = method_obj_target_mi(c, n);
      if (method_call_param_shift(c, n, mi)) {
        Scope *m = &c->scopes[mi];
        int p = sh_local_of(F, c, m, m->pnames[0], m->def_node);
        /* The wrapper's first parameter is the captured receiver, not
           an independent argument. A boxed capture holds it in an Array. */
        int pd = nt_int(nt, m->def_node, "bam_poly", 0) ? sh_elem(F, p) : p;
        sh_union(F, pd, rv);
        sh_flow(F, SHFL_ARG, n, nt_ref(nt, n, "receiver"), pd);
      }
    }
    sh_dyn_name(F, argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL);
    if (lit_blk) {
      sh_block_params(F, c, blk, F->unknown, 1);
      sh_union(F, bv, F->unknown);
      /* a define_method body's break is the method's value too */
      sh_union(F, sh_jumped(F, n, -1), F->unknown);
    }
    return -1;
  case BSH_IVAR_GET: case BSH_IVAR_SET: {
    const char *lit = argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL;
    TyKind rt = nt_ref(nt, n, "receiver") >= 0 ? c->ntype[nt_ref(nt, n, "receiver")] : TY_VOID;
    int cid = ty_is_object(rt) ? ty_object_class(rt) : rt == TY_VOID ? sh_ivar_owner(c, n) : -1;
    int iv = lit && cid >= 0 ? sh_ivar(F, c, cid, lit, n) : -1;
    if (!lit || cid < 0) { F->dyn_ivars = 1; iv = F->unknown; }
    /* A lowered ivar read must carry the handle through its reflective
       result too, even when the caller only reads the method's answer. */
    if (share == BSH_IVAR_GET && nt_int(nt, n, "builtin_only", 0))
      sh_flow(F, SHFL_WRITE, n, n, -1);
    if (share == BSH_IVAR_SET && nv >= 2) {
      sh_ivar_store(F, c, iv, argc >= 2 ? argv[1] : -1, vals[1]);
      if (argc >= 2) sh_flow(F, SHFL_MEMBER, n, argv[1], iv);
    }
    return iv;
  }
  case BSH_EXEC:
    if (!lit_blk) return sh_unknown_call(F, c, n, blk);
    sh_block_params(F, c, blk, F->unknown, 1);
    for (int i = 0; i < nv; i++) sh_union(F, vals[i], F->unknown);
    return bv;
  default:
    return -1;
  }
}

/* A builtin call no row describes on a container: it may store any
   argument and answer anything the receiver holds. */
static int sh_container_default(ShareFacts *F, Compiler *c, int n, int rv, int blk) {
  /* A fresh receiver can still retain supplied arguments (ENV's
     snapshot followed by assoc/rassoc); give those aliases a class. */
  if (rv < 0) rv = sh_new(F, SHK_VALUE);
  int vals[64];
  int nv = sh_args_vals(F, c, n, vals, 64);
  for (int i = 0; i < nv; i++) {
    sh_union(F, sh_elem(F, rv), vals[i]);
    sh_union(F, sh_elem(F, rv), sh_elem(F, vals[i]));
  }
  sh_args_flows(F, c, SHFL_ELEM, n, nt_ref(c->nt, n, "receiver"), sh_elem_peek(F, rv));
  if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockNode) {
    sh_union(F, rv, sh_elem(F, rv));
    sh_block_params(F, c, blk, rv, 1);
    sh_union(F, rv, sh_block_val(F, c, blk));
  }
  return sh_join(F, rv, sh_elem(F, rv));
}

/* The row the no-user boxed-call path already uses for a receiver whose
   type is poly/unknown. Keep its precedence in one place so a user-target
   call can compose that same row beside the user returns when its dispatch
   plan proves that those are the only returning arms. */
static int sh_boxed_builtin_row(ShareFacts *F, Compiler *c, int n,
                                int *share_out, int *family_out, int *container_out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, n, "name");
  int args = nt_ref(nt, n, "arguments"), argc = 0;
  if (!name) return 0;
  if (args >= 0) (void)nt_arr(nt, args, "arguments", &argc);
  int blk = nt_ref(nt, n, "block");
  int s = 0, family = 0, container = 1;
  /* A separator form of String#partition is the String row; Array's
     block-form Enumerable row is a different call shape. */
  if (argc == 1 && blk < 0 && is_partition_family(name)) {
    s = bop_share_boxed(TY_STRING, name);
    if (s) family = TY_STRING;
  }
  if (!s) { s = bop_share_named(BOP_ANY_ARRAY, name); if (s) family = BOP_ANY_ARRAY; }
  if (!s) { s = bop_share_named(BOP_ANY_HASH, name); if (s) family = BOP_ANY_HASH; }
  if (!s) { s = bop_share_named(BOP_ANY_RECV, name); if (s) family = BOP_ANY_RECV; }
  if (!s && !F->ostruct) { s = bop_share_named(TY_CLASS, name); if (s) family = TY_CLASS; }
  /* A String-only operation answering a new String Array keeps that fact
     even when its receiver is boxed: it is no container's row. */
  if (!s && !F->ostruct && blk < 0 && c->ntype[n] == TY_STR_ARRAY) {
    const IterRow *ir = iter_row(TY_STRING, name, argc, 0);
    if (ir && ir->nyield == 1 && ir->yield[0] == YS_FRESH && ir->answer == IA_RECV) {
      s = BSH_PURE; family = TY_STRING; container = 0;
    }
  }
  if (!s && !F->ostruct) { s = bop_share_boxed(TY_STRING, name); if (s) family = TY_STRING; }
  /* Explicit IO rows describe boxed arms too. User targets and OpenStruct
     fields stay above; an IO's wildcard cannot prove this. */
  if (!s && !F->ostruct) { s = bop_share_boxed(TY_IO, name); if (s) family = TY_IO; }
  if (!s) return 0;
  *share_out = s;
  if (family_out) *family_out = family;
  *container_out = container;
  return 1;
}

static int sh_plan_target_has(const int *tg, int ntg, int mi) {
  for (int i = 0; i < ntg; i++) if (tg[i] == mi) return 1;
  return 0;
}

static int sh_bop_shape_fits(const BuiltinOp *op, const char *name, int argc, int has_block) {
  if (!op || !op->name || !sp_streq(op->name, name) || argc < op->argc_min || argc > op->argc_max) return 0;
  if ((op->block == BF_NONE && has_block) || (op->block == BF_REQUIRED && !has_block)) return 0;
  return 1;
}

typedef struct {
  Compiler *c;
  const int *argv;
  int argc;
} ShBopArgs;

static TyKind sh_bop_arg_type(const void *ud, int i) {
  const ShBopArgs *a = ud;
  return i >= 0 && i < a->argc ? a->c->ntype[a->argv[i]] : TY_UNKNOWN;
}

static int sh_has_method_missing_candidate(Compiler *c);

/* A boxed String row can replace the container default only when the
   selected typed row describes this call shape and the dispatch plan and
   builtin ownership tables account for the other routes. The plan may offer
   PT_STR directly, or pair PB_ND_GENERIC with PT_GENERIC_TAIL for the
   generic builtin re-entry. Gather the plan facts before calling sh_builtin,
   which can recurse through arguments and invalidate memoized plan storage. */
static int sh_boxed_string_row_composable(ShareFacts *F, Compiler *c, int n,
                                          int ntg, const int *tg, int share,
                                          int family, int container,
                                          const BuiltinOp **row_out, TyKind *plan_ret_out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, n, "name");
  int blk = nt_ref(nt, n, "block");
  int argc = 0, args = nt_ref(nt, n, "arguments");
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  /* A user arm may reach a helper that reads a published singleton-accessor
     value even when that accessor is not in this call's dispatch plan. */
  if (!name || family != TY_STRING || F->ostruct || F->singleton_accessors || blk >= 0 ||
      !poly_string_read_p(name) ||
      container != 1 || share != bop_share_boxed(TY_STRING, name) ||
      !nt_call_args_plain(nt, n)) return 0;

  /* String readers that are also a different builtin face need that face's
     result too. The name-only reader table covers the boxed scalar/container
     surfaces; the exact typed rows below cover the remaining BOP owners. */
  if (bop_name_has_reader(name, BOP_READ_NUMERIC | BOP_READ_CONTAINER)) return 0;
  unsigned faces = ty_poly_face_owners(name, argc, 0, nt_call_args_plain(nt, n), 1);
  if ((faces & PF_OWNERS) & ~PF_STRING) return 0;
  ShBopArgs bop_args = { c, argv, argc };
  const BuiltinOp *op = bop_find_arg(TY_STRING, name, argc, 0,
                                     sh_bop_arg_type, &bop_args);
  if (!op || op->emit == BOPE_NONE || op->result != TY_STRING) return 0;
  int has_other_builtin = 0;
  for (int i = 0; i < bop_row_count(); i++) {
    const BuiltinOp *row = bop_row(i);
    if (row && row->recv != TY_STRING && row->recv != TY_STRBUF &&
        sh_bop_shape_fits(row, name, argc, 0)) { has_other_builtin = 1; break; }
  }
  if (has_other_builtin || bop_share_named(BOP_ANY_RECV, name)) return 0;

  /* Match the existing boxed-freshness exclusions for Object and dynamic
     lookup. These routes can answer without being an ordinary user target. */
  if (object_public_method_name(name) || comp_method_index(c, "method_missing") >= 0) return 0;
  if (sh_has_method_missing_candidate(c)) return 0;
  int missing = 0;
  comp_cmethod_candidates(c, "method_missing", &missing);
  if (missing) return 0;

  const PolyPlan *p = cplan_poly_fresh(c, n);
  if (!p || (p->ret != TY_STRING && p->ret != TY_POLY)) return 0;
  int has_str = 0, has_generic_tail = 0, has_generic_default = 0, ok = p->n > 0;
  for (int i = 0; ok && i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->kind == PA_USER || a->kind == PA_PROC_FORM) {
      if (a->mi < 0 || a->mi >= c->nscopes || !sh_plan_target_has(tg, ntg, a->mi)) ok = 0;
    }
    else if (a->kind == PA_ARITY) {
      /* The arm raises ArgumentError and has no return value to join. */
    }
    else if (a->kind == PA_TRIAL) {
      if (a->key == PA_KEY_TRIAL + PT_STR) has_str = 1;
      else if (a->key == PA_KEY_TRIAL + PT_GENERIC_TAIL) has_generic_tail = 1;
      else ok = 0;
    }
    else if (a->kind == PA_BUILTIN) {
      if (a->key == PA_KEY_BUILTIN + PB_ND_GENERIC) has_generic_default = 1;
      else ok = 0;
    }
    else ok = 0;
  }
  int generic_tail = has_generic_tail && has_generic_default;
  int partial_generic_tail = has_generic_tail != has_generic_default;
  if (!ok || partial_generic_tail || (!has_str && !generic_tail)) return 0;
  if (row_out) *row_out = op;
  if (plan_ret_out) *plan_ret_out = p->ret;
  return 1;
}

static void sh_record_boxed_fresh(ShareFacts *F, Compiler *c, int n,
                                  int share, int family, int container,
                                  const BuiltinOp *row, TyKind plan_ret,
                                  int ntg, const int *tg) {
  const NodeTable *nt = c->nt;
  if (n < 0 || n >= F->nnodes) return;
  if (!F->boxed_fresh_index) {
    F->boxed_fresh_index = calloc((size_t)(F->nnodes > 0 ? F->nnodes : 1),
                                  sizeof(*F->boxed_fresh_index));
    if (!F->boxed_fresh_index) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  int args = nt_ref(nt, n, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int recv = nt_ref(nt, n, "receiver");
  if (F->nboxed_fresh >= F->cboxed_fresh) {
    F->cboxed_fresh = F->cboxed_fresh ? F->cboxed_fresh * 2 : 8;
    F->boxed_fresh = realloc(F->boxed_fresh,
                             sizeof(*F->boxed_fresh) * (size_t)F->cboxed_fresh);
    if (!F->boxed_fresh) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  ShBoxedFreshFact fact = {
    .call = n, .share = share, .family = family, .container = container,
    .recv_type = recv >= 0 ? c->ntype[recv] : TY_UNKNOWN,
    .result_type = c->ntype[n], .plan_ret = plan_ret,
    .argc = argc, .ntg = ntg, .row = row
  };
  fact.arg_types = argc ? malloc(sizeof(*fact.arg_types) * (size_t)argc) : NULL;
  fact.targets = ntg ? malloc(sizeof(*fact.targets) * (size_t)ntg) : NULL;
  if ((argc && !fact.arg_types) || (ntg && !fact.targets)) {
    free(fact.arg_types); free(fact.targets);
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  for (int i = 0; i < argc; i++) fact.arg_types[i] = c->ntype[argv[i]];
  for (int i = 0; i < ntg; i++) fact.targets[i] = tg[i];
  int old = F->boxed_fresh_index[n] - 1;
  if (old >= 0 && old < F->nboxed_fresh) {
    free(F->boxed_fresh[old].arg_types);
    free(F->boxed_fresh[old].targets);
    F->boxed_fresh[old] = fact;
    return;
  }
  int at = F->nboxed_fresh++;
  F->boxed_fresh[at] = fact;
  F->boxed_fresh_index[n] = at + 1;
}

/* Revalidate the selected builtin row and dispatch shape against the exact
   effect signature captured by sh_builtin in the sharing walk. */
static int sh_boxed_fresh_recorded(Compiler *c, int n) {
  ShareFacts *F = c->share;
  if (!F) return 0;
  const ShBoxedFreshFact *fact = NULL;
  int at = n >= 0 && n < F->nnodes && F->boxed_fresh_index
    ? F->boxed_fresh_index[n] - 1 : -1;
  if (at >= 0 && at < F->nboxed_fresh) fact = &F->boxed_fresh[at];
  if (!fact || fact->call != n) return 0;

  int tg[CPT_MAX], ntg = cplan_targets(c, n, tg, CPT_MAX);
  int share = 0, family = 0, container = 1;
  const BuiltinOp *row = NULL;
  TyKind plan_ret = TY_UNKNOWN;
  if (ntg < 0 || !sh_boxed_builtin_row(F, c, n, &share, &family, &container) ||
      !sh_boxed_string_row_composable(F, c, n, ntg, tg, share, family, container,
                                      &row, &plan_ret)) return 0;
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, n, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int recv = nt_ref(nt, n, "receiver");
  int recv_type = recv >= 0 ? c->ntype[recv] : TY_UNKNOWN;
  if (share != fact->share || family != fact->family || container != fact->container ||
      row != fact->row || plan_ret != fact->plan_ret || c->ntype[n] != fact->result_type ||
      recv_type != fact->recv_type || argc != fact->argc || ntg != fact->ntg) return 0;
  for (int i = 0; i < argc; i++) if (c->ntype[argv[i]] != fact->arg_types[i]) return 0;
  for (int i = 0; i < ntg; i++) if (tg[i] != fact->targets[i]) return 0;
  return 1;
}

/* A call the walk does not follow: what it is handed and what it answers
   are UNKNOWN. */
static int sh_unknown_call(ShareFacts *F, Compiler *c, int n, int blk) {
  int vals[64];
  int nv = sh_args_vals(F, c, n, vals, 64);
  for (int i = 0; i < nv; i++) {
    /* A proc handed self can keep or mutate that receiver. */
    if (vals[i] >= 0 && F->kind[vals[i]] == SHK_SELF) F->own[vals[i]] |= SHE_IDENTITY | SHE_COMPARED;
    sh_union(F, vals[i], F->unknown);
  }
  if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockNode) {
    sh_block_params(F, c, blk, F->unknown, 1);
    sh_union(F, sh_block_val(F, c, blk), F->unknown);
  }
  return F->unknown;
}

/* Block blk's i-th required parameter, or -1 */
static int sh_block_param(const NodeTable *nt, int blk, int i) {
  int bp = nt_ref(nt, blk, "parameters");
  int pn = bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
  int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
  return i < nreq ? reqs[i] : -1;
}

/* The Strings exceptions keep as their messages: one value, made on first
   use. A message read answers it, so a String no exception was handed
   (a builtin's own message) is a new one. */
static int sh_exc(ShareFacts *F) {
  if (F->exc < 0) {
    F->exc = sh_new(F, SHK_VALUE);
    F->nhold[F->exc] = 1;   /* the exceptions keep their messages: a name */
  }
  return F->exc;
}
/* Call n's arguments are handed to an exception: its message may be any
   of them. */
static void sh_exc_args(ShareFacts *F, Compiler *c, int n) {
  int vals[64];
  int nv = sh_args_vals(F, c, n, vals, 64);
  for (int k = 0; k < nv; k++) sh_union(F, vals[k], sh_exc(F));
}

/* `Array.new(n, s)`, `Hash.new { }`, `Enumerator.new { |y| }`,
   `OpenStruct.new(name: s)`, `ArgumentError.new(s)`: a builtin class's
   constructor, read off its BOP_CLASS_NEW row. The container it answers
   holds what it is handed: Array.new(n, s)'s fill value and its block's
   values; Hash.new's default and its block's
   values (the block is handed the Hash, and the key each lookup asks for:
   sh_key); what Enumerator.new's block hands its yielder, which next
   answers; an OpenStruct's fields. An exception's arguments are its
   message (sh_exc). -2 for any other class
   (sh_call then reads it as a class held in a variable). */
static int sh_builtin_new(ShareFacts *F, Compiler *c, int n, int recv, int blk) {
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, recv, "name");
  if (!cn) return -2;
  int vals[64];
  /* a builtin exception keeps its argument as its message (sh_exc) */
  if (is_builtin_exception_name(cn)) {
    sh_exc_args(F, c, n);
    return -1;
  }
  int share = bop_share_named(BOP_CLASS_NEW, cn);
  if (!share) return -2;
  int lit_blk = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode;
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int r = sh_new(F, SHK_VALUE);
  int er = sh_elem(F, r);
  for (int i = 0; i < argc; i++) {
    NodeKind ak = nt_kind(nt, argv[i]);
    if (ak == NK_BlockArgumentNode) continue;
    /* keywords: OpenStruct.new(name: s)'s are its fields; Hash.new's
       capacity: keeps nothing */
    int v = ak == NK_KeywordHashNode ? sh_val(F, c, argv[i]) : sh_arg_val(F, c, argv[i]);
    if (share == BSH_NEW_FIELDS) sh_union(F, er, sh_elem(F, v));
    else if (ak == NK_KeywordHashNode) continue;
    else if ((share == BSH_NEW_FILL && i == 1) || (share == BSH_NEW_DEFAULT && i == 0)) {
      sh_union(F, er, v);
      if (ak != NK_SplatNode) sh_flow(F, SHFL_ELEM, n, argv[i], er);
    }
    /* Array.new(a) holds a's elements. Under --share-strings the containers
       join as BSH_SUB's do: the copy also retains a literal source's
       elements, including fresh Strings. The existing route checks guard
       transfers of those Strings without handles. */
    if (c->share_strings && share == BSH_NEW_FILL && argc == 1 && blk < 0)
      sh_union(F, r, v);
    if (share == BSH_NEW_FILL && i == 0) sh_union(F, er, sh_elem(F, v));
  }
  if (!lit_blk) return r;
  if (share == BSH_NEW_DEFAULT) {
    sh_target(F, c, sh_block_param(nt, blk, 0), r);
    sh_target(F, c, sh_block_param(nt, blk, 1), sh_key(F, r));
  }
  if (share == BSH_NEW_YIELDER) {
    /* the yielder is no String: what it is handed (`y << v`) is an element */
    int y = sh_new(F, SHK_VALUE);
    sh_union(F, sh_elem(F, y), er);
    sh_target(F, c, sh_block_param(nt, blk, 0), y);
  }
  if (share == BSH_NEW_FILL || share == BSH_NEW_DEFAULT) {
    sh_union(F, er, sh_block_val(F, c, blk));
    sh_block_flow(F, nt, n, blk, er);
  }
  return r;
}

/* A native class's object keeps a String as its binding declares
   (`native_share`, NSH_KEEPS): its first String argument, or with none a
   String of its own, is the element of the value the constructor answers;
   what else it is handed joins UNKNOWN. -2 for a constructor that declares
   nothing. */
static int sh_native_new(ShareFacts *F, Compiler *c, int n, int cid) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int nm = comp_native_method_find(c, cid, "new", argc, 1);
  if (nm < 0 || !(c->native_methods[nm].share & NSH_KEEPS)) return -2;
  int r = sh_new(F, SHK_VALUE);
  int er = sh_elem(F, r);
  for (int i = 0; i < argc; i++) sh_union(F, i == 0 ? er : F->unknown, sh_arg_val(F, c, argv[i]));
  if (c->native_methods[nm].share & NSH_CHANGES) sh_mark_at(F, er, SHF_MUT | SHF_INDIRECT, n);
  return r;
}
/* A native class's method, as its binding declares (`native_share`): one
   that changes the String the object keeps marks it, one that answers it
   answers it. The change is made through the object, which need not be
   named again (`StringIO.new(s).write(x)`), so no other holder of the
   String's class is needed for it to be seen (SHF_INDIRECT). One that
   answers a new String answers no value a holder shares (-1, as a pure
   builtin does). What the method
   is handed joins UNKNOWN, as for any call the walk does not follow. A
   call on self (`on_self`, in the package's own Ruby methods) takes only
   the new String. -2 for a method that declares nothing. */
static int sh_native_call(ShareFacts *F, Compiler *c, int n, int cid, const char *name, int argc, int rv, int blk,
                          int on_self) {
  int nm = comp_native_method_find(c, cid, name, argc, 0);
  unsigned sh = nm >= 0 ? c->native_methods[nm].share : 0;
  if (!sh || (on_self && !(sh & NSH_FRESH))) return -2;
  int u = sh_unknown_call(F, c, n, blk);
  if (sh & NSH_FRESH) return -1;
  if (sh & NSH_CHANGES) sh_mark_at(F, sh_elem(F, rv), SHF_MUT | SHF_INDIRECT, n);
  return sh & NSH_ANSWERS ? sh_elem(F, rv) : u;
}

/* `Klass.new(...)`: the class's initialize, a Struct's members */
static int sh_new_call(ShareFacts *F, Compiler *c, int n, int recv, int blk) {
  const NodeTable *nt = c->nt;
  NodeKind rk = nt_kind(nt, recv);
  int cid = (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)
            ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
  if (cid < 0) return sh_builtin_new(F, c, n, recv, blk);
  ClassInfo *ci = &c->classes[cid];
  /* an exception keeps its message, which #message hands back */
  if (class_is_exc_subclass(c, cid) && (!c->share_strings || cplan_initialize(c, n) < 0)) {
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    if (!c->share_strings)
      for (int k = 0; k < nv; k++) sh_union(F, vals[k], F->unknown);
    sh_exc_args(F, c, n);
  }
  if (ci->is_struct) {
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    int args = nt_ref(nt, n, "arguments"), argc = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    for (int i = 0; i < ci->nivars; i++) {
      int iv = sh_ivar(F, c, cid, ci->ivars[i], n);
      for (int k = 0; k < nv; k++) {
        sh_ivar_store(F, c, iv, argc == nv ? argv[k] : -1, vals[k]);
        if (argc == nv) sh_flow(F, SHFL_MEMBER, n, argv[k], iv);
      }
    }
    return -1;
  }
  if (ci->is_native_class) {
    int r = sh_native_new(F, c, n, cid);
    if (r != -2) return r;
  }
  int mi = cplan_initialize(c, n);
  if (mi < 0) return ci->def_node >= 0 ? -1 : -2;
  sh_bind(F, c, n, mi);
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_block_to_method(F, c, blk, mi);
  return -1;
}

/* A call on an OpenStruct, whose fields are its elements: its rows (`[]`,
   `[]=`, `to_h`), Object's, then a field's reader (`os.name`) or writer
   (`os.name = s`). */
static int sh_ostruct_call(ShareFacts *F, Compiler *c, int n, const char *name, int rv, int blk) {
  size_t ln = strlen(name);
  int s = bop_share_named(TY_OPENSTRUCT, name);
  if (!s) s = bop_share_named(BOP_ANY_RECV, name);
  if (!s && ln > 1 && name[ln - 1] == '=' && strchr("=!<>[", name[0]) == NULL) s = BSH_STORE_LAST;
  int args = nt_ref(c->nt, n, "arguments");
  int argc = 0;
  if (args >= 0) nt_arr(c->nt, args, "arguments", &argc);
  if (!s && argc == 0) s = BSH_ELEM;
  return s ? sh_builtin(F, c, n, s, rv, blk, 2) : sh_container_default(F, c, n, rv, blk);
}

/* The receiver a mutation through node n reaches: n itself, or, past a
   chain of builtin String calls whose row answers their receiver itself
   (`line << a << b`, `s.strip!.upcase!`: BOPF_SELF, BOPF_SELF_OR_NIL), the
   chain's first receiver. The chain's value is that receiver's String, so
   a mutation of it is one of that receiver's, direct when it is a holder's
   read; the walk gave each link its receiver's class (BSH_RECV), which the
   class test confirms. */
static int sh_self_chain_base(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  for (n = an_unparen(nt, n); n >= 0 && nt_kind(nt, n) == NK_CallNode; ) {
    int r = an_unparen(nt, nt_ref(nt, n, "receiver"));
    TyKind rt = r >= 0 ? c->ntype[r] : TY_VOID;
    const char *nm = nt_str(nt, n, "name");
    if ((rt != TY_STRING && rt != TY_STRBUF) || !nm ||
        !(bop_answers_self(TY_STRING, nm, call_plain_argc(c, n), nt_ref(nt, n, "block") >= 0) &
          (BOPF_SELF | BOPF_SELF_OR_NIL)) ||
        F->nval[n] < 0 || F->nval[r] < 0 || sh_find(F, F->nval[n]) != sh_find(F, F->nval[r]) ||
        sh_has_targets(c, n))
      break;
    n = r;
  }
  return n;
}

/* The names parameter node p binds (a block's ParametersNode, or one of
   its parameters, destructured or not), into F->blkp under scope si. */
static void sh_blkp_add(ShareFacts *F, const NodeTable *nt, int p, int si, int yielder) {
  if (p < 0) return;
  const char *pn = nt_str(nt, p, "name");
  if (pn) {
    if (F->nblkp >= F->cblkp) {
      F->cblkp = F->cblkp ? F->cblkp * 2 : 64;
      F->blkp = realloc(F->blkp, sizeof *F->blkp * (size_t)F->cblkp);
      if (!F->blkp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    F->blkp[F->nblkp++] = (struct ShNamed){ pn, si, yielder };
  }
  static const char *const lists[] = { "requireds", "optionals", "posts", "keywords", "lefts", "rights" };
  for (int l = 0; l < 6; l++) {
    int n = 0; const int *ps = nt_arr(nt, p, lists[l], &n);
    for (int i = 0; i < n; i++) sh_blkp_add(F, nt, ps[i], si, 0);
  }
  static const char *const refs[] = { "rest", "keyword_rest", "block" };
  for (int r = 0; r < 3; r++) sh_blkp_add(F, nt, nt_ref(nt, p, refs[r]), si, 0);
}
/* The parameters and block-locals of block (or lambda) b, under scope si;
   with `yielder`, its first required parameter is Enumerator.new's
   yielder. */
static void sh_blkp_block(ShareFacts *F, const NodeTable *nt, int b, int si, int yielder) {
  int bp = nt_ref(nt, b, "parameters");
  if (bp < 0) return;
  int pn = nt_ref(nt, bp, "parameters");
  int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
  if (yielder && nreq >= 1 && nt_kind(nt, reqs[0]) != NK_MultiTargetNode) {
    sh_blkp_add(F, nt, reqs[0], si, 1);
    /* the rest as one would add them, the yielder left out */
    for (int i = 1; i < nreq; i++) sh_blkp_add(F, nt, reqs[i], si, 0);
    static const char *const lists[] = { "optionals", "posts", "keywords" };
    for (int l = 0; l < 3; l++) {
      int n = 0; const int *ps = nt_arr(nt, pn, lists[l], &n);
      for (int i = 0; i < n; i++) sh_blkp_add(F, nt, ps[i], si, 0);
    }
    static const char *const refs[] = { "rest", "keyword_rest", "block" };
    for (int r = 0; r < 3; r++) sh_blkp_add(F, nt, nt_ref(nt, pn, refs[r]), si, 0);
  }
  else sh_blkp_add(F, nt, pn, si, 0);
  int ln = 0; const int *ls = nt_arr(nt, bp, "locals", &ln);
  for (int i = 0; i < ln; i++) sh_blkp_add(F, nt, ls[i], si, 0);
}

/* Is call u a builtin class's constructor whose block's first parameter
   is the yielder (its BSH_NEW_YIELDER row: `Enumerator.new { |y| }`)? */
static int sh_yielder_new(Compiler *c, int u) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, u, "receiver");
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? nt_str(nt, r, "name") : NULL;
  const char *nm = nt_str(nt, u, "name");
  return cn && nm && bop_share_named(BOP_ANY_RECV, nm) == BSH_NEW && comp_class_index(c, cn) < 0 &&
         bop_share_named(BOP_CLASS_NEW, cn) == BSH_NEW_YIELDER;
}

/* PolyLits.bound for an_recv_may_be_string: how a literal block or a
   lambda of scope `scope` binds variable `name`, whether or not its binder
   marked it a block's parameter (a Thread's block does not): 0 none binds
   it, 1 only as Enumerator.new's yielder (sh_builtin_new: no String), 2
   another way. The index is built once per build of the facts. */
static int sh_blk_bound(void *ctx, Compiler *c, int scope, const char *name) {
  ShareFacts *F = ctx;
  const NodeTable *nt = c->nt;
  if (!F->blkp_built) {
    F->blkp_built = 1;
    for (int si = 0; si < c->nscopes; si++)
      for (int u = comp_bcall_first(c, si); u >= 0; u = comp_bcall_next(c, u))
        sh_blkp_block(F, nt, nt_ref(nt, u, "block"), si, sh_yielder_new(c, u));
    NT_FOREACH_KIND(nt, NK_LambdaNode, l) {
      Scope *ls = comp_scope_of(c, l);
      if (ls) sh_blkp_block(F, nt, l, (int)(ls - c->scopes), 0);
    }
    if (F->nblkp) qsort(F->blkp, (size_t)F->nblkp, sizeof *F->blkp, sh_named_cmp);
  }
  int got = 0;
  for (int i = sh_named_first(F->blkp, F->nblkp, name); i < F->nblkp && sp_streq(F->blkp[i].name, name); i++)
    if (F->blkp[i].k == scope) got = F->blkp[i].yielder && got != 2 ? 1 : 2;
  return got;
}

/* An argument of kind t that a String's method could take as a String:
   a String, a value whose class is not known, or a user object (which may
   define to_str); an Integer too where `int_ok` (`<<` and concat append a
   codepoint). */
static int sh_str_arg_kind(TyKind t, int int_ok) {
  return t == TY_STRING || t == TY_STRBUF || t == TY_POLY || t == TY_UNKNOWN || t == TY_VOID ||
         t == TY_CLASS || ty_is_object(t) || (int_ok && (t == TY_INT || t == TY_BIGINT));
}

/* Call n of String mutator `name` that no String can make: an argument a
   String's method takes as a String is of another class, so on a String
   it raises TypeError before it changes anything (`acc.concat(["x"])`,
   `t.insert(0, 5)`, `h[k] = nil`, `o[:k] = v`). Through a receiver that
   may be anything it is another class's method: an Array's or a Hash's. */
static int sh_args_refuse_string(Compiler *c, int n, const char *name) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_SplatNode || k == NK_BlockArgumentNode || k == NK_KeywordHashNode) return 0;
  }
  /* the arguments taken as Strings, from first to last */
  int int_ok;
  int from = str_mutator_str_args(name, argc, &int_ok);
  /* `[]=`'s index is an Integer, a Range, a String or a Regexp */
  if (from < argc && is_element_access(name)) {
    TyKind it = c->ntype[argv[0]];
    if (it == TY_SYMBOL || it == TY_NIL || it == TY_BOOL || ty_is_array(it) || ty_is_hash(it)) return 1;
  }
  for (int i = from; i < argc; i++)
    if (!sh_str_arg_kind(c->ntype[argv[i]], int_ok)) return 1;
  return 0;
}

/* Can the box call n is made on hold a proc or a Method? Not where the
   boxed-receiver walk bounds the receiver (poly_recv_classes: a proc is no
   class it lists), nor in a program that makes none (found once per build) */
static int sh_box_may_be_callable(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (F->any_callable < 0) {
    F->any_callable = 0;
    for (int k = 0; k < nt->count && !F->any_callable; k++) {
      NodeKind kk = nt_kind(nt, k);
      const char *nm = kk == NK_CallNode ? nt_str(nt, k, "name") : NULL;
      int r = nm ? nt_ref(nt, k, "receiver") : -1;
      const char *rn = r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode ? nt_str(nt, r, "name") : NULL;
      F->any_callable = kk == NK_LambdaNode || kk == NK_BlockParameterNode ||
                        (nm && (is_proc_constructor(nm) || is_proc_conversion_name(nm) ||
                                is_object_receiver_handoff(nm) || is_proc_new(rn, nm)));
    }
  }
  int nk;
  return F->any_callable && !poly_recv_classes(c, n, &nk);
}
/* is node n a Lazy: a chain ending in a lazy stage, or a local holding
   one? Asked only of a program that makes one, found once per build */
static int sh_lazy_valued(ShareFacts *F, Compiler *c, int n) {
  if (F->any_lazy < 0) {
    F->any_lazy = 0;
    NT_FOREACH_KIND(c->nt, NK_CallNode, k) {
      const char *kn = nt_str(c->nt, k, "name");
      if (is_lazy_name(kn)) F->any_lazy = 1;
    }
  }
  if (!F->any_lazy) return 0;
  if (n >= 0 && nt_kind(c->nt, n) == NK_LocalVariableReadNode) n = lazy_alias_chain(c, n);
  return n >= 0 && chain_is_lazy_valued(c, n);
}
/* is node n a container's builtin call that answers runs of its elements
   (each_slice, slice_when: a BSH_ITER_SUB row)? A chained materializer
   (`a.slice_when { }.to_a`) can leave that answer untyped. */
static int sh_answers_runs(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (n < 0 || nt_kind(nt, n) != NK_CallNode || sh_has_targets(c, n)) return 0;
  int r = nt_ref(nt, n, "receiver");
  TyKind fam = r < 0 ? TY_VOID : c->ntype[r] == TY_POLY ? BOP_ANY_ARRAY : sh_family(c->ntype[r]);
  if (fam != BOP_ANY_ARRAY && fam != BOP_ANY_HASH) return 0;
  return bop_share_named(fam, nt_str(nt, n, "name")) == BSH_ITER_SUB;
}
/* Does builtin iterator call n (receiver type rt) keep none of its literal
   block's values, so the block's tail is a dropped value? (An Enumerator's
   each runs the iterator it was made by, map's included.) */
static int sh_iter_drops_block(Compiler *c, int n, TyKind rt) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, n, "block");
  const char *name = nt_str(nt, n, "name");
  return name && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && rt != TY_ENUMERATOR && rt != TY_POLY &&
         rt != TY_UNKNOWN && rt != TY_OPENSTRUCT && iter_keeps_no_block_value(sh_family(rt), name, call_plain_argc(c, n));
}

static int sh_builtin_fresh(Compiler *c, int call, int ostruct);
static int sh_call(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, n, "name");
  if (!name) return F->unknown;
  int recv = nt_ref(nt, n, "receiver");
  int blk = nt_ref(nt, n, "block");
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int rv = recv >= 0 ? sh_val(F, c, recv) : -1;
  TyKind rt = recv >= 0 ? c->ntype[recv] : TY_VOID;
  int maybe_str = recv >= 0 && (rt == TY_STRING || rt == TY_STRBUF || rt == TY_POLY || rt == TY_UNKNOWN);
  if (c->share_strings && is_identity_query(name)) {
    int identity = recv < 0 ? sh_self(F, c, sh_method_index(c, n)) : rv;
    if (identity >= 0) F->own[identity] |= SHE_IDENTITY | SHE_COMPARED;
    if (argc == 1 && is_equality_name(name)) {
      int other = sh_val(F, c, argv[0]);
      if (other >= 0) F->own[other] |= SHE_IDENTITY | SHE_COMPARED;
    }
  }

  /* an in-place String mutation of the receiver: through a boxed or an
     untyped receiver, only one a String can make, on a receiver that can
     be a String */
  /* Encoding and frozen-state changes are visible through every alias. */
  if (maybe_str && bop_name_mutates(name, c->share_strings ? 0 : BOP_MUT_LOCAL) &&
      (rt == TY_STRING || rt == TY_STRBUF ||
       (!sh_args_refuse_string(c, n, name) && an_recv_may_be_string(c, recv, &(PolyLits){ sh_blk_bound, F })))) {
    int base = sh_self_chain_base(F, c, recv);
    sh_mark_at(F, rv, SHF_MUT | (sh_holder_read(nt, base) ? 0 : SHF_INDIRECT), n);
    sh_flow(F, SHFL_MUTATE, n, base, -1);
  }

  /* a block passed as a value: a proc or a Method, called from wherever */
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
    int bx = nt_ref(nt, blk, "expression");
    const char *sym = sh_lit_name(nt, bx);
    if (sym && bx >= 0 && nt_kind(nt, bx) == NK_SymbolNode) {
      /* `&:upcase!` runs the name on each element */
      if (bop_name_mutates(sym, c->share_strings ? 0 : BOP_MUT_LOCAL))
        sh_mark_at(F, sh_elem(F, rv), SHF_MUT | SHF_INDIRECT, n);
      sh_dyn_name(F, sym);
    }
    else sh_union(F, sh_elem(F, rv), F->unknown);
  }

  /* `Fiber.yield(v)` in an Enumerator.new block hands v to the Enumerator,
     as its yielder does (sh_builtin_new); several values, as an Array */
  if (F->fgen && F->fgen[n] >= 0) {
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    int e = sh_elem(F, sh_val(F, c, F->fgen[n]));
    if (nv == 1) sh_union(F, e, vals[0]);
    else if (nv > 1) {
      int r = sh_new(F, SHK_VALUE);
      for (int i = 0; i < nv; i++) sh_union(F, sh_elem(F, r), vals[i]);
      sh_union(F, e, r);
    }
    return -1;
  }
  /* the reflective names */
  if (is_send_family(name)) {
    /* Reachability is known before the share walk. A dead send cannot
       make every method's parameters and returns meet UNKNOWN. */
    Scope *sc = comp_scope_of(c, n);
    if (c->share_strings && sc && !sc->reachable) return -1;
    const char *lit = argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL;
    sh_dyn_name(F, lit);
    return sh_unknown_call(F, c, n, blk);
  }
  /* a C function the program binds (ffi_func, a package's native_func):
     it reads a String argument for the length of the call and keeps none */
  if (recv >= 0 && (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode)) {
    const char *mod = nt_str(nt, recv, "name");
    if (mod && (ffi_find_func(c, mod, name) >= 0 || comp_native_find(c, mod, name) >= 0)) return -1;
  }

  /* `Thread.new(a) { |t| }` hands the block its arguments, and a resume
     the program names, its Fiber's block; any other resume hands a Fiber's
     block what the walk does not see. A Thread's value and a Fiber's
     answers go where the walk does not follow. */
  int fb = an_fiber_new_block(c, n);
  if (fb >= 0) {
    sh_block_params(F, c, fb, F->unknown, 1);
    sh_union(F, sh_block_val(F, c, fb), F->unknown);
    return -1;
  }
  int tb = an_thread_arg_block(c, n);
  if (tb >= 0) {
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64), a = -1;
    for (int i = 0; i < nv; i++) a = sh_join(F, a, vals[i]);
    /* the block's first parameter takes them (a new String joins none, so
       the holder is made here, not left to sh_block_params) */
    int bp0 = sh_block_param(nt, tb, 0);
    int bdest = bp0 >= 0 ? sh_local_at(F, c, bp0) : a;
    sh_args_flows(F, c, SHFL_ARG, n, n, bdest);
    sh_block_params(F, c, tb, a, 1);
    /* Thread.new (a constant receiver; Fiber#resume's is the fiber) */
    if (nt_kind(nt, nt_ref(nt, n, "receiver")) == NK_ConstantReadNode) {
      sh_union(F, sh_block_val(F, c, tb), F->unknown);
      return -1;
    }
  }

  /* a native class's method (its binding's `native_share`), on an object
     or, in the package's own Ruby methods, on self */
  int ncid = ty_is_object(rt) ? ty_object_class(rt) : recv < 0 ? sh_ivar_owner(c, n) : -1;
  if (ncid >= 0 && ncid < c->nclasses && c->classes[ncid].is_native_class) {
    int r = sh_native_call(F, c, n, ncid, name, argc, rv, blk, !ty_is_object(rt));
    if (r != -2) return r;
  }
  /* a Lazy (`a.lazy.map { }`, held in a variable or not) has no type of
     its own: its stages and its terminal hand out its source's elements as
     an Enumerator's do, by the container rows. So do grouped runs a
     chained materializer leaves untyped (`a.slice_when { }.to_a`). */
  if (rt == TY_UNKNOWN && (sh_lazy_valued(F, c, recv) || sh_answers_runs(c, recv))) {
    int s = bop_share_named(BOP_ANY_ARRAY, name);
    return s ? sh_builtin(F, c, n, s, rv, blk, 1) : sh_container_default(F, c, n, rv, blk);
  }

  /* ENV's rows, when the program defines no ENV of its own; an
     assignment whose value is taken answers the String it was handed,
     as the store rows' BSH_LAST describes for statements and values. */
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode && is_env_const(nt_str(nt, recv, "name")) &&
      !comp_const(c, "ENV")) {
    int es = bop_share_named(BOP_ENV, name);
    if (es) return sh_builtin(F, c, n, es, -1, blk, 0);
  }
  /* FileTest is a call receiver without a Class type unless reopened.
     User targets still win; only the builtin bypasses the unknown fallback. */
  if (nt_kind(nt, recv) == NK_ConstantReadNode && is_filetest_module_name(nt_str(nt, recv, "name")) &&
      !comp_const(c, nt_str(nt, recv, "name")) && !sh_has_targets(c, n)) {
    int fs = bop_share_named(BOP_FILETEST, name);
    if (fs) return sh_builtin(F, c, n, fs, rv, blk, 0);
  }

  /* a user method */
  int tg[64];
  int ntg = sh_targets_in(F, c, n, tg, 64);
  if (ntg != 0) sh_read_site(F, n);
  if (ntg < 0) return sh_unknown_call(F, c, n, blk);
  /* a bare `new` in a class method builds the class it runs for, or a subclass
     of it: every initialize cplan_initializers names, as for a constant receiver */
  int own_inits[CPT_MAX];
  int nown = ntg == 0 && recv < 0 && bop_share_named(BOP_ANY_RECV, name) == BSH_NEW
             ? cplan_initializers(c, n, own_inits, CPT_MAX) : 0;
  if (nown > 0) {
    for (int i = 0; i < nown; i++) {
      sh_bind(F, c, n, own_inits[i]);
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_block_to_method(F, c, blk, own_inits[i]);
    }
    return -1;
  }
  if (ntg == 0 && recv >= 0 && bop_share_named(BOP_ANY_RECV, name) == BSH_NEW) {
    int r = sh_new_call(F, c, n, recv, blk);
    if (r != -2) return r;
    if (rt == TY_CLASS || rt == TY_POLY || rt == TY_UNKNOWN) {
      /* a class held in a variable: any initialize. Its arguments join
         any_new_pos and any_new_kw, and its block the list every
         initialize takes once after the walk (sh_settle_any_new): binding
         each such call to every initialize was a pass over every method
         per call. */
      F->any_new_used = 1;
      sh_args_flows(F, c, SHFL_ARG, n, n, -1);
      for (int i = 0, pos = 0; i < argc; i++) {
        NodeKind ak = nt_kind(nt, argv[i]);
        if (ak == NK_BlockArgumentNode) continue;
        if (ak == NK_KeywordHashNode) {
          int en = 0; const int *el = nt_arr(nt, argv[i], "elements", &en);
          for (int e = 0; e < en; e++) {
            int ev = nt_kind(nt, el[e]) == NK_AssocNode ? sh_val(F, c, nt_ref(nt, el[e], "value")) : sh_arg_val(F, c, el[e]);
            if (F->any_new_kw < 0) F->any_new_kw = sh_new(F, SHK_VALUE);
            sh_union(F, F->any_new_kw, ev);
          }
          continue;
        }
        /* a splat may land in any position */
        int v = sh_arg_val(F, c, argv[i]);
        for (int j = ak == NK_SplatNode ? 0 : (pos < 16 ? pos : 15); j < 16; j++) {
          if (F->any_new_pos[j] < 0) F->any_new_pos[j] = sh_new(F, SHK_VALUE);
          sh_union(F, F->any_new_pos[j], ak == NK_SplatNode ? sh_elem(F, v) : v);
          if (ak != NK_SplatNode) break;
        }
        pos++;
      }
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
        if (F->nany_blk >= F->cany_blk) {
          F->cany_blk = F->cany_blk ? F->cany_blk * 2 : 8;
          F->any_new_blk = realloc(F->any_new_blk, sizeof(int) * (size_t)F->cany_blk);
        }
        F->any_new_blk[F->nany_blk++] = blk;
      }
      return sh_join(F, -1, rt == TY_CLASS ? -1 : sh_unknown_call(F, c, n, blk));
    }
  }
  if (ntg > 0) {
    int r = -1;
    for (int i = 0; i < ntg; i++) {
      sh_bind(F, c, n, tg[i]);
      r = sh_join(F, r, sh_scope_holder(F, SHK_RET, tg[i]));
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_block_to_method(F, c, blk, tg[i]);
      else if (blk >= 0) sh_block_value_to(F, c, n, blk, tg[i]);
      /* a block passed as a value (`&proc`, `&method(:m)`): what the method
         yields goes where the walk does not follow */
      if (blk >= 0 && nt_kind(nt, blk) != NK_BlockNode && c->scopes[tg[i]].yields) {
        F->blk_dyn[tg[i]] = 1;
        sh_union(F, sh_scope_holder(F, SHK_YIELD, tg[i]), F->unknown);
        sh_union(F, sh_scope_holder(F, SHK_BLKRET, tg[i]), F->unknown);
      }
      /* a method of a String reopen: self is the receiver */
      if (c->share_strings) sh_bind_self(F, c, n, tg[i], rv);
      else if (rv >= 0 && c->scopes[tg[i]].class_id >= 0 &&
          c->scopes[tg[i]].class_id == comp_class_index(c, "String"))
        sh_union(F, rv, F->unknown);
    }
    /* a poly receiver may be a builtin as well */
    if (c->share_strings && rt == TY_EXCEPTION && is_exception_message(name))
      return sh_join(F, r, sh_exc(F));
    if ((rt != TY_POLY && rt != TY_UNKNOWN) || sh_builtin_fresh(c, n, F->ostruct)) return r;
    /* Exception#to_s hands on its stored message beside user returns. */
    if (is_to_s_name(name) && argc == 0 && blk < 0) r = sh_join(F, r, sh_exc(F));
    if (argc == 2 && nt_kind(nt, argv[1]) == NK_KeywordHashNode &&
        bop_share_named(BOP_ANY_ARRAY, name) == BSH_PACK)
      r = sh_join(F, r, sh_builtin(F, c, n, BSH_PACK, rv, blk, 1));
    int share = 0, family = 0, container = 1;
    const BuiltinOp *row = NULL;
    TyKind plan_ret = TY_UNKNOWN;
    if (sh_boxed_builtin_row(F, c, n, &share, &family, &container) &&
        sh_boxed_string_row_composable(F, c, n, ntg, tg, share, family, container,
                                       &row, &plan_ret)) {
      int builtin = sh_builtin(F, c, n, share, rv, blk, container);
      /* This records only the builtin arm's lack of carried identity; r
         still contains every reachable user arm's return holder. */
      if (builtin == -1)
        sh_record_boxed_fresh(F, c, n, share, family, container, row, plan_ret, ntg, tg);
      return sh_join(F, r, builtin);
    }
    return sh_join(F, r, sh_container_default(F, c, n, rv, blk));
  }

  /* an attr reader or writer */
  if (argc <= 1 && (recv < 0 || ty_is_object(rt) || rt == TY_POLY || rt == TY_UNKNOWN)) {
    int writer = 0;
    int iv = sh_attr_ivars(F, c, name, n, &writer);
    if (iv >= 0 && writer == (argc == 1)) {
      int r = iv;
      if (writer) {
        int vals[1];
        int nv = sh_args_vals(F, c, n, vals, 1);
        if (nv == 1) sh_ivar_store(F, c, iv, argc == 1 ? argv[0] : -1, vals[0]);
        if (nv == 1 && argc == 1) sh_flow(F, SHFL_MEMBER, n, argv[0], iv);
        r = nv == 1 ? vals[0] : -1;
      }
      if (rt != TY_POLY && rt != TY_UNKNOWN) return r;
      return sh_join(F, r, sh_container_default(F, c, n, rv, blk));
    }
  }

  if (recv < 0 && is_catch_name(name) && argc <= 1 && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
    if (argc == 1) sh_block_params(F, c, blk, sh_val(F, c, argv[0]), 0);
    int v = sh_block_val(F, c, blk);
    return F->catch_unknown || !F->jseen[n] ? sh_join(F, v, F->unknown) : v;
  }
  if (recv < 0 && is_throw_name(name) && !F->jseen[n]) {
    sh_unknown_call(F, c, n, blk);
    return -1;
  }
  /* an exception's message: what it was handed (sh_exc). raise and fail
     hand it their arguments. */
  /* Exception#to_s with no user target hands on the stored message, as
     #message does (the shared route answers the stored handle). */
  if (c->share_strings && is_to_s_name(name) && argc == 0 && blk < 0 &&
      (rt == TY_EXCEPTION || (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)))))
    return sh_exc(F);
  if (is_exc_message_name(name)) {
    /* Formatted exception text is a new String, not the stored message.
       User targets above retain their own return facts. */
    if (c->share_strings && argc == 0 && blk < 0 && bop_share_named(TY_EXCEPTION, name) == BSH_PURE &&
        (rt == TY_EXCEPTION || (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt))))) return -1;
    return sh_exc(F);
  }
  if (recv < 0 && is_raise_alias(name) && !sh_has_targets(c, n)) {
    sh_exc_args(F, c, n);
    return -1;
  }
  /* A retaining Kernel call that never returns hands its arguments on
     through the exception, as abort's SystemExit does with its message. */
  if (recv < 0 && is_diverging_call(name) && bop_share_named(BOP_KERNEL, name) == BSH_CALL) {
    sh_exc_args(F, c, n);
    return -1;
  }

  /* a builtin */
  if (recv < 0) {
    int s = bop_share_named(BOP_KERNEL, name);
    if (!s) s = bop_share_named(BOP_ANY_RECV, name);
    return s ? sh_builtin(F, c, n, s, rv, blk, 0) : sh_unknown_call(F, c, n, blk);
  }
  if (rt == TY_POLY || rt == TY_UNKNOWN) {
    /* String#to_s returns the receiver; Exception#to_s its message.
       The Array and Hash rows below describe only their fresh text. */
    if (is_to_s_name(name) && argc == 0 && blk < 0) return sh_join(F, rv, sh_exc(F));
    /* a proc or a Method in the box, where it may hold one: called with
       what it is handed; else a container's or a String's element, at any
       depth (the container default) */
    if (bop_share_named(BOP_CALLABLE, name) == BSH_CALL) {
      if (!sh_box_may_be_callable(F, c, n)) return sh_container_default(F, c, n, rv, blk);
      sh_unknown_call(F, c, n, blk);
      return sh_join(F, F->unknown, sh_container_default(F, c, n, rv, blk));
    }
    /* dup and clone: a new object holding the receiver's elements (a
       container's copy shares them; a String's copy is a String of its
       own) */
    if (is_copy_alias(name) && argc == 0 && blk < 0) {
      int r = sh_new(F, SHK_VALUE);
      if (rv >= 0) sh_union(F, sh_elem(F, r), sh_elem(F, rv));
      return r;
    }
    /* any receiver it may be: an Array's or a Hash's row (a String's keeps
       its arguments least), or the container default */
    int s = 0, container = 1;
    if (sh_boxed_builtin_row(F, c, n, &s, NULL, &container))
      return sh_builtin(F, c, n, s, rv, blk, container);
    return sh_container_default(F, c, n, rv, blk);
  }
  if (rt == TY_OPENSTRUCT) return sh_ostruct_call(F, c, n, name, rv, blk);
  /* File's class methods, when no user class is named File (a reopen's own
     methods were taken as user methods above) */
  if (rt == TY_CLASS && nt_kind(nt, recv) == NK_ConstantReadNode && is_file_class_name(nt_str(nt, recv, "name"))) {
    int fs = bop_share_named(BOP_FILE_CLASS, name);
    if (fs) return sh_builtin(F, c, n, fs, rv, blk, 0);
  }
  TyKind fam = sh_family(rt);
  /* an iterator that keeps none of its block's values drops the block's
     own: a call there hands its method's value to no caller. (An
     Enumerator's each runs the iterator it was made by, map's included.) */
  if (sh_iter_drops_block(c, n, rt)) sh_mark_unused(F, nt, nt_ref(nt, blk, "body"), SHU_TAIL);
  int s = bop_share_named(fam, name);
  /* the Strings' answers-self names the face table lists */
  if (!s && fam == TY_STRING && str_self_call(nt, n)) s = BSH_RECV;
  if (!s) s = bop_share_named(BOP_ANY_RECV, name);
  /* a family's "*" row is its default; a container's "*" is Array#*, and
     its default is sh_container_default (a block binds what it holds) */
  if (!s && fam != BOP_ANY_ARRAY && fam != BOP_ANY_HASH) s = bop_share(fam, name);
  if (s) return sh_builtin(F, c, n, s, rv, blk, fam == BOP_ANY_HASH ? 2 : fam == BOP_ANY_ARRAY);
  if (fam == BOP_ANY_ARRAY || fam == BOP_ANY_HASH) return sh_container_default(F, c, n, rv, blk);
  return sh_unknown_call(F, c, n, blk);
}

static int sh_super(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, n, "block");
  int tg[64];
  int ntg = sh_targets_in(F, c, n, tg, 64);
  if (ntg != 0) sh_read_site(F, n);
  if (ntg < 0) return sh_unknown_call(F, c, n, blk);
  int cur = sh_method_index(c, n);
  int r = -1;
  for (int i = 0; i < ntg; i++) {
    Scope *m = &c->scopes[tg[i]];
    sh_bind_self(F, c, n, tg[i], -1);
    if (nt_kind(nt, n) == NK_ForwardingSuperNode && cur >= 0) {
      /* zsuper hands on this method's own parameters, and its block */
      Scope *s = &c->scopes[cur];
      for (int j = 0; j < s->nparams; j++) {
        int a = s->pnames[j] ? sh_local_of(F, c, s, s->pnames[j], n) : -1;
        for (int k = 0; k < m->nparams; k++)
          if (m->nparams != s->nparams || k == j)
            sh_union(F, a, m->pnames[k] ? sh_local_of(F, c, m, m->pnames[k], n) : -1);
      }
      sh_union(F, sh_scope_holder(F, SHK_YIELD, cur), sh_scope_holder(F, SHK_YIELD, tg[i]));
      F->blk_dyn[tg[i]] = 1;
      sh_fwd_edge(F, cur, tg[i]);
      sh_union(F, sh_scope_holder(F, SHK_BLKRET, cur), sh_scope_holder(F, SHK_BLKRET, tg[i]));
    }
    else {
      int vals[64];
      int nv = sh_args_vals(F, c, n, vals, 64);
      /* the values bind to m's parameters (made below for any value): the
         first one names the class, as in sh_bind */
      int dest = -1;
      for (int k = 0; k < m->nparams && nv > 0 && dest < 0; k++) {
        int p = m->pnames[k] ? sh_local_of(F, c, m, m->pnames[k], n) : -1;
        if (p >= 0) dest = k == m->rest_idx || k == m->kwrest_idx ? sh_elem(F, p) : p;
      }
      sh_args_flows(F, c, SHFL_ARG, n, n, dest);
      for (int q = 0; q < nv; q++)
        for (int k = 0; k < m->nparams; k++) {
          int p = m->pnames[k] ? sh_local_of(F, c, m, m->pnames[k], n) : -1;
          sh_union(F, k == m->rest_idx || k == m->kwrest_idx ? sh_elem(F, p) : p, vals[q]);
        }
    }
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_block_to_method(F, c, blk, tg[i]);
    /* a block value, or none: super hands on this method's own block */
    else if (blk >= 0) sh_block_value_to(F, c, n, blk, tg[i]);
    else if (nt_kind(nt, n) == NK_SuperNode) sh_fwd_edge(F, cur, tg[i]);
    r = sh_join(F, r, sh_scope_holder(F, SHK_RET, tg[i]));
  }
  if (ntg == 0) {
    /* Exception#initialize keeps its message */
    Scope *cs = cur >= 0 ? &c->scopes[cur] : NULL;
    if (cs && cs->class_id >= 0 && class_is_exc_subclass(c, cs->class_id)) {
      if (nt_kind(nt, n) == NK_ForwardingSuperNode)
        for (int j = 0; j < cs->nparams; j++)
          sh_union(F, cs->pnames[j] ? sh_local_of(F, c, cs, cs->pnames[j], n) : -1, sh_exc(F));
      else sh_exc_args(F, c, n);
      if (c->share_strings && is_initialize_family(cs->name)) return -1;
    }
    return sh_unknown_call(F, c, n, blk);
  }
  return r;
}

/* `a, b = x, y`: a multiple write of an Array literal with no splat into
   targets with no rest; each target takes its own value. */
static int sh_masgn_plain(const NodeTable *nt, int n) {
  int value = nt_ref(nt, n, "value");
  if (value < 0 || nt_kind(nt, value) != NK_ArrayNode || nt_ref(nt, n, "rest") >= 0) return 0;
  int en = 0; const int *el = nt_arr(nt, value, "elements", &en);
  for (int i = 0; i < en; i++) if (nt_kind(nt, el[i]) == NK_SplatNode) return 0;
  int nr = 0; nt_arr(nt, n, "rights", &nr);
  return nr == 0;
}

static int sh_val_compute(ShareFacts *F, Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableReadNode:
    return sh_local_at(F, c, n);
  case NK_LocalVariableWriteNode: case NK_LocalVariableOrWriteNode:
  case NK_LocalVariableAndWriteNode: case NK_LocalVariableOperatorWriteNode: {
    int l = sh_local_at(F, c, n);
    if (l >= 0) F->own[l] |= SHE_WRITTEN;
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (nt_kind(nt, n) != NK_LocalVariableOperatorWriteNode) {
      sh_union(F, l, v);
      sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    }
    return l;
  }
  case NK_InstanceVariableReadNode:
    return sh_ivar_at(F, c, n);
  case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode:
  case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableOperatorWriteNode: {
    int l = sh_ivar_at(F, c, n);
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (nt_kind(nt, n) != NK_InstanceVariableOperatorWriteNode) {
      sh_ivar_store(F, c, l, nt_ref(nt, n, "value"), v);
      sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    }
    return l;
  }
  case NK_GlobalVariableReadNode:
    return sh_gvar(F, c, nt_str(nt, n, "name"), n);
  case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:
  case NK_GlobalVariableAndWriteNode: case NK_GlobalVariableOperatorWriteNode: {
    int l = sh_gvar(F, c, nt_str(nt, n, "name"), n);
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (nt_kind(nt, n) != NK_GlobalVariableOperatorWriteNode) {
      sh_union(F, l, v);
      sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    }
    return l;
  }
  case NK_ClassVariableReadNode:
    return c->ntype[n] == TY_UNKNOWN || sh_may_hold(c, c->ntype[n])
           ? sh_holder(F, SHK_CVAR, 0, -1, nt_str(nt, n, "name"), n) : -1;
  case NK_ClassVariableWriteNode: case NK_ClassVariableOrWriteNode:
  case NK_ClassVariableAndWriteNode: case NK_ClassVariableOperatorWriteNode: {
    int l = sh_holder(F, SHK_CVAR, 0, -1, nt_str(nt, n, "name"), n);
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (nt_kind(nt, n) != NK_ClassVariableOperatorWriteNode) {
      sh_union(F, l, v);
      sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    }
    return l;
  }
  case NK_ConstantReadNode: case NK_ConstantPathNode:
    return sh_const_read(F, c, n);
  case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode: {
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (v < 0) return -1;
    int l = sh_holder(F, SHK_CONST, 0, -1, nt_str(nt, n, "name"), n);
    sh_union(F, l, v);
    sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    return l;
  }
  case NK_ConstantPathWriteNode: {
    int t = nt_ref(nt, n, "target");
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    if (v < 0 || t < 0) return -1;
    int l = sh_holder(F, SHK_CONST, 0, -1, nt_str(nt, t, "name"), n);
    sh_union(F, l, v);
    sh_flow(F, SHFL_WRITE, n, nt_ref(nt, n, "value"), l);
    return l;
  }
  case NK_ParenthesesNode:
    return sh_stmts_val(F, c, nt_ref(nt, n, "body"));
  case NK_StatementsNode:
    return sh_stmts_val(F, c, n);
  case NK_BeginNode: {
    int r = sh_stmts_val(F, c, nt_ref(nt, n, "statements"));
    for (int rc = nt_ref(nt, n, "rescue_clause"); rc >= 0; rc = nt_ref(nt, rc, "subsequent"))
      r = sh_join(F, r, sh_stmts_val(F, c, nt_ref(nt, rc, "statements")));
    int el = nt_ref(nt, n, "else_clause");
    if (el >= 0) r = sh_join(F, r, sh_stmts_val(F, c, nt_ref(nt, el, "statements")));
    return r;
  }
  case NK_IfNode:
    return sh_join(F, sh_stmts_val(F, c, nt_ref(nt, n, "statements")), sh_val(F, c, nt_ref(nt, n, "subsequent")));
  case NK_UnlessNode:
    return sh_join(F, sh_stmts_val(F, c, nt_ref(nt, n, "statements")), sh_val(F, c, nt_ref(nt, n, "else_clause")));
  case NK_ElseNode:
    return sh_stmts_val(F, c, nt_ref(nt, n, "statements"));
  case NK_CaseNode: case NK_CaseMatchNode: {
    int r = -1;
    int nw = 0; const int *ws = nt_arr(nt, n, "conditions", &nw);
    int pr = nt_kind(nt, n) == NK_CaseMatchNode ? nt_ref(nt, n, "predicate") : -1;
    int sv = pr >= 0 ? sh_val(F, c, pr) : -1;
    TyKind svt = pr >= 0 ? c->ntype[pr] : TY_UNKNOWN;
    for (int i = 0; i < nw; i++) {
      if (nt_kind(nt, n) == NK_CaseMatchNode) sh_pattern(F, c, nt_ref(nt, ws[i], "pattern"), sv, svt);
      r = sh_join(F, r, sh_stmts_val(F, c, nt_ref(nt, ws[i], "statements")));
    }
    return sh_join(F, r, sh_val(F, c, nt_ref(nt, n, "else_clause")));
  }
  case NK_MatchRequiredNode: case NK_MatchPredicateNode: {
    int mv = nt_ref(nt, n, "value");
    sh_pattern(F, c, nt_ref(nt, n, "pattern"), sh_val(F, c, mv), mv >= 0 ? c->ntype[mv] : TY_UNKNOWN);
    return -1;
  }
  case NK_AndNode:
    return sh_val(F, c, nt_ref(nt, n, "right"));
  case NK_OrNode:
    return sh_join(F, sh_val(F, c, nt_ref(nt, n, "left")), sh_val(F, c, nt_ref(nt, n, "right")));
  case NK_RescueModifierNode:
    return sh_join(F, sh_val(F, c, nt_ref(nt, n, "expression")), sh_val(F, c, nt_ref(nt, n, "rescue_expression")));
  case NK_ArrayNode: {
    int r = sh_new(F, SHK_VALUE);
    int en = 0; const int *el = nt_arr(nt, n, "elements", &en);
    for (int i = 0; i < en; i++) {
      int v = sh_arg_val(F, c, el[i]);
      /* a literal a builtin only prints keeps no element for anyone */
      if (F->unused[n] & SHU_PEEK) continue;
      sh_union(F, sh_elem(F, r), v);
      if (nt_kind(nt, el[i]) != NK_SplatNode && !(F->unused[n] & SHU_SPLIT)) sh_flow(F, SHFL_ELEM, n, el[i], sh_elem_peek(F, r));
    }
    return r;
  }
  case NK_HashNode: case NK_KeywordHashNode: {
    int r = sh_new(F, SHK_VALUE);
    int en = 0; const int *el = nt_arr(nt, n, "elements", &en);
    for (int i = 0; i < en; i++) {
      /* a String key is dup'd and frozen as it is stored: only values */
      int v = nt_kind(nt, el[i]) == NK_AssocNode ? sh_val(F, c, nt_ref(nt, el[i], "value"))
                                                 : sh_arg_val(F, c, el[i]);
      if (F->unused[n] & SHU_PEEK) continue;
      sh_union(F, sh_elem(F, r), v);
      if (nt_kind(nt, el[i]) == NK_AssocNode) sh_flow(F, SHFL_ELEM, n, nt_ref(nt, el[i], "value"), sh_elem_peek(F, r));
    }
    return r;
  }
  case NK_RangeNode: {
    int l = sh_val(F, c, nt_ref(nt, n, "left")), rr = sh_val(F, c, nt_ref(nt, n, "right"));
    if (l < 0 && rr < 0) return -1;
    int r = sh_new(F, SHK_VALUE);
    sh_union(F, sh_elem(F, r), l);
    sh_union(F, sh_elem(F, r), rr);
    return r;
  }
  case NK_MultiWriteNode: {
    int value = nt_ref(nt, n, "value");
    int v = sh_val(F, c, value);
    int en = 0; const int *el = value >= 0 && nt_kind(nt, value) == NK_ArrayNode
                                ? nt_arr(nt, value, "elements", &en) : NULL;
    int nl = 0; const int *lefts = nt_arr(nt, n, "lefts", &nl);
    if (sh_masgn_plain(nt, n)) {
      /* `a, b = x, y`: each target takes its own value */
      for (int i = 0; i < nl; i++) {
        int src = i < en ? sh_val(F, c, el[i]) : -1;
        if (i < en) sh_flow(F, SHFL_MULTI, n, el[i], sh_target_holder(F, c, lefts[i]));
        if (nt_kind(nt, lefts[i]) == NK_MultiTargetNode && i < en && nt_kind(nt, el[i]) == NK_ArrayNode)
          sh_targets_of(F, c, lefts[i], src);
        else sh_target(F, c, lefts[i], src);
      }
      return v;
    }
    /* any other value: a target may take it or one of its elements */
    int ev = sh_elem(F, v);
    for (int i = 0; i < nl; i++) { sh_target(F, c, lefts[i], ev); sh_target(F, c, lefts[i], v); }
    int nr2 = 0; const int *rs = nt_arr(nt, n, "rights", &nr2);
    for (int i = 0; i < nr2; i++) sh_target(F, c, rs[i], ev);
    int rest = nt_ref(nt, n, "rest");
    if (rest >= 0) sh_target(F, c, rest, v);
    return v;
  }
  case NK_IndexOperatorWriteNode: case NK_IndexOrWriteNode: case NK_IndexAndWriteNode: {
    int e = sh_elem(F, sh_val(F, c, nt_ref(nt, n, "receiver")));
    sh_union(F, e, sh_val(F, c, nt_ref(nt, n, "value")));
    if (nt_kind(nt, n) != NK_IndexOperatorWriteNode) sh_flow(F, SHFL_ELEM, nt_ref(nt, n, "receiver"), nt_ref(nt, n, "value"), e);
    return e;
  }
  case NK_OperatorWriteNode: case NK_CallOrWriteNode: case NK_CallAndWriteNode: {
    int v = sh_val(F, c, nt_ref(nt, n, "value"));
    const char *rn = nt_str(nt, n, "read_name");
    /* Conditional attribute writes carry the normalized member name. */
    if (nt_kind(nt, n) == NK_CallOrWriteNode || nt_kind(nt, n) == NK_CallAndWriteNode)
      rn = nt_str(nt, n, "name");
    int writer = 0;
    int iv = rn ? sh_attr_ivars(F, c, rn, n, &writer) : -1;
    if (iv < 0) { sh_union(F, v, F->unknown); return F->unknown; }
    sh_ivar_store(F, c, iv, nt_ref(nt, n, "value"), v);
    sh_flow(F, SHFL_MEMBER, n, nt_ref(nt, n, "value"), iv);
    return iv;
  }
  case NK_ForNode:
    sh_target(F, c, nt_ref(nt, n, "index"), sh_elem(F, sh_val(F, c, nt_ref(nt, n, "collection"))));
    return sh_jumped(F, n, -1);
  case NK_YieldNode: {
    int mi = sh_method_index(c, n);
    int y = mi >= 0 ? sh_scope_holder(F, SHK_YIELD, mi) : F->unknown;
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    for (int i = 0; i < nv; i++) sh_union(F, y, vals[i]);
    sh_args_flows(F, c, SHFL_YIELD, n, n, y);
    return mi >= 0 ? sh_scope_holder(F, SHK_BLKRET, mi) : F->unknown;
  }
  case NK_ReturnNode: {
    int mi = sh_method_index(c, n);
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    for (int i = 0; i < nv; i++) {
      if (nv == 1) sh_ret(F, mi, vals[i]);
      else sh_union(F, mi >= 0 ? sh_scope_holder(F, SHK_RET, mi) : -1, vals[i]);
      if (nv > 1 && mi >= 0) F->ret_joined[mi] = 1;
      if (nv > 1 && mi >= 0) sh_union(F, sh_elem(F, sh_scope_holder(F, SHK_RET, mi)), vals[i]);
    }
    return -1;
  }
  case NK_BreakNode: case NK_NextNode: {
    /* the walk handed its value to its target (sh_jumps) */
    if (F->jseen && F->jseen[n]) return -1;
    int vals[64];
    int nv = sh_args_vals(F, c, n, vals, 64);
    for (int i = 0; i < nv; i++) sh_union(F, vals[i], F->unknown);
    return -1;
  }
  case NK_WhileNode: case NK_UntilNode:
    return sh_jumped(F, n, -1);
  case NK_LambdaNode:
    sh_block_params(F, c, n, F->unknown, 1);
    sh_lambda_defaults(F, c, n);
    sh_union(F, sh_block_val(F, c, n), F->unknown);
    return -1;
  case NK_CallNode:
    return sh_jumped(F, n, sh_call(F, c, n));
  case NK_SuperNode: case NK_ForwardingSuperNode:
    return sh_jumped(F, n, sh_super(F, c, n));
  case NK_SelfNode: {
    Scope *s = comp_scope_of(c, n);
    if (c->share_strings) return sh_self(F, c, sh_method_index(c, n));
    int cid = s ? s->class_id : -1;
    return cid >= 0 && cid == comp_class_index(c, "String") ? F->unknown : -1;
  }
  default:
    return -1;
  }
}

static int sh_val(ShareFacts *F, Compiler *c, int n) {
  if (n < 0 || n >= F->nnodes) return -1;
  if (F->nval[n] != -2) return F->nval[n];
  F->nval[n] = -1;
  int v = sh_val_compute(F, c, n);
  /* a value whose type holds no String is none, whatever it flowed
     through (a write's target is still unified above) */
  if (v >= 0 && c->ntype[n] != TY_UNKNOWN && !sh_may_hold(c, c->ntype[n]) &&
      nt_kind(c->nt, n) != NK_StatementsNode && nt_kind(c->nt, n) != NK_ParenthesesNode)
    v = -1;
  if (v >= 0 && nt_kind(c->nt, n) == NK_CallNode && !(F->unused[n] & SHU_STMT)) F->flags[sh_find(F, v)] |= SHF_OUT;
  F->nval[n] = v;
  return v;
}

/* ---- lending ---- */

/* Does the default build pass String parameter lv of method scope mi by
   value (an_byref_param_by_value): an aliased method's, a Struct's, or one
   whose name another method keeps on the value ABI? Asked once per method. */
static int sh_param_by_value(ShareFacts *F, Compiler *c, int mi, const LocalVar *lv) {
  Scope *m = &c->scopes[mi];
  int pi = -1;
  for (int j = 0; j < m->nparams && pi < 0; j++)
    if (m->pnames[j] && sp_streq(m->pnames[j], lv->name)) pi = j;
  if (pi < 0) return 0;
  if (pi >= 32) return an_byref_param_by_value(c, F->byref_elig, mi, pi);
  if (!(F->byval_done[mi] & (1u << pi))) {
    F->byval_done[mi] |= 1u << pi;
    if (an_byref_param_by_value(c, F->byref_elig, mi, pi)) F->byval[mi] |= 1u << pi;
  }
  return (F->byval[mi] >> pi) & 1;
}

/* A parameter its method only reads and mutates: no write, nothing else in
   its class, not captured by a proc that can outlive the call. A String
   parameter the default build passes by value has no slot to lend: a change
   through it reaches the caller only as the shared handle. */
static int sh_lendable(ShareFacts *F, Compiler *c, int p) {
  int hi = F->hidx[p];
  if (hi < 0 || F->h[hi].kind != SHK_LOCAL) return 0;
  Scope *s = &c->scopes[F->h[hi].scope];
  LocalVar *lv = &s->locals[F->h[hi].local];
  if (!lv->is_param || lv->is_block_param || lv->cell_outlives) return 0;
  if (lv->type != TY_STRING && lv->type != TY_STRBUF) return 0;
  if (F->own[p] & SHE_WRITTEN) return 0;
  if (F->own[p] & SHE_IDENTITY) return 0;
  if (lv->type == TY_STRING && sh_param_by_value(F, c, F->h[hi].scope, lv)) return 0;
  int r = sh_find(F, p);
  return F->nmem[r] == 1 && !(F->flags[r] & SHF_UNKNOWN);
}

/* A method's String built in its own locals and returned is handed over:
   the locals die with the call, so the caller's name for it is the only
   one, and the two need not share (`out = +""; out << x; out`, the
   accumulator a builder returns). The return joins the method's value only
   once its class reaches anything else: a parameter, a captured local,
   another method's local or value, an ivar, a container (whose elements
   could be named elsewhere), or what the walk does not follow. A method no
   caller reads (sh_settle_reads) hands its value to nobody: its returns
   join nothing. */
static int sh_return_owned(const ShareFacts *F, int r, int mi) {
  return r >= 0 && F->lsc[r] == mi && F->elem[r] < 0 && !(F->flags[r] & SHF_UNKNOWN);
}
static int sh_settle_rets(ShareFacts *F) {
  int any = 0;
  for (int changed = 1; changed; ) {
    changed = 0;
    for (int i = 0; i < F->nret; i++) {
      if (F->ret_done[i] || (F->mread && !F->mread[F->ret_m[i]])) continue;
      int r = sh_find(F, F->ret_v[i]);
      if (sh_return_owned(F, r, F->ret_m[i])) continue;
      sh_union(F, sh_scope_holder(F, SHK_RET, F->ret_m[i]), F->ret_v[i]);
      F->ret_joined[F->ret_m[i]] = 1;
      F->ret_done[i] = 1;
      changed = any = 1;
    }
  }
  return any;
}

/* Each lookup's key joins the keys of its container's class once that
   class has a default proc taking them (sh_lookup_key). */
static int sh_settle_keys(ShareFacts *F) {
  int any = 0;
  for (int i = 0; i < F->nlk; i++) {
    if (F->lk_done[i] || F->key[sh_find(F, F->lk_c[i])] < 0) continue;
    sh_union(F, F->key[sh_find(F, F->lk_c[i])], F->lk_k[i]);
    F->lk_done[i] = 1;
    any = 1;
  }
  return any;
}

static int sh_str_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A name the runtime may call with no call of its own besides the
   protocols: an operator, an index or a setter (sum's `+`, case's `===`). */
static int sh_operator_name(const char *nm) {
  if (!(nm[0] == '_' || (nm[0] >= 'a' && nm[0] <= 'z') || (nm[0] >= 'A' && nm[0] <= 'Z'))) return 1;
  size_t ln = strlen(nm);
  return nm[ln - 1] == '=';
}

/* A block given to user methods none of whose yields is read drops its
   value: its tail is a value nobody reads (SHU_TAIL). A method that keeps
   its block as a value, or hands it on to a super, reads it; so does a
   builtin a poly receiver may be, and any method when a read yield sits
   in no method the walk names. */
static void sh_settle_block_tails(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  unsigned char *yread = calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
  if (!yread) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  static const NodeKind kinds[] = { NK_YieldNode, NK_SuperNode, NK_ForwardingSuperNode };
  int all = 0;
  for (int k = 0; k < 3; k++)
    NT_FOREACH_KIND(nt, kinds[k], y) {
      if (k == 0 && F->unused[y]) continue;
      int mi = sh_method_index(c, y);
      if (mi >= 0) yread[mi] = 1;
      else all = 1;
    }
  for (int i = 0; i < F->nrsite && !all; i++) {
    int n = F->rsite[i];
    int blk = nt_ref(nt, n, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int recv = nt_kind(nt, n) == NK_CallNode ? nt_ref(nt, n, "receiver") : -1;
    if (recv >= 0 && (c->ntype[recv] == TY_POLY || c->ntype[recv] == TY_UNKNOWN)) continue;
    int tg[64];
    int k = sh_targets(c, n, tg, 64), drop = k > 0;
    for (int j = 0; j < k && drop; j++) {
      Scope *m = &c->scopes[tg[j]];
      drop = !yread[tg[j]] && !m->blk_param && !m->is_lowered_yield && !m->is_proc_form;
    }
    if (drop) sh_mark_unused(F, nt, nt_ref(nt, blk, "body"), SHU_TAIL);
  }
  free(yread);
}

/* Which methods' values a caller may read (F->mread; the dynamic reach in
   sh_build sets its own): each one a call or super site the walk follows
   may reach (cplan_targets) unless the site drops its value (SHU_*); every
   method of a site's name when the plan cannot list them; and every one
   the program can enter with no call node of its own -- a runtime
   protocol or an operator, an extension entry, a proc form. The returns
   of a method no caller reads join no class (sh_settle_rets): a parameter
   it mutates and returns (`def add(line, v) line << v; line end`, called as
   a statement) stays lent. */
static void sh_settle_reads(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  const char **names = NULL;
  int nn = 0, all = 0;
  sh_settle_block_tails(F, c);
  for (int i = 0; i < F->nrsite; i++) {
    int n = F->rsite[i];
    if (F->unused[n]) continue;
    int tg[64];
    int k = sh_targets(c, n, tg, 64);
    for (int j = 0; j < k; j++) F->mread[tg[j]] = 1;
    if (k >= 0) continue;
    int cur = sh_method_index(c, n);
    const char *nm = nt_kind(nt, n) == NK_CallNode ? nt_str(nt, n, "name")
                   : cur >= 0 ? c->scopes[cur].name : NULL;
    if (!nm) { all = 1; break; }
    if (!names && !(names = malloc(sizeof *names * (size_t)F->nrsite))) {
      fprintf(stderr, "spinel: out of memory\n");
      exit(1);
    }
    names[nn++] = nm;
  }
  if (nn > 1) qsort(names, (size_t)nn, sizeof *names, sh_str_cmp);
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (m->def_node < 0 || F->mread[mi]) continue;
    F->mread[mi] = all || !m->name || method_name_implicitly_invoked(m->name) || sh_operator_name(m->name) ||
                   m->is_ext_entry || m->is_proc_form || m->is_lowered_yield ||
                   (nn > 0 && bsearch(&m->name, names, (size_t)nn, sizeof *names, sh_str_cmp));
  }
  free(names);
}

/* Does lend i hand the callee a String something else holds, through a
   value that is no variable's read? The default build passes such a value
   (a memoizing reader's `@s ||= +""`, an element) as a temporary copy, so
   a callee's appends would stop there: a parameter that changes it shares
   with it instead. A value whose class has no slot and meets nothing the
   walk does not follow (a builtin's fresh String, a method's own new local)
   is a temporary nothing else names, and stays lent. */
static int sh_lend_arg_held(ShareFacts *F, int i) {
  if (F->lend_direct[i]) return 0;
  int r = sh_find(F, F->lend_arg[i]);
  return F->nhold[r] > 0 || (F->flags[r] & SHF_UNKNOWN);
}

static void sh_settle_lends(ShareFacts *F, Compiler *c) {
  for (int again = 1; again; ) {
    again = 0;
    for (int changed = 1; changed; ) {
      changed = 0;
      for (int i = 0; i < F->nlend; i++) {
        if (F->lend_done[i] || sh_lendable(F, c, F->lend_par[i])) continue;
        sh_union(F, F->lend_arg[i], F->lend_par[i]);
        F->lend_done[i] = 1;
        changed = 1;
      }
      if (sh_settle_rets(F)) changed = 1;
      if (sh_settle_keys(F)) changed = 1;
    }
    /* a lent parameter's mutation is its argument's; one a held String
       reaches as a temporary joins it, and the classes settle again */
    for (int changed = 1; changed; ) {
      changed = 0;
      for (int i = 0; i < F->nlend; i++) {
        if (F->lend_done[i]) continue;
        int rp = sh_find(F, F->lend_par[i]);
        if (!(F->flags[rp] & SHF_MUT)) continue;
        if (sh_lend_arg_held(F, i)) {
          sh_union(F, F->lend_arg[i], F->lend_par[i]);
          F->lend_done[i] = 1;
          changed = again = 1;
          continue;
        }
        int ra = sh_find(F, F->lend_arg[i]);
        unsigned want = SHF_MUT | (F->lend_direct[i] ? 0 : SHF_INDIRECT);
        if ((F->flags[ra] & want) == want) continue;
        F->flags[ra] |= (unsigned char)want;
        changed = 1;
      }
    }
  }
}

/* ---- the build ---- */

static int sh_root(const ShareFacts *F, int x) {
  while (F->parent[x] != x) x = F->parent[x];
  return x;
}

/* How many holders of each class store a String. A variable, an ivar, a
   global, a class variable and a constant each count. An element slot
   counts only while its container can be reached again -- a container a
   holder keeps, or one UNKNOWN may keep: the elements of an Array literal
   handed to `p` die with the call, and are no second name. A class that is
   its own elements' class (a value that may be a container or one of its
   elements, unified as one) counts no element slot of its own either. */
static void sh_finalize(ShareFacts *F) {
  int n = F->n;
  unsigned char *anchored = calloc((size_t)(n > 0 ? n : 1), 1);
  F->hcount = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
  for (int e = 0; e < n; e++)
    if (F->parent[e] == e)
      anchored[e] = F->nhold[e] - F->nelem[e] > 0 || (F->flags[e] & (SHF_UNKNOWN | SHF_OUT));
  for (int changed = 1; changed; ) {
    changed = 0;
    for (int e = 0; e < n; e++) {
      if (F->kind[e] != SHK_ELEM || F->owner[e] < 0) continue;
      int o = sh_root(F, F->owner[e]), r = sh_root(F, e);
      if (anchored[o] && !anchored[r]) { anchored[r] = 1; changed = 1; }
    }
  }
  /* A String reopening's receiver that its class compares by identity, or
     hands to a consumer that keeps or mutates it, has to be the shared
     object; one that is only returned, yielded or captured is not. */
  unsigned char *compared = calloc((size_t)(n > 0 ? n : 1), 1);
  for (int e = 0; e < n; e++)
    if (F->own[e] & SHE_COMPARED) compared[sh_root(F, e)] = 1;
  for (int e = 0; e < n; e++) {
    if (F->kind[e] == SHK_SELF && compared[sh_root(F, e)])
      F->flags[sh_root(F, e)] |= SHF_IDENTITY;
    if (F->parent[e] == e) F->hcount[e] = F->nhold[e] - F->nelem[e];
  }
  free(compared);
  for (int e = 0; e < n; e++) {
    if (F->kind[e] != SHK_ELEM || F->owner[e] < 0) continue;
    int o = sh_root(F, F->owner[e]), r = sh_root(F, e);
    if (anchored[o] && o != r) F->hcount[r]++;
  }
  F->anchored = anchored;
}

/* A value that is a frozen String (a literal, a freeze, a -@): nothing can
   change it in place, so a name holding only such values needs no handle. */
static int sh_frozen_value(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 1;
  NodeKind k = nt_kind(nt, v);
  if (k == NK_StringNode) return 1;
  if (k == NK_CallNode && bop_share_named(TY_STRING, nt_str(nt, v, "name")) == BSH_FROZEN) return 1;
  return c->ntype[v] != TY_UNKNOWN && !sh_may_hold(c, c->ntype[v]);
}

/* The constants some write gives a value that is no frozen String. */
static void sh_mutable_consts(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  static const NodeKind kinds[] = { NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode,
                                    NK_ConstantPathWriteNode };
  for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
    for (int w = comp_kind_first(c, kinds[i]); w >= 0; w = comp_kind_next(c, w)) {
      if (nt_kind(nt, w) != kinds[i] || sh_frozen_value(c, nt_ref(nt, w, "value"))) continue;
      int t = kinds[i] == NK_ConstantPathWriteNode ? nt_ref(nt, w, "target") : w;
      const char *nm = t >= 0 ? nt_str(nt, t, "name") : NULL;
      if (!nm) continue;
      if (F->nmconst >= F->cmconst) {
        F->cmconst = F->cmconst ? F->cmconst * 2 : 16;
        F->mconst = realloc(F->mconst, sizeof(char *) * (size_t)F->cmconst);
      }
      F->mconst[F->nmconst++] = nm;
    }
}

/* A constant read's holder: one some write makes mutable, or a container
   the program never writes (ARGV); a constant holding a frozen String is
   none. Under --share-strings it also names a frozen String: a mutated
   alias must keep its identity and frozen state on the handle route. */
static int sh_const_read(ShareFacts *F, Compiler *c, int n) {
  const char *nm = nt_str(c->nt, n, "name");
  TyKind t = c->ntype[n];
  if (!nm || !sh_may_hold(c, t)) return -1;
  if (c->share_strings && (t == TY_STRING || t == TY_STRBUF))
    return sh_holder(F, SHK_CONST, 0, -1, nm, n);
  for (int i = 0; i < F->nmconst; i++)
    if (sp_streq(F->mconst[i], nm)) return sh_holder(F, SHK_CONST, 0, -1, nm, n);
  return t == TY_STRING || t == TY_STRBUF ? -1 : sh_holder(F, SHK_CONST, 0, -1, nm, n);
}

/* Every class's initialize (the last def of the name in it) takes what a
   `k.new(...)` on a class held in a variable hands it: the arguments joined
   in any_new reach each of its parameters, and each block it is given. */
static void sh_settle_any_new(ShareFacts *F, Compiler *c) {
  if (!F->any_new_used && F->nany_blk == 0) return;
  const NodeTable *nt = c->nt;
  int *last = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
  for (int k = 0; k < c->nclasses; k++) last[k] = -1;
  for (int k = 0; k < c->nclasses; k++) {
    int s = comp_method_in_class(c, k, "initialize");
    if (s >= 0 && c->scopes[s].def_node >= 0 && !c->scopes[s].is_cmethod) last[k] = s;
  }
  for (int k = 0; k < c->nclasses; k++) {
    int mi = last[k];
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    int pn = nt_ref(nt, m->def_node, "parameters");
    if (F->any_new_used && pn >= 0) {
      int nr = 0, no = 0, npo = 0, nk = 0;
      const int *rq = nt_arr(nt, pn, "requireds", &nr), *op = nt_arr(nt, pn, "optionals", &no);
      const int *po = nt_arr(nt, pn, "posts", &npo), *kw = nt_arr(nt, pn, "keywords", &nk);
      int rest = nt_ref(nt, pn, "rest"), kwr = nt_ref(nt, pn, "keyword_rest");
      /* a required after an optional or a rest takes a position only the
         call's own count decides: each positional parameter then takes
         every position */
      int irregular = npo > 0;
      for (int j = 0; j < nr + no + npo; j++) {
        int pnode = j < nr ? rq[j] : j < nr + no ? op[j - nr] : po[j - nr - no];
        const char *pnm = nt_str(nt, pnode, "name");
        int p = pnm ? sh_local_of(F, c, m, pnm, m->def_node) : -1;
        if (p < 0) continue;
        if (irregular) { for (int b = 0; b < 16; b++) if (F->any_new_pos[b] >= 0) sh_union(F, p, F->any_new_pos[b]); }
        else if (j < 15) { if (F->any_new_pos[j] >= 0) sh_union(F, p, F->any_new_pos[j]); }
        else if (F->any_new_pos[15] >= 0) sh_union(F, p, F->any_new_pos[15]);
      }
      if (rest >= 0 && nt_str(nt, rest, "name")) {
        int p = sh_local_of(F, c, m, nt_str(nt, rest, "name"), m->def_node);
        for (int b = nr + no < 16 ? nr + no : 15; b < 16; b++)
          if (F->any_new_pos[b] >= 0) sh_union(F, sh_elem(F, p), F->any_new_pos[b]);
      }
      for (int j = 0; j < nk; j++) {
        const char *pnm = nt_str(nt, kw[j], "name");
        int p = pnm ? sh_local_of(F, c, m, pnm, m->def_node) : -1;
        if (F->any_new_kw >= 0) sh_union(F, p, F->any_new_kw);
      }
      if (kwr >= 0 && nt_str(nt, kwr, "name") && F->any_new_kw >= 0)
        sh_union(F, sh_elem(F, sh_local_of(F, c, m, nt_str(nt, kwr, "name"), m->def_node)), F->any_new_kw);
    }
    for (int b = 0; b < F->nany_blk; b++) sh_block_to_method(F, c, F->any_new_blk[b], mi);
  }
  free(last);
}

static void sh_free(ShareFacts *F) {
  if (!F) return;
  for (int i = 0; i < F->nh; i++) free((char *)F->h[i].name);
  free(F->parent); free(F->elem); free(F->nhold); free(F->nmem); free(F->nelem); free(F->hidx);
  free(F->owner); free(F->hcount); free(F->anchored); free(F->mconst);
  free(F->mut_n); free(F->mut_v);
  free(F->fl_site); free(F->fl_val); free(F->fl_kind); free(F->fl_dest); free(F->into); free(F->pk); free(F->lam);
  free(F->lsc); free(F->ret_m); free(F->ret_v); free(F->ret_done); free(F->unused); free(F->fresh_cont);
  if (F->own_elig) free(F->byref_elig);
  free(F->byval); free(F->byval_done);
  free(F->rsite); free(F->mread); free(F->ret_joined);
  free(F->mb_m); free(F->mb_b); free(F->mb_start); free(F->mb_blk); free(F->blk_dyn);
  free(F->fw_from); free(F->fw_to); free(F->blk_unk); free(F->fresh_blk); free(F->fresh_call);
  free(F->kind); free(F->flags); free(F->own);
  free(F->h); free(F->helem); free(F->bucket); free(F->hnext); free(F->nval);
  free(F->lend_arg); free(F->lend_par); free(F->lend_node); free(F->lend_direct); free(F->lend_done);
  free(F->dyn); free(F->union_stack);
  free(F->any_new_blk); free(F->attr_r); free(F->attr_w); free(F->blkp);
  free(F->jump); free(F->jseen); free(F->fgen);
  free(F->key); free(F->lk_c); free(F->lk_k); free(F->lk_done);
  for (int i = 0; i < F->nboxed_fresh; i++) {
    free(F->boxed_fresh[i].arg_types);
    free(F->boxed_fresh[i].targets);
  }
  free(F->boxed_fresh);
  free(F->boxed_fresh_index);
  free(F);
}

/* Node n's value is dropped, and so is that of each node whose value n
   forwards as its own (sh_val's arms): a body's last statement, a
   conditional's or a case's branches, a begin's, its rescues' and its
   else's, parentheses, and the operands of `&&`, `||` and a rescue
   modifier. `if c then p a, b end` as a statement keeps neither a nor b. */
static void sh_mark_unused(ShareFacts *F, const NodeTable *nt, int n, unsigned char bit) {
  if (n < 0 || n >= F->nnodes) return;
  F->unused[n] |= bit;
  switch (nt_kind(nt, n)) {
  case NK_StatementsNode: {
    int bn = 0; const int *bv = nt_arr(nt, n, "body", &bn);
    if (bn > 0) sh_mark_unused(F, nt, bv[bn - 1], bit);
    return;
  }
  case NK_ParenthesesNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "body"), bit);
    return;
  case NK_BeginNode: {
    sh_mark_unused(F, nt, nt_ref(nt, n, "statements"), bit);
    for (int rc = nt_ref(nt, n, "rescue_clause"); rc >= 0; rc = nt_ref(nt, rc, "subsequent"))
      sh_mark_unused(F, nt, nt_ref(nt, rc, "statements"), bit);
    int el = nt_ref(nt, n, "else_clause");
    if (el >= 0) sh_mark_unused(F, nt, el, bit);
    return;
  }
  case NK_IfNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "statements"), bit);
    sh_mark_unused(F, nt, nt_ref(nt, n, "subsequent"), bit);
    return;
  case NK_UnlessNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "statements"), bit);
    sh_mark_unused(F, nt, nt_ref(nt, n, "else_clause"), bit);
    return;
  case NK_ElseNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "statements"), bit);
    return;
  case NK_CaseNode: case NK_CaseMatchNode: {
    int nw = 0; const int *ws = nt_arr(nt, n, "conditions", &nw);
    for (int i = 0; i < nw; i++) sh_mark_unused(F, nt, nt_ref(nt, ws[i], "statements"), bit);
    sh_mark_unused(F, nt, nt_ref(nt, n, "else_clause"), bit);
    return;
  }
  case NK_AndNode: case NK_OrNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "left"), bit);
    sh_mark_unused(F, nt, nt_ref(nt, n, "right"), bit);
    return;
  case NK_RescueModifierNode:
    sh_mark_unused(F, nt, nt_ref(nt, n, "expression"), bit);
    sh_mark_unused(F, nt, nt_ref(nt, n, "rescue_expression"), bit);
    return;
  default:
    return;
  }
}

/* the last statement of statements node st drops its value */
static void sh_mark_last_unused(ShareFacts *F, const NodeTable *nt, int st, unsigned char bit) {
  int bn = 0; const int *bv = st >= 0 && nt_kind(nt, st) == NK_StatementsNode ? nt_arr(nt, st, "body", &bn) : NULL;
  if (bn > 0) sh_mark_unused(F, nt, bv[bn - 1], bit);
}

/* ---- break and next ----
   `break v` makes v the value of the call whose block it leaves (`loop {
   break s }` answers s itself), or of the while, until or for loop it
   leaves; `next v` makes v the value its block answers, and a lambda's
   break is its value too. One walk from the root, and from each method
   (one the desugar made may hang from no statement), finds each one's
   target. A jump element per target is made before any value is computed,
   so a call valued before the walk reaches its block's break still joins
   it. A break with no target (outside any block or loop) joins UNKNOWN, as
   does one the walk does not reach (sh_val_compute). */
/* Catch context is lexical only. A method or deferred block starts a new
   context; an unmatched throw meets every catch through UNKNOWN. Symbol
   tags can be compared by value; other tags fall back rather than infer
   object identity. The bounded lookup adds constant work per throw. */
typedef struct ShCatch { int node, tag; struct ShCatch *outer; } ShCatch;
typedef struct {
  int *t, *n, np, cp;
  Compiler *c;
  int gen;   /* the Enumerator.new call whose block the walk is in, or -1 */
  int captures_self;   /* a block keeps the enclosing method's receiver */
  ShCatch *caught;
} ShJumps;

static void sh_jump_add(ShJumps *J, int n, int t) {
  if (J->np >= J->cp) {
    J->cp = J->cp ? J->cp * 2 : 16;
    J->t = realloc(J->t, sizeof(int) * (size_t)J->cp);
    J->n = realloc(J->n, sizeof(int) * (size_t)J->cp);
    if (!J->t || !J->n) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  J->t[J->np] = t;
  J->n[J->np++] = n;
}

static int sh_throw_target(const NodeTable *nt, ShJumps *J, int n) {
  int args = nt_ref(nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1 || nt_kind(nt, argv[0]) != NK_SymbolNode) return -2;
  const char *tag = sh_lit_name(nt, argv[0]);
  int depth = 0;
  for (ShCatch *ct = J->caught; ct && depth < 32; ct = ct->outer, depth++) {
    if (ct->tag < 0 || nt_kind(nt, ct->tag) != NK_SymbolNode) return -2;
    if (sp_streq(tag, sh_lit_name(nt, ct->tag))) return ct->node;
  }
  return -2;
}

static void sh_jump_walk(ShareFacts *F, const NodeTable *nt, ShJumps *J, int n, int brk, int nxt);

/* `Fiber.yield(...)` */
static int sh_fiber_yield(const NodeTable *nt, int n) {
  int r = nt_kind(nt, n) == NK_CallNode ? nt_ref(nt, n, "receiver") : -1;
  return r >= 0 && sp_streq(nt_str(nt, n, "name"), "yield") && nt_kind(nt, r) == NK_ConstantReadNode &&
         sp_streq(nt_str(nt, r, "name"), "Fiber");
}

/* Is node n in a method of a Struct's class? A Struct's own each, over
   members the facts do not follow (they join UNKNOWN), is left to that:
   its generator's Fiber.yield is not followed either. */
static int sh_struct_scope(Compiler *c, int n) {
  Scope *s = comp_scope_of(c, n);
  return s && s->class_id >= 0 && s->class_id < c->nclasses && c->classes[s->class_id].is_struct;
}

static void sh_jump_kids(ShareFacts *F, const NodeTable *nt, ShJumps *J, int n, int brk, int nxt) {
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) sh_jump_walk(F, nt, J, nt_ref_at(nt, n, i), brk, nxt);
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int k = 0; k < m; k++) sh_jump_walk(F, nt, J, ids[k], brk, nxt);
  }
}

/* brk and nxt: the node a break or a next here hands its value to, -1 for
   a value nothing takes (a while loop's next), -2 for none known */
static void sh_jump_walk(ShareFacts *F, const NodeTable *nt, ShJumps *J, int n, int brk, int nxt) {
  if (n < 0 || n >= F->nnodes || F->jseen[n]) return;
  F->jseen[n] = 1;
  ShCatch *saved = J->caught;
  NodeKind k = nt_kind(nt, n);
  int gen = J->gen;
  int captures_self = J->captures_self;
  switch (k) {
  case NK_SelfNode:
    if (J->captures_self) {
      int self = sh_self(F, J->c, sh_method_index(J->c, n));
      if (self >= 0) F->own[self] |= SHE_IDENTITY;
    }
    break;
  case NK_BreakNode: case NK_NextNode: {
    int t = k == NK_BreakNode ? brk : nxt;
    if (t == -1 || nt_ref(nt, n, "arguments") < 0) break;
    sh_jump_add(J, n, t);
    break;
  }
  case NK_WhileNode: case NK_UntilNode:
    brk = n; nxt = -1;
    break;
  case NK_ForNode:
    sh_jump_walk(F, nt, J, nt_ref(nt, n, "collection"), brk, nxt);
    sh_jump_walk(F, nt, J, nt_ref(nt, n, "index"), brk, nxt);
    brk = n; nxt = -1;
    break;
  case NK_LambdaNode:
    J->captures_self = 1;
    J->caught = NULL;
    brk = nxt = n;
    J->gen = -1;
    break;
  case NK_BlockNode:   /* a block no call is walked with */
    J->captures_self = 1;
    J->caught = NULL;
    brk = -2; nxt = n;
    J->gen = -1;
    break;
  case NK_DefNode: case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode:
    J->captures_self = 0;
    J->caught = NULL;
    brk = nxt = -2;
    J->gen = -1;
    break;
  case NK_CallNode: case NK_SuperNode: case NK_ForwardingSuperNode: {
    if (gen >= 0 && sh_fiber_yield(nt, n) && !sh_struct_scope(J->c, n)) {
      if (!F->fgen) {
        F->fgen = malloc(sizeof(int) * (size_t)F->nnodes);
        if (!F->fgen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        for (int i = 0; i < F->nnodes; i++) F->fgen[i] = -1;
      }
      F->fgen[n] = gen;
    }
    const char *name = k == NK_CallNode ? nt_str(nt, n, "name") : NULL;
    int tagged = name && (is_catch_name(name) || is_throw_name(name)) &&
                 nt_ref(nt, n, "receiver") < 0 && !sh_has_targets(J->c, n);
    if (tagged && is_throw_name(name)) {
      int t = sh_throw_target(nt, J, n);
      sh_jump_add(J, n, t);
      if (t < 0) F->catch_unknown = 1;
    }
    int rcv = nt_ref(nt, n, "receiver");
    TyKind rt = rcv >= 0 ? J->c->ntype[rcv] : TY_VOID;
    if (name && (is_send_family(name) || (is_proc_invoke(name) &&
        (rt == TY_PROC || rt == TY_METHOD || rt == TY_POLY || rt == TY_UNKNOWN))))
      F->catch_unknown = 1;
    int blk = nt_ref(nt, n, "block");
    if (blk >= 0 && blk < F->nnodes && !F->jseen[blk] && nt_kind(nt, blk) == NK_BlockNode) {
      int args = nt_ref(nt, n, "arguments");
      int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
      ShCatch ct = { n, argc == 1 ? argv[0] : -1, saved };
      J->caught = tagged && is_catch_name(name) && argc <= 1 ? &ct : NULL;
      F->jseen[blk] = 1;
      /* the block of Enumerator.new, or of a Fiber's own */
      int recv = k == NK_CallNode ? nt_ref(nt, n, "receiver") : -1;
      const char *rn = recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL;
      if (rn && sp_streq(nt_str(nt, n, "name"), "new") && bop_share_named(BOP_CLASS_NEW, rn) == BSH_NEW_YIELDER)
        J->gen = n;
      else if (an_fiber_new_block(J->c, n) >= 0) J->gen = -1;
      J->captures_self = 1;
      sh_jump_kids(F, nt, J, blk, n, blk);
      J->captures_self = captures_self;
      J->gen = gen;
      J->caught = saved;
    }
    break;
  }
  default:
    break;
  }
  sh_jump_kids(F, nt, J, n, brk, nxt);
  J->captures_self = captures_self;
  J->gen = gen;
  J->caught = saved;
}

/* the value a break or a next hands over: its one value, or an Array of
   several (`break a, b`, `next *xs`). A throw hands over its second
   argument, including a Hash passed as keywords; no value means nil. */
static int sh_jump_val(ShareFacts *F, Compiler *c, int n) {
  int args = nt_ref(c->nt, n, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  if (nt_kind(c->nt, n) == NK_CallNode) {
    if (argc == 2 && nt_kind(c->nt, argv[0]) != NK_SplatNode &&
        nt_kind(c->nt, argv[1]) != NK_SplatNode) return sh_val(F, c, argv[1]);
    if (argc <= 1 && (argc == 0 || nt_kind(c->nt, argv[0]) != NK_SplatNode)) return -1;
    sh_unknown_call(F, c, n, -1);
    return F->unknown;
  }
  int vals[64];
  int nv = sh_args_vals(F, c, n, vals, 64);
  if (nv == 1 && argc == 1 && nt_kind(c->nt, argv[0]) != NK_SplatNode) return vals[0];
  int r = sh_new(F, SHK_VALUE);
  for (int i = 0; i < nv; i++) sh_union(F, sh_elem(F, r), vals[i]);
  return r;
}

static void sh_jumps(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  ShJumps J = { NULL, NULL, 0, 0, c, -1, 0, NULL };
  F->jseen = calloc((size_t)(F->nnodes > 0 ? F->nnodes : 1), 1);
  if (!F->jseen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  sh_jump_walk(F, nt, &J, nt->root_id, -2, -2);
  /* and a method the desugar made, whose body hangs from no def */
  for (int mi = 0; mi < c->nscopes; mi++) {
    sh_jump_walk(F, nt, &J, c->scopes[mi].def_node, -2, -2);
    sh_jump_walk(F, nt, &J, c->scopes[mi].body, -2, -2);
  }
  if (J.np == 0) return;
  F->jump = malloc(sizeof(int) * (size_t)F->nnodes);
  if (!F->jump) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < F->nnodes; i++) F->jump[i] = -1;
  for (int i = 0; i < J.np; i++)
    if (J.t[i] >= 0 && F->jump[J.t[i]] < 0) F->jump[J.t[i]] = sh_new(F, SHK_VALUE);
  for (int i = 0; i < J.np; i++)
    sh_union(F, J.t[i] >= 0 ? F->jump[J.t[i]] : F->unknown, sh_jump_val(F, c, J.n[i]));
  free(J.t);
  free(J.n);
}

/* The literal blocks by method (mb_start/mb_blk), a counting sort of the
   pairs sh_block_to_method recorded. */
static void sh_index_blocks(ShareFacts *F, Compiler *c) {
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  F->mb_start = calloc(ns + 1, sizeof(int));
  F->mb_blk = malloc(sizeof(int) * (size_t)(F->nmb > 0 ? F->nmb : 1));
  if (!F->mb_start || !F->mb_blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < F->nmb; i++) F->mb_start[F->mb_m[i] + 1]++;
  for (int m = 0; m < c->nscopes; m++) F->mb_start[m + 1] += F->mb_start[m];
  int *at = malloc(sizeof(int) * ns);
  if (!at) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int m = 0; m < c->nscopes; m++) at[m] = F->mb_start[m];
  for (int i = 0; i < F->nmb; i++) F->mb_blk[at[F->mb_m[i]]++] = F->mb_b[i];
  free(at);
}

/* A container literal handed to Kernel's puts, print, p or pp (no method of
   the program's own name), the last two where their value is dropped (a
   statement, or the tail of a block its builtin iterator drops, which this
   marks first, as sh_call does): the call prints it and keeps none of it,
   so its elements are no class's (SHU_PEEK, before the walk values the
   literal). */
static void sh_mark_printed(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int recv = nt_ref(nt, n, "receiver");
    /* The iterator row rejects calls with no dropped block before the
       user-target lookup; an override still owns every candidate. */
    if (recv >= 0 && sh_iter_drops_block(c, n, c->ntype[recv]) && !sh_has_targets(c, n))
      sh_mark_unused(F, nt, nt_ref(nt, nt_ref(nt, n, "block"), "body"), SHU_TAIL);
  }
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    const char *nm = nt_str(nt, n, "name");
    if (!nm || nt_ref(nt, n, "receiver") >= 0 || nt_ref(nt, n, "block") >= 0) continue;
    int share = bop_share_named(BOP_KERNEL, nm);
    if (share != BSH_PURE && !(share == BSH_ARGS && F->unused[n])) continue;
    if (!is_text_print(nm) && !is_inspect_print(nm)) continue;
    if (sh_has_targets(c, n)) continue;
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    for (int i = 0; i < argc; i++) {
      NodeKind k = nt_kind(nt, argv[i]);
      if (k == NK_ArrayNode || k == NK_HashNode) F->unused[argv[i]] |= SHU_PEEK;
    }
  }
}

/* The index of local node nd's variable among every scope's locals (off:
   each scope's first), -1 for none. */
static int sh_local_idx(Compiler *c, const int *off, int nd) {
  Scope *s = comp_scope_of(c, nd);
  const char *nm = nt_str(c->nt, nd, "name");
  LocalVar *lv = s && nm ? scope_local(s, nm) : NULL;
  return lv ? off[s - c->scopes] + (int)(lv - s->locals) : -1;
}
static int sh_name_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}
/* A local written once, with a lambda literal, read only as the receiver of
   its calls (`f = -> { ... }; f.call(x)`): not a parameter, not captured
   (no read or write of the name at depth > 0 anywhere), never handed on.
   Each call answers that lambda's value: F->lam names the lambda for itself
   and for each call (share_value_fresh reads it). One pass over each node
   kind. */
static void sh_mark_local_lambdas(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  if (comp_kind_first(c, NK_LambdaNode) < 0) return;
  int *off = malloc(sizeof(int) * (size_t)(c->nscopes + 1));
  if (!off) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  off[0] = 0;
  for (int k = 0; k < c->nscopes; k++) off[k + 1] = off[k] + c->scopes[k].nlocals;
  int nl = off[c->nscopes] > 0 ? off[c->nscopes] : 1;
  int *cand = malloc(sizeof(int) * (size_t)nl), *cnt = calloc((size_t)nl, sizeof(int));
  const char **outer = NULL;
  int nouter = 0, couter = 0;
  if (!cand || !cnt) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nl; i++) cand[i] = -1;
  static const NodeKind writes[] = { NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode,
                                     NK_LocalVariableOperatorWriteNode, NK_LocalVariableTargetNode, NK_LocalVariableReadNode };
  for (unsigned w = 0; w < sizeof writes / sizeof writes[0]; w++)
    NT_FOREACH_KIND(nt, writes[w], n) {
      const char *nm = nt_str(nt, n, "name");
      if (nm && nt_int(nt, n, "depth", 0) > 0) {
        if (nouter >= couter) {
          couter = couter ? couter * 2 : 16;
          outer = realloc(outer, sizeof(char *) * (size_t)couter);
          if (!outer) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        outer[nouter++] = nm;
        continue;
      }
      int idx = sh_local_idx(c, off, n);
      if (idx < 0) continue;
      if (writes[w] == NK_LocalVariableReadNode) { cnt[idx]++; continue; }
      int v = writes[w] == NK_LocalVariableWriteNode ? an_unparen(nt, nt_ref(nt, n, "value")) : -1;
      cand[idx] = cand[idx] == -1 && v >= 0 && nt_kind(nt, v) == NK_LambdaNode ? v : -2;
    }
  /* each read must be the receiver of a call of the lambda */
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int r = nt_ref(nt, n, "receiver"), b = nt_ref(nt, n, "block");
    if (r < 0 || nt_kind(nt, r) != NK_LocalVariableReadNode || nt_int(nt, r, "depth", 0) > 0 || b >= 0 ||
        bop_share_named(BOP_CALLABLE, nt_str(nt, n, "name")) != BSH_CALL)
      continue;
    int idx = sh_local_idx(c, off, r);
    if (idx >= 0 && cand[idx] >= 0) cnt[idx]--;
  }
  if (nouter > 1) qsort(outer, (size_t)nouter, sizeof *outer, sh_name_cmp);
  int any = 0;
  for (int k = 0; k < c->nscopes; k++)
    for (int i = 0; i < c->scopes[k].nlocals; i++) {
      int idx = off[k] + i;
      LocalVar *lv = &c->scopes[k].locals[i];
      if (cand[idx] < 0 || cnt[idx] != 0 || lv->is_param || lv->is_block_param || lv->is_cell || lv->cell_outlives ||
          (nouter > 0 && bsearch(&lv->name, outer, (size_t)nouter, sizeof *outer, sh_name_cmp)))
        cand[idx] = -1;
      else any = 1;
    }
  if (any) {
    F->lam = malloc(sizeof(int) * (size_t)(F->nnodes > 0 ? F->nnodes : 1));
    if (!F->lam) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < F->nnodes; i++) F->lam[i] = -1;
    for (int i = 0; i < nl; i++) if (cand[i] >= 0) F->lam[cand[i]] = cand[i];
    NT_FOREACH_KIND(nt, NK_CallNode, n) {
      int r = nt_ref(nt, n, "receiver");
      if (r < 0 || nt_kind(nt, r) != NK_LocalVariableReadNode || nt_int(nt, r, "depth", 0) > 0 ||
          nt_ref(nt, n, "block") >= 0 || bop_share_named(BOP_CALLABLE, nt_str(nt, n, "name")) != BSH_CALL)
        continue;
      int idx = sh_local_idx(c, off, r);
      if (idx >= 0 && cand[idx] >= 0) F->lam[n] = cand[idx];
    }
  }
  free(off); free(cand); free(cnt); free(outer);
}

static ShareFacts *sh_build(Compiler *c, int closed) {
  const NodeTable *nt = c->nt;
  cplan_targets_drop();
  ShareFacts *F = calloc(1, sizeof *F);
  F->closed = closed;
  F->unknown = sh_new(F, SHK_UNKNOWN);
  F->exc = -1;
  for (int ci = 0; ci < c->nclasses; ci++)
    if (c->classes[ci].nsg_readers || c->classes[ci].nsg_writers) {
      /* Singleton accessors can publish values through class-side storage
         that the share holder graph does not connect to those readers. Keep
         the previous boxed-container effects for this program. */
      F->singleton_accessors = 1;
      break;
    }
  NT_FOREACH_KIND(nt, NK_ConstantReadNode, cr)
    if (!F->ostruct && nt_str(nt, cr, "name") && sp_streq(nt_str(nt, cr, "name"), "OpenStruct")) F->ostruct = 1;
  F->flags[F->unknown] = SHF_UNKNOWN;
  F->elem[F->unknown] = F->unknown;
  for (int j = 0; j < 16; j++) F->any_new_pos[j] = -1;
  F->any_new_kw = -1;
  F->any_dec[0] = F->any_dec[1] = -2;
  F->any_lazy = -1;
  F->any_callable = -1;
  F->nnodes = nt->count;
  F->nval = malloc(sizeof(int) * (size_t)(F->nnodes > 0 ? F->nnodes : 1));
  for (int i = 0; i < F->nnodes; i++) F->nval[i] = -2;
  sh_mutable_consts(F, c);
  F->unused = calloc((size_t)(F->nnodes > 0 ? F->nnodes : 1), 1);
  F->fresh_cont = calloc((size_t)(F->nnodes > 0 ? F->nnodes : 1), 1);
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  /* the default build's answer once compute_byref_out_params has given it,
     else this build's own: the types still move until then */
  int kept = c->byref_elig && c->nbyref_elig == c->nscopes;
  F->byref_elig = kept ? c->byref_elig : malloc(ns);
  F->own_elig = !kept;
  F->byval = calloc(ns, sizeof(unsigned));
  F->byval_done = calloc(ns, sizeof(unsigned));
  F->mread = calloc(ns, 1);
  F->ret_joined = calloc(ns, 1);
  F->blk_dyn = calloc(ns, 1);
  F->blk_unk = calloc(ns, 1);
  F->fresh_blk = malloc(ns);
  if (!F->unused || !F->byref_elig || !F->byval || !F->byval_done || !F->mread || !F->ret_joined || !F->blk_dyn ||
      !F->blk_unk || !F->fresh_blk) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  memset(F->fresh_blk, -1, ns);
  if (!kept) an_byref_eligible_scopes(c, F->byref_elig);
  NT_FOREACH_KIND(nt, NK_StatementsNode, st) {
    int bn = 0; const int *bv = nt_arr(nt, st, "body", &bn);
    for (int i = 0; i + 1 < bn; i++) sh_mark_unused(F, nt, bv[i], SHU_STMT);
  }
  /* the program's last statement, and a class body's, answer nothing */
  for (int k = 0; k < 3; k++) {
    if (k == 0) {
      int root = nt->root_id;
      int st = root >= 0 ? nt_ref(nt, root, "statements") : -1;
      sh_mark_last_unused(F, nt, st, SHU_STMT);
      continue;
    }
    NT_FOREACH_KIND(nt, k == 1 ? NK_ClassNode : NK_ModuleNode, pn) sh_mark_last_unused(F, nt, nt_ref(nt, pn, "body"), SHU_STMT);
  }
  NT_FOREACH_KIND(nt, NK_MultiWriteNode, mw)
    if (sh_masgn_plain(nt, mw)) F->unused[nt_ref(nt, mw, "value")] |= SHU_SPLIT;
  sh_mark_printed(F, c);
  sh_mark_local_lambdas(F, c);
  sh_jumps(F, c);
  /* a loop body's last statement drops its value too */
  static const NodeKind loops[] = { NK_WhileNode, NK_UntilNode, NK_ForNode };
  for (int k = 0; k < 3; k++)
    NT_FOREACH_KIND(nt, loops[k], ln) sh_mark_last_unused(F, nt, nt_ref(nt, ln, "statements"), SHU_TAIL);
  for (int n = 0; n < F->nnodes; n++) sh_val(F, c, n);
  sh_settle_any_new(F, c);
  /* each method's value is its body's last, and its defaults bind its
     parameters */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (m->def_node < 0) continue;
    if (m->body >= 0) sh_ret(F, mi, sh_stmts_val(F, c, m->body));
    for (int j = 0; j < m->nparams; j++) {
      if (!m->pdefault || m->pdefault[j] < 0 || !m->pnames[j]) continue;
      int p = sh_local_of(F, c, m, m->pnames[j], m->def_node);
      if (p >= 0) F->own[p] |= SHE_WRITTEN;
      sh_union(F, p, sh_val(F, c, m->pdefault[j]));
      sh_flow(F, SHFL_WRITE, m->def_node, m->pdefault[j], p);
    }
  }
  /* what a Method, a `send` or define_method can reach is called with what
     the walk does not see; so is a method_missing */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (m->def_node < 0 || !m->name) continue;
    int reach = F->dyn_all || sp_streq(m->name, "method_missing");
    for (int k = 0; k < F->ndyn && !reach; k++) reach = sp_streq(F->dyn[k], m->name);
    if (!reach) continue;
    sh_union(F, sh_self(F, c, mi), F->unknown);
    F->mread[mi] = 1;
    for (int j = 0; j < m->nparams; j++)
      sh_union(F, m->pnames[j] ? sh_local_of(F, c, m, m->pnames[j], m->def_node) : -1, F->unknown);
    sh_union(F, sh_scope_holder(F, SHK_RET, mi), F->unknown);
    F->ret_joined[mi] = 1;
    F->blk_unk[mi] = 1;
    if (m->yields) {
      F->blk_dyn[mi] = 1;
      sh_union(F, sh_scope_holder(F, SHK_YIELD, mi), F->unknown);
      sh_union(F, sh_scope_holder(F, SHK_BLKRET, mi), F->unknown);
    }
  }
  /* a Struct's members are read and written through `[]`, `to_a`, `each`
     and the rest, which the walk does not follow; every ivar when an ivar
     is read or written by a runtime name */
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->is_struct && !F->dyn_ivars) continue;
    for (int i = 0; i < ci->nivars; i++) sh_union(F, sh_ivar(F, c, k, ci->ivars[i], -1), F->unknown);
  }
  sh_settle_reads(F, c);
  sh_settle_lends(F, c);
  sh_settle_peeks(F, c);
  sh_finalize(F);
  sh_index_blocks(F, c);
  return F;
}

void share_facts_build(Compiler *c) {
  share_facts_free(c);
  c->share = sh_build(c, 0);
  if (!c->share_strings) return;
  ShareFacts *F = c->share;
  /* Argument flows depend on the parameter's representation. The walk's
     other flows already mark direct identity uses of self. */
  for (int i = 0; i < F->nfl; i++) {
    int v = F->nval[F->fl_val[i]];
    if (F->fl_kind[i] != SHFL_ARG || v < 0 || F->kind[v] != SHK_SELF || (F->own[v] & SHE_COMPARED)) continue;
    int tg[64], n = F->fl_site[i];
    int ntg = sh_targets(c, n, tg, 64), used = ntg <= 0;
    for (int t = 0; t < ntg && !used; t++) {
      Scope *m = &c->scopes[tg[t]];
      int bound = 0;
      for (int j = 0; j < m->nparams; j++) {
        if (arg_layout_param_node(c, m, n, j, NULL) != F->fl_val[i]) continue;
        bound = 1;
        LocalVar *lv = m->pnames[j] ? scope_local(m, m->pnames[j]) : NULL;
        Repr r = repr_of_slot(c, lv);
        if (j == m->rest_idx || j == m->kwrest_idx || r.kind == RK_BOXED || r.kind == RK_STRBUF ||
            r.cell == RC_BYREF || (lv && repr_str_shares(c, share_local_holder(c, tg[t], (int)(lv - m->locals)))))
          used = 1;
      }
      if (!bound) used = 1;
    }
    if (used) F->own[v] |= SHE_IDENTITY | SHE_COMPARED;
  }
  /* Forwarding self needs identity only when the callee does. Settle the
     existing per-method bit over the walk's call sites, including cycles:
     a byte-only cycle stays byte-only; a handle use propagates backwards. */
  for (int changed = 1; changed; ) {
    changed = 0;
    for (int i = 0; i < F->nrsite; i++) {
      int n = F->rsite[i], recv = nt_ref(c->nt, n, "receiver");
      int h = recv >= 0 ? -1 : share_self_holder(c, sh_method_index(c, n));
      int v = recv >= 0 ? F->nval[recv] : h >= 0 ? F->helem[h] : -1;
      if (v < 0 || F->kind[v] != SHK_SELF || (F->own[v] & SHE_COMPARED)) continue;
      int tg[64], ntg = sh_targets(c, n, tg, 64);
      for (int t = 0; t < ntg; t++) {
        if (!repr_self_handle(c, tg[t])) continue;
        F->own[v] |= SHE_IDENTITY | SHE_COMPARED;
        changed = 1;
        break;
      }
    }
  }
}

void share_facts_free(Compiler *c) {
  sh_free(c->share);
  c->share = NULL;
}

/* the same facts with what the walk does not follow left out: the stats'
   count of holders shared only because of UNKNOWN */
ShareFacts *share_facts_build_closed(Compiler *c) { return sh_build(c, 1); }
void share_facts_drop(ShareFacts *F) { sh_free(F); }

int share_holder_count(const Compiler *c) { return c->share ? c->share->nh : 0; }
const ShareHolder *share_holder(const Compiler *c, int h) {
  return c->share && h >= 0 && h < c->share->nh ? &c->share->h[h] : NULL;
}

static int sh_lookup(const ShareFacts *F, int kind, int a, int b, const char *name) {
  if (!F || !F->bucket) return -1;
  unsigned hb = sh_key_hash(kind, a, name) & (unsigned)(F->nbucket - 1);
  for (int i = F->bucket[hb]; i >= 0; i = F->hnext[i]) {
    const ShareHolder *h = &F->h[i];
    if (h->kind != kind) continue;
    if (kind == SHK_SELF) { if (h->scope == a) return i; continue; }
    if (kind == SHK_LOCAL ? (h->scope == a && h->local == b) : (h->cid == a && sp_streq(h->name, name)))
      return i;
  }
  return -1;
}
int share_local_holder(const Compiler *c, int scope, int local) {
  return sh_lookup(c->share, SHK_LOCAL, scope, local, NULL);
}
int share_ivar_holder(const Compiler *c, int cid, const char *name) {
  return name ? sh_lookup(c->share, SHK_IVAR, cid, -1, name) : -1;
}
int share_self_holder(const Compiler *c, int scope) {
  return sh_lookup(c->share, SHK_SELF, scope, -1, NULL);
}
int share_self_used(const Compiler *c, int holder) {
  const ShareFacts *F = c->share;
  return F && holder >= 0 && (F->own[F->helem[holder]] & SHE_IDENTITY) != 0;
}

/* The holders of root r's class that store a String (sh_finalize). */
static int sh_class_holders(const ShareFacts *F, int r) {
  return F->hcount ? F->hcount[r] : F->nhold[r];
}

static int sh_root_of_holder(const ShareFacts *F, int h) {
  int x = F->helem[h];
  while (F->parent[x] != x) x = F->parent[x];
  return x;
}
int share_elem_holder_root(const Compiler *c, int h) {
  const ShareFacts *F = c->share;
  if (!F || h < 0 || h >= F->nh) return -1;
  return F->elem[sh_root_of_holder(F, h)];
}
unsigned share_class_flags(const Compiler *c, int h) {
  const ShareFacts *F = c->share;
  if (!F || h < 0 || h >= F->nh) return 0;
  return F->flags[sh_root_of_holder(F, h)];
}
int share_class_holders(const Compiler *c, int h) {
  const ShareFacts *F = c->share;
  if (!F || h < 0 || h >= F->nh) return 0;
  return sh_class_holders(F, sh_root_of_holder(F, h));
}
/* the facts of an element (a container's elements), not a holder */
unsigned share_elem_flags(const Compiler *c, int e) {
  const ShareFacts *F = c->share;
  if (!F || e < 0 || e >= F->n) return 0;
  while (F->parent[e] != e) e = F->parent[e];
  return F->flags[e];
}
int share_elem_holders(const Compiler *c, int e) {
  const ShareFacts *F = c->share;
  if (!F || e < 0 || e >= F->n) return 0;
  while (F->parent[e] != e) e = F->parent[e];
  return sh_class_holders(F, e);
}
int share_elem_holder(const Compiler *c, int h) { return share_elem_holder_root(c, h); }


static int sh_node_root(const ShareFacts *F, int n, int elems);
/* The same ownership fact that keeps a method's local String returns
   apart from its borrowed returns. This is a return-tail fact: a local
   read elsewhere is still the local's own String, not a fresh value. */
int share_return_owned(const Compiler *c, int n, int mi) {
  return c->share && mi > 0 && sh_return_owned(c->share, sh_node_root(c->share, n, 0), mi);
}
int share_node_fresh(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  if (!F || n < 0 || n >= F->nnodes || F->nval[n] != -1) return 0;
  TyKind t = c->ntype[n];
  return t == TY_STRING || t == TY_STRBUF;
}
int share_node_anchored(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  int r = sh_node_root(F, n, 0);
  return r >= 0 && F->anchored && F->anchored[r];
}
int share_node_elems_share(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  int r = sh_node_root(F, n, 1);
  return r >= 0 && repr_str_class_shares(F->flags[r], sh_class_holders(F, r));
}
int share_node_fresh_elems(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  return F && n >= 0 && n < F->nnodes && F->fresh_cont[n] && share_node_elems_share(c, n);
}

/* The receiver of a retaining iterator call (select, reject, find_all and
   the in-place filters, or partition, whose call is rewritten onto the
   builtin definition __enum_partition__N(array)) with a literal block, or
   -1. */
static int sh_retaining_iter_recv(const Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  if (call < 0 || nt_kind(nt, call) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, call, "name");
  int blk = nt_ref(nt, call, "block"), recv = nt_ref(nt, call, "receiver");
  if (!nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return -1;
  if (recv < 0 && is_enum_partition_def(nm)) {
    int args = nt_ref(nt, call, "arguments"), an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    return an >= 1 ? av[0] : -1;
  }
  return is_retaining_filter(nm) ? recv : -1;
}
/* Is `call` a retaining iterator over a fresh Array of new Strings (a
   builtin's, as share_node_fresh_elems has it, before the rule is asked
   whether the elements share)? Its answer keeps elements the block's
   parameter names. */
int share_iter_fresh_elems(const Compiler *c, int call) {
  int recv = sh_retaining_iter_recv(c, call);
  return recv >= 0 && c->share && recv < c->share->nnodes && c->share->fresh_cont[recv];
}
/* ... and the rule shares those elements: the answer holds the handles the
   block saw, as a local's Array would. */
int share_iter_answers_handles(const Compiler *c, int call) {
  return share_iter_fresh_elems(c, call) && share_node_fresh_elems(c, sh_retaining_iter_recv(c, call));
}

int share_flow_count(const Compiler *c) { return c->share ? c->share->nfl + c->share->nlend : 0; }
int share_flow_at(const Compiler *c, int i, int *site, int *value) {
  const ShareFacts *F = c->share;
  if (i >= F->nfl) {
    /* a lend (sh_bind): a parameter still lent its argument's slot (its
       site the parameter's holder), or, unified, an argument flow
       recorded already */
    i -= F->nfl;
    *site = F->hidx[F->lend_par[i]];
    *value = F->lend_done[i] ? -1 : F->lend_node[i];
    return SHFL_LEND;
  }
  *site = F->fl_site[i];
  *value = F->fl_val[i];
  /* A byte-only self argument has no identity to carry at this boundary. */
  int e = F->nval[*value];
  if (F->fl_kind[i] == SHFL_ARG && e >= 0 && F->kind[e] == SHK_SELF && !(F->own[e] & SHE_IDENTITY))
    *value = -1;
  return F->fl_kind[i];
}
int share_method_blocks(const Compiler *c, int mi, const int **blocks) {
  const ShareFacts *F = c->share;
  *blocks = NULL;
  if (!F || !F->mb_start || mi < 0 || mi >= c->nscopes || F->blk_dyn[mi]) return -1;
  *blocks = F->mb_blk + F->mb_start[mi];
  return F->mb_start[mi + 1] - F->mb_start[mi];
}
/* Native classes can appear in comp_poly_candidates as placeholders even
   when they have no binding. Skip only those empty native rows; preserve
   every Ruby candidate and every real native binding conservatively. The
   non-arity registry query keeps real bindings regardless of signature. */
static int sh_native_placeholder(Compiler *c, const PolyCand *p, const char *name) {
  if (!p->native || p->mi >= 0 || p->rdcls >= 0) return 0;
  if (comp_reader_in_chain(c, p->cls, name, NULL)) return 0;
  return !comp_poly_arm_defines(c, p->cls, name);
}
static int sh_has_method_missing_candidate(Compiler *c) {
  int n = 0;
  const PolyCand *p = comp_poly_candidates(c, "method_missing", &n);
  for (int i = 0; i < n; i++)
    if (!sh_native_placeholder(c, &p[i], "method_missing")) return 1;
  return 0;
}

/* A boxed call's builtin arms answer a value of their own when the
   any-receiver row says so. String's receiver conversions are the
   exception to Object's row: to_s can hand its String back unchanged. */
static int sh_builtin_fresh(Compiler *c, int call, int ostruct) {
  const char *name = nt_str(c->nt, call, "name");
  if (is_receiver_conversion(name)) return 0;
  int share = bop_share_named(BOP_ANY_RECV, name);
  if (share) return share == BSH_PURE;
  /* A name no builtin owns has only the user targets' answers; every
     other receiver raises. The arity tables already record that ownership.
     The dispatch plan must also exclude readers, native bindings and
     catch-all arms such as OpenStruct's member read. Object's public
     method table covers the names the arity tables omit. A user-defined
     method_missing can answer a name with no ordinary target too. */
  if (object_public_method_name(name)) return 0;
  if (comp_method_index(c, "method_missing") >= 0) return 0;
  if (sh_has_method_missing_candidate(c)) return 0;
  int missing = 0;
  comp_cmethod_candidates(c, "method_missing", &missing);
  if (missing) return 0;
  /* With no builtin face or dynamic fields, only the user methods can
     answer. Their freshness is checked by the caller. */
  if (!ostruct && !bop_name_has_reader(name, BOP_READ_NUMERIC | BOP_READ_CONTAINER | BOP_READ_STRING) &&
      !ty_poly_face_owners(name, call_plain_argc(c, call), nt_ref(c->nt, call, "block") >= 0, 1, 1)) {
    int n = 0;
    const PolyCand *p = comp_poly_candidates(c, name, &n);
    /* The walk has no settled dispatch plan yet. Its candidate index
       still exposes aliases the same-named target set can miss. */
    int tg[CPT_MAX], ntg = cplan_targets(c, call, tg, CPT_MAX);
    int user_methods = 0;
    for (int i = 0; i < n; i++) {
      if (p[i].native) {
        if (!sh_native_placeholder(c, &p[i], name)) return 0;
        continue;
      }
      if (p[i].mi < 0 || p[i].rdcls >= 0) return 0;
      user_methods++;
      int found = 0;
      for (int j = 0; j < ntg; j++) if (tg[j] == p[i].mi) { found = 1; break; }
      if (!found) return 0;
    }
    if (user_methods > 0) return 1;
  }
  return 0;
}
/* Is poly arm a of call an IO's read with no buffer to fill (BSH_FILL1's
   row with one argument at most): a new String? The generic default arm
   and its tail read the box as an IO for such a name. */
static int sh_arm_io_read_fresh(Compiler *c, int call, const PolyArm *a) {
  if (call_plain_argc(c, call) >= 2 || bop_share_boxed(TY_IO, nt_str(c->nt, call, "name")) != BSH_FILL1) return 0;
  if (a->kind == PA_TRIAL) return a->key == PA_KEY_TRIAL + PT_GENERIC_TAIL;
  if (a->kind != PA_BUILTIN) return 0;
  int fam = a->key - PA_KEY_BUILTIN;
  return fam == PB_N_IO_READ || fam == PB_IO_SEEK_READ || fam == PB_IO_READ_NB || fam == PB_IO_READPARTIAL ||
         fam == PB_ND_GENERIC;
}
int share_builtin_fresh(Compiler *c, int call) {
  const char *name = nt_str(c->nt, call, "name");
  if (bop_share_named(BOP_ANY_RECV, name) == BSH_PURE && !is_receiver_conversion(name)) return 1;
  /* A mixed boxed call may have a fresh builtin String arm beside user
     returns. Use only the effect recorded after sh_builtin actually returned
     no carried identity, with the same row and full plan proof revalidated. */
  if (sh_boxed_fresh_recorded(c, call)) return 1;
  /* The share walk's builtin surfaces exclude typed-only methods such
     as Thread#value; the settled plan below still checks every arm. */
  if (!sh_builtin_fresh(c, call, !c->share || c->share->ostruct)) return 0;
  /* A scope-name lookup can miss an alias's method. Every returning user
     arm must occur in the target set whose return identities we check. */
  int tg[CPT_MAX], n = cplan_targets(c, call, tg, CPT_MAX);
  const PolyPlan *p = cplan_poly_fresh(c, call);
  for (int i = 0; i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->kind != PA_USER && a->kind != PA_PROC_FORM) continue;
    int found = 0;
    for (int j = 0; j < n; j++) if (tg[j] == a->mi) { found = 1; break; }
    if (!found) return 0;
  }
  for (int i = 0; i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->kind == PA_USER || a->kind == PA_ARITY || sh_arm_io_read_fresh(c, call, a)) continue;
    if (a->kind == PA_TRIAL && (a->key == PA_KEY_TRIAL + PT_DEFAULT0 ||
                               a->key == PA_KEY_TRIAL + PT_DEFAULT_N)) continue;
    return 0;
  }
  return p->n > 0;
}
static int sh_user_call_fresh(Compiler *c, int call, int depth) {
  const ShareFacts *F = c->share;
  if (!F || call < 0 || depth > 8 || nt_kind(c->nt, call) != NK_CallNode) return 0;
  /* A boxed dispatch can take a builtin arm too: its user targets alone
     do not prove freshness (String#to_s can answer its receiver). */
  if (cplan_user_fresh(c, call)->via == UC_POLY && !share_builtin_fresh(c, call)) return 0;
  int tg[64];
  int n = cplan_targets(c, call, tg, 64);
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) {
    if (tg[i] < 0 || tg[i] >= c->nscopes) return 0;
    Scope *m = &c->scopes[tg[i]];
    if (m->ret_param >= 0) {
      if (!share_value_fresh(c, arg_layout_param_source(c, m, call, m->ret_param, NULL), depth + 1)) return 0;
    }
    else if (F->ret_joined[tg[i]] && !m->ret_fresh) return 0;
  }
  return 1;
}
int share_call_fresh(Compiler *c, int call) { return sh_user_call_fresh(c, call, 0); }
/* Does the subtree at n hold a `next` that leaves it (not one in a nested
   block, lambda, method or loop)? With any, also a break or a return. */
static int sh_has_jump_k(const NodeTable *nt, int n, int any) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NextNode || (any && (k == NK_BreakNode || k == NK_ReturnNode))) return 1;
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_WhileNode || k == NK_UntilNode ||
      k == NK_ForNode)
    return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++) if (sh_has_jump_k(nt, nt_ref_at(nt, n, i), any)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (sh_has_jump_k(nt, ids[j], any)) return 1;
  }
  return 0;
}
static int sh_has_next(const NodeTable *nt, int n) { return sh_has_jump_k(nt, n, 0); }
static int sh_has_jump(const NodeTable *nt, int n) { return sh_has_jump_k(nt, n, 1); }

/* Is node r a temporary container (its own expression, held by no name)
   whose elements are new Strings: an Array literal of them, or `map(&:sym)`
   over elements that hold no String (Symbols, numbers), whose answers are
   new? */
static int sh_fresh_elems(Compiler *c, int r, int depth) {
  const NodeTable *nt = c->nt;
  r = an_unparen(nt, r);
  if (r < 0 || depth > 8) return 0;
  if (nt_kind(nt, r) == NK_ArrayNode) {
    int en = 0; const int *el = nt_arr(nt, r, "elements", &en);
    for (int i = 0; i < en; i++) if (!share_value_fresh(c, el[i], depth + 1)) return 0;
    return 1;
  }
  /* The walk already records a builtin's new String elements (split,
     scan): a read of that temporary container owns the element too. */
  if (share_node_fresh_elems(c, r)) return 1;
  if (nt_kind(nt, r) != NK_CallNode || sh_has_targets(c, r)) return 0;
  int blk = nt_ref(nt, r, "block"), recv = nt_ref(nt, r, "receiver");
  const char *nm = nt_str(nt, r, "name");
  if (!nm || recv < 0 || blk < 0 || bop_share_named(BOP_ANY_ARRAY, nm) != BSH_ITER_MAP) return 0;
  /* map with a literal block whose value is a new String, and no next */
  if (nt_kind(nt, blk) == NK_BlockNode) {
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn > 0 && !sh_has_next(nt, body) && share_value_fresh(c, bb[bn - 1], depth + 1);
  }
  if (nt_kind(nt, blk) != NK_BlockArgumentNode || nt_kind(nt, nt_ref(nt, blk, "expression")) != NK_SymbolNode) return 0;
  TyKind at = c->ntype[recv];
  TyKind et = ty_is_array(at) ? ty_array_elem(at) : TY_UNKNOWN;
  return et != TY_UNKNOWN && !sh_may_hold(c, et);
}

/* Does every block method mi can run answer a new String: its literal
   blocks' values (with no next, break or return), and those of the methods
   that hand mi their own block, with no block the facts do not list? Asked
   once per method (fresh_blk); a cycle answers no. */
static int sh_blocks_fresh(Compiler *c, int mi, int depth) {
  ShareFacts *F = c->share;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (F->fresh_blk[mi] >= 0) return F->fresh_blk[mi];
  if (depth > 8 || F->blk_unk[mi] || m->is_lowered_yield || m->is_proc_form || !F->mb_start) return 0;
  F->fresh_blk[mi] = 0;
  int ok = 1;
  for (int i = F->mb_start[mi]; ok && i < F->mb_start[mi + 1]; i++) {
    int body = nt_ref(nt, F->mb_blk[i], "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    ok = bn > 0 && !sh_has_jump(nt, body) && share_value_fresh(c, bb[bn - 1], depth + 1);
  }
  for (int e = 0; ok && e < F->nfw; e++)
    if (F->fw_to[e] == mi) ok = sh_blocks_fresh(c, F->fw_from[e], depth + 1);
  F->fresh_blk[mi] = (signed char)ok;
  return ok;
}

/* Is callable literal k's value (pivs_callable_lit: a lambda's, a proc's
   or a method's tail, with no jump) a new String, or a value no String
   is? */
static int sh_callable_fresh(Compiler *c, int k, int depth) {
  const NodeTable *nt = c->nt;
  const char *un = nt_kind(nt, k) == NK_CallNode ? nt_str(nt, k, "name") : NULL;
  int body;
  if (is_method_ref_name(un)) {
    int a = nt_ref(nt, k, "arguments"), ac = 0;
    const int *av = nt_arr(nt, a, "arguments", &ac);
    int mi = comp_method_index(c, nt_str(nt, av[0], "value"));
    body = mi >= 0 && c->scopes[mi].def_node >= 0 ? nt_ref(nt, c->scopes[mi].def_node, "body") : -1;
  }
  else body = nt_ref(nt, un ? nt_ref(nt, k, "block") : k, "body");
  int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn == 0 || sh_has_jump(nt, body)) return 0;
  TyKind t = c->ntype[bb[bn - 1]];
  return (t != TY_STRING && t != TY_STRBUF && t != TY_POLY && t != TY_UNKNOWN) ||
         share_value_fresh(c, bb[bn - 1], depth + 1);
}
/* Is `call` n, on a callable the boxed-receiver walk bounds to literals
   (pivs_callables), a new String or no String whichever it runs? */
static int sh_call_fresh(Compiler *c, int n, int depth) {
  ShareFacts *F = c->share;
  if (!F->fresh_call) {
    F->fresh_call = malloc((size_t)(F->nnodes > 0 ? F->nnodes : 1));
    if (!F->fresh_call) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(F->fresh_call, -1, (size_t)(F->nnodes > 0 ? F->nnodes : 1));
  }
  if (n >= F->nnodes) return 0;
  if (F->fresh_call[n] >= 0) return F->fresh_call[n];
  F->fresh_call[n] = 0;
  int ks[16];
  int nk = pivs_callables(c, n, ks, 16);
  int ok = nk > 0;
  for (int i = 0; ok && i < nk; i++) ok = sh_callable_fresh(c, ks[i], depth + 1);
  F->fresh_call[n] = (signed char)ok;
  return ok;
}

/* Is arm a of a conditional (its statements, an else or an elsif) a new
   String or nil, or does it leave no value (none, or empty)? */
static int sh_arm_fresh(Compiler *c, int a, int depth) {
  const NodeTable *nt = c->nt;
  a = an_unparen(nt, a);
  if (a < 0) return 1;
  switch (nt_kind(nt, a)) {
  case NK_NilNode: return 1;
  case NK_ElseNode: return sh_arm_fresh(c, nt_ref(nt, a, "statements"), depth);
  case NK_StatementsNode: {
    int n = 0; const int *bb = nt_arr(nt, a, "body", &n);
    return n == 0 || sh_arm_fresh(c, bb[n - 1], depth);
  }
  default: return share_value_fresh(c, a, depth);
  }
}
/* Is node n the frozen String literal itself: a literal of a file whose
   literals are frozen (`fzl`), or a `freeze`, `-@` or `dedup` of one, which
   answer their receiver? Every evaluation answers the one object the
   literal names, so the handle a shared slot takes for it is that
   literal's own (sp_String_literal_handle), never a new one. Adjacent
   literals (`"a" "b"`) and a squiggly heredoc of mixed indents parse as an
   interpolated String that emit_interp folds into one literal: that is one
   too (interp_is_literal_fold). A `+"lit"` is a call that copies, and a
   String with a part to evaluate is built each time: neither is one. */
int share_frozen_literal(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  n = an_unparen(nt, n);
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_StringNode) return nt_int(nt, n, "fzl", 0) != 0;
  if (k == NK_InterpolatedStringNode) return nt_int(nt, n, "fzl", 0) != 0 && interp_is_literal_fold(nt, n);
  if (k != NK_CallNode || nt_ref(nt, n, "arguments") >= 0 || nt_ref(nt, n, "block") >= 0 ||
      bop_share_named(TY_STRING, nt_str(nt, n, "name")) != BSH_FROZEN) return 0;
  return share_frozen_literal(c, nt_ref(nt, n, "receiver"));
}
int share_value_fresh(Compiler *c, int n, int depth) {
  const NodeTable *nt = c->nt;
  n = an_unparen(nt, n);
  if (n < 0 || depth > 8) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NilNode) return 1;
  if (k == NK_StringNode || k == NK_InterpolatedStringNode) return 1;
  if (k == NK_RescueModifierNode)
    return share_value_fresh(c, nt_ref(nt, n, "expression"), depth + 1) &&
           share_value_fresh(c, nt_ref(nt, n, "rescue_expression"), depth + 1);
  /* a conditional each of whose arms is one, or nil */
  if (k == NK_IfNode || k == NK_UnlessNode)
    return sh_arm_fresh(c, nt_ref(nt, n, "statements"), depth + 1) &&
           sh_arm_fresh(c, nt_ref(nt, n, k == NK_IfNode ? "subsequent" : "else_clause"), depth + 1);
  /* a call the walk found answers no String any name holds (a share row's
     new or frozen String) */
  if (k == NK_CallNode && c->share && n < c->share->nnodes && c->share->nval[n] == -1) return 1;
  if (k == NK_YieldNode) {
    int mi = sh_method_index(c, n);
    return mi >= 0 && sh_blocks_fresh(c, mi, depth + 1);
  }
  if (k != NK_CallNode) return 0;
  if (sh_user_call_fresh(c, n, depth)) return 1;
  /* ENV's [] answers a new String each read; to_s, to_str and itself
     answer a new String receiver itself */
  int rcv = nt_ref(nt, n, "receiver");
  const char *cn = nt_str(nt, n, "name");
  if (cn && rcv >= 0 && nt_ref(nt, n, "block") < 0 && !sh_has_targets(c, n)) {
    if (nt_kind(nt, rcv) == NK_ConstantReadNode && is_env_const(nt_str(nt, rcv, "name")) && is_aref_name(cn))
      return 1;
    if (is_receiver_conversion(cn) && call_plain_argc(c, n) == 0)
      return share_value_fresh(c, rcv, depth + 1);
    /* a proc, a lambda or a Method the walk bounds to literals */
    if (is_call_alias(cn) && sh_call_fresh(c, n, depth)) return 1;
  }
  /* a call of a lambda a local holds and only calls, whose value (with no
     next, break or return) is a new String */
  const ShareFacts *F = c->share;
  int lam = F && F->lam && n < F->nnodes ? F->lam[n] : -1;
  if (lam >= 0 && lam != n) {
    int body = nt_ref(nt, lam, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn > 0 && !sh_has_jump(nt, body) && share_value_fresh(c, bb[bn - 1], depth + 1);
  }
  /* an element read of a temporary container of new Strings */
  int recv = nt_ref(nt, n, "receiver");
  const char *nm = nt_str(nt, n, "name");
  int sh = nm && recv >= 0 && !sh_has_targets(c, n) ? bop_share_named(BOP_ANY_ARRAY, nm) : 0;
  return (sh == BSH_ELEM || sh == BSH_ELEM_N) && nt_ref(nt, n, "block") < 0 && sh_fresh_elems(c, recv, depth + 1);
}

unsigned share_node_flags(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  int r = sh_node_root(F, n, 0);
  return r >= 0 ? F->flags[r] : 0;
}
int share_node_peeked(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  return F && n >= 0 && n < F->nnodes && (F->unused[n] & SHU_PEEK);
}
int share_node_transient(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  return F && n >= 0 && n < F->nnodes && (F->unused[n] & (SHU_STMT | SHU_TAIL | SHU_PEEK));
}
int share_node_one_name(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  int r = sh_node_root(F, n, 0);
  return r >= 0 && sh_class_holders(F, r) <= 1 && !(F->flags[r] & (SHF_UNKNOWN | SHF_MULTI));
}
/* A fresh value, or a local's fresh value handed on at its only read. */
int share_value_unobserved(Compiler *c, int n) {
  return share_value_fresh(c, n, 0) || share_node_one_name(c, n) || an_local_read_once(c, n);
}
int share_node_shares(const Compiler *c, int n) {
  const ShareFacts *F = c->share;
  int r = sh_node_root(F, n, 0);
  return r >= 0 && repr_str_class_shares(F->flags[r], sh_class_holders(F, r));
}

/* Does the rule share the class of element e (a flow's destination)? */
static int sh_elem_shares(const ShareFacts *F, int e) {
  int r = sh_root(F, e);
  return repr_str_class_shares(F->flags[r], sh_class_holders(F, r));
}
int share_flow_dest_shares(const Compiler *c, int i) {
  const ShareFacts *F = c->share;
  int d = i >= F->nfl ? F->lend_par[i - F->nfl] : F->fl_dest[i];
  return d < 0 ? -1 : sh_elem_shares(F, d);
}

/* Mark node n and the nodes inside the parentheses around it, which are the
   ones a flow's value or an emitter's value can name. */
static void sh_into_mark(const NodeTable *nt, ShareFacts *F, int n, unsigned char bit) {
  for (; n >= 0 && n < F->nnodes; ) {
    F->into[n] |= bit;
    if (nt_kind(nt, n) != NK_ParenthesesNode) break;
    int pb = nt_ref(nt, n, "body"), pn = 0;
    const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    n = pn == 1 ? pd[0] : -1;
  }
}

/* Build ShareFacts.into from the flows, once the facts are final: a node
   that is the value of a flow into a holder whose class the rule shares
   (its own class may not: a new String joins none), the receiver chain of
   an in-place change, and an element of a container literal whose Strings
   the rule does not hand out. */
static void sh_into_build(const Compiler *c) {
  ShareFacts *F = c->share;
  const NodeTable *nt = c->nt;
  F->into = calloc((size_t)F->nnodes + 1, 1);
  if (!F->into) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0, nf = share_flow_count(c); i < nf; i++) {
    int site, v;
    int kind = share_flow_at(c, i, &site, &v);
    if (v < 0) continue;
    /* (a bang method the program defines on String is a call of its own, not
       the builtin's change in place: the walk records the flow by the name,
       which the seal keeps; this query does not take it for one) */
    if (kind == SHFL_MUTATE && cplan_user((Compiler *)c, site)->mi < 0) {
      /* the call's receiver, and each link of the chain of self-answering
         calls down to the base the flow names (sh_self_chain_base) */
      for (int r = nt_ref(nt, site, "receiver"); r >= 0; ) {
        sh_into_mark(nt, F, r, SHI_MUTATED);
        int u = an_unparen(nt, r);
        if (u == v || u < 0 || nt_kind(nt, u) != NK_CallNode) break;
        r = nt_ref(nt, u, "receiver");
      }
    }
    if (share_flow_dest_shares(c, i) > 0) sh_into_mark(nt, F, v, SHI_INTO);
  }
  static const NodeKind lits[] = { NK_ArrayNode, NK_HashNode };
  for (int k = 0; k < 2; k++)
    NT_FOREACH_KIND(nt, lits[k], lit) {
      if (lit >= F->nnodes || (share_node_elems_share(c, lit) && share_node_anchored(c, lit))) continue;
      int en = 0; const int *el = nt_arr(nt, lit, "elements", &en);
      for (int e = 0; e < en; e++) {
        if (nt_kind(nt, el[e]) != NK_AssocNode) { sh_into_mark(nt, F, el[e], SHI_LITOUT); continue; }
        sh_into_mark(nt, F, nt_ref(nt, el[e], "key"), SHI_LITOUT);
        sh_into_mark(nt, F, nt_ref(nt, el[e], "value"), SHI_LITOUT);
      }
    }
}

int share_node_mutated(Compiler *c, int v) {
  ShareFacts *F = c->share;
  if (!F || v < 0 || v >= F->nnodes) return 0;
  if (!F->into) sh_into_build(c);
  return (F->into[v] & SHI_MUTATED) != 0;
}

/* Does the String (or a box that may hold one) node v evaluates to have to
   be the shared handle where it is emitted -- stored, boxed, mutated -- and
   not a copy of its bytes? The checks, in order: it is reachable, its type
   can hold a String, the rule shares its class (or a flow takes it into a
   holder whose class the rule shares, which a new String's own class never
   is), it is no frozen literal (that is the literal's own handle), it is no
   transient read (except an in-place change's receiver, whatever the use
   marks say), and a container literal's element only if the literal's
   elements are shared and the literal can be reached again. A String no
   other name can see may be copied, but where a box keeps it, the box has
   to be the handle's: SHN_FRESH. */
int share_value_needs_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  ShareFacts *F = c->share;
  if (!c->share_strings || !F || v < 0 || v >= F->nnodes) return SHN_NO;
  NodeKind k = nt_kind(nt, v);
  if (k == NK_NilNode || k == NK_StringNode || k == NK_ArrayNode || k == NK_HashNode ||
      k == NK_RangeNode || k == NK_SymbolNode)
    return SHN_NO;
  TyKind t = c->ntype[v];
  if (t != TY_STRING && t != TY_STRBUF && t != TY_POLY) return SHN_NO;
  Scope *sc = comp_scope_of(c, v);
  if (!sc || !sc->reachable || share_frozen_literal(c, v)) return SHN_NO;
  if (!F->into) sh_into_build(c);
  unsigned char bits = F->into[v];
  if (!share_node_shares(c, v) && !(bits & SHI_INTO)) return SHN_NO;
  if (bits & SHI_LITOUT) return SHN_NO;
  if (share_node_transient(c, v) && !(bits & SHI_MUTATED)) return SHN_NO;
  return strbuf_flow_unseen(c, v) || share_node_fresh(c, v) ? SHN_FRESH : SHN_YES;
}

/* ---- master's route refusals under the flag (share.h) ---- */

ShareRoute share_route(int site, int value, int elems) {
  ShareRoute r;
  memset(&r, 0, sizeof r);
  r.site = site;
  r.value = value;
  r.elems = elems;
  r.to = -1;
  r.carry = -1;
  r.sole = -1;
  return r;
}

static int sh_ivar_owner(Compiler *c, int node);
/* A String method answering its receiver or nil (bop_share_self_answer:
   a bang method, an iterator given a block) called on a slot that holds
   the shared handle -- a local, a global, a constant, a class variable or
   an ivar: its value is that slot's String, or nil. */
static int sh_bang_self_slot(const Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode ||
      !bop_share_self_answer(nt_str(nt, v, "name"), nt_ref(nt, v, "block") >= 0)) return 0;
  int r = nt_ref(nt, v, "receiver");
  if (r < 0) return 0;
  if (nt_kind(nt, r) == NK_LocalVariableReadNode) return repr_of(c, r).kind == RK_STRBUF;
  if (repr_static_share(c, r)) return 1;
  if (nt_kind(nt, r) == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, r, "name");
    int cid = nm ? sh_ivar_owner((Compiler *)c, r) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && repr_of_ivar(c, cid, iv).share;
  }
  return 0;
}
/* Does node n hand over the shared handle (or a box holding it), not a
   copy of its bytes? */
static int sh_carries_handle(const Compiler *c, int n) {
  /* The return route takes the handle its callee publishes, including
     when a borrowed parameter is returned beside a method-owned String. */
  if (repr_call_returns_handle((Compiler *)c, n)) return 1;
  Repr r = repr_of(c, n);
  /* A boxed holder's read keeps what its incoming flows stored there. */
  if (r.kind == RK_BOXED && strbuf_boxed_local((Compiler *)c, n)) return 1;
  /* The boxed unary-plus arm keeps a mutable String's handle and copies
     a frozen one, exactly as the typed value route does. */
  if (r.kind == RK_BOXED && nt_kind(c->nt, n) == NK_CallNode &&
      is_unary_plus(nt_str(c->nt, n, "name")) &&
      cplan_user((Compiler *)c, n)->dispatch == CP_NONE) return 1;
  /* a bang method on a handle local: a write hands over the local's handle
     (emit_strbuf_value) */
  return r.kind == RK_STRBUF || r.strbuf_src != RS_NONE || sh_bang_self_slot(c, n) ||
         /* a boxed variable's read lifted into the handle (poly_strbuf_lift) */
         r.poly_lift ||
         /* A yielding call's result and a receiver-returning expression
            use the same handle routes as their eventual store. */
         strbuf_value_carries((Compiler *)c, n);
}

/* The class of node n's value (with elems, of its elements), or -1. */
static int sh_node_root(const ShareFacts *F, int n, int elems) {
  if (!F || n < 0 || n >= F->nnodes || F->nval[n] < 0) return -1;
  int r = sh_root(F, F->nval[n]);
  if (elems) r = F->elem[r] >= 0 ? sh_root(F, F->elem[r]) : -1;
  return r;
}

/* The class the route reaches: local to_name in the scope of node `to`,
   or node `to`'s value (with to_elems, its elements). */
static int sh_route_to_root(const Compiler *c, const ShareRoute *q) {
  const ShareFacts *F = c->share;
  if (!q->to_name) return sh_node_root(F, q->to, q->to_elems);
  Scope *s = q->to >= 0 ? comp_scope_of((Compiler *)c, q->to) : NULL;
  LocalVar *lv = s ? scope_local(s, q->to_name) : NULL;
  int h = lv ? share_local_holder(c, (int)(s - c->scopes), (int)(lv - s->locals)) : -1;
  return h >= 0 ? sh_root(F, F->helem[h]) : -1;
}

/* The route's answer from the final facts: 1 when it compiles as the rule
   says, 0 when it has to stay refused. The facts must see the route (the
   String and the holder it reaches in one class): a route the walk does
   not follow says nothing about who else can see the copy. Then a class
   the rule does not share has one name, and the copy is unobservable; one
   it shares holds the handle in every holder (seal's holder check), and
   the carrying node hands it along. */
enum { SH_ROUTE_OK, SH_ROUTE_UNSEEN, SH_ROUTE_COPIES };
static int sh_route_why(const Compiler *c, const ShareRoute *q) {
  const ShareFacts *F = c->share;
  /* a route that vouches for a local by its being the only name its String
     has: the final facts have to show a plain local that no other holder,
     no capture and nothing the walk does not follow reaches */
  if (q->sole >= 0) {
    Scope *ss = comp_scope_of((Compiler *)c, q->sole);
    LocalVar *sl = ss ? scope_local(ss, nt_str(c->nt, q->sole, "name")) : NULL;
    if (!sl || sl->is_param || sl->is_block_param || sl->is_cell || sl->cell_outlives ||
        !share_node_one_name(c, q->sole)) return SH_ROUTE_UNSEEN;
  }
  int v = sh_node_root(F, q->value, q->elems);
  /* a value the walk reached and found no String identity in (`"a#{i}"`,
     a builtin's fresh answer) is a String no other name holds: its class
     is the holder it reaches */
  if (v < 0 && F && !q->elems && q->to >= 0 && q->value >= 0 && q->value < F->nnodes &&
      F->nval[q->value] == -1)
    v = sh_route_to_root(c, q);
  /* the same for the elements of a container the walk reached and found
     none in -- a fresh one (`s.split("\n")`) or one whose elements no name
     holds -- where the route's site vouches that they reach only its holder
     (fresh_elems: `each` over it, its value dropped); an iterator that keeps
     the elements it yields (partition, select) names them again */
  if (v < 0 && F && q->elems && q->fresh_elems && q->to >= 0 && q->value >= 0 && q->value < F->nnodes &&
      (F->nval[q->value] == -1 || (F->nval[q->value] >= 0 && F->elem[sh_root(F, F->nval[q->value])] < 0)))
    v = sh_route_to_root(c, q);
  if (v < 0) return SH_ROUTE_UNSEEN;
  if (q->to >= 0 && sh_route_to_root(c, q) != v) return SH_ROUTE_UNSEEN;
  if (!repr_str_class_shares(F->flags[v], sh_class_holders(F, v))) return SH_ROUTE_OK;
  /* a fresh Array's elements bound by an iterator that keeps them: the
     iterator's typed answer holds copies (only a dropped `each` hands each
     one to its block alone, and a retaining iterator over a fresh Array
     answers the handles instead, whatever round recorded the route:
     share_iter_fresh_elems asks the final facts) */
  if (q->elems && !q->fresh_elems && !share_iter_fresh_elems(c, q->site) &&
      share_node_fresh_elems(c, q->value))
    return SH_ROUTE_COPIES;
  if (q->carry == SHARE_CARRY_COPY) return SH_ROUTE_COPIES;
  return q->carry < 0 || sh_carries_handle(c, q->carry) ? SH_ROUTE_OK : SH_ROUTE_COPIES;
}
static int sh_route_ok(const Compiler *c, const ShareRoute *q) {
  return sh_route_why(c, q) == SH_ROUTE_OK;
}

int share_route_defer(Compiler *c, const ShareRoute *q, const char *msg) {
  if (!c->share_strings) return 0;
  if (repr_sealed()) return sh_route_ok(c, q);
  for (int i = 0; i < c->nshare_route; i++) {
    const ShareRoute *r = &c->share_route[i];
    if (r->site == q->site && r->value == q->value && r->elems == q->elems && r->to == q->to &&
        r->to_elems == q->to_elems && r->carry == q->carry && r->fresh_elems == q->fresh_elems &&
        r->sole == q->sole &&
        (r->to_name == q->to_name || (r->to_name && q->to_name && sp_streq(r->to_name, q->to_name))))
      return 1;
  }
  if (c->nshare_route >= c->cshare_route) {
    c->cshare_route = c->cshare_route ? c->cshare_route * 2 : 16;
    c->share_route = realloc(c->share_route, sizeof(ShareRoute) * (size_t)c->cshare_route);
  }
  ShareRoute *r = &c->share_route[c->nshare_route++];
  *r = *q;
  r->msg = strdup(msg);
  return 1;
}

void share_routes_check(Compiler *c) {
  for (int i = 0; i < c->nshare_route; i++) {
    const ShareRoute *r = &c->share_route[i];
    const char *stats = getenv("SPINEL_SHARE_STATS");
    if (stats && stats[0] == '2')
      fprintf(stderr, "share-route: site %d value %d%s to %d%s%s%s carry %d ok=%d\n", r->site, r->value,
              r->elems ? " (elements)" : "", r->to, r->to_elems ? " (elements)" : "",
              r->to_name ? " local " : "", r->to_name ? r->to_name : "", r->carry, sh_route_ok(c, r));
    int why = sh_route_why(c, r);
    if (why == SH_ROUTE_OK) continue;
    /* the refusal says which half the rule could not answer */
    const char *lead = why == SH_ROUTE_UNSEEN
      ? "under --share-strings, the share analysis does not follow this route, so it cannot prove "
        "that no other name sees the copy: "
      : "under --share-strings, this String is shared with another name, and the route does not "
        "carry the shared handle yet: ";
    size_t n = strlen(lead) + strlen(r->msg) + 1;
    char *m = malloc(n);
    if (!m) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    snprintf(m, n, "%s%s", lead, r->msg);
    unsupported_feature(c, r->site, m);
  }
}

void share_routes_free(Compiler *c) {
  for (int i = 0; i < c->nshare_route; i++) free(c->share_route[i].msg);
  free(c->share_route);
  c->share_route = NULL;
  c->nshare_route = c->cshare_route = 0;
}

/* Under SPINEL_SHARE_STATS, a holder of `closed` (a build without UNKNOWN)
   with the same key as holder h of c->share: its class's facts. */
int share_closed_shares(const ShareFacts *F, const ShareHolder *h) {
  if (!F || !h) return 0;
  int i = h->kind == SHK_LOCAL ? sh_lookup(F, SHK_LOCAL, h->scope, h->local, NULL)
        : h->kind == SHK_IVAR ? sh_lookup(F, SHK_IVAR, h->cid, -1, h->name) : -1;
  if (i < 0) return 0;
  int x = F->helem[i];
  while (F->parent[x] != x) x = F->parent[x];
  unsigned f = F->flags[x];
  return (f & SHF_MUT) && (sh_class_holders(F, x) >= 2 || (f & SHF_INDIRECT));
}

/* ---- a Hash key borrows a handle's bytes ----

   A String the rule shares is an sp_String * handle, and a String-keyed
   Hash call handed one as its key read it out through the handle's read
   face, a full copy, before the store copied it again (sp_hash_key_str, the
   copy CRuby makes when it dups and freezes a new key) or the lookup only
   compared it. The key can take the handle's live buffer instead, as
   strbuf_read_raw already lets a builtin accessor do (#5745), when nothing
   runs between the borrow and the call: the receiver and every other
   operand are plain reads. (A read-only parameter and a bound C function
   borrow through master's own passes, mark_param_read_only_operands and
   mark_native_str_operands, which run under the flag too.) */

/* An operand evaluated beside the borrowed argument: a variable read, a
   literal, or scalar arithmetic over those. Nothing in it can change the
   String between the borrow and the call. */
static int sh_plain_operand(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 1;
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode: case NK_GlobalVariableReadNode:
  case NK_ConstantReadNode: case NK_SelfNode: case NK_IntegerNode: case NK_FloatNode:
  case NK_StringNode: case NK_SymbolNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    return 1;
  case NK_CallNode: {
    int recv = nt_ref(nt, n, "receiver");
    TyKind rt = recv >= 0 ? c->ntype[recv] : TY_VOID;
    if ((rt != TY_INT && rt != TY_FLOAT) || nt_ref(nt, n, "block") >= 0) return 0;
    if (sh_has_targets(c, n) || !sh_plain_operand(c, recv)) return 0;
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    for (int i = 0; i < argc; i++)
      if (!sh_plain_operand(c, argv[i])) return 0;
    return 1;
  }
  default:
    return 0;
  }
}

/* Is argument node a a read of a String slot held as the shared handle,
   which reads out through the copying face? */
static int sh_handle_read(Compiler *c, int a) {
  const NodeTable *nt = c->nt;
  if (c->strbuf_box[a] || c->strbuf_handle_demand[a] || c->strbuf_read_raw[a]) return 0;
  if (nt_kind(nt, a) == NK_LocalVariableReadNode) {
    const char *ln = nt_str(nt, a, "name");
    LocalVar *lv = ln ? scope_local(comp_scope_of(c, a), ln) : NULL;
    return lv && lv->type == TY_STRBUF && repr_of_slot(c, lv).kind == RK_STRBUF;
  }
  if (nt_kind(nt, a) == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, a, "name");
    int cid = nm ? sh_ivar_owner(c, a) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF;
  }
  return 0;
}


/* Is call n a builtin String-keyed Hash call whose first argument is a key
   it only compares, or copies to store? */
static int sh_hash_key_call(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, n, "receiver");
  const char *name = nt_str(nt, n, "name");
  if (recv < 0 || !name) return 0;
  TyKind rt = c->ntype[recv];
  if (rt != TY_STR_INT_HASH && rt != TY_STR_STR_HASH && rt != TY_STR_POLY_HASH) return 0;
  if (sh_has_targets(c, n)) return 0;
  if (is_store_alias(name)) return rt != TY_STR_POLY_HASH;
  return is_hash_key_lookup(name);
}


int share_mark_borrows(Compiler *c) {
  if (!c->share_strings) return 0;
  const NodeTable *nt = c->nt;
  int marked = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc == 0 || nt_ref(nt, n, "block") >= 0) continue;
    /* a String-keyed Hash's key: a lookup only compares it, and a store
       copies it (sp_hash_key_str), as CRuby dups and freezes it */
    if (sh_hash_key_call(c, n) && sh_handle_read(c, argv[0]) &&
        sh_plain_operand(c, nt_ref(nt, n, "receiver"))) {
      int plain = 1;
      for (int i = 1; i < argc && plain; i++) plain = sh_plain_operand(c, argv[i]);
      if (plain) { c->strbuf_read_raw[argv[0]] = 1; marked++; }
    }
  }
  c->share_borrows = marked;
  return marked;
}

/* SPINEL_SHARE_STATS=3: each in-place mutation whose class is UNKNOWN's,
   which makes every String that meets what the walk does not follow a
   handle (#6765's never-mutated proof under dynamic calls) */
void share_dump_unknown_mutations(Compiler *c) {
  const ShareFacts *F = c->share;
  if (!F) return;
  int ru = sh_root(F, F->unknown);
  for (int i = 0; i < F->nmut; i++) {
    if (sh_root(F, F->mut_v[i]) != ru) continue;
    int n = F->mut_n[i];
    int recv = nt_ref(c->nt, n, "receiver");
    fprintf(stderr, "share-unknown-mut: line %d `%s` on %s (%s)\n", (int)nt_int(c->nt, n, "node_line", 0),
            nt_str(c->nt, n, "name") ? nt_str(c->nt, n, "name") : "?",
            recv >= 0 ? nt_type(c->nt, recv) : "-", recv >= 0 ? ty_name(c->ntype[recv]) : "-");
  }
}
