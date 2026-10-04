#include "analyze_internal.h"
#include "builtin_ops.h"
#include <stdint.h>
#include <limits.h>

/* Per-iteration memo for the (cid, ivname) full-table-scan narrow helpers
   below. Each is O(nodes); they are queried once per `@ivar[i]` expression,
   so on a large input (the self-compile) the naive form is O(nodes^2). The
   result is stable within one fixpoint iteration, so a generation-stamped
   direct-mapped cache collapses the repeats to O(nodes) per (cid, ivar).
   The generation is bumped once per fixpoint iteration (sp_narrow_memo_bump),
   so a stale entry can only survive within an iteration the fixpoint will
   re-run anyway. */
/* Receiver node whose type the boxed-hash face re-inference pretends is the
   general boxed-key/value hash (see infer_call's last resort). -1 when idle;
   set for the duration of one nested infer_call only. */
/* The type a READ of ivar slot `iv` yields. A shared-mutable string slot
   stores an sp_String handle but reads as a plain String (the read copies),
   so every reader -- attr_reader, a `def m = @x` shim, instance_variable_get
   -- reports the value form, exactly as a direct ivar read does. */
TyKind ivar_value_ty(ClassInfo *ci, int iv) {
  if (iv < 0 || iv >= ci->nivars) return TY_UNKNOWN;
  return ci->ivar_types[iv] == TY_STRBUF ? TY_STRING : ci->ivar_types[iv];
}

/* Codegen unboxes a poly receiver to one concrete kind before re-entering the
   typed emitter (the face table in types.h), and needs the inference to
   answer the same way for the duration -- poking the node's cached type does
   not survive, because anything under the re-emission that asks re-establishes
   it (under a safe-navigation guard the re-emission asks again, and the typed
   emitters then decline the very call they were re-entered to serve). One
   node at a time; -1 clears the pin. */
/* inference's own face pins (the face loop in infer_call), innermost last */
static struct { int node; TyKind kind; } an_pin[16];
static int an_npin;
void an_face_push(int node, TyKind kind) {
  if (an_npin >= 16) { fprintf(stderr, "spinel: internal error: face pins nested too deep\n"); exit(1); }
  an_pin[an_npin].node = node; an_pin[an_npin].kind = kind; an_npin++;
}
void an_face_pop(void) { if (an_npin > 0) an_npin--; }
TyKind face_of(int node) {
  int fn; TyKind fk;
  if (view_face_top(&fn, &fk)) return fn == node ? fk : TY_UNKNOWN;
  if (an_npin > 0) return an_pin[an_npin - 1].node == node ? an_pin[an_npin - 1].kind : TY_UNKNOWN;
  return TY_UNKNOWN;
}
int face_active(void) {
  int fn; TyKind fk;
  return view_face_top(&fn, &fk) || an_npin > 0;
}

/* What a receiver answers universally, as a RAW C scalar, with the type it
   answers. ONE table: the inference below reads it, and so does the
   implicit-self redirect in analyze.c, through an_poly_raw_argc. A second
   hand-kept copy of "what an object answers" goes stale -- the safe-navigation
   emitter carried one and it did (#4070 follow-up: `begin`, `end`, `count` and
   `bytes` missing, `infinite?` with the wrong type), and the redirect's own
   short list did too (#4387). Integer parts retain their rows for the redirect,
   but now answer boxed values to preserve Bigints. */
const struct an_poly_raw_row AN_POLY_RAW[] = {
      { "frozen?", 0, TY_BOOL }, { "nil?", 0, TY_BOOL }, { "zero?", 0, TY_BOOL },
      { "positive?", 0, TY_BOOL }, { "negative?", 0, TY_BOOL },
      { "even?", 0, TY_BOOL }, { "odd?", 0, TY_BOOL }, { "nan?", 0, TY_BOOL },
      { "finite?", 0, TY_BOOL }, { "integer?", 0, TY_BOOL }, { "empty?", 0, TY_BOOL },
      { "eql?", 1, TY_BOOL }, { "equal?", 1, TY_BOOL }, { "instance_of?", 1, TY_BOOL },
      { "bytesize", 0, TY_INT }, { "ord", 0, TY_INT }, { "bit_length", 0, TY_INT },
      /* Integer parts can themselves be boxed Bigints. */
      { "numerator", 0, TY_POLY }, { "denominator", 0, TY_POLY },
      { "to_i", 0, TY_INT }, { "hash", 0, TY_INT }, { "object_id", 0, TY_INT },
      { "begin", 0, TY_POLY }, { "end", 0, TY_POLY }, { "count", 0, TY_INT },   /* a Range's bound: Integer, Float or nil */
      { "to_f", 0, TY_FLOAT }, { "to_r", 0, TY_RATIONAL }, { "to_c", 0, TY_COMPLEX },
      { "class", 0, TY_CLASS }, { "bytes", 0, TY_INT_ARRAY },
      { NULL, 0, TY_UNKNOWN } };

/* The argument count the universal table answers `name` with, or -1 when the
   table does not carry it. */
int an_poly_raw_argc(const char *name) {
  for (int q = 0; AN_POLY_RAW[q].n; q++)
    if (sp_streq(name, AN_POLY_RAW[q].n)) return AN_POLY_RAW[q].ac;
  return -1;
}


/* Per-tree memo for infer_type. The arms of infer_call_inner each re-ask the
   receiver's type, and the receiver of a call is often a call itself: on a
   left-leaning chain (`R0 | R1 | ... | R16`, the OR-of-flags idiom) every
   level multiplied the levels below it and one statement took time
   exponential in the chain's length (#4467). Within ONE top-level infer_type
   tree the answer for a node cannot move -- the tables it is derived from
   only change between trees -- so a generation stamped at each depth-0 entry
   collapses the repeats to one compute per node, and the fixpoint's
   re-inference across trees is untouched because every top-level call opens
   a fresh generation.
   The transient modes are the exception: a pinned face (face_of), the
   builtin-only re-derivation (an_builtin_only), an instance_eval rebind
   (an_ie_class_id) and a `then`-block param pin (g_infer_blv_pin) make the
   SAME node answer differently for their duration, so while any is active
   the memo is neither consulted nor fed. */
static unsigned g_imemo_gen = 0;
static unsigned *g_imemo_stamp = NULL;
static TyKind *g_imemo_val = NULL;
static int g_imemo_cap = 0;
static int g_infer_depth = 0;
static int g_infer_blv_pin = 0;

#define SP_NMEMO_SZ 16384
static unsigned g_narrow_gen = 1;
static struct { unsigned gen; long key; signed char val; } g_nmemo[SP_NMEMO_SZ];
/* A user class owns this name as a method, a class method, or an attr / Struct
   / Data reader. The poly String shortcuts must decline to the general dispatch
   when it does, or a reader named after a String method answers that method's
   result on the receiver's #inspect (#3364). Mirrors user_defines_or_reads on
   the codegen side. */
/* `exception: false` on a *_nonblock call: the would-block answer is the
   :wait_readable / :wait_writable marker rather than a raise, so the result is
   boxed (a symbol or the value). */
static int an_nonblock_no_exception(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, id, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (!av || an == 0) return 0;
  const char *lty = nt_type(nt, av[an - 1]);
  if (!lty || !sp_streq(lty, "KeywordHashNode")) return 0;
  int e = kwh_lookup(nt, av[an - 1], "exception");
  return e >= 0 && nt_type(nt, e) && sp_streq(nt_type(nt, e), "FalseNode");
}

/* Set while re-deriving a poly-receiver call's type as if no user class owned
   the name: the answer the builtin surface alone would give. A union whose
   receiver provably carries a container needs both that answer and the user
   one, and codegen needs to know the builtin shape to emit its arm (#3459). */
static int an_builtin_only = 0;
int an_builtin_only_p(void) { return an_builtin_only; }
/* What call `id` would be typed if no user class owned the name: the
   builtin answer alone. Nothing is cached and no node's type is written
   under the flag (see infer_type), so this is safe to ask after analysis --
   the why-chain asks it of a send on a poly receiver, to tell a result the
   builtin already makes untyped from one a user candidate's return made so. */
/* The receiver of the call being asked. The question is about that call
   alone: the receiver is what it is, and re-derived under the flag a
   receiver that is itself a poly call (`newest(subs).attributes`) lost its
   user answers and left the builtin one unknown (#5116). */
static int an_bo_recv = -1;
TyKind an_builtin_answer(Compiler *c, int id) {
  int was = an_builtin_only, was_recv = an_bo_recv;
  an_builtin_only = 1;
  an_bo_recv = nt_kind(c->nt, id) == NK_CallNode ? nt_ref(c->nt, id, "receiver") : -1;
  TyKind t = infer_call(c, id);
  an_builtin_only = was;
  an_bo_recv = was_recv;
  return t;
}

/* Name-keyed answer memo, the same shape (and the same staleness argument) as
   udm_ in an_user_defines_method: the question below crosses every class with
   the ancestor chain, and infer_call asks it for every poly-receiver call node
   on every fixpoint iteration -- on a 40k-line program that is hundreds of
   millions of chain walks for a few thousand distinct answers. The answer only
   moves when scope or class shape does, which the (gen, nscopes, nclasses)
   stamp tracks. */
static char **udr_names = NULL;
static signed char *udr_ans = NULL;
static int *udr_next = NULL, *udr_head = NULL;
static int udr_n = 0, udr_cap = 0;
static unsigned udr_gen = (unsigned)-1;
static int udr_nscopes = -1, udr_nclasses = -1;

/* Does the program spawn a Thread anywhere (`Thread.new` / `.start` /
   `.fork`)? Then a boxed receiver's `join` may be a Thread's, whose answer is
   the thread itself, not the arrays' joined String (#4466 gave the dispatch
   the arm; the call's type still said String, so the thread came back as
   "" and `t.join.value` had nothing to ask). Cached per node table. */
int an_program_spawns_threads(Compiler *c) {
  static const NodeTable *cached_nt = NULL;
  static int cached_count = -1, cached = 0;
  const NodeTable *nt = c->nt;
  if (cached_nt == nt && cached_count == nt->count) return cached;
  cached_nt = nt; cached_count = nt->count; cached = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "new") && !sp_streq(nm, "start") && !sp_streq(nm, "fork"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *cn = nt_str(nt, recv, "name");
    if (cn && sp_streq(cn, "Thread")) { cached = 1; break; }
  }
  return cached;
}

/* Does a class other than a native one read `name` as an attribute: an
   attr_reader, a Struct/Data member, or an alias of one? Asked the way the
   emitter asks (poly_name_user_claimed with readers, comp_reader_in_chain),
   so inference and emission take the same arm. */
static int an_class_reads_name(Compiler *c, const char *name) {
  if (an_builtin_only) return 0;   /* deriving the builtin-only answer (#3459) */
  /* asked per call site: one answer per name while the class and scope
     tables stand still (the memo an_user_defines_or_reads keeps, for the
     one name this is asked about) */
  static char memo_name[64];
  static unsigned memo_gen;
  static int memo_ncls = -1, memo_nsc = -1, memo_ans;
  int memo = comp_scope_index_is_frozen() && strlen(name) < sizeof memo_name;
  if (memo && memo_ncls == c->nclasses && memo_nsc == c->nscopes &&
      memo_gen == comp_scope_index_gen() && sp_streq(memo_name, name)) return memo_ans;
  int ans = 0;
  for (int k = 0; k < c->nclasses && !ans; k++)
    if (!c->classes[k].is_native_class && comp_reader_in_chain(c, k, name, NULL)) ans = 1;
  if (memo) {
    snprintf(memo_name, sizeof memo_name, "%s", name);
    memo_gen = comp_scope_index_gen(); memo_ncls = c->nclasses; memo_nsc = c->nscopes;
    memo_ans = ans;
  }
  return ans;
}

/* Does the program define a Struct or a Data class? */
static int an_program_has_struct(Compiler *c) {
  static int memo_ncls = -1, memo_ans;
  if (memo_ncls == c->nclasses) return memo_ans;
  int ans = 0;
  for (int k = 0; k < c->nclasses && !ans; k++)
    if (c->classes[k].is_struct || c->classes[k].is_data) ans = 1;
  memo_ncls = c->nclasses; memo_ans = ans;
  return ans;
}

int an_user_defines_or_reads(Compiler *c, const char *name) {
  if (an_builtin_only) return 0;
  if (!name) return 0;
  /* Outside the frozen fixpoint scope shape can still change without the counts
     moving (a rename), so neither consult nor populate the memo there. */
  int memo = comp_scope_index_is_frozen();
  unsigned b = 0;
  if (memo) {
    unsigned gen = comp_scope_index_gen();
    if (gen != udr_gen || c->nscopes != udr_nscopes || c->nclasses != udr_nclasses) {
      for (int i = 0; i < udr_n; i++) free(udr_names[i]);
      udr_n = 0;
      if (!udr_cap) {
        udr_cap = 4096;
        udr_names = malloc(sizeof(char *) * (size_t)udr_cap);
        udr_ans = malloc((size_t)udr_cap);
        udr_next = malloc(sizeof(int) * (size_t)udr_cap);
        udr_head = malloc(sizeof(int) * (size_t)udr_cap);
        if (!udr_names || !udr_ans || !udr_next || !udr_head) udr_cap = 0;
      }
      for (int i = 0; i < udr_cap; i++) udr_head[i] = -1;
      udr_gen = gen; udr_nscopes = c->nscopes; udr_nclasses = c->nclasses;
    }
    if (udr_cap) {
      b = sp_strhash(name) & (unsigned)(udr_cap - 1);
      for (int i = udr_head[b]; i >= 0; i = udr_next[i])
        if (sp_streq(udr_names[i], name)) return udr_ans[i];
    }
    else memo = 0;
  }
  int ans = 0;
  for (int k = 0; k < c->nclasses && !ans; k++) {
    /* comp_poly_arm_defines: a native class counts only through its
       declared bindings (#4504) -- counting its Ruby-side defs here made
       every caller stand its builtin arm down, and merely loading
       IO::Buffer turned `h.values` on a poly Hash into NoMethodError. */
    if (comp_poly_arm_defines(c, k, name)) ans = 1;
    else if (!c->classes[k].is_native_class && comp_is_reader(&c->classes[k], name)) ans = 1;
  }
  /* A CLASS method of the same name is deliberately not consulted: it is only
     reachable through a Class-valued receiver, so it cannot be the answer to
     an instance call. Counting it made `k.downcase` on a String bind to a
     `def self.downcase(s)` elsewhere in the program and compile to the arity
     raise (#3520). A TOP-LEVEL method is out for the same reason one step on:
     it lands on Object as a PRIVATE method, so an explicit receiver can never
     reach it -- CRuby answers `nil.size` after `def size(v)` with "private
     method 'size' called", not with the method. Counting it made every caller
     of this predicate stand its builtin arm down, and `def upcase(v) = v.upcase`
     compiled its own body to an unconditional NoMethodError. */
  if (memo && udr_n < udr_cap) {
    udr_names[udr_n] = strdup(name);
    if (udr_names[udr_n]) {
      udr_ans[udr_n] = (signed char)ans;
      udr_next[udr_n] = udr_head[b]; udr_head[b] = udr_n; udr_n++;
    }
  }
  return ans;
}

void sp_narrow_memo_bump(void) { g_narrow_gen++; }

/* Is node `id` the receiver of some call? Asked while typing every
   `Hash.new` (#3823), it walked every call in the program per ask, per round
   (rubys/roundhouse#72). A bit per node, rebuilt per fixpoint iteration like
   the narrow memo above, and when the table grows. */
static unsigned char *g_recv_used;
static unsigned g_recv_gen;
static const NodeTable *g_recv_nt;
static int g_recv_cnt = -1;
static int node_is_call_receiver(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (g_recv_gen != g_narrow_gen || g_recv_nt != nt || g_recv_cnt != nt->count) {
    free(g_recv_used);
    g_recv_used = calloc((size_t)(nt->count > 0 ? nt->count : 1), 1);
    if (!g_recv_used) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    NT_FOREACH_KIND(nt, NK_CallNode, use) {
      int r = nt_ref(nt, use, "receiver");
      if (r >= 0 && r < nt->count) g_recv_used[r] = 1;
    }
    g_recv_gen = g_narrow_gen; g_recv_nt = nt; g_recv_cnt = nt->count;
  }
  return id >= 0 && id < g_recv_cnt && g_recv_used[id];
}

/* Whether node `id` is an element of an array literal or a value of a hash
   literal: a value placed straight into a container, with no local of its
   own whose uses could narrow it. Cached per node table like the receiver
   set above. */
static unsigned char *g_elem_used; static int g_elem_cnt = -1, g_elem_gen = -1;
static const NodeTable *g_elem_nt;
static int node_is_container_elem(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (g_elem_gen != g_narrow_gen || g_elem_nt != nt || g_elem_cnt != nt->count) {
    free(g_elem_used);
    g_elem_used = calloc((size_t)(nt->count > 0 ? nt->count : 1), 1);
    if (!g_elem_used) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    NT_FOREACH_KIND(nt, NK_ArrayNode, a) {
      int en = 0; const int *el = nt_arr(nt, a, "elements", &en);
      for (int k = 0; k < en; k++) if (el[k] >= 0 && el[k] < nt->count) g_elem_used[el[k]] = 1;
    }
    NT_FOREACH_KIND(nt, NK_AssocNode, a) {
      int v = nt_ref(nt, a, "value");
      if (v >= 0 && v < nt->count) g_elem_used[v] = 1;
    }
    g_elem_gen = g_narrow_gen; g_elem_nt = nt; g_elem_cnt = nt->count;
  }
  return id >= 0 && id < g_elem_cnt && g_elem_used[id];
}

/* ---- A hash whose values are all one class (#4846) ----
   `@items = {}` filled only by `@items[k] = item` keeps boxed values (there is
   no object-valued hash kind), so every read of a value was poly and every
   call on it a switch over each class answering the name -- the K^2 term of
   #4847's synthetic program. When every value written into such a slot is
   an instance of one class (or nil), and the slot is only ever the receiver
   of a known set of hash operations, the READS are typed as that class: the
   storage stays boxed, and codegen unboxes each read with the class check
   emit_unbox_text already makes.

   A slot is an ivar (keyed by the root class of its hierarchy, which shares
   the storage) or a local (keyed by its scope). It is kept only while:
     - every assignment to it is an empty `{}`;
     - every read of it is the receiver of one of the operations below;
     - every `[]=` / `store` into it writes an object of one class (never nil);
     - for an ivar, no module method, class method or class body names it.
   Computed once per fixpoint round, over the whole table. */
typedef struct { int kind, owner; const char *name; int cls; int bad; int writes; } HvSlot;
static HvSlot *g_hv = NULL;
static int g_hv_n = 0, g_hv_cap = 0;
static unsigned g_hv_gen = 0;
static int g_hv_building = 0;
static const NodeTable *g_hv_nt = NULL;

static int hv_root_class(Compiler *c, int cid) {
  int guard = 0;
  while (cid >= 0 && c->classes[cid].parent >= 0 && guard++ < 1000) cid = c->classes[cid].parent;
  return cid;
}
/* The slot a read or write node names: kind 0 ivar (owner = root class), kind
   1 local (owner = scope). 0 when it is neither, or an ivar outside an
   instance method (poisoned by the caller). */
static int hv_slot_key(Compiler *c, int node, int *kind, int *owner, const char **name, int *outside) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  *outside = 0;
  *name = nt_str(nt, node, "name");
  if (!*name) return 0;
  Scope *sc = comp_scope_of(c, node);
  if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
      k == NK_InstanceVariableOrWriteNode || k == NK_InstanceVariableAndWriteNode ||
      k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableTargetNode) {
    if (!sc || !sc->name || sc->is_cmethod || sc->class_id < 0 ||
        comp_class_is_module(c, &c->classes[sc->class_id])) { *outside = 1; return 0; }
    *kind = 0; *owner = hv_root_class(c, sc->class_id); return 1;
  }
  if (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode ||
      k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
      k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableTargetNode) {
    if (!sc) return 0;
    *kind = 1; *owner = (int)(sc - c->scopes); return 1;
  }
  return 0;
}
static HvSlot *hv_find(int kind, int owner, const char *name, int create) {
  for (int i = 0; i < g_hv_n; i++)
    if (g_hv[i].kind == kind && g_hv[i].owner == owner && sp_streq(g_hv[i].name, name)) return &g_hv[i];
  if (!create) return NULL;
  if (g_hv_n == g_hv_cap) {
    g_hv_cap = g_hv_cap ? g_hv_cap * 2 : 64;
    g_hv = realloc(g_hv, sizeof(HvSlot) * (size_t)g_hv_cap);
    if (!g_hv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  HvSlot *h = &g_hv[g_hv_n++];
  h->kind = kind; h->owner = owner; h->name = name; h->cls = -1; h->bad = 0; h->writes = 0;
  return h;
}
/* The hash operations a slot may be the receiver of, with their argument and
   block shapes. `[]=` / `store` are the writes. */
static int hv_op_ok(const NodeTable *nt, int call) {
  const char *nm = nt_str(nt, call, "name");
  if (!nm) return 0;
  int a = nt_ref(nt, call, "arguments");
  int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  for (int i = 0; i < an; i++) {
    NodeKind ak = nt_kind(nt, av[i]);
    if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode) return 0;
  }
  int blk = nt_ref(nt, call, "block");
  int has_blk = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode;
  if (blk >= 0 && !has_blk) return 0;
  if ((sp_streq(nm, "[]=") || sp_streq(nm, "store")) && an == 2 && !has_blk) return 2;
  if ((sp_streq(nm, "[]") || sp_streq(nm, "delete")) && an == 1 && !has_blk) return 1;
  if (sp_streq(nm, "fetch") && an == 1 && !has_blk) return 1;
  if ((sp_streq(nm, "key?") || sp_streq(nm, "has_key?") || sp_streq(nm, "include?") ||
       sp_streq(nm, "member?")) && an == 1 && !has_blk) return 1;
  if ((sp_streq(nm, "size") || sp_streq(nm, "length") || sp_streq(nm, "empty?") ||
       sp_streq(nm, "keys") || sp_streq(nm, "values") || sp_streq(nm, "clear")) && an == 0 && !has_blk) return 1;
  if (sp_streq(nm, "each_value") && an == 0 && has_blk) return 1;
  return 0;
}
static int hv_is_poly_hash(TyKind t) {
  return t == TY_STR_POLY_HASH || t == TY_SYM_POLY_HASH || t == TY_POLY_POLY_HASH;
}
static void hv_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  g_hv_building = 1;
  g_hv_n = 0;
  int n = nt->count;
  unsigned char *ok = calloc((size_t)(n > 0 ? n : 1), 1);
  if (!ok) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* receivers of allowed operations, and the writes' value classes */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int r = nt_ref(nt, id, "receiver");
    if (r < 0) continue;
    NodeKind rk = nt_kind(nt, r);
    if (rk != NK_InstanceVariableReadNode && rk != NK_LocalVariableReadNode) continue;
    int op = hv_op_ok(nt, id);
    if (!op) continue;
    ok[r] = 1;
    if (op != 2) continue;
    int kind, owner, outside; const char *name;
    if (!hv_slot_key(c, r, &kind, &owner, &name, &outside)) continue;
    HvSlot *h = hv_find(kind, owner, name, 1);
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = nt_arr(nt, a, "arguments", &an);
    TyKind vt = infer_type(c, av[1]);
    h->writes++;
    /* a nil value would reach each_value / values as a NULL the loop body
       then calls on; a hash that ever stores nil stays boxed */
    if (!ty_is_object(vt)) { h->bad = 1; continue; }
    int vc = ty_object_class(vt);
    if (h->cls < 0) h->cls = vc;
    else if (h->cls != vc) h->bad = 1;
  }
  /* every other appearance of a slot: a read that is not such a receiver, or
     an assignment other than an empty `{}`, disqualifies it; an ivar named
     outside an instance method poisons the name */
  for (int id = 0; id < n; id++) {
    NodeKind k = nt_kind(nt, id);
    int is_read = (k == NK_InstanceVariableReadNode || k == NK_LocalVariableReadNode);
    int is_ivw = (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
                  k == NK_InstanceVariableAndWriteNode || k == NK_InstanceVariableOperatorWriteNode ||
                  k == NK_InstanceVariableTargetNode);
    int is_lvw = (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
                  k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode ||
                  k == NK_LocalVariableTargetNode);
    if (!is_read && !is_ivw && !is_lvw) continue;
    int kind, owner, outside; const char *name;
    if (!hv_slot_key(c, id, &kind, &owner, &name, &outside)) {
      if (outside && name)
        for (int i = 0; i < g_hv_n; i++)
          if (g_hv[i].kind == 0 && sp_streq(g_hv[i].name, name)) g_hv[i].bad = 1;
      continue;
    }
    HvSlot *h = hv_find(kind, owner, name, 0);
    if (!h) continue;
    if (is_read) { if (!ok[id]) h->bad = 1; continue; }
    int v = (k == NK_InstanceVariableWriteNode || k == NK_LocalVariableWriteNode) ? nt_ref(nt, id, "value") : -1;
    int en = -1;
    if (v >= 0 && nt_kind(nt, v) == NK_HashNode) nt_arr(nt, v, "elements", &en);
    if (en != 0) h->bad = 1;
  }
  /* the name poisoning above only reaches slots already seen; do it again now
     that every slot exists */
  for (int id = 0; id < n; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_InstanceVariableReadNode && k != NK_InstanceVariableWriteNode) continue;
    int kind, owner, outside; const char *name;
    if (!hv_slot_key(c, id, &kind, &owner, &name, &outside) && outside && name)
      for (int i = 0; i < g_hv_n; i++)
        if (g_hv[i].kind == 0 && sp_streq(g_hv[i].name, name)) g_hv[i].bad = 1;
  }
  free(ok);
  g_hv_gen = g_narrow_gen;
  g_hv_nt = nt;
  g_hv_building = 0;
}
/* The class every value of the hash read by `recv` has, or -1. */
int hv_value_class(Compiler *c, int recv) {
  if (recv < 0 || g_hv_building) return -1;
  NodeKind rk = nt_kind(c->nt, recv);
  if (rk != NK_InstanceVariableReadNode && rk != NK_LocalVariableReadNode) return -1;
  if (g_hv_gen != g_narrow_gen || g_hv_nt != c->nt) hv_build(c);
  int kind, owner, outside; const char *name;
  if (!hv_slot_key(c, recv, &kind, &owner, &name, &outside)) return -1;
  HvSlot *h = hv_find(kind, owner, name, 0);
  if (!h || h->bad || h->cls < 0 || h->writes == 0) return -1;
  /* the slot must actually be a boxed-value hash */
  if (kind == 0) {
    Scope *sc = comp_scope_of(c, recv);
    int iv = comp_ivar_index(&c->classes[sc->class_id], name);
    if (iv < 0 || !hv_is_poly_hash(c->classes[sc->class_id].ivar_types[iv])) return -1;
  }
  else {
    LocalVar *lv = scope_local(&c->scopes[owner], name);
    if (!lv || !hv_is_poly_hash(lv->type)) return -1;
  }
  return h->cls;
}
static long narrow_key(int which, int cid, const char *ivname) {
  unsigned long h = 1469598103934665603UL ^ (unsigned)which;
  h = (h * 1099511628211UL) ^ (unsigned)cid;
  for (const char *p = ivname; p && *p; p++) h = (h * 1099511628211UL) ^ (unsigned char)*p;
  return (long)(h & 0x7fffffffffffffffUL);
}
static int narrow_memo_get(long key, int *hit) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  if (g_nmemo[slot].gen == g_narrow_gen && g_nmemo[slot].key == key) { *hit = 1; return g_nmemo[slot].val; }
  *hit = 0; return 0;
}
static void narrow_memo_put(long key, int val) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  g_nmemo[slot].gen = g_narrow_gen; g_nmemo[slot].key = key; g_nmemo[slot].val = (signed char)val;
}

/* Unify the value type of every splice-bound break/next in `node` (break/next
   inside a nested loop or block bind there, not to the splice). TY_UNKNOWN if
   none carry a value. Mirrors codegen's ie_splice_value_ty so the inferred
   instance_exec result type matches the slot codegen sizes. */
TyKind ie_block_break_next_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "BreakNode") || sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an > 0) {
      /* `next *x` delivers the splat-built ARRAY, not the element type that
         infer_type reports for a SplatNode in an array literal. */
      const char *aty = nt_type(nt, av[0]);
      if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
      return infer_type(c, av[0]);
    }
    /* a bare `next` yields nil: `[1,2].map { |v| next if v == 1; v }` is [nil, 2] */
    return sp_streq(ty, "NextNode") ? TY_NIL : TY_UNKNOWN;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind s = ie_block_break_next_ty(c, nt_ref_at(nt, node, i));
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = ie_block_break_next_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
}

/* The array kind a `next <v>` arm hands a `then` block's value slot, joined
   over every arm that binds to this block (one inside a nested loop, block or
   def binds there). Two kinds widen to the poly array, the one both convert
   to. An empty `[]` names no kind: it is built at the slot's kind (#3978). A
   splat delivers the array it builds. Anything but an array is left to the
   tail, as before. TY_UNKNOWN when no arm answers an array. */
static int then_array_kind_joins(TyKind t) {
  return t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY || t == TY_POLY_ARRAY;
}
static TyKind then_next_array_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an != 1) return TY_UNKNOWN;
    const char *aty = nt_type(nt, av[0]);
    if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
    if (aty && sp_streq(aty, "ArrayNode")) {
      int en = 0; nt_arr(nt, av[0], "elements", &en);
      if (en == 0) return TY_UNKNOWN;
    }
    TyKind t = infer_type(c, av[0]);
    return then_array_kind_joins(t) ? t : TY_UNKNOWN;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind s = then_next_array_ty(c, nt_ref_at(nt, node, i));
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN || r == s) ? s : TY_POLY_ARRAY;
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = then_next_array_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN || r == s) ? s : TY_POLY_ARRAY;
    }
  }
  return r;
}

/* The value of a `then` block is its tail, widened to the poly array when a
   `next` arm leaves the block with an array of another kind: the arm's value
   lands in the slot the tail is typed for, and only the poly array holds
   both. Under --int-overflow=promote the tail `[v, v * 2]` is a poly array
   while `next [v]` over the block parameter (an Integer, not widened) is an
   Integer array, so the slot took the tail's kind and the arm did not build.
   The codegen converts the odd one out at its assignment (#4747). */
/* Does a `next` binding to this `then` block (not one in a nested loop,
   block or def) leave with a value the tail's type cannot hold: one of
   another scalar kind, or a bare `next`'s nil? Its value is the block's,
   so the slot boxes it. */
static int then_next_other_scalar(Compiler *c, int node, TyKind tail) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    TyKind t = an == 1 ? infer_type(c, av[0]) : TY_NIL;
    if (then_array_kind_joins(t) || t == TY_UNKNOWN) return 0;
    return ty_unify(t, tail) != tail || (t == TY_NIL && tail != TY_NIL && tail != TY_POLY);
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (then_next_other_scalar(c, nt_ref_at(nt, node, i), tail)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) if (then_next_other_scalar(c, ids[k], tail)) return 1;
  }
  return 0;
}

TyKind then_block_value_ty(Compiler *c, int body, TyKind tail) {
  if (!then_array_kind_joins(tail)) return then_next_other_scalar(c, body, tail) ? TY_POLY : tail;
  TyKind a = then_next_array_ty(c, body);
  if (a == TY_UNKNOWN || a == tail) return tail;
  return TY_POLY_ARRAY;
}

int g_infer_ignore_brk = 0;

/* Post-backstop return re-derivation must not newly widen a return to poly
   (see infer_return_types); set only around analyze_program's post-pass. */
int g_ret_no_new_poly = 0;

/* Top-level `break` detector: a BreakNode binding to the enclosing block,
   stopping at nested loops and nested block-bearing calls (which capture their
   own break). Mirrors codegen_stmt's subtree_has_loop_break. */
int block_has_top_break(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "BreakNode")) return 1;
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, node, "block") >= 0) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (block_has_top_break(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) if (block_has_top_break(c, el[j])) return 1;
  }
  return 0;
}

/* A call is a break-wrapped iterator when it takes a literal block whose body
   has a top-level break and is either a receiver-bearing builtin iterator or
   a call resolving to an inline-able yielding user method (whose body -- and
   so the block, at its yield sites -- is spliced at this call site, putting
   the wrapper's setjmp in exactly the right C scope). Receiverless
   NON-methods (loop / catch / proc / lambda literals) run their own scopes
   and stay excluded, as does instance_exec/eval (handled inline). */
int call_breaks(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;   /* not &proc / &:sym */
  const char *name = nt_str(nt, id, "name");
  if (name && (sp_streq(name, "instance_exec") || sp_streq(name, "instance_eval"))) return 0;
  if (nt_ref(nt, id, "receiver") < 0 && call_user_yield_mi(c, id) < 0) return 0;
  return block_has_top_break(c, nt_ref(nt, block, "body"));
}

/* str.unpack1(fmt) with a literal single-directive numeric format: the
   directive fixes the extracted value's type, so the result does not need
   to stay poly (`data[4, 4].unpack1('V')` reading a WAD header count in
   doom). Count/endian suffixes ("V2", "l<", "q>*") keep the first value's
   type. Only the numeric directives sp_str_unpack decodes qualify --
   integers to TY_INT, the float/double directives to TY_FLOAT;
   multi-directive, interpolated, and other formats stay TY_POLY. */
static TyKind an_unpack1_lit_type(const NodeTable *nt, int arg) {
  const char *aty = nt_type(nt, arg);
  if (!aty || !sp_streq(aty, "StringNode")) return TY_POLY;
  const char *f = nt_str(nt, arg, "content");
  if (!f || !f[0]) return TY_POLY;
  char d = f[0];
  const char *p = f + 1;
  while (*p == '<' || *p == '>' || *p == '!' || *p == '_') p++;
  while (*p >= '0' && *p <= '9') p++;
  if (*p == '*') p++;
  if (*p) return TY_POLY;  /* further directives: not this one's type */
  /* The 64-bit pair is the exception: `Q` above 2**63-1 is a Bignum, and
     `q`'s INT64_MIN is the value an sp_int slot spells as nil. An sp_int
     slot holds neither, so they keep the boxed answer the runtime already
     hands back -- typed int, the Bignum truncated and the INT64_MIN read
     back as nil (#4588). The narrower directives all fit. */
  if (strchr("cCsSlLnNvV", d)) return TY_INT;
  if (strchr("dDfFeEgG", d)) return TY_FLOAT;
  return TY_POLY;
}

/* Whether every element written into the poly-array ivar `@<ivname>` of class
   `cid` is an int-returning kind: a bound method (a dispatch-table entry, called
   with an int arg returns int), an int array, an int, or nil filler. Mirrors
   legacy's cls_ivar_observed_types check in poly_index_narrow_int. */
/* Classify a value type stored as an element of a dispatch/data table:
   1 = int-returning when indexed/called (int bit, int array get, bound
   method/proc call, or a poly assumed to be such a callable); 2 = neutral
   filler (nil/unknown); 0 = a clearly non-int kind. */
static int table_elem_int_returning(TyKind vt) {
  if (vt == TY_INT || vt == TY_INT_ARRAY || vt == TY_METHOD || vt == TY_PROC ||
      vt == TY_POLY)
    return 1;
  if (vt == TY_NIL || vt == TY_UNKNOWN) return 2;
  return 0;
}

static int ivar_array_elems_int_returning_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* element write `@ivar[i] = v` */
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0) continue;
      const char *rty = nt_type(nt, recv);
      if (!rty || !sp_streq(rty, "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      int k = table_elem_int_returning(comp_ntype(c, av[1]));
      if (k == 0) return 0;
      if (k == 1) saw = 1;
      continue;
    }
    /* whole-ivar write `@ivar = [..]` / `@ivar = [x] * n` */
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;   /* the ArrayNode whose elements to inspect */
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");   /* `[..] * n` */
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      if (arr < 0) return 0;   /* non-literal whole write: can't verify */
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        int k = table_elem_int_returning(comp_ntype(c, els[e]));
        if (k == 0) return 0;
        if (k == 1) saw = 1;
      }
      continue;
    }
  }
  return saw;
}

/* `@table[i][j]` where @table is a poly array of int-returning callables/arrays
   (a method dispatch table) yields an int. Returns 1 if `id` matches that shape
   for the enclosing class. The index types don't matter (they may themselves be
   poly mid-fixpoint); the result type follows from the table's element kinds. */
static int ivar_array_elems_int_returning(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(0, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_int_returning_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

static int poly_double_index_int(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;   /* slice, not an element */
  int inner = nt_ref(nt, id, "receiver");
  if (inner < 0) return 0;
  const char *ity = nt_type(nt, inner);
  if (!ity || !sp_streq(ity, "CallNode")) return 0;
  const char *inm = nt_str(nt, inner, "name");
  if (!inm || !sp_streq(inm, "[]")) return 0;
  int iargs = nt_ref(nt, inner, "arguments");
  int ian = 0;
  const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &ian) : NULL;
  if (ian != 1) return 0;
  (void)iav;
  int ivnode = nt_ref(nt, inner, "receiver");
  if (ivnode < 0) return 0;
  const char *vty = nt_type(nt, ivnode);
  if (!vty || !sp_streq(vty, "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, ivnode, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_int_returning(c, cid, ivname);
}

/* Each node's parent (-1 for a root), for walks that ask where a value
   goes. The caller frees it. */
int *an_parent_map(const NodeTable *nt) {
  int *parent = malloc(sizeof(int) * ((size_t)nt->count + 1));
  if (!parent) return NULL;
  for (int id = 0; id < nt->count; id++) parent[id] = -1;
  for (int id = 0; id < nt->count; id++) {
    int nr = nt_num_refs(nt, id);
    for (int i = 0; i < nr; i++) {
      int ch = nt_ref_at(nt, id, i);
      if (ch >= 0 && ch < nt->count) parent[ch] = id;
    }
    int na = nt_num_arrs(nt, id);
    for (int i = 0; i < na; i++) {
      int an = 0;
      const int *av = nt_arr_at(nt, id, i, &an);
      for (int e = 0; e < an; e++)
        if (av[e] >= 0 && av[e] < nt->count) parent[av[e]] = id;
    }
  }
  return parent;
}

/* Whether the value of `node` is thrown away: a statement that is not the
   value of its sequence, or the last one of a loop body or of the block of
   an iterator that does not answer the block's value. */
int an_value_dropped(const NodeTable *nt, const int *parent, int node) {
  int st = parent[node];
  if (st < 0 || nt_kind(nt, st) != NK_StatementsNode) return 0;
  int sn = 0;
  const int *sb = nt_arr(nt, st, "body", &sn);
  if (sn > 0 && sb[sn - 1] != node) return 1;
  int owner = parent[st];
  if (owner < 0) return 0;
  NodeKind ok = nt_kind(nt, owner);
  if (ok == NK_WhileNode || ok == NK_UntilNode) return 1;
  if (ok != NK_BlockNode) return 0;
  int bc = parent[owner];
  const char *bn = bc >= 0 && nt_kind(nt, bc) == NK_CallNode ? nt_str(nt, bc, "name") : NULL;
  return bn && is_block_loop_method(bn);
}

static int an_elems_int_rows(Compiler *c, int arr, int *saw) {
  int en = 0;
  const int *els = nt_arr(c->nt, arr, "elements", &en);
  for (int e = 0; e < en; e++) {
    TyKind et = comp_ntype(c, els[e]);
    if (et == TY_INT_ARRAY) { *saw = 1; continue; }
    if (et == TY_NIL || et == TY_UNKNOWN) continue;
    return 0;
  }
  return 1;
}

/* Whether every element stored into poly-array ivar `@<ivname>` is an int
   array (a nested array of int arrays, e.g. @chr_banks / @nmt_mem). Element
   reads then yield an int array rather than a boxed poly. */
static int ivar_array_elems_all_int_array_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  const char *bare = ivname[0] == '@' ? ivname + 1 : ivname;
  ClassInfo *cl = &c->classes[cid];
  if (comp_is_reader(cl, bare) || comp_is_writer(cl, bare)) return 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm) continue;
      int is_store = sp_streq(nm, "[]=") || sp_streq(nm, "store");
      int is_insert = sp_streq(nm, "insert");
      int is_concat = sp_streq(nm, "concat");
      int is_append = sp_streq(nm, "<<") || sp_streq(nm, "push") || sp_streq(nm, "append") ||
                      sp_streq(nm, "unshift") || sp_streq(nm, "prepend");
      if (!is_store && !is_insert && !is_concat && !is_append) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (is_store && an < 2) continue;
      for (int a = is_store || is_insert ? 1 : 0; a < an; a++) {
        const char *aty = nt_type(nt, av[a]);
        if (aty && sp_streq(aty, "SplatNode")) return 0;
        if (is_concat) {
          if (!aty || !sp_streq(aty, "ArrayNode") || !an_elems_int_rows(c, av[a], &saw)) return 0;
          continue;
        }
        TyKind vt = comp_ntype(c, av[a]);
        if (vt == TY_INT_ARRAY) { saw = 1; continue; }
        if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
        return 0;
      }
      continue;
    }
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
        /* `@x = src.map { ... }`: elements are the block's result type. */
        int blk = nt_ref(nt, v, "block");
        int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn <= 0) return 0;
        TyKind et = comp_ntype(c, bb[bn - 1]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      if (arr < 0 || !an_elems_int_rows(c, arr, &saw)) return 0;
      continue;
    }
  }
  if (!saw) return 0;
  /* The stores above are only every row when no other reference to the
     table can be appended to: each read must be the receiver of a call
     that answers something other than the table, or of one of the vetted
     stores / `each` whose answer (the table itself) is dropped. A read
     handed on as a value (a `def t = @t` reader, `t = @t`, an argument,
     `@t.itself`, `@t.tap { }`, `@t.push(r).push(r2)`) could be appended to
     where this scan does not look. */
  int *parent = an_parent_map(nt);
  if (!parent) return 0;
  for (int id = 0; id < nt->count && saw; id++) {
    NodeKind k = nt_kind(nt, id);
    int is_read = k == NK_InstanceVariableReadNode;
    if (!is_read && k != NK_InstanceVariableOperatorWriteNode &&
        k != NK_InstanceVariableOrWriteNode && k != NK_InstanceVariableAndWriteNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, ivname)) continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || s->class_id != cid) continue;
    if (!is_read) { saw = 0; break; }
    int call = parent[id];
    if (call < 0 || nt_kind(nt, call) != NK_CallNode || nt_ref(nt, call, "receiver") != id) { saw = 0; break; }
    const char *cn = nt_str(nt, call, "name");
    if (!cn) { saw = 0; break; }
    static const char *const answers_other[] = {
      "[]", "at", "fetch", "dig", "first", "last", "size", "length", "count",
      "empty?", "any?", "include?", "index", "map", "collect", "sum", "min", "max",
      "each_slice", "==", "!=", "inspect", "to_s", "hash", "[]=", "store", NULL };
    static const char *const answers_self[] = {
      "<<", "push", "append", "unshift", "prepend", "insert", "concat",
      "each", "each_with_index", "each_index", "reverse_each", NULL };
    int other = 0, self_ans = 0;
    for (int i = 0; answers_other[i]; i++) if (sp_streq(cn, answers_other[i])) { other = 1; break; }
    for (int i = 0; !other && answers_self[i]; i++) if (sp_streq(cn, answers_self[i])) { self_ans = 1; break; }
    if (other) {
      /* a block-less enumerator answer (`@t.map`) is a view of the table */
      if ((sp_streq(cn, "map") || sp_streq(cn, "collect") || sp_streq(cn, "each_slice")) &&
          nt_ref(nt, call, "block") < 0) saw = 0;
      continue;
    }
    if (!self_ans) { saw = 0; break; }
    if ((sp_streq(cn, "each") || sp_streq(cn, "each_with_index") || sp_streq(cn, "each_index") ||
         sp_streq(cn, "reverse_each")) && nt_ref(nt, call, "block") < 0) { saw = 0; break; }
    /* the answer is the table: it must be dropped */
    if (!an_value_dropped(nt, parent, call)) saw = 0;
  }
  free(parent);
  return saw;
}

/* `@nested[i]` (single index) where @nested is a poly array whose every
   element is an int array yields an int array (not a boxed poly). Returns the
   ivar name via *out_iv for codegen, or NULL. */
static int ivar_array_elems_all_int_array(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(1, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_all_int_array_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

/* Whether every element of poly-array constant `CNAME` is an int array
   (e.g. `WAVE_FORM = [..].map { (0..7).map { .. } }`). Element reads then
   yield sp_IntArray* instead of a boxed poly. All writes to the constant and
   any `CNAME[i] = v` mutation must agree. */
static int const_array_elems_all_int_array_impl(Compiler *c, const char *cname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, cname)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (!sp_streq(ty, "ConstantWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, cname)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return 0;
    /* `CNAME = [...].freeze` binds the same literal */
    if (nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze") &&
        nt_ref(nt, v, "receiver") >= 0)
      v = nt_ref(nt, v, "receiver");
    const char *vty = nt_type(nt, v);
    int arr = -1;
    if (vty && sp_streq(vty, "ArrayNode")) arr = v;
    else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
             (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
      int blk = nt_ref(nt, v, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn <= 0) return 0;
      TyKind et = comp_ntype(c, bb[bn - 1]);
      if (et == TY_INT_ARRAY) { saw = 1; continue; }
      return 0;
    }
    if (arr < 0 || !an_elems_int_rows(c, arr, &saw)) return 0;
  }
  return saw;
}

int const_array_elems_all_int_array(Compiler *c, const char *cname) {
  long k = narrow_key(2, 0, cname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = const_array_elems_all_int_array_impl(c, cname);
  narrow_memo_put(k, v);
  return v;
}

/* `CONST[i]` on a poly-array constant of int arrays yields an int array. */
static int const_poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) return 0;
  const char *cname = nt_str(nt, recv, "name");
  LocalVar *cv = cname ? comp_const(c, cname) : NULL;
  if (!cv || cv->type != TY_POLY_ARRAY) return 0;
  return const_array_elems_all_int_array(c, cname);
}

static int poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, recv, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_all_int_array(c, cid, ivname);
}

/* A plain int literal value (not an out-of-int64 bigint literal). */
static int infer_const_int_node(const NodeTable *nt, int id, long long *out) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "IntegerNode")) return 0;
  if (nt_str(nt, id, "bigval")) return 0;
  *out = (long long)nt_int(nt, id, "value", 0);
  return 1;
}

/* Whether base**exp does not fit in signed 64 bits (so it must be a Bignum). */
static int infer_int_pow_overflows(long long base, long long exp) {
  if (exp <= 0) return 0;
  if (base >= -1 && base <= 1) return 0;
  long long r = 1;
  for (long long i = 0; i < exp; i++)
    if (__builtin_mul_overflow(r, base, &r)) return 1;
  return 0;
}

/* Whether base << amount does not fit in signed 64 bits (a Bignum in CRuby --
   `1 << 64` is 2**64, not a wrapped/UB C shift). */
static int infer_int_shl_overflows(long long base, long long amount) {
  if (base == 0 || amount <= 0) return 0;
  long long r = base;
  for (long long i = 0; i < amount; i++)
    if (__builtin_mul_overflow(r, 2, &r)) return 1;
  return 0;
}

/* A blockless `range.each` is an external Enumerator only when used standalone
   or consumed by an enumerator method (#next/#peek/#rewind/#size). When it is
   the receiver of a collection method (.to_a/.map/.select/...), it materializes
   to a typed array instead, so those chains keep the fast unboxed path. */
/* Literal Float::INFINITY as a range endpoint (the infer-side twin of
   codegen's lazy_endpoint_is_infinite; nil/missing ends are checked by the
   callers directly). */
int infer_end_is_float_inf(Compiler *c, int right) {
  const NodeTable *nt = c->nt;
  if (right < 0) return 0;
  const char *rty = nt_type(nt, right);
  if (!rty || !sp_streq(rty, "ConstantPathNode")) return 0;
  const char *cpnm = nt_str(nt, right, "name");
  if (!cpnm || !sp_streq(cpnm, "INFINITY")) return 0;
  int par = nt_ref(nt, right, "parent");
  const char *parnm = (par >= 0 && nt_type(nt, par) &&
                       sp_streq(nt_type(nt, par), "ConstantReadNode"))
                      ? nt_str(nt, par, "name") : NULL;
  return parnm && sp_streq(parnm, "Float");
}

/* A range endpoint that is an infinite Float: the `Float::INFINITY` constant
   or its negation. Such a bound has no sp_int value at all (#3670). */
static int infer_endpoint_is_infinite(Compiler *c, int ep) {
  const NodeTable *nt = c->nt;
  if (ep < 0) return 0;
  if (infer_end_is_float_inf(c, ep)) return 1;
  if (nt_kind(nt, ep) == NK_CallNode && nt_str(nt, ep, "name") &&
      sp_streq(nt_str(nt, ep, "name"), "-@"))
    return infer_end_is_float_inf(c, nt_ref(nt, ep, "receiver"));
  return 0;
}

/* The right endpoint of a literal Range receiver when it was written as a
   finite Float over an Integer begin (1..5.5): that Range keeps the integer
   representation (CRuby iterates it), but its END readers answer the Float the
   caller wrote, which the sp_int fields cannot hold (#3896). */
int range_lit_float_end(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rnode = recv;
  for (int g = 0; g < 8 && rnode >= 0 && nt_kind(nt, rnode) == NK_ParenthesesNode; g++) {
    int pb = nt_ref(nt, rnode, "body");
    int pn = 0; const int *ps = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    rnode = (pn == 1 && ps) ? ps[0] : -1;
  }
  /* a local holding only such a literal reads the same (sole assignment) */
  if (rnode >= 0 && nt_kind(nt, rnode) != NK_RangeNode) {
    int sl = local_sole_range_node(c, rnode);
    if (sl >= 0) rnode = sl;
  }
  if (rnode < 0 || nt_kind(nt, rnode) != NK_RangeNode) return -1;
  int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
  if (lo < 0 || hi < 0) return -1;
  if (infer_type(c, lo) != TY_INT) return -1;
  if (infer_type(c, hi) != TY_FLOAT || infer_endpoint_is_infinite(c, hi)) return -1;
  return hi;
}


static int range_each_is_external(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (nt_ref(nt, n, "receiver") != id) continue;
    const char *m = nt_str(nt, n, "name");
    if (m && (sp_streq(m, "next") || sp_streq(m, "peek") ||
              sp_streq(m, "rewind") || sp_streq(m, "size") ||
              sp_streq(m, "class") || sp_streq(m, "inspect") ||
              /* Enumerator-only chains (Array has no such method): the each
                 must stay an Enumerator, not materialize to an int array (#3228) */
              sp_streq(m, "with_index") || sp_streq(m, "with_object") ||
              sp_streq(m, "each_with_index") || sp_streq(m, "each_index") ||
              /* the forms that answer the RECEIVER: materializing the each
                 would answer the elements instead (#3857). each_entry is
                 renamed to each, so the recorded self-result marks it. */
              sp_streq(m, "each_entry") ||
              ((sp_streq(m, "each_slice") || sp_streq(m, "each_cons")) &&
               nt_ref(nt, n, "block") >= 0))) return 1;
    if (nt_int(nt, n, "enum_self_result", -1) == id) return 1;
    /* the drain a self-result rewrite interposed: the each stays an Enumerator
       (the call answers it) and the drain reads its elements (#3857) */
    if (m && sp_streq(m, "to_a") && nt_str(nt, n, "enum_recv")) return 1;
    return 0;   /* receiver of a collection method -> materialize to an array */
  }
  return 1;     /* standalone -> enumerator */
}

int range_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  int args = nt_ref(nt, id, "arguments"); int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  /* Only an integer range materializes faithfully: sp_Range holds sp_int
     bounds, so redispatch only when a literal range receiver has both bounds
     present and typed TY_INT. A float/string bound would truncate or fail to
     compile, and a beginless/endless range (`(1..)`, `(..5)`) has no int array
     to build -- all fall through to the loud `unsupported` reject rather than
     silently miscompiling. (A non-literal receiver, e.g. `r = (1.0..5.0);
     r.find`, is the pre-existing int-only-sp_Range limitation, not detectable
     here.) */
  int rn = an_unparen(nt, nt_ref(nt, id, "receiver"));
  if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
    int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
    if (lo < 0 || hi < 0) return 0;
    /* A BOXED bound is fine: the range literal's own emission coerces it to the
       sp_int sp_Range holds, exactly as it does for `(1...m).sum`. Demanding a
       statically int bound refused a destructured block parameter, whose leaf
       binds poly (#3363). A float / string bound still declines. */
    for (int e = 0; e < 2; e++) {
      TyKind bt = infer_type(c, e ? hi : lo);
      if (bt != TY_INT && bt != TY_POLY) return 0;
    }
  }
  /* Non-collecting Enumerable methods: their result does not depend on the
     block-produced element type, so materializing the range to an int array is
     transparent. The array-building collectors (chunk_while) are not typed
     there yet, so they stay a clean reject rather than miscompile. */
  if (sp_streq(name, "group_by") || sp_streq(name, "find") ||
      sp_streq(name, "detect") || sp_streq(name, "zip") ||
      sp_streq(name, "tally")) return 1;
  /* each_slice/each_cons: the block form and the blockless Enumerator form
     (.to_a / .map chains) both materialize transparently -- the slices carry
     the range's own ints. */
  if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && argc >= 1)
    return 1;
  /* block-taking Enumerable forms the array emitters serve: partition,
     each_with_index, sort_by, chunk_while, sum { }, and the block forms of
     inject/reduce with an initial value. */
  if (block >= 0 &&
      (sp_streq(name, "partition") || sp_streq(name, "each_with_index") ||
       sp_streq(name, "sort_by") || sp_streq(name, "chunk_while") ||
       sp_streq(name, "slice_when") ||
       sp_streq(name, "chunk") ||
       sp_streq(name, "sum") || sp_streq(name, "each_with_object")))
    return 1;
  if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) && block >= 0)
    return 1;
  /* cycle { }: the array emitter serves both the counted and endless forms;
     the yielded elements are the range's own ints */
  if (sp_streq(name, "cycle") && block >= 0) return 1;
  /* reduce/inject: the explicit symbol / initial-value forms (no block). */
  if ((sp_streq(name, "reduce") || sp_streq(name, "inject")) && argc >= 1 && block < 0) return 1;
  /* count: the block / argument forms (bare count is size, handled natively). */
  if (sp_streq(name, "count")) return block >= 0 || argc >= 1;
  /* take/drop and reverse_each materialize transparently (the results carry
     the range's own ints). */
  if ((sp_streq(name, "take") || sp_streq(name, "drop")) && argc == 1) return 1;
  if (sp_streq(name, "reverse_each")) return 1;
  /* min(n)/max(n)/minmax with a count return arrays of the range's ints */
  if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc >= 1) return 1;
  /* blockless all?/any?/none?/one?: a truthiness scan, which the materialized
     int array performs identically (an int is always truthy) (#3859). The
     pattern-argument forms scan the same elements with `===`. */
  if (block < 0 && argc <= 1 &&
      is_quantifier(name)) return 1;
  /* blockless cycle(n) / each_entry: an Enumerator over the range's own ints,
     which the materialized array yields identically (#3840). A countless
     `cycle` never ends, so it keeps its own emitter. */
  if (block < 0 && sp_streq(name, "cycle") && argc == 1) return 1;
  if (block < 0 && sp_streq(name, "each_entry") && argc == 0) return 1;
  /* blockless select/reject/map: an Enumerator over the range's own ints,
     which the materialized array builds identically (#3062). */
  if (block < 0 && argc == 0 &&
      (sp_streq(name, "select") || sp_streq(name, "filter") ||
       sp_streq(name, "find_all") || sp_streq(name, "reject"))) return 1;
  return 0;
}

/* A Hash Enumerable method served by materializing the hash into its
   [key, value] pair array (sp_enum_items_from) and re-dispatching as a poly
   array: reduce/inject and each_with_index block forms. The hash-native
   folds (sum/count/find/map/...) keep their own emitters. */
int hash_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  /* blockless pair-order Enumerables: min/max compare the [k, v] pairs */
  if (block < 0 && (sp_streq(name, "min") || sp_streq(name, "max") ||
                    sp_streq(name, "minmax")))
    return 1;
  /* blockless each_with_index: an external Enumerator of [[k, v], i] */
  if (block < 0 && sp_streq(name, "each_with_index")) return 1;
  /* inject(:op) / reduce(:op): the Symbol-operator fold runs over the [k, v]
     pairs, the same materialization the block form below rides (#3830) */
  if (block < 0 && (sp_streq(name, "reduce") || sp_streq(name, "inject"))) return 1;
  /* pair-array Enumerables with no dedicated hash emitter: find_index, uniq,
     zip, tally, reverse_each ride the materialized redispatch, block or not
     (#2372) */
  if (sp_streq(name, "find_index") || sp_streq(name, "uniq") ||
      sp_streq(name, "zip") || sp_streq(name, "tally") ||
      sp_streq(name, "reverse_each")) return 1;
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  /* comparator-block min/max/minmax compare the [k, v] pairs like the
     blockless forms */
  if (sp_streq(name, "min") || sp_streq(name, "max") || sp_streq(name, "minmax")) return 1;
  if (sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "find_all")) return 1;
  if (sp_streq(name, "each_with_index")) return 1;
  if (sp_streq(name, "reduce") || sp_streq(name, "inject")) return 1;
  /* comparator-block sort over the [k, v] pairs -> a poly array of pairs */
  if (sp_streq(name, "sort")) return 1;
  /* flat_map keeps its dedicated hash emitter */
  return 0;
}

/* True if a reduce/inject block's tail value flows FROM the accumulator param
   `accp`: a bare read of it (`...; acc`) or a call whose receiver is it
   (`acc << x`, `acc.merge(...)`). Such a tail carries the accumulator's own
   (seed) type even when the param currently infers poly, so it must not trip
   the poly-widening of the fold's result type (#3240). */
int reduce_tail_from_acc(Compiler *c, int tail, const char *accp) {
  const NodeTable *nt = c->nt;
  if (tail < 0 || !accp) return 0;
  const char *ty = nt_type(nt, tail);
  if (!ty) return 0;
  if (sp_streq(ty, "LocalVariableReadNode"))
    return nt_str(nt, tail, "name") && sp_streq(nt_str(nt, tail, "name"), accp);
  if (sp_streq(ty, "CallNode")) {
    int rcv = nt_ref(nt, tail, "receiver");
    if (rcv >= 0 && nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "LocalVariableReadNode"))
      return nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), accp);
  }
  return 0;
}

/* obj.methods / public_methods / singleton_methods on an instance of `cid`:
   listable ahead of time when the class does not define the name itself and
   every ancestor is a user class whose whole method surface spinel knows --
   not an exception, a native class, or a class whose superclass is a
   builtin. */
int an_object_methods_listable(Compiler *c, int cid, const char *name) {
  const NodeTable *nt = c->nt;
  if (cid < 0 || cid >= c->nclasses || class_is_exc_subclass(c, cid)) return 0;
  if (comp_method_in_chain(c, cid, name, NULL) >= 0) return 0;
  for (int k = cid, g = 0; k >= 0 && g <= c->nclasses; k = c->classes[k].parent, g++) {
    if (c->classes[k].is_native_class) return 0;
    if (c->classes[k].parent < 0 && !c->classes[k].is_struct) {
      int dn = c->classes[k].def_node;
      if (dn >= 0 && nt_kind(nt, dn) == NK_ClassNode && nt_ref(nt, dn, "superclass") >= 0) return 0;
    }
  }
  return 1;
}

/* Klass.singleton_methods: listable ahead of time when the class does not
   define the name itself and every ancestor is a user class spinel knows the
   class methods of -- not a Struct/Data (whose generated class methods are
   not scopes), an exception, a native class, or a builtin's subclass. */
int an_class_singleton_methods_listable(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  if (cid < 0 || cid >= c->nclasses || class_is_exc_subclass(c, cid)) return 0;
  if (comp_cmethod_in_chain(c, cid, "singleton_methods", NULL) >= 0) return 0;
  for (int k = cid, g = 0; k >= 0 && g <= c->nclasses; k = c->classes[k].parent, g++) {
    ClassInfo *ci = &c->classes[k];
    if (ci->is_native_class || ci->is_struct || ci->is_data) return 0;
    if (ci->parent < 0) {
      int dn = ci->def_node;
      if (dn >= 0 && nt_kind(nt, dn) == NK_ClassNode && nt_ref(nt, dn, "superclass") >= 0) return 0;
    }
  }
  return 1;
}

/* Does the program build Method objects (`method(:x)` / `instance_method(:x)`)?
   Only then can a boxed value answering #name be a Method (#3692). */
int an_program_builds_methods(Compiler *c) {
  static const NodeTable *memo_nt = NULL; static int memo = 0;
  if (memo_nt == c->nt) return memo;
  memo_nt = c->nt; memo = 0;
  for (int id = 0; id < c->nt->count && !memo; id++) {
    if (nt_kind(c->nt, id) != NK_CallNode) continue;
    const char *n = nt_str(c->nt, id, "name");
    if (n && (sp_streq(n, "method") || sp_streq(n, "instance_method") ||
              sp_streq(n, "unbind"))) memo = 1;
  }
  return memo;
}

/* Does this subtree pull from an external Enumerator (`e.next`)? Such a call
   is what ends a break-less `loop` with StopIteration (#3588). */
static int an_subtree_calls_enum_next(Compiler *c, int root) {
  const NodeTable *nt = c->nt;
  if (root < 0) return 0;
  if (nt_kind(nt, root) == NK_CallNode) {
    const char *n = nt_str(nt, root, "name");
    if (n && sp_streq(n, "next") && nt_ref(nt, root, "receiver") >= 0) return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (an_subtree_calls_enum_next(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0; const int *el = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++)
      if (an_subtree_calls_enum_next(c, el[j])) return 1;
  }
  return 0;
}

/* Zero-argument builtin methods the poly dispatch already serves with a real
   arm. `require "ostruct"` turns any other bare name on a poly receiver into
   a possible OpenStruct member read (#3197); these must NOT be swallowed by
   that catch-all, or the member lookup replaces the builtin and returns nil
   (#3341: `switches.uniq` became an OpenStruct member fetch). */
int poly_builtin_zero_arg_name(const char *m) {
  static const char *const B[] = {
    "to_s", "inspect", "length", "size", "count",
    "uniq", "sort", "reverse", "flatten", "compact", "to_a", "to_h", "to_i",
    "to_f", "to_sym", "keys", "values", "first", "last", "min", "max", "sum",
    "pop", "shift", "clear", "dup", "clone", "freeze", "chars", "bytes",
    "strip", "chomp", "chop", "upcase", "downcase", "capitalize", "swapcase",
    "succ", "next", "abs", "round", "floor", "ceil", "arity", "call", NULL };
  return str_in(m, B);
}
/* The return a user class gives `name` at this arity, unified over every class
   that defines it, or TY_UNKNOWN when none does (or none has settled). The
   poly-dispatch union rules ask this to tell a name whose user answer AGREES
   with the builtin one from a name whose answer does not. */
static TyKind an_user_read_ty(Compiler *c, const char *name, int argc) {
  if (an_builtin_only) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN; int found = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (c->classes[k].is_native_class) {
      int nmk = comp_native_method_find(c, k, name, argc, 0);
      if (nmk >= 0 && native_takes(&c->native_methods[nmk], argc)) {
        TyKind nr = sp_streq(c->native_methods[nmk].ret, "self")
                      ? ty_object(k) : native_spec_to_ty(c->native_methods[nmk].ret);
        r = found ? ty_unify(r, nr) : nr; found = 1;
      }
      continue;
    }
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi >= 0 && c->scopes[mi].ret != TY_UNKNOWN) {
      r = found ? ty_unify(r, c->scopes[mi].ret) : (TyKind)c->scopes[mi].ret;
      found = 1;
    }
  }
  return found ? r : TY_UNKNOWN;
}

/* The analyze twin of codegen's bare_call_class_owned: a receiverless call the
   enclosing class's own chain answers. The class sits ABOVE Kernel, so the
   builtin arms below must stand down for it, or the two halves of the compiler
   name different methods for the same call. */
/* 1 when a user class defines `name` as an instance method whose return is not
   `want`. The poly dispatch emits that class's arm beside the builtin ones and
   accumulates them all in one C temp, so the call's type has to be one both can
   hold: `Tags#include?` answering an index came back through an sp_bool slot as
   true/false, and `0` -- truthy in Ruby -- arrived as false (#4072). */
int an_user_ret_disagrees(Compiler *c, const char *name, TyKind want) {
  if (!name) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    /* a native class answers by its declared bindings, as the dispatch
       arms do (#4504); a Ruby-side def on it has no arm to disagree with */
    if (c->classes[k].is_native_class) {
      int nmi = comp_native_method_find(c, k, name, 0, 0);
      if (nmi >= 0) {
        TyKind r = sp_streq(c->native_methods[nmi].ret, "self")
                     ? ty_object(k) : native_spec_to_ty(c->native_methods[nmi].ret);
        if (r != want && r != TY_UNKNOWN) return 1;
      }
      continue;
    }
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi < 0 || mi >= c->nscopes) continue;
    TyKind r = (TyKind)c->scopes[mi].ret;
    if (r != want && r != TY_UNKNOWN) return 1;
  }
  return 0;
}

/* Whether a chunk_while/slice_when/chunk block call is consumed by a `.to_a`
   terminal. Without one it answers a first-class Enumerator; with one, the
   poly array of runs the terminal materializes. Both the typed-receiver arm
   and the poly-receiver arm have to make the same call, or the slot the value
   lands in disagrees with what the emitter renders. */
int an_chunk_family_to_a(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, w) {
    if (nt_ref(nt, w, "receiver") != id) continue;
    const char *wn = nt_str(nt, w, "name");
    if (wn && sp_streq(wn, "to_a")) return 1;
  }
  return 0;
}

/* Can this type's C slot hold a nil of its own? Integer and Float have their
   sentinels; String, the arrays and every reference object have NULL. A bool, a
   Symbol, a Class, a Rational, a Complex, and the by-value Range and Time
   structs have no such value, so a nil in one of those slots has to be boxed.
   Stated once: the `&.` widening below asks the same question, and the two
   copies would drift. */
int an_ty_holds_nil(TyKind t) {
  return !(t == TY_BOOL || t == TY_CLASS || t == TY_SYMBOL ||
           t == TY_RATIONAL || t == TY_COMPLEX ||
           t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE ||
           t == TY_TIME || t == TY_TMS);
}

/* Is the array call `id` answers mutated in place: the receiver of an Array
   mutator, or written to a local that is? */
static int an_to_a_result_mutated(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  for (int q = comp_kind_first(c, NK_CallNode); q >= 0; q = comp_kind_next(c, q))
    if (nt_kind(nt, q) == NK_CallNode && nt_ref(nt, q, "receiver") == id &&
        array_mutator_name(nt_str(nt, q, "name"))) return 1;
  for (int w = comp_kind_first(c, NK_LocalVariableWriteNode); w >= 0; w = comp_kind_next(c, w)) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode || nt_ref(nt, w, "value") != id) continue;
    const char *wn = nt_str(nt, w, "name");
    Scope *ws = comp_scope_of(c, w);
    for (int q = comp_kind_first(c, NK_CallNode); wn && q >= 0; q = comp_kind_next(c, q)) {
      if (nt_kind(nt, q) != NK_CallNode) continue;
      int r = nt_ref(nt, q, "receiver");
      if (r < 0 || nt_kind(nt, r) != NK_LocalVariableReadNode || comp_scope_of(c, r) != ws) continue;
      const char *rn = nt_str(nt, r, "name");
      if (rn && sp_streq(rn, wn) && array_mutator_name(nt_str(nt, q, "name"))) return 1;
    }
  }
  return 0;
}
/* The answer the poly-receiver section reaches, filtered through the question
   above. Every concrete type that section returns becomes the C type of the ONE
   temp the dispatch accumulates into, and a user class's arm writes its own
   return into that same temp -- so a disagreeing return reads back through the
   wrong union member. `Chain#join` answering a Chain came back through the
   builtin's `const char *` as `.v.s` and printed "" (#4083); `Tags#include?`
   answering an index came back through an sp_bool (#4072). Three arms in that
   section had thought to ask; the rest had not, which is why the answer
   depended on which name you picked. Ask once, on the way out. */
static TyKind an_poly_concrete(Compiler *c, const char *name, TyKind t) {
  if (t == TY_POLY || t == TY_UNKNOWN || !name) return t;
  /* The builtin-only re-derivation asks what this call would be if NO user
     class owned the name, so the union with a user return is the one thing it
     must not take: the dispatch records that answer to shape its BUILTIN arm,
     and a poly there made the arm assign sp_poly_values()'s raw sp_PolyArray *
     into the boxed slot the user arms need. */
  if (an_builtin_only) return t;
  /* the classes answering the name, memoized per name: this ran
     comp_method_in_chain over every class on every poly call (#4965) */
  int npc = 0;
  const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
  for (int pi = 0; pi < npc; pi++) {
    int k = pcs[pi].cls;
    /* A native class's methods are its declared bindings, and that is the
       rule the poly dispatch counts candidates by (codegen's iocand loop,
       #4474): a Ruby-side def on one -- IO::Buffer#read over an IO -- is no
       dispatch arm, so its return must not widen the builtin answer either.
       This loop was the one place still counting them after 97589546 changed
       the codegen half: `[$stdin, nil][0].read(4)` was typed poly here while
       the poly-IO arm answered its raw const char *, and the C did not
       build. an_user_read_ty and the dispatch union already skip them. */
    if (c->classes[k].is_native_class) continue;
    int mi = pcs[pi].mi;
    if (mi < 0 || mi >= c->nscopes) continue;
    TyKind r = (TyKind)c->scopes[mi].ret;
    if (r == t || r == TY_UNKNOWN || r == TY_VOID) continue;
    /* A nil answer FITS a slot that has a nil of its own -- a NULL array is
       nil, and that is exactly why the array trio may stay concrete (#3461).
       Widening those would leave the builtin arm's sp_PolyArray * meeting an
       sp_RbVal, which is the shape #3461 fixed. */
    if (r == TY_NIL && an_ty_holds_nil(t)) continue;
    return TY_POLY;
  }
  return t;
}

int an_bare_call_class_owned(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "receiver") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  Scope *sc = comp_scope_of(c, id);
  int cid = sc ? sc->class_id : -1;
  if (cid < 0 || cid >= c->nclasses) return 0;
  if (sc->is_cmethod) return comp_cmethod_in_chain(c, cid, name, NULL) >= 0;
  return comp_method_in_chain(c, cid, name, NULL) >= 0 ||
         comp_reader_in_chain(c, cid, name, NULL);
}

static TyKind infer_call_inner(Compiler *c, int id);

/* A builtin call whose count the arity guard refuses raises before it
   answers, so any type is sound for it; nil lets the shapes that need one
   -- a send candidate, which is dropped when it types unknown, an argument
   -- compile down to the raise instead of a miss. The guard runs after a
   few arms of emit_call, so none of those may claim a call it refuses, or
   the value typed here is not the one emitted. */
/* The type of an implicit-self call to `name` bound to `mi` in scope `self`:
   the method's return unified with every descendant's direct override,
   since codegen's dispatch switches over them all. */
static TyKind an_self_call_ret(Compiler *c, Scope *self, const char *name, int mi, int id) {
  TyKind r = an_user_call(c, id, mi, c->scopes[mi].is_cmethod ? UC_CMETH : UC_INST, self->class_id);
  return dispatch_ret_over(c, self->class_id, name, self->is_cmethod, mi, r, id);
}
/* `k[]` or `k[1, 2, 3]` on a boxed receiver: no builtin `[]` takes that
   many arguments, so the call was left untyped; a Struct or Data class
   value takes it as `new`, which the call's emitter branches to
   (emit_boxed_class_aref), so in a program with such a class the call's
   value is boxed. */
static int boxed_struct_aref_may_construct(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  if (!nm || !sp_streq(nm, "[]") || recv < 0 || comp_ntype(c, recv) != TY_POLY) return 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *k = &c->classes[ci];
    if (!k->is_struct || is_builtin_reopen(k->name) || k->is_native_class) continue;
    if (comp_cmethod_in_chain(c, ci, "[]", NULL) >= 0 ||
        comp_cmethod_in_chain(c, ci, "new", NULL) >= 0) continue;
    return 1;
  }
  return 0;
}

/* Whether `call`, a builtin on a yield of a method inlined per call site,
   has an answer in ty_recv_builtin_result for every site's block kind: the
   condition under which the yield is widened to poly (YU_RECEIVER below) and
   each site's call is typed from that table at codegen
   (yield_builtin_method_site_type). A proc-form or lowered method runs one
   body for every site, with no per-site typing to lean on. */
/* The yield at the root of a chain of such builtins (`yield.reverse.first`),
   or -1. Each link must be a block-less call. */
static int yield_recv_chain_root(const NodeTable *nt, int call) {
  int depth = 0;
  for (int n = call; n >= 0 && depth < 16; depth++) {
    if (nt_kind(nt, n) == NK_YieldNode) return n;
    if (nt_kind(nt, n) != NK_CallNode || nt_ref(nt, n, "block") >= 0) return -1;
    n = nt_ref(nt, n, "receiver");
  }
  return -1;
}
/* What the chain from the yield up to `node` answers at a site whose block
   answers `bt`, by ty_recv_builtin_result one link at a time. */
static int yield_recv_chain_kind(Compiler *c, int node, TyKind bt, TyKind *out) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, node) == NK_YieldNode) { *out = bt; return 1; }
  TyKind rk;
  if (!yield_recv_chain_kind(c, nt_ref(nt, node, "receiver"), bt, &rk)) return 0;
  int an = nt_ref(nt, node, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  TyKind a0 = ac == 1 && av ? comp_ntype(c, av[0]) : TY_UNKNOWN;
  /* The table names the builtin's answer, and a program that reopened the
     kind's class with its own method of the name runs that one instead.
     That reopen is what codegen calls for such a site, so the site answers
     its return type. An Array's or Hash's reopen of a builtin name is
     dispatched too since #7078; declining it here, as before that, left the
     yield typed as one site, and `yield.first` over a Hash reopen and an
     Array block wrote the Array's element into the reopen's Symbol slot (a
     C compile error). */
  { const char *rn = nt_str(nt, node, "name");
    /* a call an alias captured the builtin for (builtin_only) runs the builtin */
    int rmi = nt_int(nt, node, "builtin_only", 0) ? -1 : comp_builtin_kind_reopen_mi(c, rk, rn);
    if (rmi >= 0) {
      /* still settling in an early round is not a reason to decline: the
         site is answered by the reopen either way, and codegen reads the
         settled return */
      TyKind rr = method_call_ret(c, rmi, node);
      *out = (rr == TY_UNKNOWN || rr == TY_VOID) ? TY_POLY : rr;
      return 1;
    } }
  return ty_recv_builtin_result(nt_str(nt, node, "name"), ac, a0, rk, out);
}
static int yield_recv_builtin_every_site(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, call) != NK_CallNode) return 0;
  int y = yield_recv_chain_root(nt, call);
  if (y < 0 || y == call) return 0;
  Scope *ys = comp_scope_of(c, y);
  if (!ys || ys->is_proc_form || ys->is_lowered_yield) return 0;
  int ymi = (int)(ys - c->scopes);
  int tails[32], any = 0;
  int nsite = yield_block_tails(c, ymi, tails, 32);
  for (int s = 0; s < nsite; s++) {
    TyKind bt = tails[s] >= 0 ? infer_type(c, tails[s]) : TY_NIL, rr;
    if (bt == TY_UNKNOWN) continue;
    any = 1;
    if (!yield_recv_chain_kind(c, call, bt, &rr)) return 0;
  }
  return any;
}

TyKind infer_call(Compiler *c, int id) {
  /* the class arm of a builtin's receiver test (`__r.is_a?(K) ? __r.m { }
     : ...`, enum_own) answers what the classes' own methods answer, as the
     emitter dispatches it: typed as the builtin (any? is a Boolean), a
     truthy String from `def any? = @v` read as false (#4952) */
  if (nt_int(c->nt, id, "enum_own", 0)) {
    const char *on = nt_str(c->nt, id, "name");
    TyKind r = TY_UNKNOWN; int pending = 0;
    for (int k = 0; on && k < c->nclasses; k++) {
      if (c->classes[k].is_native_class) continue;
      int mi = comp_method_in_class(c, k, on);
      if (mi < 0) continue;
      if (c->scopes[mi].ret == TY_UNKNOWN) { pending = 1; continue; }
      r = r == TY_UNKNOWN ? (TyKind)c->scopes[mi].ret : ty_unify(r, (TyKind)c->scopes[mi].ret);
    }
    if (pending) return TY_UNKNOWN;
    if (r != TY_UNKNOWN) {
      /* the arm shares its slot with the builtin's; a class answering what
         the builtin would keeps that type, any other answer rides boxed */
      TyKind bt = infer_call_inner(c, id);
      return (bt == r || bt == TY_UNKNOWN) ? r : TY_POLY;
    }
  }
  TyKind t = infer_call_inner(c, id);
  if (t == TY_UNKNOWN && boxed_struct_aref_may_construct(c, id)) return TY_POLY;
  if (t == TY_UNKNOWN && builtin_arity_violation(c, id)) return TY_NIL;
  return t;
}

/* The call's trailing keyword hash says `exception: false` (the literal:
   the form that asks for nil instead of a raise). */
static int kconv_noraise_kw(Compiler *c, int argc, const int *argv) {
  if (argc < 1) return 0;
  int kw = argv[argc - 1];
  const char *kt = nt_type(c->nt, kw);
  if (!kt || !sp_streq(kt, "KeywordHashNode")) return 0;
  int en = 0; const int *el = nt_arr(c->nt, kw, "elements", &en);
  for (int e = 0; e < en; e++) {
    int k = nt_ref(c->nt, el[e], "key"), v = nt_ref(c->nt, el[e], "value");
    const char *kn = (k >= 0 && nt_type(c->nt, k) && sp_streq(nt_type(c->nt, k), "SymbolNode"))
                     ? nt_str(c->nt, k, "value") : NULL;
    const char *vt = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (kn && sp_streq(kn, "exception") && vt && sp_streq(vt, "FalseNode")) return 1;
  }
  return 0;
}
/* Kernel#Integer's static kind. Integer, unless the argument is a user
   object whose #to_int or #to_i is typed as answering a Bignum: then the
   answer is carried, not truncated, in a Bignum slot for the strict form
   (which never answers nil; a Bignum slot holds the small values too) and
   in a boxed slot for the `exception: false` form, whose nil a Bignum slot
   cannot hold. A method whose answer the analysis cannot pin keeps the
   Integer slot in the strict form, where a Bignum answer is a loud
   RangeError at run time: typing it as a Bignum would refuse to build every
   site that needs an Integer, a Range bound for one. The `exception: false`
   form of such a call is boxed already, so it carries the answer. */
static TyKind kconv_integer_kind(Compiler *c, int arg, int noraise) {
  static const char *const names[] = { "to_int", "to_i" };
  TyKind at = infer_type(c, arg);
  /* promote mode reads a String too wide for sp_int as a Bignum
     (sp_str_to_i_promote) */
  if (g_promote_mode && at == TY_STRING) return TY_POLY;
  if (!ty_is_object(at)) return TY_INT;
  for (int k = 0; k < 2; k++) {
    int mi = comp_method_in_chain(c, ty_object_class(at), names[k], NULL);
    /* the same admission as conv_bridge_callee's: a method with parameters
       or one that yields has no bridge row, so it must not type the slot */
    if (mi < 0 || c->scopes[mi].nparams != 0 || c->scopes[mi].yields) continue;
    if (c->scopes[mi].ret == TY_BIGINT) return noraise ? TY_POLY : TY_BIGINT;
    if (c->scopes[mi].ret == TY_POLY && noraise) return TY_POLY;
  }
  return TY_INT;
}

/* What Kernel#Hash answers for the argument `arg`, called bare or on a
   receiver that takes it as Kernel's. */
static TyKind kconv_hash_kind(Compiler *c, int arg) {
  TyKind at = infer_type(c, arg);
  if (ty_is_hash(at)) return at;                 /* Hash(hash) -> the hash */
  /* an object answers through its own #to_hash (#3721) */
  if (ty_is_object(at)) {
    int hci2 = ty_object_class(at), hmi = comp_method_in_chain(c, hci2, "to_hash", NULL);
    if (hmi >= 0) return (TyKind)c->scopes[hmi].ret;
  }
  if (at == TY_POLY) return TY_POLY;             /* nil-or-hash decided at runtime */
  return TY_POLY_POLY_HASH;                      /* Hash(nil) / Hash([]) -> {} */
}

static int an_reopen_method(Compiler *c, const char *cls, const char *name) {
  int oc_ci = comp_class_index(c, cls);
  return oc_ci >= 0 ? comp_method_in_chain(c, oc_ci, name, NULL) : -1;
}

int class_has_subclass(Compiler *c, int ocid);

static int sg_accessor_type(Compiler *c, int ci, const char *name, TyKind *out) {
  ClassInfo *cls = &c->classes[ci];
  int nlen = (int)strlen(name);
  char base[256];
  const char *rn;
  /* setter: name ends with '=' */
  if (nlen > 1 && name[nlen - 1] == '=') {
    int blen = nlen - 1;
    if (blen >= (int)sizeof base) return 0;
    memcpy(base, name, (size_t)blen); base[blen] = '\0';
    if (!comp_is_sg_writer(cls, base)) return 0;
    rn = base;
  }
  else {
    /* an accessor backed by the class-level ivar reads that slot's type;
       the alias table maps a renamed accessor onto it (#3776) */
    rn = comp_resolve_alias(c, ci, name);
    if (!rn) rn = name;
    if (!comp_is_sg_reader(cls, rn)) return 0;
  }
  *out = TY_POLY;
  if (comp_is_sg_civ(cls, rn)) {
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn);
    int ivi = comp_ivar_index(cls, ivn);
    if (ivi >= 0 && cls->ivar_types[ivi] != TY_UNKNOWN) *out = ivar_value_ty(cls, ivi);
  }
  return 1;
}

/* A program's own Object method on a builtin receiver whose class has no
   method of that name: CRuby's lookup reaches Object after the class itself,
   and the emitter's reopened-Object fallback calls it on the boxed receiver.
   The Time, File and Float-range arms end in a catch-all (Integer, poly,
   unknown) that answered first, so `(Time.at(100) - 100).me` with
   `class Object; def me = self; end` was an int slot around a call answering
   a boxed value: invalid C, or `==` refused. A name the builtin class or
   Object's own surface has stays theirs, as the emitter keeps it. Returns 1
   with the type through `out`. */
int object_reopen_answers(Compiler *c, const char *cls, int call_id, TyKind *out) {
  const char *name = nt_str(c->nt, call_id, "name");
  if (!name || builtin_method_known(cls, name) || builtin_object_method_known(name)) return 0;
  int oci = comp_class_index(c, "Object");
  int omi = oci >= 0 ? comp_method_in_chain(c, oci, name, NULL) : -1;
  if (omi < 0) return 0;
  /* a yielding one given a block answers what its proc form does, a boxed
     value (#5779), as the general Object fallback says */
  *out = c->scopes[omi].yields && nt_ref(c->nt, call_id, "block") >= 0
         ? TY_POLY : an_user_call(c, call_id, omi, UC_REOPEN, oci);
  return 1;
}

static int an_user_poly_arm(Compiler *c, const char *name, int argc) {
  if (an_builtin_only) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (comp_poly_arm_defines_n(c, k, name, argc)) return 1;
  return 0;
}

void an_user_call_record(Compiler *c, int id, int mi, int via, int owner_ci) {
  if (!g_plan_check || an_builtin_only || id < 0 || id >= c->node_cap) return;
  c->ucall_inf[id].mi = mi;
  c->ucall_inf[id].owner_ci = (short)owner_ci;
  c->ucall_inf[id].via = (unsigned char)via;
}

TyKind an_user_call(Compiler *c, int id, int mi, int via, int owner_ci) {
  an_user_call_record(c, id, mi, via, owner_ci);
  return method_call_ret(c, mi, id);
}

const BuiltinOp *an_bop_find(Compiler *c, int id, TyKind rt, const char *name,
                             int argc, int has_block) {
  const BuiltinOp *op = bop_find(rt, name, argc, has_block);
  if (g_plan_check && !an_builtin_only && op && op->result != TY_UNKNOWN &&
      id >= 0 && id < c->node_cap)
    c->bop_inf[id] = op;
  return op;
}

/* A boxed receiver or a boxed first operand (infer_call_inner's rules, in their order) */
static int infer_poly_operand_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, TyKind *out) {
  /* poly receiver / poly operand: result type of operations on sp_RbVal */
  if (recv >= 0 && (rt == TY_POLY || a0 == TY_POLY)) {
    /* array * n is repetition (yielding the same array type), not poly
       arithmetic, even when the count `n` widened to poly under promote. */
    if ((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "*") && argc == 1)
      { *out = rt; return 1; }
    /* `arr - x` / `arr & x` / `arr | x` with a poly operand are SET operations,
       not arithmetic. codegen coerces the operand at run time -- an Array
       becomes a poly array, anything else raises CRuby's TypeError -- so the
       result is a poly array. Typed as arithmetic instead, `-` reached
       sp_poly_sub, which had no array case and answered "no implicit
       conversion of Array into Array" on two real Arrays (#3475). */
    if ((ty_is_array(rt) || rt == TY_POLY_ARRAY) && argc == 1 &&
        is_set_op(name))
      { *out = TY_POLY_ARRAY; return 1; }
    /* String operators with a poly operand are NOT poly arithmetic: `str % x`
       is printf formatting, `str + x` is concatenation, `str * n` is repeat --
       all yield a string. Defer them to the rt==TY_STRING path below. */
    if (!(rt == TY_STRING && (sp_streq(name, "%") || sp_streq(name, "+") || sp_streq(name, "*"))) &&
        is_arith_op(name))
      { *out = TY_POLY; return 1; }
    /* unary numeric operators on a poly receiver: negation/unary-plus stay
       poly, bitwise complement yields int. Resolve them here so the poly
       method-dispatch below does not bind `-@`/`+@` to a user class that
       happens to define one (e.g. `-@cents` with @cents widened to poly must
       not infer the enclosing Money type). */
    if (argc == 0 && (sp_streq(name, "-@") || sp_streq(name, "+@"))) { *out = TY_POLY; return 1; }
    if (argc == 0 && sp_streq(name, "~")) { *out = TY_INT; return 1; }
    if ((sp_streq(name, "include?") || sp_streq(name, "member?")) &&
        an_user_ret_disagrees(c, name, TY_BOOL))
      { *out = TY_POLY; return 1; }   /* the user arm answers something a bool cannot hold */
    /* a user comparison operator answering something other than a bool
       (ruby-vips' Image#> builds an image) makes the boxed call's answer
       that value, not the builtin comparison's bool */
    if (is_cmp_op(name)) {
      for (int k = 0; k < c->nclasses; k++) {
        if (c->classes[k].is_native_class) continue;
        int mi = comp_method_in_chain(c, k, name, NULL);
        if (mi < 0 || mi >= c->nscopes) continue;
        TyKind ur = (TyKind)c->scopes[mi].ret;
        if (ty_is_object(ur) || ur == TY_POLY) { *out = TY_POLY; return 1; }
      }
    }
    if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
        sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "include?"))
      { *out = TY_BOOL; return 1; }
    if (rt == TY_POLY) {
      /* A container read the poly dispatch may have to serve from a builtin
         Array or Hash: record what the builtin surface alone would answer, so
         codegen can shape that arm (its emitters read the node's own type, and
         the node here holds the union). Computed once per node. */
      /* The String surface needs the same record, for the same reason: a
         String reaching the dispatch because a user class owns the name is
         served by re-entering the emission, whose answer is a raw `const
         char *` for half these names and a boxed value for the other half
         (#4816). */
      if (recv >= 0 && !an_builtin_only &&
          (poly_container_read_p(name) || poly_string_read_p(name)) &&
          nt_ref(nt, id, "block") < 0 && c->poly_builtin_ty &&
          id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN &&
          an_user_defines_or_reads(c, name)) {
        TyKind bt = an_builtin_answer(c, id);
        c->poly_builtin_ty[id] = bt;
      }
      if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2) { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      /* a numeric argument makes it Thread#join(limit), whose answer is the
         thread or nil, not a joined string (#4287) */
      if (sp_streq(name, "join") && argc == 1 && ty_is_numeric(infer_type(c, argv[0])))
        { *out = TY_POLY; return 1; }
      /* in a program that spawns threads the receiver may be one, and
         Thread#join answers the thread: the slot has to hold either */
      if (sp_streq(name, "join") && argc == 0 && an_program_spawns_threads(c))
        { *out = TY_POLY; return 1; }
      if (sp_streq(name, "join")) { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      /* The multi-set forms of String#count/#delete/#squeeze, and
         Hash#store, on a boxed value: the runtime helper answers the
         string's (or the hash's) own result, and raises NoMethodError for
         anything else, so the type is the string form's (#4195). delete's
         single-set form has its own rules. */
      if (argc >= 1 && nt_ref(nt, id, "block") < 0) {
        if (argc >= 2 && sp_streq(name, "count")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
        if ((argc >= 2 && sp_streq(name, "delete")) || sp_streq(name, "squeeze"))
          { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
        if (argc == 2 && sp_streq(name, "store")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
        /* count(v): the value-equality count (sp_poly_count_val). A user
           definition blocks it only when it can TAKE one positional
           argument -- the same judgement codegen's arm makes; an
           arity-incompatible `count(a, b)` cannot answer this call, and
           counting it left the two halves naming different methods. */
        if (sp_streq(name, "count")) {
          int can1 = 0;
          for (int k2 = 0; k2 < c->nclasses && !can1; k2++) {
            if (c->classes[k2].is_native_class) {   /* bindings only (#4504) */
              if (comp_poly_arm_defines_n(c, k2, "count", 1)) can1 = 1;
              continue;
            }
            int mi2 = comp_method_in_chain(c, k2, "count", NULL);
            if (mi2 >= 0 && mi2 < c->nscopes) {
              Scope *cs2 = &c->scopes[mi2];
              if (cs2->rest_idx >= 0 || (1 >= cs2->nrequired && 1 <= cs2->nparams))
                can1 = 1;
            }
          }
          if (!can1) { *out = TY_INT; return 1; }
        }
      }
      /* A length-like read answers an Integer -- unless a user class owns the
         name and answers something else, in which case the call's value is
         that union. The dispatch ALWAYS emits the builtin length arms (a
         symbol, a string, every array and hash kind), so pinning the call to
         Integer left their sp_int and the user arm's own return meeting in one
         slot, and the build stopped. Only a DISAGREEING user return widens it:
         one that answers an Integer already agrees, and widening every
         `poly.size` would box a count the whole program reads as a number. */
      if (sp_streq(name, "to_i") || sp_streq(name, "length") || sp_streq(name, "size")) {
        TyKind ur = an_user_read_ty(c, name, argc);
        if (sp_streq(name, "to_i") || ur == TY_UNKNOWN || ur == TY_INT || ur == TY_VOID)
          { *out = an_poly_concrete(c, name, TY_INT); return 1; }
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      if (sp_streq(name, "to_f")) { *out = an_poly_concrete(c, name, TY_FLOAT); return 1; }
      /* Hash#keys / #values on a poly hash -> a poly array (boxed elements).
         to_a on a poly value follows the same rule: nil -> [], arrays and
         hashes materialize, anything else raises (sp_poly_to_a_arr). A user
         class owning the name is handled by the container-union rule at the
         user lookup below. */
      if ((sp_streq(name, "keys") || sp_streq(name, "values") ||
           sp_streq(name, "to_a")) && argc == 0) {
        if (!an_user_poly_arm(c, name, argc)) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      }
      if (sp_streq(name, "clamp")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }  /* boxed numeric clamp -> poly */
      /* a boxed Encoding value, as the concrete String arm answers. Without a
         type the call is UNKNOWN, and emit_boxed's untyped arm renders
         `(expr, sp_box_nil())` -- it evaluates the answer and throws it away. */
      if (sp_streq(name, "encoding") && argc == 0) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* nil-aware conversions (a nil local widens to poly): boxed results */
      if (nt_ref(nt, id, "block") < 0 &&
          ((argc == 0 && (sp_streq(name, "to_a") || sp_streq(name, "to_h") ||
                          sp_streq(name, "to_r"))) ||
           (argc <= 1 && sp_streq(name, "rationalize")))) {   /* an optional epsilon */
        if (!an_user_poly_arm(c, name, argc)) {
          /* concrete result types, matching the TY_NIL receiver arm so a
             local settled on an early (pre-widening) pass stays consistent */
          if (sp_streq(name, "to_a")) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
          /* Hash#to_h is the identity, so a boxed receiver keeps whatever
             variant it really holds: narrowing to the symbol-keyed one made
             `opts.to_h` on a String-keyed hash raise (#3972). nil.to_h is the
             empty hash, which the boxed answer covers too. */
          if (sp_streq(name, "to_h")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
          { *out = an_poly_concrete(c, name, TY_RATIONAL); return 1; }   /* to_r, rationalize */
        }
      }
      /* to_h with a block (a proc argument too): the pairs it answers, boxed */
      if (argc == 0 && sp_streq(name, "to_h") && nt_ref(nt, id, "block") >= 0 &&
          !an_user_poly_arm(c, name, argc))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (argc == 1 && sp_streq(name, "===")) {
        int has_user = an_user_poly_arm(c, name, argc);
        /* A boxed receiver can be a Proc, whose #=== answers the proc's
           return value rather than a boolean (#3818); a poly slot holds the
           booleans every other kind answers just as well. */
        if (!has_user) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      /* & | ^ on a poly receiver dispatch on the runtime tag (nil/bool take
         the boolean ops, ints the bitwise ones) via sp_poly_bitop, whose
         result is a boxed value -- so the static type stays poly (#2401). */
      if (argc == 1 && is_bit_op(name))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* parameters(lambda: true/false/nil), read by proc_parameters_lambda_mode,
         is an Array, as the no-argument call is, unless a class method of the
         name exists (a boxed Class may be the receiver) */
      if (sp_streq(name, "parameters") && proc_parameters_lambda_mode(nt, argc, argv) != -2) {
        int pcm = 0;
        for (int k = 0; k < c->nclasses && !pcm; k++)
          if (comp_cmethod_in_chain(c, k, name, NULL) >= 0) pcm = 1;
        if (!pcm) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      }
      /* poly.arity on a Method read out of a container: the stamped arity, an
         Integer (#3231). */
      if (argc == 0 && sp_streq(name, "arity")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      /* the rest of the Proc face on a value read out of a container (#3685) */
      if (argc == 0 && sp_streq(name, "lambda?")) { *out = an_poly_concrete(c, name, TY_BOOL); return 1; }
      if (argc == 0 && sp_streq(name, "parameters")) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      if (argc == 0 && sp_streq(name, "curry")) { *out = an_poly_concrete(c, name, TY_CURRY); return 1; }
      /* the count form too, but not where a reopened Object or Kernel has a
         curry, which answers for every receiver the Proc arm does not */
      if (argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode && sp_streq(name, "curry") &&
          !(comp_class_index(c, "Object") >= 0 &&
            comp_method_in_chain(c, comp_class_index(c, "Object"), name, NULL) >= 0) &&
          !(comp_class_index(c, "Kernel") >= 0 &&
            comp_method_in_chain(c, comp_class_index(c, "Kernel"), name, NULL) >= 0))
        { *out = an_poly_concrete(c, name, TY_CURRY); return 1; }
      if (argc == 0 && sp_streq(name, "to_proc")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* Integer#chr(Encoding::X) on a boxed Integer (emit_poly_call) */
      if (argc == 1 && sp_streq(name, "chr") && nt_type(nt, argv[0]) &&
          sp_streq(nt_type(nt, argv[0]), "ConstantPathNode"))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* String transforms on a boxed value: emit_poly_call routes these
         through sp_poly_to_s and re-boxes the result, so the value stays
         poly (mirrors the codegen list in codegen_call_recv.c). */
      if (argc == 0 &&
          (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
           sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
           sp_streq(name, "strip") || sp_streq(name, "reverse") ||
           sp_streq(name, "chomp") || sp_streq(name, "chop") ||
           sp_streq(name, "succ") || sp_streq(name, "next") ||
           sp_streq(name, "chr") ||
           /* `strip` was here and its one-sided siblings were not, which is
              what most of this line is: a String reaching the dispatch
              through a poly slot answered NoMethodError for a method String
              has. `to_str` matters most of them -- it is the implicit
              conversion protocol, so a poly slot holding a String has to
              answer it. The answers stay boxed, which is why they are POLY
              here: `ascii_only?` is a boxed boolean and reads as one. */
           sp_streq(name, "lstrip") || sp_streq(name, "rstrip") ||
           sp_streq(name, "to_str") || sp_streq(name, "ascii_only?") ||
           sp_streq(name, "ascii_compatible?") || sp_streq(name, "dummy?") ||
           sp_streq(name, "valid_encoding?") || sp_streq(name, "encode") ||
           sp_streq(name, "scrub") || sp_streq(name, "b")))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* ...and the same names where they take arguments. unpack answers a
         boxed array, byteslice a boxed String or nil; the codegen arm for
         these sits outside its argc==0 block, and this mirrors it. */
      if ((sp_streq(name, "unpack") && argc == 1) ||
          (sp_streq(name, "byteslice") && (argc == 1 || argc == 2)) ||
          (sp_streq(name, "scrub") && argc == 1) ||
          (sp_streq(name, "encode") && argc >= 1 && argc <= 3) ||
          ((sp_streq(name, "force_encoding") || sp_streq(name, "encode!")) && argc >= 1 && argc <= 2))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* chomp / chop / delete_prefix / delete_suffix answer a String and are
         served at argc 0 only, so the separator forms -- `line.chomp("|")`,
         which is what a line reader does with its own separator -- fell to
         NoMethodError on a boxed receiver with a clean C build. */
      if (argc == 1 && (sp_streq(name, "chomp") || sp_streq(name, "delete_prefix") ||
                        sp_streq(name, "delete_suffix")))
        { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      /* poly.ljust/rjust/center(width[, pad]): a String read from a container
         widened to poly; emit_poly_call pads via sp_poly_to_s and re-boxes, so
         the result stays poly (#3222). */
      if ((sp_streq(name, "ljust") || sp_streq(name, "rjust") || sp_streq(name, "center")) &&
          (argc == 1 || argc == 2))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* poly.bytes / poly.codepoints on a value that is really a String (a
         binary lump read whose method widened to poly): a concrete int array,
         emitted via sp_str_bytes(sp_poly_to_s(...)) with no boxing. */
      if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
          nt_ref(nt, id, "block") < 0) {
        if (!an_user_defines_or_reads(c, name)) { *out = an_poly_concrete(c, name, TY_INT_ARRAY); return 1; }
        /* A user class owns the name too, so the value may be a String (an int
           array) or that class's member (whatever it holds). One C slot cannot
           be both, and letting the member's type win made the String answer 0.
           Box it: the codegen's tag pre-arm fills either side (#3380). */
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      /* poly.unpack1(fmt): String#unpack1 on a value that widened to poly
         (pervasive in doom's binary WAD parsing). Mirrors the rt==TY_STRING
         rule so a single-directive int format stays int, not poly. */
      if (sp_streq(name, "unpack1") && argc == 1) {
        /* a user class owning the name puts its own arm in the dispatch, and
           both arms share one C temp: type the call for what both can hold */
        TyKind u1 = an_unpack1_lit_type(nt, argv[0]);
        { *out = an_user_ret_disagrees(c, name, u1) ? TY_POLY : u1; return 1; }
      }
      /* poly.delete(chars): String#delete on a value that widened to poly
         (`data[offset, 8].delete("\x00").upcase` stripping NUL padding off a
         fixed-width WAD name field in doom's texture parser). Resolve it here
         so the poly method-dispatch below does not bind `delete` to whatever
         user class happens to define one: the receiver can still be a string,
         so a user-class `delete` (e.g. the bundled Set's) unifies WITH
         TY_STRING (-> poly) instead of replacing it. No user class keeps the
         concrete TY_STRING, like the rt==TY_STRING rule. */
      if (sp_streq(name, "delete") && argc == 1) {
        /* The answer is whatever the receiver's own kind returns, so it is
           boxed: a Hash gives the deleted value, an Array the object, a
           String the stripped copy. Committing to TY_STRING here made the
           emitter commit to String#delete, which stringified a Hash receiver
           and answered a substring of its inspect text (#3806). The container
           check below stays as documentation of the same conclusion. */
        (void)poly_expr_flows_container;
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      if (sp_streq(name, "[]") && argc == 1) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }  /* boxed array element access */
      if (sp_streq(name, "[]") && argc == 2) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }  /* 2-arg poly slice */
      /* fetch on a poly Hash yields a boxed (poly) value, like `[]` -- the
         hash-value type is not statically known through the poly widening. Type
         it here so the boxed dispatch result is not discarded as nil (without
         this, `fetch` fell through to the non-hash `fetch(k, default)` rule or
         to nil, and its value-position result was dropped). */
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* `x = v` through a writer on a poly receiver is the assigned value as
         written, like `[]=` below: the dispatch calls the writer for effect
         and yields the argument's own temp, so no arm's return widens it.
         Only when some class has the writer -- otherwise the call is the
         NoMethodError the dispatch raises. */
      if (argc == 1 && name_is_plain_setter(name) && nt_ref(nt, id, "block") < 0) {
        int has_def = 0, has_writer = 0;
        char sbase[256];
        int has_base = setter_base_name(name, sbase, sizeof sbase);
        for (int k = 0; k < c->nclasses && !(has_def && has_writer); k++) {
          if (comp_poly_arm_defines_n(c, k, name, argc)) has_def = 1;
          if (has_base && !c->classes[k].is_native_class &&
              comp_writer_in_chain(c, k, sbase, NULL)) has_writer = 1;
        }
        /* A call the send desugar retargeted (`obj.send(:x=, v)`, and a
           public_send dispatch arm) is a plain call, not an assignment
           (#4921): a hand-written `def x=` answers its body through the
           method dispatch below. An attr writer has no body to answer with
           and stores exactly the argument, so the plain call answers the
           argument as the assignment does: gating it on the assignment shape
           left `d.public_send("silenced=", v)` over a boxed receiver untyped,
           and the dispatch dropped the arm. */
        int owned = call_is_setter_assign(nt, id) ? (has_def || has_writer)
                                                  : (has_writer && !has_def);
        TyKind at = owned ? infer_type(c, argv[0]) : TY_UNKNOWN;
        if (at != TY_UNKNOWN) { *out = at; return 1; }
      }
      /* []= on a poly receiver yields the assigned value, emitted boxed */
      if (sp_streq(name, "[]=") && (argc == 2 || argc == 3)) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (sp_streq(name, "dig") && argc >= 1) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          TyKind et = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
          TyKind bnt = ie_block_break_next_ty(c, body);
          if (bnt != TY_UNKNOWN) et = (et == TY_UNKNOWN) ? bnt : ty_unify(et, bnt);
          { *out = et != TY_UNKNOWN ? ty_array_of(et) : TY_POLY_ARRAY; return 1; }
        }
      }
      /* poly method dispatch: unify the return type over every class that
         defines `name` (the runtime cls_id picks the impl). */
      TyKind r = TY_UNKNOWN; int found = 0, nat_found = 0;
      int npc = 0;
      const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
      char ivn_same[256] = "";   /* "@name" for a reader under its own name */
      for (int pi = 0; pi < npc; pi++) {
        int k = pcs[pi].cls;
        if (an_builtin_only) continue;   /* the builtin answer alone is wanted */
        if (c->classes[k].is_native_class) {
          /* A native class no reachable code constructs is no candidate: its
             binding's return type must not widen the union (a dead FFI
             wrapper's Vector2 put a float `x` beside every int `x`, #4460).
             Native classes only: a user class left out here would type the
             call narrower than the emitters, which still count every user
             candidate, and the two disagreed on three tests. */
          if (!c->classes[k].ctor_reachable) continue;
          /* The lookup's loose fallback answers a same-name binding of ANY
             arity; one that cannot take this call's arguments is no answer to
             it. StringIO's zero-argument `getbyte` typed `s.getbyte(i)` on a
             boxed String as Integer while the emission was the builtin
             sp_poly_getbyte, whose value is boxed (#4432). */
          int nmk = comp_native_method_find(c, k, name, argc, 0);
          if (nmk >= 0 && native_takes(&c->native_methods[nmk], argc)) {
            TyKind nr = sp_streq(c->native_methods[nmk].ret, "self")
                          ? ty_object(k) : native_spec_to_ty(c->native_methods[nmk].ret);
            r = found ? ty_unify(r, nr) : nr; found = 1; nat_found = 1;
          }
          continue;
        }
        int mi = pcs[pi].mi;
        /* A candidate whose own return has not been derived yet contributes
           nothing: "not known yet" is not an answer, and taking it as one is
           permanent. `def zero?; @value.zero?; end` on a union receiver
           resolves to ITSELF -- the only class defining the name -- so the
           union was its own unfinished return and the method came out void,
           answering nil for every receiver. `<=>` did the same and took
           Comparable's operators down with it (#3488, #3490). With the
           candidate skipped the builtin answer for the name applies, and once
           the method's return does settle it unifies in on a later round. */
        if (mi >= 0 && c->scopes[mi].ret == TY_UNKNOWN) continue;
        if (mi >= 0) {
          if (!found) an_user_call_record(c, id, mi, UC_POLY, k);
          r = found ? ty_unify(r, c->scopes[mi].ret) : c->scopes[mi].ret; found = 1; continue;
        }
        int rdcls = pcs[pi].rdcls;
        if (rdcls >= 0) {
          /* resolve alias so `alias_method :required?, :required` reads the
             backing @required, not a bogus @required? */
          const char *rname = comp_resolve_alias(c, k, name);
          /* the "@name" string once per call, not per candidate class: a
             reader shared by K classes formatted it K times per ask
             (rubys in #5035); an alias names another ivar */
          char ivn[256];
          if (rname != name || !ivn_same[0]) {
            snprintf(ivn, sizeof ivn, "@%s", rname);
            if (rname == name) memcpy(ivn_same, ivn, sizeof ivn_same);
          }
          else memcpy(ivn, ivn_same, sizeof ivn);
          int iv = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind rt2 = iv >= 0 ? ivar_value_ty(&c->classes[rdcls], iv) : TY_UNKNOWN;
          r = found ? ty_unify(r, rt2) : rt2; found = 1;
        }
      }
      /* The dispatch also answers a receiver that is a Class through the class
         methods of the name (the SP_TAG_CLASS arms), into the same result
         slot: their returns join the union. Typed from the instance methods
         alone, `URI::HTTP.build` returned an object into another class's nil
         slot (#4930). */
      if (found && !an_builtin_only) {
        int ncc = 0;
        const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
        for (int ki = 0; ki < ncc; ki++) {
          int cmi = ccs[ki].mi;
          if (cmi >= 0 && cmi < c->nscopes && c->scopes[cmi].ret != TY_UNKNOWN)
            r = ty_unify(r, (TyKind)c->scopes[cmi].ret);
        }
      }
      /* The receiver is a union. When it provably carries a builtin Array or
         Hash (see infer_container_flow), a container read's value is the user
         return OR the builtin answer -- pinning it to the user return left the
         dispatch no room for the builtin arm, which then raised NoMethodError
         on a genuine Array or Hash (#3459). Poly holds both. Without the
         evidence the slot is a user object the fixpoint has not settled yet,
         and widening it there poisons the class (Set's own @data).
         The builtin answer's own shape is recorded for codegen, which emits
         the builtin arm by re-entering the ordinary emission and has to know
         whether that produces an array pointer or a boxed value. */
      if (found && !an_builtin_only && poly_container_read_p(name) &&
          nt_ref(nt, id, "block") < 0 && poly_expr_flows_container(c, recv)) {
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      /* Nor do the blockless ENUMERATOR producers: a boxed receiver can always
         be an Array, which answers them with an Enumerator. A Struct gets one
         of these synthesized, so any program with a Struct in it typed
         `arr.each_with_index` as that Struct (#4021). */
      if (found && !an_builtin_only && argc == 0 && nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "each_with_index") || sp_streq(name, "each_index"))) {
        /* ... but the user arm is in the dispatch too, and both arms share one
           C temp. Typed from the builtin alone, its answer was crammed into an
           sp_Enumerator * -- so when the two disagree the call is poly, which
           is the only thing that holds either. */
        if (an_user_ret_disagrees(c, name, TY_ENUMERATOR)) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
        { *out = an_poly_concrete(c, name, TY_ENUMERATOR); return 1; }
      }
      /* `merge` needs no container precondition either: a boxed receiver can
         always be a Hash, and the dispatch ends in the runtime merge that lets
         it answer for itself. Typed from the user arm alone the switch and the
         slot disagreed -- and where that arm was ALSO dropped as
         type-incompatible, the switch came out empty and the call quietly
         answered its zero initializer (#4033). */
      if (found && !an_builtin_only && sp_streq(name, "merge") &&
          nt_ref(nt, id, "block") < 0 && argc >= 1)
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* the numeric surface needs no container precondition: a boxed receiver
         can always be a number (#4012) */
      if (found && !an_builtin_only && argc == 0 && poly_numeric_read_p(name) &&
          nt_ref(nt, id, "block") < 0) {
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      /* A binary operator on a boxed receiver is lowered to sp_poly_<op>,
         whose value is boxed however the runtime dispatches it. Taking the
         user return from this union instead left the type and the emission
         disagreeing, and the two met at the assignment (#3502). */
      if (found && argc == 1 &&
          (is_arith_op(name) || is_int_bit_op(name)))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* The user arms agreed on `r`, and the dispatch writes them into one C
         temp -- but it also emits whatever the BUILTIN surface answers for the
         name, into that same temp. Ask what that would be. `Box#index`
         answering a String pinned the temp to `const char *` while the Array
         and String arms boxed their answers, and the build stopped (#4083).
         The rules above name the cases someone hit one at a time; this asks
         for every name, which is the same question an_poly_concrete asks from
         the other side. */
      /* A user answer that is already poly needs no widening, but the
         dispatch's default arm is still shaped from the builtin answer: not
         recorded, a genuine Hash reaching `slice` beside a NodeSet#slice
         answering an object or an element had no arm and raised (#5114).
         Asked once per node per iteration, like the question below. */
      if (found && !an_builtin_only && r == TY_POLY && recv >= 0 &&
          c->poly_builtin_ty && id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN &&
          (nat_found || an_user_defines_or_reads(c, name))) {
        long pk = narrow_key(4, id, "");
        int phit; (void)narrow_memo_get(pk, &phit);
        if (!phit) {
          TyKind pbt = an_builtin_answer(c, id);
          narrow_memo_put(pk, (int)pbt);
          if (pbt != TY_UNKNOWN && pbt != TY_VOID) c->poly_builtin_ty[id] = pbt;
        }
      }
      if (found && !an_builtin_only && r != TY_POLY && r != TY_UNKNOWN &&
          recv >= 0 && (nat_found || an_user_defines_or_reads(c, name))) {
        /* Asking costs a full re-inference of the call, and the same node is
           asked many times inside one fixpoint iteration: counted on a 51k-line
           Rails emit, 294,164 asks over 18k nodes, 96% of them a repeat of a
           node already asked in that same iteration.  Memoize per node on the
           narrow generation, which is bumped once per iteration and so releases
           every answer when the types move.
           The answer is stable across the repeats but not perfectly: on the same
           tree 2 asks of 87,033 saw it change mid-iteration (TY_ENUMERATOR then
           TY_UNKNOWN, for one name).  The memo pins the first answer, so those
           take the earlier one.  It changed no output on either app measured. */
        long bk = narrow_key(3, id, "");
        int bhit; int bcached = narrow_memo_get(bk, &bhit);
        TyKind bt;
        /* A cached answer is trusted only while it says "no disagreement",
           which is the common case and where the whole win is -- 2 asks of
           87,033 were measured changing mid-iteration, so the rest hit. The
           answer that WIDENS is the consequential one, and the one those two
           were, so confirm it against a fresh ask rather than pinning a stale
           one. Pinning it widened calls that should have stayed typed: a
           concrete object became TY_POLY, its direct call became a runtime
           cls_id switch with a NoMethodError arm, and four rubyspec examples
           went with it (an identity assertion through .equal? cannot survive
           the value boxing). */
        int btrust = bhit && ((TyKind)bcached == TY_UNKNOWN ||
                              (TyKind)bcached == TY_VOID ||
                              (TyKind)bcached == r);
        if (btrust) { bt = (TyKind)bcached; }
        else {
          bt = an_builtin_answer(c, id);
          narrow_memo_put(bk, (int)bt);
        }
        /* Record what the builtin surface alone answers, the way the
           container and String reads above do. It is already computed here
           for every name, and the poly dispatch's default arm needs it to
           shape an arm for a receiver that really is a builtin -- otherwise
           the arm has to be written out by hand, one name at a time, which
           is how the same gap came back three times. Only when it is not
           already set: the reads above ask a narrower question first and
           their answer is the one their arms were built against. */
        if (bt != TY_UNKNOWN && bt != TY_VOID && c->poly_builtin_ty &&
            id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN)
          c->poly_builtin_ty[id] = bt;
        if (bt != TY_UNKNOWN && bt != TY_VOID && bt != r) { *out = TY_POLY; return 1; }
      }
      if (found) { *out = r; return 1; }
      /* Every user method of a container-read name is still unsettled -- one
         in a class nothing constructs never settles, its ivars untyped -- so
         the builtin surface is the only answer there is. Waiting left the
         call untyped for good and a genuine Array's pop was dropped for nil
         (#5099). Poly, so a user answer that settles later still fits and the
         dispatch keeps its container arm. */
      if (!an_builtin_only && npc > 0 && recv >= 0 && poly_container_read_p(name) &&
          nt_ref(nt, id, "block") < 0) {
        TyKind bt = an_builtin_answer(c, id);
        if (bt != TY_UNKNOWN && bt != TY_VOID) {
          if (c->poly_builtin_ty && id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN)
            c->poly_builtin_ty[id] = bt;
          { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
        }
      }
      /* Numeric queries / rounding on a boxed value: the sp_poly_* helpers
         dispatch on the runtime tag (a non-numeric tag raises CRuby's
         NoMethodError). abs keeps the receiver's class and floor/... can
         return a bigint unchanged, so those stay boxed. */
      if (argc == 0) {
        if (sp_streq(name, "nan?") || sp_streq(name, "finite?") ||
            sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
            sp_streq(name, "negative?")) { *out = an_poly_concrete(c, name, TY_BOOL); return 1; }
        /* A class method of the name is a boxed Class's, dispatched on the
           class tag (#3215) beside the Float helper: the call is still Float
           while every such method answers Float (or is not settled yet), and
           poly once one answers something else. The builtin-only derivation,
           which shapes the dispatch's default arm, answers Float. */
        if (sp_streq(name, "next_float") || sp_streq(name, "prev_float")) {
          int cm_other = 0;
          for (int k = 0; k < c->nclasses && !cm_other && !an_builtin_only; k++) {
            int mi = comp_cmethod_in_chain(c, k, name, NULL);
            if (mi >= 0 && c->scopes[mi].ret != TY_FLOAT && c->scopes[mi].ret != TY_UNKNOWN)
              cm_other = 1;
          }
          { *out = an_poly_concrete(c, name, cm_other ? TY_POLY : TY_FLOAT); return 1; }
        }
        if (sp_streq(name, "abs") || sp_streq(name, "infinite?") ||
            sp_streq(name, "floor") || sp_streq(name, "ceil") ||
            sp_streq(name, "round") || sp_streq(name, "truncate") ||
            sp_streq(name, "conjugate") || sp_streq(name, "conj") ||
            sp_streq(name, "abs2") || sp_streq(name, "magnitude") ||
            sp_streq(name, "numerator") || sp_streq(name, "denominator") ||
            sp_streq(name, "nonzero?")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
        if (sp_streq(name, "bytesize") || sp_streq(name, "ord") ||
            sp_streq(name, "bit_length")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
        /* a boxed Range's bound is boxed: an Integer, a Float or nil */
        if (sp_streq(name, "begin") || sp_streq(name, "end")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      }
      /* Numeric#round(ndigits) on a boxed value: Float when n > 0, Integer
         when n <= 0 -- either way a boxed poly (sp_poly_round_n). */
      /* `round(half: :even)` and `round(n, half: :even)` answer a number
         just as the digits-only form does: the trailing keyword hash is the
         tie-break mode, not an argument that makes the call valueless. Typed
         as void, the emitted call was evaluated for effect and its value
         dropped -- `z.round(2, half: :even)` answered nil. */
      if ((argc == 1 || argc == 2) &&
          is_round_family(name) &&
          nt_type(nt, argv[argc - 1]) &&
          sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode"))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (argc == 1 && is_round_family(name))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* divmod answers a pair, modulo and quo a number whose class follows the
         operands. Without a type here the emitted call was evaluated for
         effect and its value dropped (#3512). */
      if (argc == 1 && (sp_streq(name, "divmod") || sp_streq(name, "modulo") ||
                        sp_streq(name, "quo") || sp_streq(name, "div") ||
                        sp_streq(name, "remainder") || sp_streq(name, "coerce")))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* String#getbyte on a boxed value: int byte or nil on out-of-range. */
      if (argc == 1 && sp_streq(name, "getbyte")) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* The count-taking Array reads on a boxed array. Their value is a new
         array; without a rule they typed nil/void and the call emitted as a
         discarded statement (#3464). rotate's count is optional. */
      if ((argc == 1 || (argc == 0 && (sp_streq(name, "rotate") || sp_streq(name, "shuffle")))) &&
          nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "first") || sp_streq(name, "last") ||
           sp_streq(name, "take") || sp_streq(name, "drop") ||
           sp_streq(name, "rotate") || sp_streq(name, "sample") ||
           (sp_streq(name, "shuffle") && argc == 0) ||
           sp_streq(name, "min") || sp_streq(name, "max"))) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_poly_arm_defines_n(c, k, name, argc) ||
              (!c->classes[k].is_native_class && comp_reader_in_chain(c, k, name, NULL))) has_user = 1;
        if (!has_user) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      }
      if (argc >= 1 && sp_streq(name, "values_at") && nt_ref(nt, id, "block") < 0) {
        if (!an_user_poly_arm(c, name, argc)) { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      }
      /* Array-reduction methods on a boxed array element (a run from
         chunk_while etc.): the concrete element type is erased to poly, so the
         result is a boxed poly value resolved at runtime by cls_id. */
      if (argc == 0 &&
          (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
           sp_streq(name, "first") || sp_streq(name, "last") || sp_streq(name, "sample")))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* Fiber/Thread/IO/File instance methods: fallback when no user class defines `name`. */
      if (sp_streq(name, "resume") || sp_streq(name, "value") || sp_streq(name, "join") ||
          sp_streq(name, "status") || sp_streq(name, "transfer") ||
          (sp_streq(name, "raise") && argc <= 3))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      /* the Queue names no other builtin has: a popped value, the queue, a count */
      if ((sp_streq(name, "deq") && argc <= 1) || (sp_streq(name, "enq") && (argc == 1 || argc == 2)))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (sp_streq(name, "num_waiting") && argc == 0) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      if (sp_streq(name, "alive?") || sp_streq(name, "dead?") || sp_streq(name, "closed?") ||
          (sp_streq(name, "blocking?") && argc == 0) ||
          sp_streq(name, "eof?") || sp_streq(name, "tty?") || sp_streq(name, "isatty") ||
          sp_streq(name, "sync"))
        { *out = an_poly_concrete(c, name, TY_BOOL); return 1; }
      /* sync= answers its argument (only its truth sets the mode) */
      if (sp_streq(name, "sync=") && argc == 1)
        { *out = an_poly_concrete(c, name, infer_type(c, argv[0])); return 1; }
      /* IO#winsize on a poly-carried handle: [rows, cols], same as the TY_IO
         arm. Without this the call falls through to a plain poly result and the
         `size[0]` that follows reads it as an untyped value. */
      if (sp_streq(name, "winsize") && sp_feature_enabled("io/console"))
        { *out = an_poly_concrete(c, name, TY_INT_ARRAY); return 1; }
      /* a boxed socket's addresses, as the TY_IO arm types them */
      if ((sp_streq(name, "addr") || sp_streq(name, "peeraddr")) && argc == 0 &&
          sp_feature_required("socket"))
        { *out = an_poly_concrete(c, name, TY_POLY_ARRAY); return 1; }
      /* the non-blocking pair on a poly-carried handle, typed as the TY_IO arm
         types it: `exception: false` answers a wait symbol (read) or nil
         (write) as well as the ordinary result, so that shape is poly and a
         String slot cannot hold it (#4236/#4237) */
      if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) &&
          an_nonblock_no_exception(c, id))
        { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (sp_streq(name, "read_nonblock")) { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      if ((sp_streq(name, "wait_readable") || sp_streq(name, "wait_writable") ||
           sp_streq(name, "wait_priority")) && argc <= 1)
        { *out = an_poly_concrete(c, name, TY_IO); return 1; }
      if (sp_streq(name, "write_nonblock")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      if (sp_streq(name, "read") || sp_streq(name, "gets") ||
          sp_streq(name, "readline") || sp_streq(name, "pread") ||
          sp_streq(name, "readpartial")) { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      /* the descriptor surface, typed as the TY_IO arms type it: a stat is
         carried as the handle itself, the offsets and counts are ints */
      if (sp_streq(name, "stat") && argc == 0) { *out = an_poly_concrete(c, name, TY_IO); return 1; }
      if (sp_streq(name, "seek") || sp_streq(name, "tell") || sp_streq(name, "pos") ||
          sp_streq(name, "pwrite") || sp_streq(name, "fsync") ||
          sp_streq(name, "fdatasync")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      if (sp_streq(name, "write") || sp_streq(name, "syswrite"))
        { *out = an_poly_concrete(c, name, TY_INT); return 1; }   /* IO#write / #syswrite: the byte count */
      /* close answers nil for an IO or a Dir, and the queue for a Queue */
      if (sp_streq(name, "close") && argc == 0) { *out = an_poly_concrete(c, name, TY_POLY); return 1; }
      if (sp_streq(name, "close") || sp_streq(name, "flush")) { *out = an_poly_concrete(c, name, TY_NIL); return 1; }
      if (sp_streq(name, "fileno")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      /* the descriptor controls, at the arities the poly-IO arm takes them:
         integers, and advise's nil boxed as the TY_IO arm boxes it. Only
         where that arm emits the call: a class of the program's own (Object
         reopened included) that defines the name, as an instance or a class
         method, a reader or (for `pos=`) a writer, owns it instead, and its
         answer may be anything; so it
         is asked here even in the builtin-only derivation, which shapes the
         dispatch's default arm. */
      if ((((sp_streq(name, "pos=") || sp_streq(name, "flock")) && argc == 1) ||
           ((sp_streq(name, "sysseek") || sp_streq(name, "fcntl")) && argc >= 1 && argc <= 2) ||
           (sp_streq(name, "advise") && argc >= 1 && argc <= 3))) {
        int owned = 0;
        char wbase[16] = "";
        if (sp_streq(name, "pos=")) memcpy(wbase, "pos", 4);
        for (int k = 0; k < c->nclasses && !owned; k++)
          if (comp_poly_arm_defines(c, k, name) || comp_cmethod_in_chain(c, k, name, NULL) >= 0 ||
              (!c->classes[k].is_native_class && comp_is_reader(&c->classes[k], name)) ||
              (wbase[0] && comp_writer_in_chain(c, k, wbase, NULL)))
            owned = 1;
        if (!owned) { *out = (sp_streq(name, "advise") || sp_streq(name, "flock")) ? TY_POLY : TY_INT; return 1; }
      }
      /* The rest of the names the poly-IO arm emits. Left untyped, the call
         read as valueless and the emitted value was DISCARDED -- `@fds[k].path`
         through a method answered nil while the arm had produced the path
         (#4626). The types are the TY_IO arms': a path is a String, readlines
         the lines, rewind and truncate ints, and the output family nil. */
      if (sp_streq(name, "path") || sp_streq(name, "to_path"))
        { *out = an_poly_concrete(c, name, TY_STRING); return 1; }
      if (sp_streq(name, "readlines")) { *out = an_poly_concrete(c, name, TY_STR_ARRAY); return 1; }
      /* a stat's mode and numeric fields, as the TY_IO arms type them, where
         the poly-IO arm emits them: not where a class method may own the
         name or an OpenStruct may carry it */
      if (argc == 0 && !sp_feature_required("ostruct") &&
          (sp_streq(name, "mode") || sp_streq(name, "uid") || sp_streq(name, "gid") ||
           sp_streq(name, "nlink") || sp_streq(name, "dev") || sp_streq(name, "ino") ||
           sp_streq(name, "blksize") || sp_streq(name, "blocks") || sp_streq(name, "rdev"))) {
        int cm = 0;
        for (int k = 0; k < c->nclasses && !cm; k++)
          if (comp_cmethod_in_chain(c, k, name, NULL) >= 0) cm = 1;
        if (!cm) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      }
      if (sp_streq(name, "rewind")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      /* a stat's predicates, as the TY_IO arms type them, where the poly-IO
         arm emits them (not where a class method may own the name): size?
         is the int-or-nil count */
      int stat_pred_cm = 0;
      for (int k = 0; k < c->nclasses && !stat_pred_cm; k++)
        if (comp_cmethod_in_chain(c, k, name, NULL) >= 0) stat_pred_cm = 1;
      if (argc == 0 && !stat_pred_cm && sp_streq(name, "size?")) { *out = an_poly_concrete(c, name, TY_INT); return 1; }
      if (argc == 0 && !stat_pred_cm && (sp_streq(name, "pipe?") || sp_streq(name, "readable?") ||
                        sp_streq(name, "writable?") || sp_streq(name, "executable?") ||
                        sp_streq(name, "blockdev?") || sp_streq(name, "chardev?") ||
                        sp_streq(name, "file?") || sp_streq(name, "directory?") ||
                        sp_streq(name, "symlink?") || sp_streq(name, "owned?") ||
                        sp_streq(name, "grpowned?") || sp_streq(name, "setuid?") ||
                        sp_streq(name, "setgid?") || sp_streq(name, "sticky?") ||
                        sp_streq(name, "socket?")))
        { *out = an_poly_concrete(c, name, TY_BOOL); return 1; }
      if (sp_streq(name, "puts") || sp_streq(name, "print") || sp_streq(name, "putc"))
        { *out = an_poly_concrete(c, name, TY_NIL); return 1; }
      /* printf answers the boxed nil the TY_IO arm answers */
      if (sp_streq(name, "printf")) { *out = TY_POLY; return 1; }
      if (sp_streq(name, "synchronize")) {
        int blk_id = nt_ref(nt, id, "block");
        if (blk_id >= 0) {
          int bdy = nt_ref(nt, blk_id, "body");
          int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
          if (bbn > 0) { *out = infer_type(c, bbb[bbn - 1]); return 1; }
        }
        { *out = an_poly_concrete(c, name, TY_NIL); return 1; }
      }
      /* The last resort, after every name's own rule above: no user answer
         is settled yet -- a method answering through this very call (`def
         to_ary = @body.to_ary`, where @body may be another wrapper) never
         settles on its own, and the candidate loop skips it -- but the
         builtin surface has an answer. Poly, as the container reads get, so
         the call has a type and the dispatch a default arm for a genuine
         builtin. Only once the fixpoint has converged without it (not
         g_infer_optimistic): a candidate that settles in a later round would
         otherwise be widened for good. */
      if (!found && npc > 0 && !an_builtin_only && !g_infer_optimistic && recv >= 0 &&
          nt_ref(nt, id, "block") < 0 && (nat_found || an_user_defines_or_reads(c, name))) {
        long uk = narrow_key(5, id, "");
        int uhit; int ucached = narrow_memo_get(uk, &uhit);
        TyKind ubt = uhit ? (TyKind)ucached : an_builtin_answer(c, id);
        if (!uhit) narrow_memo_put(uk, (int)ubt);
        if (ubt != TY_UNKNOWN && ubt != TY_VOID) {
          if (c->poly_builtin_ty && id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN)
            c->poly_builtin_ty[id] = ubt;
          { *out = TY_POLY; return 1; }
        }
      }
    }
  }
  return 0;
}

/* A constructor call: a class's .new, and the builtin constructors (infer_call_inner's rules, in their order) */
static int infer_new_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind *out) {
  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && (sp_streq(name, "new") || sp_streq(name, "__hash_new_default"))) {
    const char *rty = nt_type(nt, recv);
    /* a namespaced class (M::Sub) or root-qualified builtin (::Array etc) */
    if (rty && sp_streq(rty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          { *out = (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci)); return 1; }
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) { *out = (TyKind)c->scopes[ucnew].ret; return 1; }
        /* a reopened builtin (`class String; def ...`) keeps its builtin
           representation: `String.new` is a String, not a user object (#3109) */
        if (!(cn && is_builtin_reopen(cn))) { *out = ty_object(ci); return 1; }
      }
      /* a builtin exception by its whole path (Errno::ENOENT.new) */
      if (cn && (is_builtin_exception_name(cn) || superclass_builtin_exc_name(nt, recv)))
        { *out = TY_EXCEPTION; return 1; }
      /* ::Array.new / ::String.new / ::StringIO.new etc. */
      if (cn && sp_streq(cn, "Array") && argc == 2) { *out = ty_array_of(infer_type(c, argv[1])); return 1; }
      if (cn && sp_streq(cn, "Array")) { *out = TY_POLY_ARRAY; return 1; }
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) { *out = TY_POLY; return 1; }
      if (cn && sp_streq(cn, "String")) { *out = TY_STRING; return 1; }
      if (cn && sp_streq(cn, "Hash"))
        { *out = sp_streq(name, "__hash_new_default") ? TY_POLY_POLY_HASH : TY_UNKNOWN; return 1; }
      if (cn && sp_streq(cn, "Regexp")) { *out = TY_REGEX; return 1; }
      if (cn && sp_streq(cn, "Fiber")) { *out = TY_FIBER; return 1; }
      if (cn && sp_streq(cn, "File")) { *out = TY_IO; return 1; }   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "IO")) { *out = TY_IO; return 1; }     /* IO.new(fd) is IO.for_fd */
      if (cn && sp_streq(cn, "Dir")) { *out = TY_DIR; return 1; }   /* Dir.new is an open handle (#2821) */
      /* the socket classes ARE IO handles (#2922) */
      if (cn && (sp_streq(cn, "TCPServer") || sp_streq(cn, "TCPSocket") ||
                 sp_streq(cn, "UDPSocket") || sp_streq(cn, "UNIXSocket") ||
                 sp_streq(cn, "UNIXServer") || sp_streq(cn, "Socket")) &&
          sp_feature_required("socket")) { *out = TY_IO; return 1; }
      if (cn && sp_streq(cn, "OpenStruct") && sp_feature_required("ostruct")) { *out = TY_OPENSTRUCT; return 1; }
      /* A Mutex reached through a PATH is the same Mutex. `Thread::Mutex` is
         CRuby's own name for the class, and the bare-constant branch types
         `Mutex.new` TY_MUTEX -- but the path spelling fell into the boxed
         catch-all below, so the local was declared sp_RbVal while the emitter
         still wrote sp_Mutex_new() and the C did not compile (#4421).
         Thread::Queue, Thread::SizedQueue and Thread::ConditionVariable were
         never in that catch-all and have always worked through the path
         spelling, which is what made this one look arbitrary rather than
         missing. */
      if (cn && (sp_streq(cn, "Mutex") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor"))))
        { *out = TY_MUTEX; return 1; }
      if (cn && (sp_streq(cn, "Thread") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor")) ||
                 sp_streq(cn, "Random") || sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) { *out = TY_POLY; return 1; }
    }
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "LocalVariableReadNode") ||
                (sp_streq(rty, "CallNode") && is_struct_call(c, recv)))) {  /* inline Data.define(...).new (#2682) */
      const char *cn = sp_streq(rty, "ConstantReadNode") ? nt_str(nt, recv, "name") : NULL;
      int ci = cn ? comp_class_index(c, cn) : class_var_static_ci(c, recv);
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          { *out = (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci)); return 1; }
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) { *out = (TyKind)c->scopes[ucnew].ret; return 1; }
        /* a reopened builtin keeps its builtin representation (#3109) */
        if (!(cn && is_builtin_reopen(cn))) { *out = ty_object(ci); return 1; }
      }
      if (cn && is_builtin_exception_name(cn)) { *out = TY_EXCEPTION; return 1; }
      if (cn && sp_streq(cn, "Array") && argc == 2) { *out = ty_array_of(infer_type(c, argv[1])); return 1; }
      if (cn && sp_streq(cn, "Array")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* Array.new(n) { body }: element type from last expression of block body */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          if (bn > 0 && bb) {
            TyKind et = infer_type(c, bb[bn - 1]);
            if (et != TY_UNKNOWN) { *out = ty_array_of(et); return 1; }
            /* Element type unsettled: stay UNKNOWN rather than latch
               POLY_ARRAY. "I do not know what this holds yet" is not "it holds
               anything" -- and the difference is permanent, because a
               POLY_ARRAY binds monotonically into a callee parameter that can
               never un-widen to the INT_ARRAY it really was.

               That matters most where the answer feeds back into what it was
               derived from. An extension-field add is
               `Array.new(4) { |i| Field.add(a[i], b[i]) }`: answering
               POLY_ARRAY on the round before Field.add's own return settles
               makes the caller's accumulator poly, which makes this method's
               own parameters poly, which makes `a[i]` poly -- and the cycle has
               no way back. Waiting one round instead lets the concrete entry
               point (an int-array literal, a zero element) propagate all the
               way round, and the whole field settles on the Integer array.

               The second stage, with g_infer_optimistic cleared, still answers
               POLY_ARRAY for an element that genuinely never settles -- an
               empty `[]`, a heterogeneous block. The narrower rule below is
               what that stage keeps: an index param still being inferred gives
               the element type once it arrives (#3157). */
            if (g_infer_optimistic) { *out = TY_UNKNOWN; return 1; }
            int bpn = a_proc_params_node(c, id);   /* id is the Array.new call */
            int brn = 0; const int *breqs = bpn >= 0 ? nt_arr(nt, bpn, "requireds", &brn) : NULL;
            if (brn > 0 && breqs) {
              Scope *bsc = comp_scope_of(c, bb[bn - 1]);   /* the block's own scope */
              const char *bpname = nt_str(nt, breqs[0], "name");
              LocalVar *bplv = (bsc && bpname) ? scope_local(bsc, bpname) : NULL;
              if (bplv && bplv->type == TY_UNKNOWN) { *out = TY_UNKNOWN; return 1; }
            }
          }
        }
        /* a bare `Array.new` carries no element type; leave it UNKNOWN (like an
           empty `[]`) so the push-promotion pass can narrow it from `<<`/push. */
        if (argc == 0 && blk < 0) { *out = TY_UNKNOWN; return 1; }
        { *out = TY_POLY_ARRAY; return 1; }
      }
      if (cn && sp_streq(cn, "Array")) { *out = TY_POLY_ARRAY; return 1; } /* Array.new / Array.new(n) */
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) { *out = TY_POLY; return 1; }  /* identity sentinel */
      if (cn && sp_streq(cn, "String")) { *out = TY_STRING; return 1; }
      /* Hash.new { |hash, key| default } : a poly-keyed poly hash with a
         default-proc (the block computes the missing-key value). A default-block
         hash is polymorphic by nature (keys are populated dynamically), so the
         faithful PolyPoly variant boxes each key by value -- inspect then renders
         symbol keys as `a:`, string keys as `"a"=>`, etc., all correctly. */
      if (cn && sp_streq(cn, "Hash") && nt_ref(nt, id, "block") >= 0) { *out = TY_POLY_POLY_HASH; return 1; }
      if (cn && sp_streq(cn, "Hash")) {
        /* argument-position Hash.new was renamed: PolyPoly; else key usage decides */
        if (sp_streq(name, "__hash_new_default")) { *out = TY_POLY_POLY_HASH; return 1; }
        /* keys of more than one class are written into it (#3927) */
        if (c->hash_want && id < c->node_cap && c->hash_want[id] == TY_POLY_POLY_HASH)
          { *out = TY_POLY_POLY_HASH; return 1; }
        /* A Hash.new called on DIRECTLY has no key usage to decide it, and
           staying unknown made every method on it an unresolved call
           ("undefined method 'fetch' for unknown", #3823). The faithful
           variant is the one the argument position already uses. */
        if (node_is_call_receiver(c, id)) { *out = TY_POLY_POLY_HASH; return 1; }
        /* ...and so does one placed straight into an array or hash literal
           (`[Hash.new(4), 0]`): left unknown, the array took an Integer
           element and the build failed */
        if (node_is_container_elem(c, id)) { *out = TY_POLY_POLY_HASH; return 1; }
        /* ...and a Hash.new that is a method's VALUE has no receiver use of
           its own either, so it stayed unknown and the method emitted as
           void: `def mk = Hash.new(0)` answered nothing, and every call on
           the result reported Hash.new itself as undefined (#4291). The
           value position is the same "nothing narrows it" case the receiver
           scan above covers. */
        {
          Scope *hs = comp_scope_of(c, id);
          if (hs && hs->body >= 0) {
            int hbn = 0; const int *hbb = nt_arr(nt, hs->body, "body", &hbn);
            /* ...but only when nothing else settles the method. The CALLERS
               narrow a returned Hash.new -- `h = str_hash; h["k"] = "v"`
               gives Hash[String, String] -- and answering the widest variant
               here overrode that narrowing rather than filling in an unknown,
               re-emitting every such helper as a poly hash (#4304).

               Both halves below are load-bearing, and neither alone fixes it.
               The wait: the callers' narrowing happens during the optimistic
               rounds, so answering there latches the widest variant before
               there is anything to stand aside for. The concrete test: once
               the rounds have settled, a return that reached a real variant
               is not the "nothing narrows it" case, and without this test the
               final round widens it straight back. */
            int narrowed = g_infer_optimistic ||
                           (ty_is_hash(hs->ret) && hs->ret != TY_POLY_POLY_HASH);
            if (hbb && hbn > 0 && hbb[hbn - 1] == id && !narrowed) { *out = TY_POLY_POLY_HASH; return 1; }
          }
        }
        { *out = TY_UNKNOWN; return 1; }
      }
      if (cn && sp_streq(cn, "Regexp")) { *out = TY_REGEX; return 1; }
      /* Builtin object types */
      if (cn && sp_streq(cn, "Fiber")) { *out = TY_FIBER; return 1; }
      /* Thread.new { block }: an eager green thread (sp_thread) on the scheduler. */
      if (cn && sp_streq(cn, "Thread") && nt_ref(nt, id, "block") >= 0) { *out = TY_THREAD; return 1; }
      if (cn && (sp_streq(cn, "Queue") || sp_streq(cn, "SizedQueue"))) { *out = TY_QUEUE; return 1; }
      if (cn && (sp_streq(cn, "Mutex") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor")))) { *out = TY_MUTEX; return 1; }
      if (cn && sp_streq(cn, "ConditionVariable")) { *out = TY_CONDVAR; return 1; }
      if (cn && sp_streq(cn, "Random")) { *out = TY_RANDOM; return 1; }
      if (cn && sp_streq(cn, "File")) { *out = TY_IO; return 1; }   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "IO")) { *out = TY_IO; return 1; }     /* IO.new(fd) is IO.for_fd */
      if (cn && sp_streq(cn, "Dir")) { *out = TY_DIR; return 1; }   /* Dir.new is an open handle (#2821) */
      /* the socket classes ARE IO handles (#2922) */
      if (cn && (sp_streq(cn, "TCPServer") || sp_streq(cn, "TCPSocket") ||
                 sp_streq(cn, "UDPSocket") || sp_streq(cn, "UNIXSocket") ||
                 sp_streq(cn, "UNIXServer") || sp_streq(cn, "Socket")) &&
          sp_feature_required("socket")) { *out = TY_IO; return 1; }
      if (cn && sp_streq(cn, "OpenStruct") && sp_feature_required("ostruct")) { *out = TY_OPENSTRUCT; return 1; }
      if (cn && (sp_streq(cn, "Thread") ||
                 sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) { *out = TY_POLY; return 1; }
    }
  }
  return 0;
}

/* A class method called on a builtin class constant: Time, File, IO, Fiber, Thread, Random, Marshal, JSON, Encoding (infer_call_inner's rules, in their order) */
/* A literal Range argument to rand with a Float bound: the draw is a Float
   ((1..2.5) included, which the Integer representation iterates) */
static int rand_lit_range_float(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  const char *at = nt_type(nt, arg);
  if (!at || !sp_streq(at, "RangeNode")) return 0;
  int lo = nt_ref(nt, arg, "left"), hi = nt_ref(nt, arg, "right");
  return (lo >= 0 && infer_type(c, lo) == TY_FLOAT) || (hi >= 0 && infer_type(c, hi) == TY_FLOAT);
}

static int infer_builtin_cmethod_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind *out) {
  /* StringIO: a native-bound class (packages/stringio); no arms here. .new
     resolves through the class table, .open is Ruby in the package, and
     instance methods use the native_method declarations. */

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Time") &&
        (sp_streq(name, "now") || sp_streq(name, "at") || sp_streq(name, "local") ||
         sp_streq(name, "mktime") || sp_streq(name, "utc") || sp_streq(name, "gm") ||
         sp_streq(name, "new")))
      { *out = TY_TIME; return 1; }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        (sp_streq(name, "start") || sp_streq(name, "compact")))
      { *out = TY_NIL; return 1; }
    /* Encoding.find(name): a boxed Encoding (nil for "internal") */
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Encoding") &&
        sp_streq(name, "find") && argc == 1)
      { *out = TY_POLY; return 1; }
    /* Warning[] / Warning[]= / Warning.warn (codegen_call.c's arm) */
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Warning")) {
      if (sp_streq(name, "[]")) { *out = TY_BOOL; return 1; }
      if (sp_streq(name, "[]=")) { *out = TY_POLY; return 1; }   /* the assignment's value: the RHS, boxed */
      if (sp_streq(name, "warn")) { *out = TY_NIL; return 1; }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        sp_streq(name, "stat"))
      { *out = TY_STR_INT_HASH; return 1; }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Process")) {
      if (sp_streq(name, "times") && argc == 0) { *out = TY_TMS; return 1; }
      if (sp_streq(name, "pid") || sp_streq(name, "ppid") ||
          sp_streq(name, "uid") || sp_streq(name, "gid") ||
          sp_streq(name, "euid") || sp_streq(name, "egid") ||
          sp_streq(name, "getsid") || sp_streq(name, "getpgrp") ||
          (sp_streq(name, "getpriority") && argc == 2)) { *out = TY_INT; return 1; }
      if (sp_streq(name, "groups") && argc == 0) { *out = TY_INT_ARRAY; return 1; }
      if (sp_streq(name, "clock_gettime") || sp_streq(name, "clock_getres")) {
        /* an integer unit (:nanosecond/:microsecond/:millisecond/:second) makes
           the result an Integer; the default and float units keep it Float. */
        if (argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode")) {
          const char *u = nt_str(nt, argv[1], "value");
          if (u && (sp_streq(u, "nanosecond") || sp_streq(u, "microsecond") ||
                    sp_streq(u, "millisecond") || sp_streq(u, "second"))) { *out = TY_INT; return 1; }
        }
        { *out = TY_FLOAT; return 1; }
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
        sp_streq(name, "sqrt"))
      { *out = TY_INT; return 1; }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Marshal")) {
      if (sp_streq(name, "dump") && argc == 1) { *out = TY_STRING; return 1; }
      /* Marshal.dump(obj, io) writes the bytes to io and answers io (#4112) */
      if (sp_streq(name, "dump") && argc == 2) { *out = TY_IO; return 1; }
      if (sp_streq(name, "load") && argc == 1) { *out = TY_POLY; return 1; }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "sin") || sp_streq(name, "cos") || sp_streq(name, "tan") ||
         sp_streq(name, "asin") || sp_streq(name, "acos") || sp_streq(name, "atan") ||
         sp_streq(name, "atan2") || sp_streq(name, "sinh") || sp_streq(name, "cosh") ||
         sp_streq(name, "tanh") || sp_streq(name, "asinh") || sp_streq(name, "acosh") ||
         sp_streq(name, "atanh") || sp_streq(name, "exp") || sp_streq(name, "log") ||
         sp_streq(name, "log2") || sp_streq(name, "log10") || sp_streq(name, "sqrt") ||
         sp_streq(name, "cbrt") || sp_streq(name, "hypot") ||
         sp_streq(name, "expm1") || sp_streq(name, "log1p") ||
         sp_streq(name, "ldexp") || sp_streq(name, "erf") || sp_streq(name, "erfc") ||
         sp_streq(name, "gamma")))
      { *out = TY_FLOAT; return 1; }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "lgamma") || sp_streq(name, "frexp")) && argc == 1)
      { *out = TY_POLY_ARRAY; return 1; }  /* [log(|gamma|), sign] / [fraction, exponent] */
    /* JSON.generate/dump return type comes from the native binding
       (packages/json, inferred in the FFI/native block above), not a hardcoded
       arm. */
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
        (sp_streq(name, "exist?") || sp_streq(name, "exists?")))
      { *out = TY_BOOL; return 1; }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
      if (sp_streq(name, "pwd") || sp_streq(name, "home")) { *out = TY_STRING; return 1; }
      if (sp_streq(name, "glob") || sp_streq(name, "entries") || sp_streq(name, "children")) { *out = TY_STR_ARRAY; return 1; }
      if (sp_streq(name, "mkdir") || sp_streq(name, "rmdir") || sp_streq(name, "chdir"))
        { *out = TY_INT; return 1; }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") &&
        (sp_streq(nt_str(nt, recv, "name"), "File") ||
         sp_streq(nt_str(nt, recv, "name"), "FileTest"))) {
      if (sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname") ||
          sp_streq(name, "read") || sp_streq(name, "binread") || sp_streq(name, "expand_path") ||
          sp_streq(name, "join") || sp_streq(name, "realpath") ||
          sp_streq(name, "realdirpath") || sp_streq(name, "ftype") ||
          sp_streq(name, "path") || sp_streq(name, "absolute_path"))
        { *out = TY_STRING; return 1; }
      if (sp_streq(name, "exist?") || sp_streq(name, "exists?"))
        { *out = TY_BOOL; return 1; }
      if (sp_streq(name, "write") || sp_streq(name, "binwrite") || sp_streq(name, "delete") ||
          sp_streq(name, "unlink") || sp_streq(name, "rename") || sp_streq(name, "size") ||
          sp_streq(name, "size?") || sp_streq(name, "chmod") || sp_streq(name, "truncate") ||
          sp_streq(name, "chown") || sp_streq(name, "symlink") || sp_streq(name, "link") ||
          sp_streq(name, "mkfifo") || sp_streq(name, "umask") || sp_streq(name, "utime") ||
          sp_streq(name, "lutime") ||
          sp_streq(name, "world_readable?") || sp_streq(name, "world_writable?"))
        { *out = TY_INT; return 1; }   /* world_*? are nullable int (bits or nil) */
      if (sp_streq(name, "readlink")) { *out = TY_STRING; return 1; }
      if (sp_streq(name, "absolute_path?")) { *out = TY_BOOL; return 1; }
      if (sp_streq(name, "readable?") || sp_streq(name, "directory?") || sp_streq(name, "file?") ||
          sp_streq(name, "zero?") || sp_streq(name, "empty?") || sp_streq(name, "symlink?") ||
          sp_streq(name, "writable?") || sp_streq(name, "executable?") || sp_streq(name, "pipe?") ||
          sp_streq(name, "readable_real?") || sp_streq(name, "writable_real?") ||
          sp_streq(name, "executable_real?") ||
          sp_streq(name, "identical?") || sp_streq(name, "fnmatch") || sp_streq(name, "fnmatch?") ||
          sp_streq(name, "owned?") || sp_streq(name, "grpowned?") || sp_streq(name, "setuid?") ||
          sp_streq(name, "setgid?") || sp_streq(name, "sticky?") || sp_streq(name, "socket?") ||
          sp_streq(name, "blockdev?") || sp_streq(name, "chardev?"))
        { *out = TY_BOOL; return 1; }
      if (sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ctime") ||
          sp_streq(name, "birthtime"))
        { *out = TY_TIME; return 1; }
      if (sp_streq(name, "readlines") || sp_streq(name, "split")) { *out = TY_STR_ARRAY; return 1; }
      if (sp_streq(name, "stat") || sp_streq(name, "lstat")) { *out = TY_IO; return 1; }   /* the path-carrying stat handle */
      /* File.open / File.new without a block -> a typed IO handle */
      if (sp_streq(name, "open") || sp_streq(name, "new")) {
        int blk = nt_ref(nt, id, "block");
        if (blk < 0) { *out = TY_IO; return 1; }
        /* Pin block param to TY_IO so body dispatch works (f.write, f.puts, etc.) */
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_IO;
        { *out = TY_POLY; return 1; }
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Addrinfo") && sp_feature_required("socket")) {
      if (((sp_streq(name, "tcp") || sp_streq(name, "udp")) && argc == 2) ||
          ((sp_streq(name, "ip") || sp_streq(name, "unix")) && argc == 1))
        { *out = TY_ADDRINFO; return 1; }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Socket") && sp_feature_required("socket")) {
      if (sp_streq(name, "gethostname") && argc == 0) { *out = TY_STRING; return 1; }
      if ((sp_streq(name, "pair") || sp_streq(name, "socketpair")) && argc >= 2) { *out = TY_POLY_ARRAY; return 1; }
      if (sp_streq(name, "getaddrinfo") && argc >= 2) { *out = TY_POLY_ARRAY; return 1; }
      /* The packed sockaddr is a byte String (it carries NUL), and
         unpack answers [port, host] (#4137). */
      if ((sp_streq(name, "sockaddr_in") || sp_streq(name, "pack_sockaddr_in")) && argc == 2) { *out = TY_STRING; return 1; }
      if ((sp_streq(name, "sockaddr_un") || sp_streq(name, "pack_sockaddr_un")) && argc == 1) { *out = TY_STRING; return 1; }
      if (sp_streq(name, "unpack_sockaddr_in") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "IO")) {
      /* IO.pipe -> [reader, writer], boxed IO handles (#2815) */
      if (sp_streq(name, "pipe")) { *out = TY_POLY_ARRAY; return 1; }
      if (sp_streq(name, "copy_stream") || sp_streq(name, "sysopen")) { *out = TY_INT; return 1; }
      /* IO.new(fd, ...) is the descriptor form, same as IO.for_fd */
      if ((sp_streq(name, "for_fd") || sp_streq(name, "new")) && argc >= 1) { *out = TY_IO; return 1; }
      /* [ready_read, ready_write, ready_error] or nil on timeout */
      if (sp_streq(name, "select") && argc >= 1) { *out = TY_POLY; return 1; }
    }

    /* <local>.yield(v) or bare <local>.yield: a generator yielder / fiber yield
       returns the value the next resume (or Enumerator#feed) supplies -- poly.
       Gated on a local receiver so Fiber.yield (const receiver) and the yield
       keyword are untouched; without this the return is typed nil and
       `x = y.yield` drops the fed value. Zero-arg `y.yield` is valid too. */
    if (recv >= 0 && sp_streq(name, "yield") &&
        rty && sp_streq(rty, "LocalVariableReadNode"))
      { *out = TY_POLY; return 1; }
    /* Fiber.new {} / Thread.new {} / Fiber.current etc.
       Handles both bare Const and ::Const path forms. */
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *cn2 = nt_str(nt, recv, "name");
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Enumerator") &&
          nt_ref(nt, id, "block") >= 0) { *out = TY_ENUMERATOR; return 1; }
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Fiber")) { *out = TY_FIBER; return 1; }
      /* Thread.new { block }: an eager green thread on the scheduler. */
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Thread") &&
          nt_ref(nt, id, "block") >= 0)
        { *out = TY_THREAD; return 1; }
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Queue") || sp_streq(cn2, "SizedQueue"))) { *out = TY_QUEUE; return 1; }
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Mutex") || (sp_streq(cn2, "Monitor") && sp_feature_enabled("monitor")))) { *out = TY_MUTEX; return 1; }
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "ConditionVariable")) { *out = TY_CONDVAR; return 1; }
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Random")) { *out = TY_RANDOM; return 1; }
      if (cn2 && sp_streq(cn2, "Enumerator") && sp_streq(name, "product"))
        { *out = TY_ENUMERATOR; return 1; }   /* #2484; any number of factors */
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "current")) { *out = TY_THREAD; return 1; }
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "main")) { *out = TY_THREAD; return 1; }
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "list")) { *out = TY_POLY_ARRAY; return 1; }
      if (cn2 && sp_streq(cn2, "Thread") && (sp_streq(name, "pass") || sp_streq(name, "stop"))) { *out = TY_NIL; return 1; }
      if (cn2 && sp_streq(cn2, "Thread") &&
          (sp_streq(name, "report_on_exception") || sp_streq(name, "report_on_exception="))) { *out = TY_BOOL; return 1; }
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "current")) { *out = TY_FIBER; return 1; }
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "yield")) { *out = TY_POLY; return 1; }
      /* Fiber.blocking? (1 or false), Fiber.blocking { } (the block's value),
         and the scheduler, which is always nil */
      if (cn2 && sp_streq(cn2, "Fiber") &&
          (sp_streq(name, "blocking?") || sp_streq(name, "blocking") ||
           sp_streq(name, "scheduler") || sp_streq(name, "current_scheduler"))) { *out = TY_POLY; return 1; }
      /* Random class methods: Random.rand(float)->float / Random.rand(int)->int
         / Random.rand->float */
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "rand")) {
        if (argc < 1) { *out = TY_FLOAT; return 1; }
        TyKind rr0 = infer_type(c, argv[0]);
        /* a boxed argument draws by its run-time kind (sp_rand_poly), a Float
           Range or a literal Range with a Float bound a Float */
        if (rr0 == TY_POLY) { *out = TY_POLY; return 1; }
        if (rr0 == TY_FLOAT_RANGE || rand_lit_range_float(c, argv[0])) { *out = TY_FLOAT; return 1; }
        { *out = rr0 == TY_FLOAT ? TY_FLOAT : rr0 == TY_BIGINT ? TY_BIGINT : TY_INT; return 1; }
      }
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "bytes")) { *out = TY_STRING; return 1; }
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "new_seed")) { *out = TY_INT; return 1; }
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "urandom")) { *out = TY_STRING; return 1; }
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "srand")) { *out = TY_INT; return 1; }
    }
  }
  return 0;
}

/* A runtime handle receiver: Random, ARGF, Dir, Socket::Option and Addrinfo, IO, Time (infer_call_inner's rules, in their order) */
static int infer_handle_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* TY_RANDOM instance methods */
  if (recv >= 0 && rt == TY_RANDOM) {
    if (sp_streq(name, "rand")) {
      if (argc < 1) { *out = TY_FLOAT; return 1; }
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_FLOAT || a0 == TY_FLOAT_RANGE) { *out = TY_FLOAT; return 1; }   /* rand(Float range) -> Float (#2521) */
      if (a0 == TY_BIGINT) { *out = TY_BIGINT; return 1; }   /* rand(Bignum bound) -> Bigint (#3058) */
      /* a boxed argument draws by its run-time kind (sp_rand_poly) */
      if (a0 == TY_POLY) { *out = TY_POLY; return 1; }
      /* rand(Float range) -> Float (#2521), either bound a Float */
      if (rand_lit_range_float(c, argv[0])) { *out = TY_FLOAT; return 1; }
      { *out = TY_INT; return 1; }
    }
    if (sp_streq(name, "bytes")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "seed")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "class")) { *out = TY_CLASS; return 1; }
    if (is_equality_name(name) && argc == 1)
      { *out = TY_BOOL; return 1; }
  }

  /* ARGF pseudo-IO methods */
  if (recv >= 0 && rt == TY_ARGF) {
    if (sp_streq(name, "read") || sp_streq(name, "gets") || sp_streq(name, "readline") ||
        sp_streq(name, "filename") || sp_streq(name, "path") || sp_streq(name, "to_s")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "readlines") || sp_streq(name, "to_a")) { *out = TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "each_line") || sp_streq(name, "each_string") || sp_streq(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      { *out = TY_ARGF; return 1; }
    }
  }

  /* Dir.new / Dir.open and the Dir handle instance surface (#2821) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (sp_streq(name, "new") || sp_streq(name, "open")) && argc >= 1) {
    int dblk = nt_ref(nt, id, "block");
    if (dblk < 0) { *out = TY_DIR; return 1; }
    const char *dp0 = block_param_name(c, dblk, 0);
    Scope *dbs = dp0 ? comp_scope_of(c, dblk) : NULL;
    LocalVar *dlv = (dbs && dp0) ? scope_local(dbs, dp0) : NULL;
    if (dlv) dlv->type = TY_DIR;
    { *out = TY_POLY; return 1; }   /* block form: the block's boxed value (File.open's rule) */
  }
  if (recv >= 0 && rt == TY_DIR) {
    if (sp_streq(name, "class")) { *out = TY_CLASS; return 1; }
    if (sp_streq(name, "read") || sp_streq(name, "path") || sp_streq(name, "to_path"))
      { *out = TY_STRING; return 1; }
    if (sp_streq(name, "children") || sp_streq(name, "entries")) { *out = TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "tell") || sp_streq(name, "pos")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "fileno")) { *out = TY_INT; return 1; }      /* dirfd (#2967) */
    if (sp_streq(name, "pos=")) { *out = TY_INT; return 1; }        /* -> assigned value (#2968) */
    if (sp_streq(name, "close")) { *out = TY_POLY; return 1; }   /* nil */
    if (sp_streq(name, "rewind") || sp_streq(name, "seek")) { *out = TY_DIR; return 1; }
    /* Enumerable#each_entry yields what #each yields (dots included) and
       answers the receiver, so on a Dir it IS #each (#3395). */
    if (sp_streq(name, "each") || sp_streq(name, "each_child") ||
        sp_streq(name, "each_entry")) {
      int dblk3 = nt_ref(nt, id, "block");
      if (dblk3 >= 0) {
        const char *dbp3 = block_param_name(c, dblk3, 0);
        Scope *dbs3 = dbp3 ? comp_scope_of(c, dblk3) : NULL;
        LocalVar *dlv3 = (dbs3 && dbp3) ? scope_local(dbs3, dbp3) : NULL;
        if (dlv3) dlv3->type = TY_STRING;
      }
      { *out = TY_DIR; return 1; }
    }
  }

  /* Socket::Option and Addrinfo readers: builtin-op rows */
  if (recv >= 0 && (rt == TY_SOCKOPT || rt == TY_ADDRINFO)) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }
  if (recv >= 0 && rt == TY_IO) {
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    /* sync= answers its argument, whatever it is; only its truth sets the mode */
    if (sp_streq(name, "sync=") && argc >= 1) { *out = infer_type(c, argv[0]); return 1; }
    /* socket methods on the IO handle (#2922) */
    if (sp_feature_required("socket")) {
      if (sp_streq(name, "accept") && argc == 0) { *out = TY_IO; return 1; }
      if ((sp_streq(name, "addr") || sp_streq(name, "peeraddr")) && argc == 0)
        { *out = TY_POLY_ARRAY; return 1; }
      if ((sp_streq(name, "local_address") || sp_streq(name, "remote_address")) && argc == 0)
        { *out = TY_ADDRINFO; return 1; }
      /* the non-blocking family: the handle / the bytes / the byte count, each
         nullable so `exception: false` can answer nil */
      if (sp_streq(name, "accept_nonblock") || sp_streq(name, "recv_nonblock") ||
          sp_streq(name, "connect_nonblock"))
        { *out = an_nonblock_no_exception(c, id)
               ? TY_POLY                                  /* :wait_* or the value */
               : sp_streq(name, "accept_nonblock") ? TY_IO
               : sp_streq(name, "recv_nonblock") ? TY_STRING : TY_INT; return 1; }
      if (sp_streq(name, "recv") && argc == 1) { *out = TY_STRING; return 1; }
      if (sp_streq(name, "recvfrom") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
      if (((sp_streq(name, "bind") || sp_streq(name, "connect")) && argc == 2) ||
          (sp_streq(name, "send") && (argc == 2 || argc == 4)) ||
          (sp_streq(name, "shutdown") && argc <= 1) ||
          (sp_streq(name, "listen") && argc == 1) ||
          (sp_streq(name, "setsockopt") && argc == 3))
        { *out = TY_INT; return 1; }
      if (sp_streq(name, "getsockopt") && argc == 2) { *out = TY_SOCKOPT; return 1; }
    }
    /* fd-backed IO instance methods (#3038) */
    if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) &&
        an_nonblock_no_exception(c, id))
      { *out = TY_POLY; return 1; }
    if (sp_streq(name, "write_nonblock")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "read_nonblock")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) { *out = TY_INT_ARRAY; return 1; }
    if (sp_streq(name, "each_line") || sp_streq(name, "each") ||
        sp_streq(name, "each_char") || sp_streq(name, "each_byte") ||
        sp_streq(name, "each_codepoint")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = (sp_streq(name, "each_byte") ||
                              sp_streq(name, "each_codepoint")) ? TY_INT : TY_STRING;
      }
      { *out = TY_IO; return 1; }
    }
    { TyKind ort; if (object_reopen_answers(c, "File", id, &ort)) { *out = ort; return 1; } }
    { *out = TY_POLY; return 1; }
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (sp_streq(name, "-") && argc > 0) {
      TyKind at = infer_type(c, argv[0]);
      /* Time - Time is a Float duration. Time - poly is either: a Time held
         there gives the duration (#2456), a number an earlier Time, which
         only the run time tells apart. An int/float offset keeps the Time
         type via the general `-` arm below. */
      if (at == TY_TIME) { *out = TY_FLOAT; return 1; }
      if (at == TY_POLY) { *out = TY_POLY; return 1; }
      /* an argument not typed yet (a parameter whose callers are still
         being read) may turn out poly: answering Time now pins a local
         that later holds the boxed answer */
      if (at == TY_UNKNOWN) { *out = TY_UNKNOWN; return 1; }
    }
    /* builtin-op rows (builtin_ops.c). Every row names a known Time (or
       Object) method, which the Object reopen check below never answers. */
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) { *out = TY_STRING; return 1; }
    if ((sp_streq(name, "httpdate") || sp_streq(name, "rfc2822") || sp_streq(name, "rfc822")) &&
        argc == 0 && sp_feature_enabled("time")) { *out = TY_STRING; return 1; }
    /* Time <=> Time is an Integer; against a non-Time operand it is nil, so
       the result is poly (#2677). */
    if (sp_streq(name, "<=>") && argc == 1)
      { *out = infer_type(c, argv[0]) == TY_TIME ? TY_INT : TY_POLY; return 1; }
    /* predicates (is_a?/kind_of?/instance_of?/between?/...) before the int
       catch-all below swallows them */
    { size_t tnl = strlen(name); if (tnl > 0 && name[tnl - 1] == '?') { *out = TY_BOOL; return 1; } }
    { TyKind ort; if (object_reopen_answers(c, "Time", id, &ort)) { *out = ort; return 1; } }
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    { *out = TY_INT; return 1; }
  }
  return 0;
}

/* A Range receiver, and a .lazy pipeline over a range or an array (infer_call_inner's rules, in their order) */
static int infer_range_lazy_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* range receiver methods */
  if (recv >= 0 && rt == TY_RANGE) {
    /* a literal string range ("a".."z") yields strings, not ints */
    if (sp_streq(name, "to_a")) {
      int rn = an_unparen(nt, recv);
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 && infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING)
          { *out = TY_STR_ARRAY; return 1; }
      }
    }
    /* String-endpoint range accessors read/return strings, not ints (#2467) */
    {
      int rn = an_unparen(nt, recv);
      if (rn >= 0 && nt_type(nt, rn) && !sp_streq(nt_type(nt, rn), "RangeNode")) {
        int sl = local_sole_range_node(c, rn);
        if (sl >= 0) rn = sl;
      }
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 &&
            infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING) {
          if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "end") ||
                            sp_streq(name, "first") || sp_streq(name, "last") ||
                            sp_streq(name, "min") || sp_streq(name, "max")))
            { *out = TY_STRING; return 1; }
          if (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last")))
            { *out = TY_STR_ARRAY; return 1; }
        }
      }
    }
    if (sp_streq(name, "to_a") || sp_streq(name, "entries")) { *out = TY_INT_ARRAY; return 1; }  /* (#2414) */
    if (sp_streq(name, "minmax")) { *out = TY_POLY_ARRAY; return 1; }   /* [nil, nil] when empty (#2412) */
    /* `x..Float::INFINITY`: the int range records only "unbounded", but the
       literal says what the bound was, so #end answers the Float (#3670) */
    if (sp_streq(name, "end") && argc == 0) {
      int _ri = an_unparen(nt, recv);
      if (_ri >= 0 && nt_kind(nt, _ri) == NK_RangeNode &&
          infer_endpoint_is_infinite(c, nt_ref(nt, _ri, "right")) &&
          nt_ref(nt, _ri, "right") >= 0)
        { *out = TY_FLOAT; return 1; }
    }
    /* an ENDLESS literal range: #end is nil (#2413) */
    if (sp_streq(name, "end") && ({ int _rn = an_unparen(nt, recv);
        _rn >= 0 && nt_type(nt, _rn) && sp_streq(nt_type(nt, _rn), "RangeNode") &&
        nt_ref(nt, _rn, "right") < 0; })) { *out = TY_POLY; return 1; }
    /* step { } in value position returns the receiver range (#2415) */
    if (sp_streq(name, "step") && nt_ref(nt, id, "block") >= 0) { *out = TY_RANGE; return 1; }
    if (sp_streq(name, "include?") || sp_streq(name, "member?") ||
        sp_streq(name, "cover?") || sp_streq(name, "exclude_end?") ||
        sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "overlap?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "step")) {
      /* step with a block walks the range and returns self */
      if (nt_ref(nt, id, "block") >= 0) { *out = rt; return 1; }
      /* a float step, or a literal range with float bounds, yields floats */
      int sfloat = argc >= 1 && infer_type(c, argv[0]) == TY_FLOAT;
      int rn = an_unparen(nt, recv);
      int bfloat = 0;
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        bfloat = (lo >= 0 && infer_type(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && infer_type(c, hi) == TY_FLOAT);
      }
      { *out = (sfloat || bfloat) ? TY_FLOAT_ARRAY : TY_INT_ARRAY; return 1; }
    }
    if (is_quantifier(name)) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "each") && nt_ref(nt, id, "block") < 0)
      { *out = range_each_is_external(c, id) ? TY_ENUMERATOR : TY_INT_ARRAY; return 1; }
    if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) { *out = TY_ENUMERATOR; return 1; }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) { *out = TY_INT_ARRAY; return 1; }
    if (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "size") || sp_streq(name, "count") ||
        sp_streq(name, "begin") || sp_streq(name, "end"))  { *out = TY_INT; return 1; }
    if (sp_streq(name, "bsearch")) {
      /* a float-bounded range yields a float member (or nil); an int range an
         int member. The bound types are on the receiver's RangeNode. */
      int brn = an_unparen(nt, recv);
      if (brn >= 0 && nt_type(nt, brn) && sp_streq(nt_type(nt, brn), "RangeNode")) {
        int bl = nt_ref(nt, brn, "left"), br = nt_ref(nt, brn, "right");
        /* both bounds must be real NUMERIC nodes (the float-bisection branch
           needs a finite interval; a beginless/endless bound is nil) and at
           least one float -> a float member */
        TyKind blt = bl >= 0 ? infer_type(c, bl) : TY_NIL;
        TyKind brt = br >= 0 ? infer_type(c, br) : TY_NIL;
        if ((blt == TY_INT || blt == TY_FLOAT) && (brt == TY_INT || brt == TY_FLOAT) &&
            (blt == TY_FLOAT || brt == TY_FLOAT)) { *out = TY_FLOAT; return 1; }
      }
      { *out = TY_INT; return 1; }  /* a member, or nil (nullable int) */
    }
    int block = nt_ref(nt, id, "block");
    /* finite-range Enumerable methods that materialize to an int array in
       codegen: select/reject/filter (fused loop). */
    if ((ty_iter_shape(name) == TY_ITER_SELECT || ty_iter_shape(name) == TY_ITER_REJECT) &&
        block >= 0) { *out = TY_INT_ARRAY; return 1; }
    if (block >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind et = bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN;
      TyKind bnt = ie_block_break_next_ty(c, body);
      if (bnt != TY_UNKNOWN) et = (et == TY_UNKNOWN) ? bnt : ty_unify(et, bnt);
      { *out = ty_array_of(et); return 1; }
    }
  }

  /* A lazy chain answers Enumerator::Lazy for #class. The chain itself has no
     runtime value, so this is the one non-forcing call it can serve (#3358). */
  if (sp_streq(name, "class") && argc == 0 && recv >= 0 && chain_is_lazy_valued(c, recv))
    { *out = TY_CLASS; return 1; }

  /* (range).lazy[.select/reject{blk}].first(n) / .first. The chain may be held
     in a variable (`p = src.lazy.select{}; p.first(n)`) -- resolve the alias to
     the chain node so the forced type matches emit_lazy_pipeline_expr (#2932). */
  if (sp_streq(name, "first") || sp_streq(name, "last")) {
    int lrecv = lazy_resolve_chain(c, recv);
  if (lrecv >= 0 && nt_type(nt, lrecv) && sp_streq(nt_type(nt, lrecv), "CallNode")) {
    int lazy_src = -1;
    int grouped = 0;   /* a terminal-adjacent each_cons/each_slice groups the stream */
    {
      /* peel the chain terminal-first: an optional grouping stage, then the
         filtering stages, down to the blockless `lazy` (mirrors the shapes
         emit_lazy_pipeline_expr fuses) */
      int cur9 = lrecv;
      const char *rname9 = nt_str(nt, cur9, "name");
      if (rname9 && (sp_streq(rname9, "each_cons") || sp_streq(rname9, "each_slice")) &&
          nt_ref(nt, cur9, "block") < 0) {
        grouped = 1;
        cur9 = nt_ref(nt, cur9, "receiver");
      }
      while (cur9 >= 0 && nt_type(nt, cur9) && sp_streq(nt_type(nt, cur9), "CallNode")) {
        const char *nm9 = nt_str(nt, cur9, "name");
        if (!nm9) break;
        if (sp_streq(nm9, "lazy") && nt_ref(nt, cur9, "block") < 0) {
          lazy_src = nt_ref(nt, cur9, "receiver");
          break;
        }
        if (sp_streq(nm9, "select") || sp_streq(nm9, "reject") || sp_streq(nm9, "filter")) {
          cur9 = nt_ref(nt, cur9, "receiver");
          continue;
        }
        break;
      }
    }
    (void)grouped;
    TyKind lst = lazy_src >= 0 ? infer_type(c, lazy_src) : TY_UNKNOWN;
    /* emit_lazy_pipeline_expr collects into a PolyArray; the counted form is
       that array, the bare form its first (boxed) element (#2994). */
    if (lazy_src >= 0 && lst == TY_RANGE)
      { *out = (argc == 1) ? TY_POLY_ARRAY : TY_POLY; return 1; }
    /* An array-source lazy first(n) (e.g. the `arr.lazy.take(n).to_a` that the
       take->first desugar produces) materializes n boxed elements; the bare
       form is that array's first element, exactly as for a range source --
       leaving it out typed `[1,2,3].lazy.first` nil and the pipeline's value
       was discarded (#3357). */
    if (lazy_src >= 0 &&
        (ty_is_array(lst) ||
         (lst == TY_UNKNOWN && nt_type(nt, lazy_src) &&
          sp_streq(nt_type(nt, lazy_src), "ArrayNode"))))
      { *out = (argc == 1) ? TY_POLY_ARRAY : (argc == 0 ? TY_POLY : TY_UNKNOWN); return 1; }
  }
  }

  /* `<source>.lazy.<ops>.size` -> the propagated source size: nil if any stage
     changes the element count, Float::INFINITY for an unbounded endless source,
     else Integer. (#2485) */
  if (sp_streq(name, "size") && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    int cur = recv, lazy_src = -1, ok = 1, kill = 0, has_take = 0;
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) { lazy_src = nt_ref(nt, cur, "receiver"); break; }
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop")) && nt_ref(nt, cur, "block") < 0) {
        if (sp_streq(nm, "take")) has_take = 1;
        cur = nt_ref(nt, cur, "receiver"); continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (sp_streq(nm, "map") || sp_streq(nm, "collect")) { cur = nt_ref(nt, cur, "receiver"); continue; }
      if (sp_streq(nm, "select") || sp_streq(nm, "filter") || sp_streq(nm, "find_all") ||
          sp_streq(nm, "reject") || sp_streq(nm, "take_while") || sp_streq(nm, "drop_while") ||
          sp_streq(nm, "filter_map") || sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) {
        kill = 1; cur = nt_ref(nt, cur, "receiver"); continue;
      }
      ok = 0; break;
    }
    /* unwrap `(1..n)` parentheses so the endless check sees the RangeNode */
    lazy_src = an_unparen(nt, lazy_src);
    if (ok && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      int is_arr_lit = nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ArrayNode");
      if (st == TY_RANGE || ty_is_array(st) || is_arr_lit) {
        if (kill) { *out = TY_POLY; return 1; }   /* nil */
        int endless = 0;
        if (st == TY_RANGE && nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "RangeNode"))
          endless = lazy_endpoint_is_infinite(c, nt_ref(nt, lazy_src, "right"));
        if (endless && !has_take) { *out = TY_FLOAT; return 1; }   /* Float::INFINITY */
        { *out = TY_INT; return 1; }
      }
    }
  }

  /* General lazy pipeline: <int range | int array>.lazy.<map/select/reject/
     filter/take_while...>.{first(n) | take(n) | to_a | force} -> an int array. */
  if ((sp_streq(name, "first") ||
       sp_streq(name, "to_a") || sp_streq(name, "force")) &&
      recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "CallNode") ||
       (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") &&
        lazy_alias_chain(c, recv) >= 0)) &&
      nt_ref(nt, id, "block") < 0 &&
      !(sp_streq(name, "first") && argc > 1) &&
      !((sp_streq(name, "to_a") || sp_streq(name, "force")) && argc != 0)) {
    int cur = recv, lazy_src = -1, ok = 1, saw_op = 0;
    /* the chain may be held in a variable (#3012); resolve it like the
       first/last arm above does */
    if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
      int a = lazy_alias_chain(c, cur);
      if (a >= 0) cur = a;
    }
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) {
        int lrcv9 = nt_ref(nt, cur, "receiver");
        if (chain_is_lazy_valued(c, lrcv9)) { saw_op = 1; cur = lrcv9; continue; }
        lazy_src = lrcv9;
        break;
      }
      /* blockless counter stages fuse into the pipeline (codegen re-validates
         the single integer argument). */
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop") ||
           (sp_streq(nm, "each_slice") && cur == recv) ||
           sp_streq(nm, "with_index") || sp_streq(nm, "each_with_index") ||
           sp_streq(nm, "each_cons")) &&
          nt_ref(nt, cur, "block") < 0) {
        saw_op = 1; cur = nt_ref(nt, cur, "receiver");
        if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
          int a9 = lazy_alias_chain(c, cur);
          if (a9 >= 0) cur = a9;
        }
        continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (!sp_streq(nm, "map") && !sp_streq(nm, "collect") && !sp_streq(nm, "select") &&
          !sp_streq(nm, "filter") && !sp_streq(nm, "reject") && !sp_streq(nm, "take_while") &&
          !sp_streq(nm, "drop_while") &&
          !sp_streq(nm, "filter_map") && !sp_streq(nm, "flat_map") &&
          !sp_streq(nm, "collect_concat")) { ok = 0; break; }
      saw_op = 1;
      cur = nt_ref(nt, cur, "receiver");
      if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
        int a = lazy_alias_chain(c, cur);
        if (a >= 0) cur = a;
      }
    }
    /* An endless range needs no `.lazy` to be one: there is no array to
       materialize, so the pipeline over it is the only well-defined reading
       (#3840). */
    if (ok && lazy_src < 0 && saw_op) {
      int rr = an_unparen(nt, cur);
      if (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "RangeNode") &&
          nt_ref(nt, rr, "right") < 0 && nt_ref(nt, rr, "left") >= 0)
        lazy_src = rr;
    }
    /* `first` needs no stage between it and the lazy source: `e.lazy.first(2)`
       is as well defined as `e.lazy.map { }.first(2)` (#3586) */
    if (ok && (saw_op || sp_streq(name, "to_a") || sp_streq(name, "force") ||
               sp_streq(name, "first")) && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      /* bare `first` unwraps to the single element, not the collected array */
      TyKind res = (sp_streq(name, "first") && argc == 0) ? TY_POLY : TY_POLY_ARRAY;
      if (st == TY_RANGE || st == TY_INT_ARRAY || st == TY_ENUMERATOR ||
          st == TY_POLY_ARRAY || st == TY_STR_ARRAY || st == TY_FLOAT_ARRAY) { *out = res; return 1; }
      /* a source only known at run time streams as an Enumerator over it
         (sp_poly_lazy_src), unless the program defines a lazy of its own */
      if (st == TY_POLY && !an_user_recv_defines_method(c, "lazy")) { *out = res; return 1; }
      /* an empty array literal has no element type and so types UNKNOWN, but
         the pipeline over it is still well defined -- it yields [] (#2996) */
      if (st == TY_UNKNOWN && nt_type(nt, lazy_src) &&
          sp_streq(nt_type(nt, lazy_src), "ArrayNode")) { *out = res; return 1; }
    }
  }
  return 0;
}

/* A String receiver (infer_call_inner's rules, in their order) */
static int infer_string_recv_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    /* promote mode: a String#to_i past sp_int is a Bignum (sp_str_to_i_promote).
       It reads the mode, so it sits ahead of the to_i row. */
    if (g_promote_mode && sp_streq(name, "to_i") && argc <= 1) { *out = TY_POLY; return 1; }
    /* builtin-op rows (builtin_ops.c): the calls typed by name, arity and
       block form */
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    /* casecmp/casecmp? with a statically non-string argument: CRuby answers
       nil rather than raising, so the call types nil (the emitter drops the
       comparison and evaluates the argument for effect). */
    if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1) {
      TyKind at0 = infer_type(c, argv[0]);
      if (at0 == TY_POLY) { *out = TY_POLY; return 1; }  /* runtime tag decides: boxed result or nil */
      /* an operand that answers #to_str is converted and compared
         (rb_check_string_type). The call is typed POLY, not the Integer or
         boolean a String operand gives, because the conversion can still
         answer nothing: a #to_str answering nil is CRuby's nil casecmp. The
         emitter takes the same shape test, so both agree. */
      if (ty_is_object(at0) && class_has_to_str_shape(c, ty_object_class(at0)))
        { *out = TY_POLY; return 1; }
      if (at0 != TY_STRING && at0 != TY_UNKNOWN) { *out = TY_NIL; return 1; }
      { *out = sp_streq(name, "casecmp") ? TY_INT : TY_BOOL; return 1; }
    }
    if (sp_streq(name, "unpack1") && (argc == 1 || argc == 2)) { *out = an_unpack1_lit_type(nt, argv[0]); return 1; }
    /* byteindex/byterindex over a String or Regexp needle -> byte offset or
       nil (SP_INT_NIL). */
    if ((sp_streq(name, "byteindex") || sp_streq(name, "byterindex")) &&
        (argc == 1 || argc == 2) &&
        (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_REGEX))
      { *out = TY_INT; return 1; }
    /* each_line and lines read their separator argument */
    if (sp_streq(name, "each_line") && argc == 0 && nt_ref(nt, id, "block") < 0)
      { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))
      { *out = TY_ENUMERATOR; return 1; }  /* each_line(chomp: ...) blockless */
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        infer_type(c, argv[0]) == TY_STRING)
      { *out = TY_ENUMERATOR; return 1; }  /* each_line(sep) blockless */
    if (sp_streq(name, "lines") && argc == 1 && infer_type(c, argv[0]) == TY_STRING)
      { *out = TY_STR_ARRAY; return 1; }   /* lines(sep) */
    if (sp_streq(name, "lines") && argc == 2 && infer_type(c, argv[0]) == TY_STRING &&
        nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode"))
      { *out = TY_STR_ARRAY; return 1; }   /* lines(sep, chomp: true) (#3546) */
    if (sp_streq(name, "each_line")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "lines")) { *out = nt_ref(nt, id, "block") >= 0 ? TY_STRING : TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "scan") && argc == 1) {
      /* the block form iterates and returns self (the receiver string) */
      if (nt_ref(nt, id, "block") >= 0) { *out = TY_STRING; return 1; }
      /* scan with capture groups returns poly_array (array of arrays or
         strings). Whether the rows are whole matches or capture rows is a
         property of the pattern's SOURCE, and the pattern may be reached
         through a name: a constant or a local bound to a literal is exactly as
         visible as the literal, which is what an_regex_lit_src resolves (and
         what codegen's re_lit_node resolves on its side). Reading only a direct
         literal node left the two disagreeing the moment the pattern had a
         name -- a capturing constant compiled to sp_re_scan_poly under a
         str_array type (#3391). */
      const char *rsrc = an_regex_lit_src(c, argv[0]);
      if (rsrc) { *out = an_re_has_captures(rsrc) ? TY_POLY_ARRAY : TY_STR_ARRAY; return 1; }
      /* A regex whose source is not visible at all -- an interpolated literal,
         an inline `Regexp.new(s)`, a method's return -- can only be asked at
         run time whether it captures, so take the shape that answers both:
         sp_re_scan_poly pushes the whole match when the pattern has no groups
         and a captures row when it does (#3389). */
      if (infer_type(c, argv[0]) == TY_REGEX) { *out = TY_POLY_ARRAY; return 1; }
      /* so is a BOXED pattern, a Regexp or a String only at run time, as the
         poly-receiver rule already has it (sp_scan_boxed_poly) */
      if (infer_type(c, argv[0]) == TY_POLY) { *out = TY_POLY_ARRAY; return 1; }
      { *out = TY_STR_ARRAY; return 1; }
    }
    /* blockless gsub(pattern) and gsub!(pattern): an Enumerator of matches,
       for a literal or a pattern held in a value (a Regexp, a String, or
       either one boxed) */
    if ((sp_streq(name, "gsub") || sp_streq(name, "gsub!")) && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        ((nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RegularExpressionNode")) ||
         infer_type(c, argv[0]) == TY_REGEX || infer_type(c, argv[0]) == TY_STRING ||
         infer_type(c, argv[0]) == TY_POLY))
      { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "gsub")) { *out = TY_STRING; return 1; }
  }
  return 0;
}

/* An Integer or Float receiver, and a numeric step (infer_call_inner's rules, in their order) */
static int infer_int_float_recv_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* numeric.step(...) without a block materializes the sequence as an array */
  /* a Float's step with a block answers the receiver, as an Integer's does
     (the rule below is inside the Integer arm); it read as nothing, and the
     value printed nil (#4763) */
  if (recv >= 0 && rt == TY_FLOAT && sp_streq(name, "step") && nt_ref(nt, id, "block") >= 0) { *out = TY_FLOAT; return 1; }
  if (recv >= 0 && rt == TY_BIGINT && sp_streq(name, "step") && nt_ref(nt, id, "block") >= 0) { *out = TY_BIGINT; return 1; }   /* #4779 */
  if (recv >= 0 && ty_is_numeric(rt) && sp_streq(name, "step") && nt_ref(nt, id, "block") < 0) {
    int args = nt_ref(nt, id, "arguments");
    int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
    int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
              (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
    /* a Bignum limit or step walks the sequence boxed (#3006) */
    for (int sk = 0; sk < sc; sk++)
      if (infer_type(c, sv[sk]) == TY_BIGINT) { *out = TY_POLY_ARRAY; return 1; }
    { *out = isf ? TY_FLOAT_ARRAY : TY_INT_ARRAY; return 1; }
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    /* pow(exp, mod) with a Bignum modulus stays in bigint; lcm with a Bignum
       argument is at least that large (#3006) */
    if (sp_streq(name, "pow") && argc == 2 && infer_type(c, argv[1]) == TY_BIGINT)
      { *out = TY_BIGINT; return 1; }
    if (sp_streq(name, "lcm") && argc == 1 && infer_type(c, argv[0]) == TY_BIGINT)
      { *out = TY_BIGINT; return 1; }
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    /* pow with a literal negative exponent yields the exact Rational */
    if (sp_streq(name, "pow") && argc == 1 && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
        nt_int(nt, argv[0], "value", 0) < 0) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "pow") && argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "pow") && argc >= 1) { *out = TY_INT; return 1; }
    /* clamp keeps the applied operand's class: a Float bound can be returned, so
       the mixed int-receiver/float-bound form is poly; pure-int stays Integer. */
    if (sp_streq(name, "clamp")) {
      if (argc == 2) {
        TyKind b0 = infer_type(c, argv[0]), b1 = infer_type(c, argv[1]);
        if (b0 == TY_FLOAT || b1 == TY_FLOAT || b0 == TY_POLY || b1 == TY_POLY) { *out = TY_POLY; return 1; }
      }
      { *out = TY_INT; return 1; }
    }
    if (sp_streq(name, "coerce") && argc == 1) {
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_BIGINT) { *out = TY_POLY_ARRAY; return 1; }   /* [big, big] boxed pair (#2419) */
      if (a0 == TY_FLOAT || a0 == TY_RATIONAL || a0 == TY_COMPLEX) { *out = TY_FLOAT_ARRAY; return 1; }
      /* Only a NUMBER coerces. Typing anything else as the int pair put the
         argument straight into an sp_int slot, so a String stopped the C build
         and a nil answered a coerced 0 where CRuby raises (#4011). The boxed
         pair carries whatever the runtime decides, including the raise. */
      if (a0 == TY_POLY) { *out = TY_POLY_ARRAY; return 1; }   /* the tag decides at run time */
      /* everything else is the Float() pair (and its errors) */
      if (a0 != TY_INT && a0 != TY_UNKNOWN) { *out = TY_FLOAT_ARRAY; return 1; }
      { *out = TY_INT_ARRAY; return 1; }
    }
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    /* promote mode promises that an integer result too wide for a machine
       word widens instead of failing, and a Float conversion asks the same
       question: (2.0**70).floor is a Bignum in CRuby, where raise mode
       answers RangeError (#4688). Only the forms whose result IS an Integer
       widen; `round(2)` stays a Float, and raise/wrap keep their sp_int so
       no hot loop boxes for this. */
    if (g_promote_mode) {
      /* A trailing `half:` keyword only picks the tie-break mode, and the arm
         that serves it answers the same Integer, so the same widening
         applies: `f.round(half: :even)` cannot be the one form that fails
         where `f.round` promotes. It is peeled off the positional count.
         floor / ceil / truncate reject a keyword outright and keep the arm
         that raises, so they stay where they were. */
      int pv_kw = argc >= 1 && nt_type(nt, argv[argc - 1]) &&
                  sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode");
      int pv_round_kw = pv_kw && sp_streq(name, "round");
      if (!pv_kw || pv_round_kw) {
        int pv_argc = argc - (pv_round_kw ? 1 : 0);
        if ((sp_streq(name, "to_i") || sp_streq(name, "to_int")) && argc == 0) { *out = TY_POLY; return 1; }
        /* No argument, or a literal 0 -- both reach the same emitter and are
           exact. A NEGATIVE literal does not: it rounds to a power of ten by
           dividing and multiplying in double, which loses bits past 2**53, so
           (2.0**70).floor(-1) came out ...424 where CRuby says ...420.
           Widening that would trade a RangeError for a wrong answer, so it
           keeps raising until the rounding itself is done in Bignum. */
        if (is_round_family(name)) {
          if (pv_argc == 0) { *out = TY_POLY; return 1; }
          if (pv_argc == 1) {
            const char *pv_aty = nt_type(nt, argv[0]);
            if (pv_aty && sp_streq(pv_aty, "IntegerNode") &&
                nt_int(nt, argv[0], "value", 0) == 0) { *out = TY_POLY; return 1; }
          }
        }
      }
    }
    /* Float <=> Rational: compare via the rational's float value (#2596) */
    if (sp_streq(name, "<=>") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) { *out = TY_INT; return 1; }
    /* Float#fdiv(Complex) is self / c, a Complex */
    if (sp_streq(name, "fdiv") && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "fdiv") && argc == 1) { *out = TY_FLOAT; return 1; }
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    /* clamp with float bounds returns a float (matches codegen in codegen_call.c);
       a mixed/int bound can return the Integer bound, so leave that poly. */
    if (sp_streq(name, "clamp") && argc == 2 &&
        infer_type(c, argv[0]) == TY_FLOAT && infer_type(c, argv[1]) == TY_FLOAT)
      { *out = TY_FLOAT; return 1; }
    if (is_round_family(name)) {
      /* CRuby chooses the return class from the runtime ndigits value: Integer
         when ndigits <= 0, Float when ndigits > 0. With a literal ndigits we
         match it exactly. A NON-literal ndigits can't be classified statically,
         so the result stays Float and the value is still computed exactly (x
         rounded to n places); only #class differs from CRuby when n turns out
         <= 0 -- the documented residual divergence (docs/float-rounding.md). */
      /* a trailing `half:` keyword only picks the tie-break mode; peel it
         off the positional count for the class choice */
      int fr_argc = argc;
      if (fr_argc >= 1 && nt_type(nt, argv[fr_argc - 1]) &&
          sp_streq(nt_type(nt, argv[fr_argc - 1]), "KeywordHashNode"))
        fr_argc--;
      if (fr_argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        /* Non-literal ndigits: the class (Integer when <= 0, Float when > 0) is
           only known at runtime, so the result is a boxed poly chosen there. */
        if (!aty || !sp_streq(aty, "IntegerNode")) { *out = TY_POLY; return 1; }
        { *out = nt_int(nt, argv[0], "value", 0) > 0 ? TY_FLOAT : TY_INT; return 1; }
      }
      { *out = TY_INT; return 1; }  /* no arg -> self truncated to Integer */
    }
  }
  return 0;
}

/* An arithmetic, comparison, equality or bit operator by its operands' kinds, and a Bignum receiver (infer_call_inner's rules, in their order) */
static int infer_operator_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, TyKind *out) {
  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (sp_streq(name, "%")) { *out = TY_STRING; return 1; }  /* sprintf (array or single value) */
      if (sp_streq(name, "+") || sp_streq(name, "*")) {
        /* `str + x` / `str * n` always yield a String; a poly operand (which
           holds a string at runtime) is coerced via sp_poly_to_s in codegen. */
        { *out = TY_STRING; return 1; }
      }
      { *out = TY_UNKNOWN; return 1; }
    }
    /* array + same-kind -> same kind; different-kind -> poly_array */
    if (sp_streq(name, "+") && ty_is_array(rt) && a0 == rt) { *out = rt; return 1; }
    if (sp_streq(name, "+") && ty_is_array(rt) && ty_is_array(a0) && a0 != rt) { *out = TY_POLY_ARRAY; return 1; }
    /* typed array +/- an empty literal [] (a0 UNKNOWN) keeps the receiver kind */
    if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
         sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
         sp_streq(name, "intersection")) && ty_is_array(rt) && a0 == TY_UNKNOWN) { *out = rt; return 1; }
    /* array * int -> same array type (repeat); array * string -> join string */
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_INT) { *out = rt; return 1; }
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_STRING) { *out = TY_STRING; return 1; }
    if (ty_is_numeric(rt) && ty_is_numeric(a0)) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) { *out = TY_FLOAT; return 1; }
      if (rt == TY_BIGINT || a0 == TY_BIGINT) { *out = TY_BIGINT; return 1; }
      /* --int-overflow=promote: an int `+`, `-` or `*` whose operands are
         not both known constants can escape the word at run time and
         promote to a Bignum (codegen lowers it to sp_poly_add / sub / mul),
         the `<<` and `**` rule. Promotion is otherwise decided per SLOT,
         and a value that never passes through one -- a block parameter, an
         element read, a size -- was typed sp_int at the expression and took
         the raising int helper in the mode whose contract is to promote
         (#4681). `/` and `%` cannot leave the word. */
      if (g_promote_mode && rt == TY_INT && a0 == TY_INT &&
          is_add_sub_mul(name)) {
        long long pa, pb;
        if (!(infer_const_int_node(nt, recv, &pa) && infer_const_int_node(nt, argv[0], &pb)))
          { *out = TY_POLY; return 1; }
        /* Two constants escape the word too when their result does: `max + 1`
           was typed sp_int and took the raising helper (#4968). Judged in
           intptr_t, sp_int's own type in the compiler that emits for it. */
        intptr_t ia = (intptr_t)pa, ib = (intptr_t)pb, ir;
        if ((long long)ia != pa || (long long)ib != pb) { *out = TY_POLY; return 1; }
        int ovf = sp_streq(name, "+") ? __builtin_add_overflow(ia, ib, &ir)
                : sp_streq(name, "-") ? __builtin_sub_overflow(ia, ib, &ir)
                : __builtin_mul_overflow(ia, ib, &ir);
        if (ovf) { *out = TY_POLY; return 1; }
      }
      { *out = TY_INT; return 1; }
    }
    /* numeric receiver <op> a coercing user object: the result is what the
       object's own <op> returns (coerce yields a pair of that class). */
    if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_RATIONAL || rt == TY_BIGINT) &&
        ty_is_object(a0)) {
      int acls = ty_object_class(a0);
      if (comp_method_in_chain(c, acls, "coerce", NULL) >= 0) {
        int op_mi = comp_method_in_chain(c, acls, name, NULL);
        /* Only when the pair is of the object's own class does its operator
           decide the type. The documented plain-number idiom never reaches
           that method -- the pair's receiver is a Float -- so taking the
           method's return there answered one operator from the class and
           another from the pair. */
        if (op_mi >= 0 && !class_has_coerce_shape(c, acls))
          { *out = (TyKind)c->scopes[op_mi].ret; return 1; }
        /* The standard idiom answers a pair of plain NUMBERS and defines no
           operator of its own, so the result is whatever the pair computes --
           known only at run time. Left UNKNOWN, the expression emitted a nil
           and `5 + obj` answered nil instead of the coerced sum. The emittable
           test is the one emit_numeric_coerce_call makes, so a #coerce this TU
           cannot call keeps the type it had rather than promising a pair that
           never gets computed. */
        { *out = TY_POLY; return 1; }
      }
    }
    /* a poly operand makes the +,-,*,/ result poly: codegen lowers these to
       sp_poly_<op>, which returns a (boxed) poly, so the static type must agree. */
    if ((rt == TY_POLY || a0 == TY_POLY) &&
        (is_basic_arith(name) ||
         /* every operator codegen lowers the same way. Left out, `>>` took the
            user return from the poly-dispatch union while the emission still
            produced an sp_RbVal, and the two met at the assignment (#3502). */
         sp_streq(name, "%") || sp_streq(name, "**") ||
         sp_streq(name, "<<") || sp_streq(name, ">>") ||
         sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
      { *out = TY_POLY; return 1; }
    /* An Integer/Bignum arith op with a non-coercible (String/Symbol/nil/bool/
       Array/Hash/Range) argument raises TypeError at run time; type the raising
       expression as int so any value position (p, assignment) can emit it -- the
       codegen raise-expr is int-typed too (#2471). */
    if ((rt == TY_INT || rt == TY_BIGINT) &&
        (a0 == TY_STRING || a0 == TY_SYMBOL || a0 == TY_NIL || a0 == TY_BOOL ||
         ty_is_array(a0) || ty_is_hash(a0) || a0 == TY_RANGE))
      { *out = TY_INT; return 1; }
    /* The same rule on the Float side (codegen_call.c's own "coercion rule
       on the Float side", #3645, already emits this raise as `(sp_float)0`)
       -- missing here, this expression stayed TY_UNKNOWN, which poisoned the
       return type of any GENERIC method whose only live branch for a given
       call-site clone was this one (a Ruby-defined Integer#fdiv's `self.to_f
       / other` in the branch that only Integer/Float ever actually reach:
       the SAME expression, unreachable for an Array-typed `other` at THAT
       clone, still has to type as something other than UNKNOWN or the whole
       method compiled as void and every caller's `_t = fdiv(...)` failed to
       compile with "void value not ignored", found migrating fdiv there). */
    if (rt == TY_FLOAT &&
        (a0 == TY_STRING || a0 == TY_SYMBOL || a0 == TY_NIL || a0 == TY_BOOL ||
         ty_is_array(a0) || ty_is_hash(a0) || a0 == TY_RANGE))
      { *out = TY_FLOAT; return 1; }
    { *out = TY_UNKNOWN; return 1; }
  }
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) { *out = TY_INT; return 1; }
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) { *out = TY_BOOL; return 1; }
  if (argc == 1 && is_eq_op(name)) { *out = TY_BOOL; return 1; }

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      is_int_bit_op(name)) {
    /* --int-overflow=promote: an int `<<` whose operands are not both known
       constants can escape the word at run time and promote to a Bignum
       (codegen lowers it to sp_poly_shl), so the value is boxed. A
       const-const pair is exact -- an overflowing one answered TY_BIGINT
       above (the infer_int_shl_overflows arm), the rest fit an int. `>>`
       cannot overflow and keeps its int in every mode. */
    if (g_promote_mode && sp_streq(name, "<<")) {
      long long shb, sha;
      if (!(infer_const_int_node(nt, recv, &shb) &&
            infer_const_int_node(nt, argv[0], &sha)))
        { *out = TY_POLY; return 1; }
    }
    { *out = TY_INT; return 1; }
  }
  /* bigint bitwise ops keep arbitrary precision (a `<<` widening that overflows
     int is exactly why the receiver was promoted to bigint; and `bignum & MASK`
     can still exceed int64, e.g. 0x9e37…c16 & ((1<<64)-1)). */
  if (recv >= 0 && argc == 1 && rt == TY_BIGINT &&
      is_int_bit_op(name))
    { *out = TY_BIGINT; return 1; }
  /* Integer#bit_length on a Bignum answers an int (the bit count fits int64). */
  if (recv >= 0 && argc == 0 && rt == TY_BIGINT && sp_streq(name, "bit_length"))
    { *out = TY_INT; return 1; }
  if (recv >= 0 && rt == TY_BIGINT) {
    if ((sp_streq(name, "even?") || sp_streq(name, "odd?")) && argc == 0) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "abs") && argc == 0) { *out = TY_BIGINT; return 1; }
    /* coerce pairs the operand with self; clamp stays in bigint (#3129) */
    if (sp_streq(name, "coerce") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "clamp") && argc == 2) { *out = TY_BIGINT; return 1; }
    if ((sp_streq(name, "magnitude") || sp_streq(name, "abs2")) && argc == 0) { *out = TY_BIGINT; return 1; }  /* (#2418/#2424) */
    /* Bignum#downto/#upto with no block: materialized poly array of Bignums (#2305) */
    if ((sp_streq(name, "downto") || sp_streq(name, "upto")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "to_s") && argc == 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "digits") && argc <= 1) { *out = TY_INT_ARRAY; return 1; }   /* face-table fallback only, see codegen_call.c */
    /* #2318 / #2319: query + reflection on a Bignum */
    if ((sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
         sp_streq(name, "negative?") || sp_streq(name, "integer?")) && argc == 0) { *out = TY_BOOL; return 1; }
    /* to_i / to_int is self (the full Bignum, not a truncated int); succ/pred
       stay Bignum */
    if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "succ") ||
         sp_streq(name, "next") || sp_streq(name, "pred")) && argc == 0) { *out = TY_BIGINT; return 1; }
    if (sp_streq(name, "class") && argc == 0) { *out = TY_CLASS; return 1; }
    if (is_round_family(name) &&
        (argc == 0 || argc == 1)) { *out = TY_BIGINT; return 1; }  /* #2303 */
    /* Integer/Float/bool-returning Bignum methods that need no Rational (#2469).
       to_r/rationalize/quo would need a bigint-backed Rational and stay
       unsupported. */
    if (sp_streq(name, "~") && argc == 0) { *out = TY_BIGINT; return 1; }
    if ((sp_streq(name, "numerator") || sp_streq(name, "ord")) && argc == 0) { *out = TY_BIGINT; return 1; }
    if ((sp_streq(name, "denominator") || sp_streq(name, "size")) && argc == 0) { *out = TY_INT; return 1; }
    if (sp_streq(name, "nonzero?") && argc == 0) { *out = TY_POLY; return 1; }   /* self or nil */
    if (sp_streq(name, "fdiv") && argc == 1) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "pow") && argc == 1) { *out = TY_BIGINT; return 1; }
    /* A Float operand divides in floats, as CRuby converts the Bignum to its
       nearest double: modulo and remainder answer a Float, and div the
       Integer floor of the Float quotient, which may or may not fit a word */
    if ((sp_streq(name, "modulo") || sp_streq(name, "%") || sp_streq(name, "remainder")) &&
        argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "div") && argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_POLY; return 1; }
    /* modulo/%/remainder/modular-pow stay Bignum; divmod is a [q, r] pair;
       #[] is a single bit (0/1) (#2594) */
    if ((sp_streq(name, "modulo") || sp_streq(name, "%") || sp_streq(name, "remainder")) &&
        argc == 1) { *out = TY_BIGINT; return 1; }
    if (sp_streq(name, "pow") && argc == 2) { *out = TY_BIGINT; return 1; }
    if (sp_streq(name, "divmod") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    /* #[] is a single bit, a bit-slice (Range or start,len) -- all narrowed to
       int here (a very wide slice truncates, like the int-receiver arm) (#3156) */
    if (sp_streq(name, "[]") && (argc == 1 || argc == 2)) { *out = TY_INT; return 1; }
    if ((sp_streq(name, "div") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
         sp_streq(name, "ceildiv")) && argc == 1) { *out = TY_BIGINT; return 1; }
    if (is_bits_query(name) &&
        argc == 1) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "gcdlcm") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    /* to_r/rationalize/quo on a Bignum produce a boxed big Rational (#2469) */
    if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "quo") && argc == 1) { *out = TY_POLY; return 1; }
  }
  /* poly recv bitwise op / `>>`: the runtime keeps a bignum operand in bignum
     space (a positive value past 2^63 must not truncate to a negative int), so
     the result is boxed like the receiver was (#3371). */
  if (recv >= 0 && argc == 1 && rt == TY_POLY &&
      (sp_streq(name, ">>") || sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
    { *out = TY_POLY; return 1; }
  /* poly recv `<<` is ambiguous (Integer#<< shift vs Array#push append); the
     runtime sp_poly_shl dispatches on the tag and returns a boxed result either
     way, so the static type is poly. A downstream bitwise op coerces it back to
     int, and an append keeps its (boxed) array -- both stay consistent. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && sp_streq(name, "<<"))
    { *out = TY_POLY; return 1; }
  /* boolean &/|/^ */
  if (recv >= 0 && argc == 1 && rt == TY_BOOL &&
      is_bit_op(name))
    { *out = TY_BOOL; return 1; }
  return 0;
}

/* A Class receiver, Class and Module reflection, and the declarations of a class body (infer_call_inner's rules, in their order) */
static int infer_class_module_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* k = Struct.new(:a, :b): the value IS the synthesized anonymous struct
     class, as a first-class class object */
  if (anon_struct_ci_for_value(c, id) >= 0) { *out = TY_CLASS; return 1; }

  /* TY_CLASS method dispatch -- .new on a dynamic class variable returns TY_POLY.
     Exception: self.class.new(...) resolves to the enclosing class statically. */
  if (recv >= 0 && rt == TY_CLASS && sp_streq(name, "new") &&
      nt_type(nt, recv) && !sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      !sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
    int _is_self_class = self_class_static_ci(c, recv) >= 0;
    /* a local statically holding one STRUCT class (k = Struct.new(..) /
       k = StructKlass) falls through to the static-class .new arms below --
       its typed member accessors then dispatch statically. A plain class
       stays on the Tier-5 dynamic boxed path (test/dynamic_class_new.rb). */
    int _cvi = _is_self_class ? -1 : class_var_static_ci(c, recv);
    if (!_is_self_class &&
        (_cvi < 0 || !(c->classes[_cvi].is_struct || c->classes[_cvi].is_data)))
      { *out = TY_POLY; return 1; }
  }

  if (recv >= 0 && rt == TY_CLASS && !sp_streq(name, "new")) {
    if (argc == 0 && is_name_reader(name))
      { *out = TY_STRING; return 1; }
    /* Thread.current / Fiber.current through a class value: a boxed handle
       (activesupport's IsolatedExecutionState keeps its scope as the class
       itself); a user class method of that name keeps its dispatch below */
    if (argc == 0 && sp_streq(name, "current") && nt_type(nt, recv) &&
        !sp_streq(nt_type(nt, recv), "ConstantReadNode") && !sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
      int ncc = 0; comp_cmethod_candidates(c, name, &ncc);
      if (ncc == 0) { *out = TY_POLY; return 1; }
    }
    if (argc == 0 && sp_streq(name, "nil?")) { *out = TY_BOOL; return 1; }
    if (argc == 0 && sp_streq(name, "singleton_class?")) { *out = TY_BOOL; return 1; }
    if (argc == 0 && sp_streq(name, "frozen?")) { *out = TY_BOOL; return 1; }
    /* Module#constants -> sym array, recovered from the AST (#2674) */
    /* Class.const_set(:K, v) stores into the existing constant and yields the
       value; only a literal name whose type matches is emittable (#2675). */
    if (sp_streq(name, "const_set") && argc == 2) {
      const char *cs_aty = nt_type(nt, argv[0]);
      const char *cs_qm = NULL;
      if (cs_aty && sp_streq(cs_aty, "SymbolNode")) cs_qm = nt_str(nt, argv[0], "value");
      else if (cs_aty && sp_streq(cs_aty, "StringNode")) cs_qm = nt_str(nt, argv[0], "content");
      LocalVar *cv = cs_qm ? comp_const(c, cs_qm) : NULL;
      if (cv && cv->type != TY_UNKNOWN) { *out = cv->type; return 1; }
    }
    if (sp_streq(name, "constants") && argc <= 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "class_variables") && argc == 0) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "included_modules") && argc == 0) { *out = TY_POLY_ARRAY; return 1; }
    if (argc == 0 && sp_streq(name, "class")) { *out = TY_CLASS; return 1; }
    /* #superclass is a (nullable) class value: BasicObject's is the nil-class
       sentinel, carried within TY_CLASS (#2654). */
    if (argc == 0 && sp_streq(name, "superclass")) { *out = TY_CLASS; return 1; }
    if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "!=") ||
                      sp_streq(name, "==="))) { *out = TY_BOOL; return 1; }
    /* Class ordering is tri-state: true/false when related, nil when the two
       classes have no subclass relationship (CRuby). <=> is -1/0/1 or nil.
       A class that defines the operator itself answers with its own method's
       type, from the class-method dispatch below. */
    if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
                      sp_streq(name, ">=") || sp_streq(name, "<=>"))) {
      int oci = class_recv_static_ci(c, recv);
      if (oci < 0 || comp_cmethod_in_chain(c, oci, name, NULL) < 0) { *out = TY_POLY; return 1; }
    }
    if (argc == 0 && sp_streq(name, "ancestors")) { *out = TY_POLY_ARRAY; return 1; }
    if (argc == 0 && sp_streq(name, "subclasses")) { *out = TY_POLY_ARRAY; return 1; }
    if (argc == 1 && is_kind_query(name)) { *out = TY_BOOL; return 1; }
    if (argc == 1 && sp_streq(name, "include?")) { *out = TY_BOOL; return 1; }
    if (argc == 1 && sp_streq(name, "class_variable_defined?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "class_variable_get") || sp_streq(name, "class_variable_set")) { *out = TY_POLY; return 1; }
    if (argc <= 1 && (sp_streq(name, "instance_methods") ||
                      sp_streq(name, "public_instance_methods") ||
                      sp_streq(name, "private_instance_methods") ||
                      sp_streq(name, "protected_instance_methods"))) { *out = TY_POLY; return 1; }
    if (argc <= 1 && sp_streq(name, "singleton_methods") &&
        nt_kind(nt, recv) == NK_ConstantReadNode &&
        an_class_singleton_methods_listable(c, comp_class_index(c, nt_str(nt, recv, "name"))))
      { *out = TY_POLY; return 1; }
    /* a user class method on a Class-typed value carried in a plain variable
       (`model.table_name`): unify the return types of every user class
       defining it (poly on disagreement). A constant/accessor receiver keeps
       its existing dispatch, so only fire for a variable receiver (#2445). */
    {
      int recv_is_var = class_recv_is_dynamic(c, recv);
      TyKind uret = TY_UNKNOWN; int nc = recv_is_var ? 0 : -1000, set = 0;
      int ncc = 0, nblk = 0;
      int has_blk = nt_ref(nt, id, "block") >= 0, splat = 0;
      for (int a = 0; a < argc; a++)
        if (nt_kind(nt, argv[a]) == NK_SplatNode) splat++;
      const PolyCand *ccs = recv_is_var ? comp_cmethod_candidates(c, name, &ncc) : NULL;
      for (int ki = 0; ki < ncc; ki++) {
        int k = ccs[ki].cls;
        if (is_builtin_reopen(c->classes[k].name)) continue;
        int kmi = ccs[ki].mi;
        /* A candidate that yields, takes a block, or has a rest param has no
           arm the emitter can build, so it kills the whole dispatch. A
           candidate with the WRONG ARITY does not: this call cannot reach it,
           so it neither contributes a return type nor vetoes the others
           (#4129). Codegen's cls_arm_takes_argc is the same rule.
           A candidate taking a block (a yielding one through its
           proc form) is reached by the poly receiver's class-tag dispatch
           instead, which answers poly. */
        if (c->scopes[kmi].rest_idx < 0 &&
            (c->scopes[kmi].yields || (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0]))) {
          if (splat ? argc - splat <= c->scopes[kmi].nparams : argc >= c->scopes[kmi].nrequired && argc <= c->scopes[kmi].nparams) nblk++;
          continue;
        }
        /* a *rest the emitter packs is an arm like any other */
        int rest_ok = rest_packable_arm(c, &c->scopes[kmi]);
        if ((c->scopes[kmi].rest_idx >= 0 && !rest_ok) || c->scopes[kmi].yields ||
            (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0])) { nc = 0; nblk = 0; break; }
        /* with a splat, the count is the gather's to judge at run time: each
           splat may spread to nothing, so only the other arguments count */
        if (splat ? c->scopes[kmi].rest_idx < 0 && argc - splat > c->scopes[kmi].nparams
                  : argc < c->scopes[kmi].nrequired ||
                    (c->scopes[kmi].rest_idx < 0 && argc > c->scopes[kmi].nparams)) continue;
        nc++;
        TyKind kr = (TyKind)c->scopes[kmi].ret;
        if (!set) { uret = kr; set = 1; }
        else if (kr != uret) uret = TY_POLY;
      }
      if (nblk > 0) { *out = TY_POLY; return 1; }
      if (nc > 0 && !has_blk)
        { *out = (uret == TY_UNKNOWN || uret == TY_VOID) ? TY_POLY : uret; return 1; }
    }
  }

  /* `attr_reader :a` in a class body answers the names it defined */
  if (attr_decl_call(c, id)) { *out = TY_POLY_ARRAY; return 1; }
  /* `private def m ... end` in a class body answers :m */
  switch (vis_decl_call(c, id)) {
    case VIS_DECL_NIL: { *out = TY_NIL; return 1; }
    case VIS_DECL_SYM: { *out = TY_SYMBOL; return 1; }
    case VIS_DECL_ARRAY: { *out = TY_POLY_ARRAY; return 1; }
    case VIS_DECL_SELF: { *out = TY_CLASS; return 1; }
  }

  /* __method__ / __callee__ -> the enclosing method's name (a symbol), or
     nil at the top level, where the enclosing scope has no name (matching
     the codegen, which emits sp_box_nil() there) */
  if (recv < 0 && argc == 0 &&
      (sp_streq(name, "__method__") || sp_streq(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    { *out = (s && s->name && s->name[0]) ? TY_SYMBOL : TY_NIL; return 1; }
  }

  /* identity methods: return the receiver unchanged (clone also with its
     freeze: keyword argument) */
  if (recv >= 0 &&
      (argc == 0 ||
       (argc == 1 && sp_streq(name, "clone") && argv && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))) &&
      is_self_copy(name) &&
      /* a generated READER of the name owns it on a concrete object, as any
         reader does in CRuby: fall through to the member-read rule (#4190);
         so does a method the class defines itself, whose value is what its
         body answers -- `def dup = self.class.new(...)` answers boxed (#5461) */
      !(ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) != SP_MEMBER_NONE))
    { *out = rt; return 1; }

  /* bareword freeze (implicit self) returns self, so `def seal = freeze` and
     other freeze-as-value uses stay typed as the instance. (Matches the
     codegen arm that lowers bareword freeze to self.) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "freeze")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      { *out = ty_object(s->class_id); return 1; }
  }
  /* bareword frozen? reads the instance's GC-header bit (see the codegen arm) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "frozen?")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && !s->is_cmethod &&
        comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      { *out = TY_BOOL; return 1; }
  }

  /* x.class -> a first-class Class value for every known receiver kind
     (name-backed for builtins, id-backed for user objects) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "class")) {
    /* empty container literal receivers coerce like everywhere else */
    if (rt == TY_UNKNOWN && nt_type(nt, recv)) {
      const char *rty0 = nt_type(nt, recv);
      int en0 = 0;
      if (sp_streq(rty0, "ArrayNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) { *out = TY_CLASS; return 1; } }
      if (sp_streq(rty0, "HashNode") || sp_streq(rty0, "KeywordHashNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) { *out = TY_CLASS; return 1; } }
      /* an argument-less `Array.new` is that same empty array (#3613) */
      if (sp_streq(rty0, "CallNode") && nt_str(nt, recv, "name") &&
          sp_streq(nt_str(nt, recv, "name"), "new") && nt_ref(nt, recv, "block") < 0) {
        int arn0 = nt_ref(nt, recv, "receiver");
        int aa0 = nt_ref(nt, recv, "arguments"); int aac0 = 0;
        if (aa0 >= 0) nt_arr(nt, aa0, "arguments", &aac0);
        if (aac0 == 0 && arn0 >= 0 && nt_type(nt, arn0) &&
            sp_streq(nt_type(nt, arn0), "ConstantReadNode") &&
            nt_str(nt, arn0, "name") && sp_streq(nt_str(nt, arn0, "name"), "Array"))
          { *out = TY_CLASS; return 1; }
      }
      /* top-level `self.class`: self is main (an Object) -> Object (#3035) */
      if (sp_streq(rty0, "SelfNode")) {
        Scope *ss = comp_scope_of(c, id);
        if (!ss || ss->class_id < 0) { *out = TY_CLASS; return 1; }
      }
    }
    /* a member/method literally named `class` (a Data/Struct member) shadows
       Object#class: fall through to the reader/method dispatch (#2975) */
    if (ty_is_object(rt) &&
        !(ty_object_class(rt) >= 0 && comp_reader_in_chain(c, ty_object_class(rt), "class", NULL)))
      { *out = TY_CLASS; return 1; }
    if (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
        rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL || rt == TY_POLY ||
        rt == TY_METHOD || rt == TY_PROC || rt == TY_IO || rt == TY_ARGF ||
        rt == TY_MATCHDATA || rt == TY_REGEX ||
        rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_CURRY ||
        rt == TY_FIBER || rt == TY_ENUMERATOR || rt == TY_TMS ||
        ty_is_array(rt) || ty_is_hash(rt))
      { *out = TY_CLASS; return 1; }
  }

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "name") || sp_streq(name, "to_s")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class"))
    { *out = TY_STRING; return 1; }

  /* <enc>.encoding.name -> the encoding name string */
  if (recv >= 0 && argc == 0 && sp_streq(name, "name") && rt == TY_POLY &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    { *out = TY_STRING; return 1; }

  /* Module.singleton_writer= / Module.singleton_reader */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
       sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    TyKind sgt;
    if (ci >= 0 && sg_accessor_type(c, ci, name, &sgt)) { *out = sgt; return 1; }
  }
  /* self.singleton_writer= / self.singleton_reader: inside a class method
     or directly in a class/module body (g_cbody_class_id). */
  if ((recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) ||
      (recv < 0 && argc == 0)) {
    Scope *_self = comp_scope_of(c, id);
    int _sg_cid = (_self && _self->is_cmethod && _self->class_id >= 0)
                  ? _self->class_id : g_cbody_class_id;
    if (_sg_cid < 0 && _self && _self->class_id < 0) _sg_cid = c->node_cbody[id];
    TyKind sgt;
    if (_sg_cid >= 0 && sg_accessor_type(c, _sg_cid, name, &sgt)) { *out = sgt; return 1; }
  }

  /* FFI: call on a module that registered ffi_func/ffi_buffer/ffi_read_* */
  if (recv >= 0 && nt_type(nt, recv)) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (sp_streq(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (sp_streq(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      /* native binding (Path B): a native_func returns its declared type,
         gated by the module's require-gate feature. */
      int nvi = comp_native_find(c, rcmod, name);
      if (nvi >= 0) {
        const char *feat = c->native_funcs[nvi].feat;
        if (!feat || !feat[0] || sp_feature_enabled(feat))
          { *out = native_spec_to_ty(c->native_funcs[nvi].ret); return 1; }
      }
      int fi = ffi_find_func(c, rcmod, name);
      if (fi >= 0) { *out = ffi_spec_to_ty(c->ffi_funcs[fi].ret); return 1; }
      /* ffi_buffer: Module.buf_name returns the static char* (ptr type -> TY_POLY) */
      if (ffi_find_buf(c, rcmod, name) >= 0) { *out = TY_POLY; return 1; }
      /* ffi_read_*: Module.reader_name(buf) returns int or ptr */
      int ri = ffi_find_reader(c, rcmod, name);
      if (ri >= 0) {
        const char *kind = c->ffi_readers[ri].kind;
        if (kind && sp_streq(kind, "ptr")) { *out = TY_POLY; return 1; }
        { *out = TY_INT; return 1; }
      }
      /* ffi_struct: Name_new -> ptr, Name_get_<f> -> the field's type,
         Name_set_<f> -> nil. */
      int fsi, ffi;
      int fsm = ffi_struct_method(c, rcmod, name, &fsi, &ffi);
      if (fsm == FFI_SM_NEW) { *out = TY_POLY; return 1; }
      if (fsm == FFI_SM_GET) { *out = ffi_spec_to_ty(c->ffi_structs[fsi].fields[ffi].spec); return 1; }
      if (fsm == FFI_SM_SET) { *out = TY_NIL; return 1; }
      /* ffi_write_*: Module.writer_name(buf, val) returns the written value */
      int wi = ffi_find_writer(c, rcmod, name);
      if (wi >= 0) {
        const char *kind = c->ffi_writers[wi].kind;
        if (kind && sp_streq(kind, "ptr")) { *out = TY_POLY; return 1; }
        { *out = TY_INT; return 1; }
      }
    }
  }

  /* SomeClass.superclass -> sp_Class value for the parent class */
  if (recv >= 0 && argc == 0 && sp_streq(name, "superclass") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    { *out = TY_CLASS; return 1; }

  /* SomeClass.ancestors -> PolyArray of class objects */
  if (recv >= 0 && argc == 0 && sp_streq(name, "ancestors") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    { *out = TY_POLY_ARRAY; return 1; }

  /* SomeClass.instance_methods / .public_instance_methods -> PolyArray of symbols */
  if (recv >= 0 && argc <= 1 &&
      (sp_streq(name, "instance_methods") || sp_streq(name, "public_instance_methods") ||
       sp_streq(name, "private_instance_methods") || sp_streq(name, "protected_instance_methods")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    { *out = TY_POLY_ARRAY; return 1; }

  /* self.class.new(...) in a class no class inherits from -> an instance of it */
  if (recv >= 0 && sp_streq(name, "new") && self_class_static_ci(c, recv) >= 0)
    { *out = ty_object(self_class_static_ci(c, recv)); return 1; }

  /* allocate on the class the method runs for: an instance of that class
     (a subclass's, typed as the base, as a bare `new` in a class method is) */
  {
    int oc = allocate_on_own_class(c, id);
    if (oc >= 0 && !class_is_exc_subclass(c, oc)) { *out = ty_object(oc); return 1; }
  }
  /* Class#allocate -> a bare instance of that class (no initialize run). */
  if (recv >= 0 && sp_streq(name, "allocate") && argc == 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      /* Use the same exception-subclass predicate as codegen (class_is_exc_subclass)
         so inference and emission agree on which classes take the allocate path. */
      if (ci >= 0 && !class_is_exc_subclass(c, ci)) { *out = ty_object(ci); return 1; }
      const char *bcn = nt_str(nt, recv, "name");
      if (bcn && sp_streq(bcn, "String")) { *out = TY_STRING; return 1; }
      if (bcn && sp_streq(bcn, "Array"))  { *out = TY_POLY_ARRAY; return 1; }
      if (bcn && sp_streq(bcn, "Hash"))   { *out = TY_POLY_POLY_HASH; return 1; }
      if (bcn && sp_streq(bcn, "Object")) { *out = TY_POLY; return 1; }
    }
  }
  return 0;
}

/* A call with no receiver (implicit self, a class body's, a free function) and a Kernel conversion sent to an explicit receiver (infer_call_inner's rules, in their order) */
static int infer_receiverless_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind *out) {
  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0 && ie_class_of(c, id) < 0) {
      /* inside a class method self is the class, so its class methods come
         first, as comp_self_call_mi and the inline splice resolve the call:
         an instance method or reader of the same name stood in for
         `def self.go` and typed the call by the wrong body */
      if (self->is_cmethod && !sp_streq(name, "new")) {
        int cmi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
        if (cmi >= 0) { *out = an_self_call_ret(c, self, name, cmi, id); return 1; }
      }
      { int rdcls2 = -1;
        if (comp_reader_in_chain(c, self->class_id, name, &rdcls2)) {
          const char *rname2 = comp_resolve_alias(c, self->class_id, name);
          char ivn[256];
          snprintf(ivn, sizeof ivn, "@%s", rname2);
          ClassInfo *rci2 = (rdcls2 >= 0 && rdcls2 < c->nclasses) ? &c->classes[rdcls2] : &c->classes[self->class_id];
          int iv = comp_ivar_index(rci2, ivn);
          if (iv >= 0) {
            /* a def in a subclass overrides the reader for that subclass */
            TyKind rt2 = ivar_value_ty(rci2, iv);
            int base_mi2 = comp_method_in_chain(c, self->class_id, name, NULL);
            for (int k = 0; k < c->nclasses; k++) {
              if (k == self->class_id || !is_descendant(c, k, self->class_id)) continue;
              int kmi = comp_method_in_chain(c, k, name, NULL);
              if (kmi < 0 || kmi == base_mi2) continue;
              TyKind kr = (TyKind)c->scopes[kmi].ret;
              if (kr != TY_UNKNOWN && kr != rt2) rt2 = ty_unify(rt2, kr);
            }
            { *out = rt2; return 1; }
          }
        }
      }
      /* bare `new` inside a class method returns an instance of self's class */
      if (self->is_cmethod && sp_streq(name, "new"))
        { *out = ty_object(self->class_id); return 1; }
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi < 0 && self->is_cmethod)
        mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) { *out = an_self_call_ret(c, self, name, mi, id); return 1; }
      /* A reopened Object's method is every object's: a bare call to it from
         an instance method reaches it with self as the receiver (the codegen
         boxes self for it), so the call answers the method's return. Below
         the builtin surface, as Ruby's lookup puts Object last. */
      if (mi < 0 && !self->is_cmethod) {
        int oc = comp_class_index(c, "Object");
        int omi = oc >= 0 && oc != self->class_id ? comp_method_in_chain(c, oc, name, NULL) : -1;
        if (omi >= 0) { *out = c->scopes[omi].ret; return 1; }
      }
      /* Method defined only in descendants (not in base chain): unify the
         return types of all descendant implementations -- codegen emits a
         cls_id virtual dispatch for exactly this shape, so leaving the node
         UNKNOWN made emit_boxed discard the dispatch's value through the
         effect-comma nil (a Comparable base <=> over int/float subclass
         keys compared nil, #3237). Instance methods included. */
      {
        TyKind r = TY_UNKNOWN; int found = 0;
        int nd = 0; const int *ds = comp_descendants(c, self->class_id, &nd);
        for (int di = 0; di < nd; di++) {
          int k = ds[di];
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name)
                                     : comp_method_in_class(c, k, name);
          if (dmi < 0) continue;
          r = found ? ty_unify(r, (TyKind)c->scopes[dmi].ret) : (TyKind)c->scopes[dmi].ret;
          found = 1;
        }
        if (found) { *out = r; return 1; }
      }
    }
  }

  /* bare call inside a module/class body -> class method of that module/class.
     Use the per-node enclosing-cbody: g_cbody_class_id is only set during the
     scope pass, not the inference fixpoint, so relying on it leaves a bare
     module-body cmethod call (e.g. `take(mk)` where mk is `def self.mk`) typed
     void. The scope pass records the enclosing cbody per node in node_cbody[id]
     (cf. analyze_scope.c, analyze.c which already read it during inference). */
  if (recv < 0) {
    int cbody = c->node_cbody[id];
    if (cbody < 0) cbody = g_cbody_class_id;
    if (cbody >= 0) {
      int smi = comp_cmethod_in_chain(c, cbody, name, NULL);
      if (smi >= 0) { *out = an_user_call(c, id, smi, UC_CMETH, cbody); return 1; }
    }
  }
  /* bare call inside an instance_eval/exec block: dispatch on receiver class */
  if (recv < 0) {
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) { *out = an_user_call(c, id, imi, UC_IE, iec); return 1; }
      TyKind at;
      if (argc == 0 && attr_reader_ty(c, iec, name, &at)) { *out = at; return 1; }
    }
    int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    TyKind pr = TY_UNKNOWN;
    for (int i = 0; i < npk; i++) {
      int imi = comp_method_in_chain(c, pk[i], name, NULL);
      if (imi < 0) continue;
      TyKind t = method_call_ret(c, imi, id);
      pr = pr == TY_UNKNOWN ? t : ty_unify(pr, t);
    }
    if (pr != TY_UNKNOWN) { *out = pr; return 1; }
  }
  /* Kernel conversion with an explicit user-object receiver: obj.send(:Float, x)
     desugars to obj.Float(x); the private Kernel method is available on every
     object, so it types like the receiverless form when the receiver's chain
     does not define the name (mirrors the codegen dispatch).
     The NAME gate must come first: inferring the receiver for every 1/2-arg
     call added an infer_call<->infer_type recursion edge that other arms
     avoid structurally, and looped forever on whole-program shapes with
     zero conversion calls (the tep regression). Only the six capitalized
     conversion names ever pay the receiver inference. */
  /* Kernel#Integer/#Float take `exception: false`; the keyword hash is not one
     of the value arguments, so it must not shift the arity (#3718) */
  int kw_argc = argc;
  if (argc > 0) {
    const char *lkt = nt_type(c->nt, argv[argc - 1]);
    if (lkt && sp_streq(lkt, "KeywordHashNode")) kw_argc--;
  }
  if (recv >= 0 && (kw_argc == 1 || kw_argc == 2) && name[0] >= 'A' && name[0] <= 'Z' &&
      (sp_streq(name, "Integer") || sp_streq(name, "Float") ||
       sp_streq(name, "String") || sp_streq(name, "Rational") ||
       sp_streq(name, "Complex") || sp_streq(name, "Array") ||
       sp_streq(name, "Hash"))) {
    TyKind krt = infer_type(c, recv);
    int kdisp = (ty_is_object(krt) &&
                 comp_method_in_chain(c, ty_object_class(krt), name, NULL) < 0) ||
                ((krt == TY_NIL || krt == TY_POLY || krt == TY_UNKNOWN) &&
                 comp_method_index(c, name) < 0);
    if (kdisp) {
      if (sp_streq(name, "Integer") && (kw_argc == 1 || kw_argc == 2)) {
        /* a Float argument converts to an Integer that promote mode lets be a
           Bignum, as Float#to_i does there (#4688) */
        if (g_promote_mode && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_POLY; return 1; }
        { *out = kconv_integer_kind(c, argv[0], kw_argc < argc && kconv_noraise_kw(c, argc, argv)); return 1; }
      }
      if (kw_argc == 1) {
        if (sp_streq(name, "Float"))    { *out = TY_FLOAT; return 1; }
        if (sp_streq(name, "String"))   { *out = TY_STRING; return 1; }
        if (sp_streq(name, "Rational")) { *out = TY_RATIONAL; return 1; }
        if (sp_streq(name, "Complex"))  { *out = TY_COMPLEX; return 1; }
        /* the emitter takes Hash on such a receiver as Kernel's too
           (codegen_call_kernel.c); left untyped, its value read as nil */
        if (sp_streq(name, "Hash"))     { *out = kconv_hash_kind(c, argv[0]); return 1; }
        if (sp_streq(name, "Array")) {
          TyKind kat = infer_type(c, argv[0]);
          if (ty_is_array(kat)) { *out = kat; return 1; }
          if (kat == TY_INT)    { *out = TY_INT_ARRAY; return 1; }
          if (kat == TY_FLOAT)  { *out = TY_FLOAT_ARRAY; return 1; }
          if (kat == TY_STRING) { *out = TY_STR_ARRAY; return 1; }
          /* an object answers through its own to_ary/to_a (#3721) */
          if (ty_is_object(kat)) {
            int aci = ty_object_class(kat), ami = comp_method_in_chain(c, aci, "to_ary", NULL);
            if (ami < 0) ami = comp_method_in_chain(c, aci, "to_a", NULL);
            if (ami >= 0) { *out = (TyKind)c->scopes[ami].ret; return 1; }
          }
        }
      }
    }
  }

  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name), fvia = UC_TOP;
    if (mi < 0) { mi = comp_included_method_index(c, name, id); fvia = UC_INCLUDED; }
    if (mi >= 0) { *out = an_user_call(c, id, mi, fvia, fvia == UC_TOP ? -1 : c->scopes[mi].class_id); return 1; }
    /* Kernel conversions */
    if (sp_streq(name, "Integer") && (kw_argc == 1 || kw_argc == 2)) {
      if (g_promote_mode && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_POLY; return 1; }   /* see above (#4688) */
      { *out = kconv_integer_kind(c, argv[0], kw_argc < argc && kconv_noraise_kw(c, argc, argv)); return 1; }
    }
    if (sp_streq(name, "Float") && kw_argc == 1) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "String") && argc == 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "Array") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (ty_is_array(at)) { *out = at; return 1; }
      /* an object answers through its own to_ary/to_a (#3721) */
      if (ty_is_object(at)) {
        int aci2 = ty_object_class(at), ami2 = comp_method_in_chain(c, aci2, "to_ary", NULL);
        if (ami2 < 0) ami2 = comp_method_in_chain(c, aci2, "to_a", NULL);
        if (ami2 >= 0) { *out = (TyKind)c->scopes[ami2].ret; return 1; }
      }
      if (at == TY_INT)    { *out = TY_INT_ARRAY; return 1; }    /* Array(int)   -> [int]   */
      if (at == TY_FLOAT)  { *out = TY_FLOAT_ARRAY; return 1; }  /* Array(float) -> [float] */
      if (at == TY_STRING) { *out = TY_STR_ARRAY; return 1; }    /* Array(str)   -> [str]   */
      if (at == TY_RANGE)  { *out = TY_INT_ARRAY; return 1; }    /* Array(range) enumerates */
      { *out = TY_POLY_ARRAY; return 1; }
    }
    if (sp_streq(name, "Hash") && argc == 1) { *out = kconv_hash_kind(c, argv[0]); return 1; }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && argc >= 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "system") && argc >= 1) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "trap") && argc >= 1) { *out = TY_POLY; return 1; }  /* the previous handler: a command string or a Proc */
    /* at_exit answers the Proc it registered, so the handler stays callable (#3727) */
    if (sp_streq(name, "at_exit") && nt_ref(nt, id, "block") >= 0) { *out = TY_PROC; return 1; }
    if (sp_streq(name, "rand")) {
      if (argc == 0) { *out = TY_FLOAT; return 1; }
      if (infer_type(c, argv[0]) == TY_FLOAT_RANGE) { *out = TY_FLOAT; return 1; }   /* rand(float range) */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && sp_streq(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        int hi = nt_ref(nt, argv[0], "right");
        if (rand_lit_range_float(c, argv[0])) { *out = TY_FLOAT; return 1; }   /* rand(float_range), either bound */
        /* a statically empty/reversed int range yields nil (#2519) */
        if (lo >= 0 && hi >= 0 &&
            nt_type(nt, lo) && sp_streq(nt_type(nt, lo), "IntegerNode") &&
            nt_type(nt, hi) && sp_streq(nt_type(nt, hi), "IntegerNode")) {
          long long lov = nt_int(nt, lo, "value", 0);
          long long hiv = nt_int(nt, hi, "value", 0);
          int excl = (nt_int(nt, argv[0], "flags", 0) & 4) ? 1 : 0;
          if ((excl ? hiv - 1 : hiv) < lov) { *out = TY_POLY; return 1; }   /* nil */
        }
        { *out = TY_INT; return 1; }
      }
      /* rand(int range held in a variable): an Integer, or nil if the range is
         empty at runtime -> a poly (#3221). (A Float range in a variable is
         TY_FLOAT_RANGE, handled above.) */
      if (infer_type(c, argv[0]) == TY_RANGE) { *out = TY_POLY; return 1; }
      /* rand(literal 0) is a Float in [0,1) like rand(); nonzero -> Integer */
      if (atype && sp_streq(atype, "IntegerNode") && nt_int(nt, argv[0], "value", 0) == 0)
        { *out = TY_FLOAT; return 1; }
      if (infer_type(c, argv[0]) == TY_BIGINT) { *out = TY_BIGINT; return 1; }   /* rand(Bignum bound) (#3058) */
      /* rand(Float max): CRuby truncates a positive max to an Integer range and
         returns an Integer, but max 0.0 falls back to a Float in [0,1) -- so a
         Float argument is a runtime-chosen poly (#2549). */
      if (infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_POLY; return 1; }
      /* a literal nonzero Integer is an Integer; a dynamic Integer could be 0
         (Float) or nonzero (Integer), so its result is a runtime-chosen poly. */
      if (atype && sp_streq(atype, "IntegerNode")) { *out = TY_INT; return 1; }
      /* Any dynamic (non-literal) argument -- an Integer var, a poly value, a
         destructured tuple element -- may be 0 (a Float [0,1)) or nonzero (an
         Integer), so codegen boxes the result; type it poly to match rather
         than the old TY_INT default, which left `rand(poly) == 0` comparing a
         boxed value with an int (#2897). */
      { *out = TY_POLY; return 1; }
    }
    if (sp_streq(name, "srand")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "sleep") && argc <= 1) { *out = TY_INT; return 1; }
    if (sp_streq(name, "gets") && argc == 0 && comp_bare_gets_is_argf(c))
      { *out = TY_STRING; return 1; }   /* ARGF's next line, or nil */
  }
  return 0;
}

/* A method the program defines on the receiver (a class method, a Struct's member, a reopened builtin's method) and instance-variable reflection (infer_call_inner's rules, in their order) */
static int infer_user_method_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* Class.cmethod(...) / M::Sub.cmethod(...) -> the class method's return type.
     method_call_ret, not the raw scope ret: a tail-yield class method carries
     THIS call site's block value (the instance/implicit-self arms already
     route through it; the raw ret is the first site's type and a diverging
     second site miscompiled through it). */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    int sci = self_class_static_ci(c, recv);
    if (sci >= 0 || (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))) {
      int ci = sci >= 0 ? sci : comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) { *out = an_user_call(c, id, mi, UC_CMETH, ci); return 1; }
      }
      int rmi = class_reopen_cmethod(c, recv, name);
      if (rmi >= 0) { *out = an_user_call(c, id, rmi, UC_CMETH, c->scopes[rmi].class_id); return 1; }
    }
    /* obj.class.cmeth(...) -> unify class method return types across hierarchy */
    if (rty && sp_streq(rty, "CallNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
      int robj = nt_ref(nt, recv, "receiver");
      TyKind rrt = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
      if (ty_is_object(rrt)) {
        int cid = ty_object_class(rrt);
        int mi = comp_cmethod_in_chain(c, cid, name, NULL);
        if (mi >= 0) {
          TyKind r = (TyKind)c->scopes[mi].ret;
          for (int k = 0; k < c->nclasses; k++) {
            int _desc = 0;
            for (int _p = c->classes[k].parent; _p >= 0; _p = c->classes[_p].parent)
              if (_p == cid) { _desc = 1; break; }
            if (!_desc) continue;
            int kmi = comp_cmethod_in_class(c, k, name);
            if (kmi >= 0) r = ty_unify(r, (TyKind)c->scopes[kmi].ret);
          }
          { *out = r; return 1; }
        }
        TyKind sgt;
        if (!class_has_subclass(c, cid) && sg_accessor_type(c, cid, name, &sgt)) { *out = sgt; return 1; }
      }
    }
  }

  /* Struct instance methods */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct &&
      /* a method written in the Struct.new / Data.define block overrides the
         generated one of that name, so its own return type is the answer
         (#3794), and so does a member accessor: Struct.new(:members) reads
         the member, not the member names -- mirrors the same guard in the
         emitter */
      comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_NONE) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    if (sp_streq(name, "with") && sc->is_data) { *out = rt; return 1; }  /* copy-update returns the same type */
    if (sp_streq(name, "to_a") || sp_streq(name, "values") ||
        sp_streq(name, "deconstruct") || sp_streq(name, "members")) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "to_h")) {
      int block = nt_ref(nt, id, "block");
      if (block >= 0) {
        /* to_h { |k,v| [nk, nv] }: hash type from the block's pair */
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, els[0]), vt = infer_type(c, els[1]);
            if (kt == TY_SYMBOL) { *out = TY_SYM_POLY_HASH; return 1; }
            if (kt == TY_STRING && vt == TY_STRING) { *out = TY_STR_STR_HASH; return 1; }
            if (kt == TY_STRING) { *out = TY_STR_POLY_HASH; return 1; }
            TyKind h = ty_hash_of(kt, vt);
            /* the key is not a String here, so the string-keyed fallback would
               have put whatever the block answers into a const char * slot
               (#3602) */
            { *out = h != TY_UNKNOWN ? h : TY_POLY_POLY_HASH; return 1; }
          }
        }
      }
      { *out = TY_SYM_POLY_HASH; return 1; }
    }
    if (is_len_alias(name) && argc == 0) {
      /* a member of that name wins: its generated reader is the method */
      char szn[272]; snprintf(szn, sizeof szn, "@%s", name);
      if (comp_ivar_index(sc, szn) < 0) { *out = TY_INT; return 1; }
    }
    if (sp_streq(name, "values_at")) { *out = TY_POLY_ARRAY; return 1; }   /* no keys selects nothing */
    if (sp_streq(name, "hash") && argc == 0) {
      /* a member of that name wins, like size/length above (#4190) */
      char hn2[272]; snprintf(hn2, sizeof hn2, "@%s", name);
      if (comp_ivar_index(sc, hn2) < 0) { *out = TY_INT; return 1; }
    }
    if (sp_streq(name, "deconstruct_keys") && argc == 1) { *out = TY_SYM_POLY_HASH; return 1; }
    if (sp_streq(name, "dig") && argc >= 1) {
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) {
        TyKind mt = sc->ivar_types[mi];
        if (argc == 1) { *out = mt; return 1; }
        /* nested struct members: walk the remaining literal keys */
        {
          ClassInfo *cur = sc; int cmi = mi; int di = 1;
          while (di < argc) {
            TyKind mt2 = cur->ivar_types[cmi];
            if (!ty_is_object(mt2) || !c->classes[ty_object_class(mt2)].is_struct) break;
            ClassInfo *nx = &c->classes[ty_object_class(mt2)];
            int nmi = struct_member_idx(c, nx, argv[di]);
            if (nmi < 0) break;
            cur = nx; cmi = nmi; di++;
          }
          if (di == argc && di > 1) { *out = cur->ivar_types[cmi]; return 1; }
        }
        /* dig(member, key, ...): index into the member's container */
        if (ty_is_hash(mt) && argc == 2) { *out = ty_hash_val(mt); return 1; }
        if (ty_is_array(mt) && argc == 2) { *out = ty_array_elem(mt); return 1; }
        { *out = TY_POLY; return 1; }
      }
      /* a key no literal member matches resolves at run time (#3849) */
      { *out = TY_POLY; return 1; }
    }
    if (sp_streq(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int]: return specific member type if known */
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) { *out = sc->ivar_types[mi]; return 1; }
      /* integer index: try to resolve literal */
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "IntegerNode")) {
        long long idx = (long long)nt_int(nt, argv[0], "value", 0);
        if (idx < 0) idx += (long long)sc->nmembers;
        if (idx >= 0 && idx < sc->nmembers) { *out = sc->ivar_types[(int)idx]; return 1; }
      }
      { *out = TY_POLY; return 1; }
    }
    if (sp_streq(name, "[]=") && argc == 2) { *out = sc->nmembers > 0 ? sc->ivar_types[0] : TY_POLY; return 1; }
  }

  /* built-in class reopening: look up user-defined methods on scalar built-in
     types -- not for an alias that captured the builtin (builtin_only) */
  if (recv >= 0 && !nt_int(nt, id, "builtin_only", 0)) {
    const char *oc_cn = NULL;
    switch (rt) {
    case TY_STRING: oc_cn = "String"; break;
    case TY_INT:    oc_cn = "Integer"; break;
    case TY_FLOAT:  oc_cn = "Float"; break;
    case TY_SYMBOL: oc_cn = "Symbol"; break;
    case TY_BOOL:   oc_cn = "TrueClass"; break;
    case TY_RANGE:  oc_cn = "Range"; break;
    case TY_TIME:   oc_cn = "Time"; break;
    case TY_THREAD: oc_cn = "Thread"; break;
    case TY_FIBER:  oc_cn = "Fiber"; break;
    case TY_IO:     oc_cn = "File"; break;
    case TY_CLASS:  oc_cn = "Class"; break;
    default: break;
    }
    if (oc_cn) {
      int oc_ci = rt == TY_IO ? io_reopen_class(c, name) : comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0 && rt == TY_IO && io_reopen_ret_mixed(c, name)) { *out = TY_POLY; return 1; }
        if (oc_mi >= 0) { *out = an_user_call(c, id, oc_mi, UC_REOPEN, oc_ci); return 1; }
      }
    }
    /* ...and a nil receiver's NilClass reopen, an Integer or Float
       receiver's Numeric reopen, ahead of Object's: the emitter's reopened-
       builtin dispatch calls them for a name the receiver's own class does
       not define, as CRuby's ancestry does. Answered by Object's method, the
       call was typed from a method it never ran (`3.kind_tag` a String slot
       around Numeric's Float). A name the builtin surface knows stays the
       builtin's, where the emitter's earlier arms keep it. */
    if (rt == TY_NIL || rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) {
      const char *own = rt == TY_NIL ? "NilClass" : rt == TY_FLOAT ? "Float" : "Integer";
      const char *anc = rt == TY_NIL ? "NilClass" : "Numeric";
      if (!builtin_method_known(own, name) && !builtin_object_method_known(name)) {
        int aci = comp_class_index(c, anc);
        int ami = aci >= 0 ? comp_method_in_chain(c, aci, name, NULL) : -1;
        if (ami >= 0 && c->scopes[ami].class_id == aci)
          { *out = an_user_call(c, id, ami, UC_REOPEN, aci); return 1; }
      }
    }
  }

  /* instance_variable_get(:@x) on a POLY receiver: unify @x's declared type
     across every instantiated class that has the slot (all the same concrete
     type -> that type; mixed or none -> poly). Without this the call fell
     through to an unrelated rule and inferred a bogus type, so the whole
     chain was silently dropped. Codegen dispatches on cls_id per class. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") && argc >= 1) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) {
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      if (sym && sym[0] == '@') {
        TyKind uni = TY_UNKNOWN;
        for (int ci = 0; ci < c->nclasses; ci++) {
          if (!c->classes[ci].instantiated) continue;
          int iv = comp_ivar_index(&c->classes[ci], sym);
          if (iv < 0) continue;
          TyKind t = c->classes[ci].ivar_types[iv];
          if (uni == TY_UNKNOWN) uni = t;
          else if (uni != t) { uni = TY_POLY; break; }
        }
        /* a bare Object can hold any value there (sp_Object_ivar_get) */
        if (uni != TY_UNKNOWN && an_program_news_object(c)) uni = TY_POLY;
        { *out = uni == TY_UNKNOWN ? TY_POLY : uni; return 1; }
      }
      { *out = TY_POLY; return 1; }
    }
  }
  /* instance_variable_defined? and instance_variables on a POLY receiver:
     answered per class, a bare Object from its own table */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_defined?") && argc == 1) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) { *out = TY_BOOL; return 1; }
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variables") && argc == 0 &&
      nt_ref(nt, id, "block") < 0)
    { *out = TY_POLY_ARRAY; return 1; }

  /* instance_variable_set(:@x, v) on a POLY receiver answers v, boxed (the
     codegen twin stores it per class) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_set") && argc == 2) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) { *out = TY_POLY; return 1; }
  }

  /* nil? on a pointer-backed Enumerator: bool (NULL-as-nil test) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") && rt == TY_ENUMERATOR)
    { *out = TY_BOOL; return 1; }

  /* frozen? on an immutable value type: constantly-true bool */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?") &&
      (rt == TY_INT || rt == TY_FLOAT || rt == TY_SYMBOL || rt == TY_BOOL ||
       rt == TY_NIL || rt == TY_RANGE || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_BIGINT))
    { *out = TY_BOOL; return 1; }
  return 0;
}

/* A Method or Proc object (method(:x), bind_call, composition, call), with a boxed Time's strftime, which sits between them (infer_call_inner's rules, in their order) */
static int infer_method_proc_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* method(:sym) / <recv>.method(:sym) -> a bound Method object */
  if (name && sp_streq(name, "method") && method_sym_arg(c, id) != NULL) { *out = TY_METHOD; return 1; }
  /* `k.instance_method(:m).bind_call(obj, args)` on a Class value known only
     at run time: whichever class k holds binds its own `m`, each arm boxing
     its answer. */
  if (recv >= 0 && argc >= 1 && sp_streq(name, "bind_call") &&
      class_value_instance_method_sym(c, recv))
    { *out = TY_POLY; return 1; }

  /* <method>.call(args) / [] / bind_call(obj, args) -> the target's return
     type (bind_call = bind(obj).call(args), #3246). */
  if (recv >= 0 && rt == TY_METHOD &&
      (is_call_alias(name) ||
       (sp_streq(name, "===") && argc >= 1) ||
       (sp_streq(name, "bind_call") && argc >= 1))) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (mi >= 0) { *out = c->scopes[mi].ret == TY_UNKNOWN ? TY_INT : c->scopes[mi].ret; return 1; }
    /* Unresolved target -- a Method that arrived through a parameter or a
       slot, or a typed-array adapter (`<array>.method(:op)`) with no target
       scope. The value is whatever the target answers, boxed by the return
       kind its bind site stamped, so the call yields poly (#4445); the
       adapter's kind is stamped from method_obj_adapter_ret at the bind site.
       Reading the raw sp_int as an Integer answered a String's pointer.
       Under promote it was always poly: every method is poly-signatured
       there. */
    { *out = TY_POLY; return 1; }
  }
  if (recv >= 0 && rt == TY_METHOD) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }
  /* A Method read out of a container answers these from its sp_BoundMethod;
     the value is boxed, so the call is poly (#3692). */
  if (recv >= 0 && argc == 0 && infer_type(c, recv) == TY_POLY &&
      sp_streq(name, "owner") && !an_user_defines_or_reads(c, name))
    { *out = TY_POLY; return 1; }
  /* proc << / >> a Proc read out of a container: a composed Proc (#3655) */
  if (recv >= 0 && argc == 1 && (sp_streq(name, "<<") || sp_streq(name, ">>"))) {
    TyKind crt = infer_type(c, recv), cat = infer_type(c, argv[0]);
    /* a curried Proc composes like the proc it stands for (#3864) */
    int cr_p = (crt == TY_PROC || crt == TY_CURRY), ca_p = (cat == TY_PROC || cat == TY_CURRY);
    if ((cr_p && (ca_p || cat == TY_POLY)) ||
        (ca_p && crt == TY_POLY && sp_streq(name, ">>")))
      { *out = TY_PROC; return 1; }
    /* a statically non-callable operand still types as the composition:
       the codegen arm raises CRuby's TypeError (callable object is
       expected) in its place. A program that reopens a builtin with its
       own #call disarms the rule -- 5.call works there, so composing a 5
       must too. */
    if ((cr_p || crt == TY_METHOD) && ty_never_callable(cat) &&
        !an_user_defines_or_reads(c, "call"))
      { *out = TY_PROC; return 1; }
  }
  if (recv >= 0 && rt == TY_PROC) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }
  /* Klass.instance_method(:m) -> an (unbound) method object; #bind re-binds (#2676) */
  if (recv >= 0 && sp_streq(name, "instance_method") && method_sym_arg(c, id) != NULL &&
      method_obj_target_mi(c, id) >= 0) { *out = TY_METHOD; return 1; }
  /* Method#receiver is the bound receiver (#2701) */
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "receiver")) {
    int mn = method_recv_node(c, recv);
    int mrecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
    if (mrecv >= 0) { *out = infer_type(c, mrecv); return 1; }
  }
  /* <poly>.call(args): a boxed Proc publishes its result through the boxed
     return slot, so the value is genuinely dynamic -- type it poly and let
     the call site read the slot intact (unboxing to int truncated an array
     or string result to garbage). A boxed slot may hold a Proc regardless
     of whether some user class defines `call` (the poly dispatch's callable
     pre-arm routes it through the callable machinery), so the result stays
     dynamic even then; the user methods' return type must not constrain it.
     Proc#yield is the same call. */
  if (recv >= 0 && rt == TY_POLY &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "yield")))
    { *out = TY_POLY; return 1; }

  /* strftime on a poly value that is really a Time formats to a String. A
     nilable Time (`created_at : Time?`) is held as a poly sp_RbVal, so the
     call would otherwise infer poly/unknown and the formatted string get
     discarded; the codegen poly dispatch gives it a SP_BUILTIN_TIME arm that
     formats a real Time and raises NoMethodError otherwise, so its non-raising
     result is always a String. Only when no user class supplies strftime, to
     match the codegen guard. Issue #2457 (family2 nilable value-method). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "strftime") && argc == 1 &&
      infer_type(c, argv[0]) == TY_STRING) {
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      if (c->classes[k].is_native_class) {   /* bindings only (#4504) */
        if (comp_poly_arm_defines_n(c, k, name, argc)) ncand++;
        continue;
      }
      int mi = comp_method_in_chain(c, k, name, NULL);
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand == 0) { *out = TY_STRING; return 1; }
  }

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) { *out = TY_PROC; return 1; }

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type;
     Proc#=== (case/when dispatch) IS a call in CRuby */
  /* Proc#=== answers the proc's return VALUE (#3818). Typing it from the
     proc's body pins it to one shape, and a proc that arrives through a slot
     has no body to read, so the answer is boxed. */
  if (recv >= 0 && rt == TY_PROC &&
      is_call_alias(name)) {
    /* In a proc form, a call on the block parameter is the yield: the block is
       a real proc supplied per call site, so its result is poly uniformly --
       the same reason the YieldNode arm answers poly there. Typing it from any
       one site's block fixes the clone to that site (#3408). */
    { *out = proc_call_ret(c, recv); return 1; }
  }

  /* Proc composition: proc << proc / proc >> proc -> a new Proc. */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (sp_streq(name, "<<") || sp_streq(name, ">>")) &&
      infer_type(c, argv[0]) == TY_PROC)
    { *out = TY_PROC; return 1; }

  /* Proc identity: equal?/eql?/== against another Proc -> bool */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      is_equality_name(name) &&
      infer_type(c, argv[0]) == TY_PROC)
    { *out = TY_BOOL; return 1; }
  return 0;
}

/* A name any receiver answers by its shape: ===, the unary operators, !, respond_to?, nil?, object_id and hash, between?, match and =~, the operators (infer_operator_call), a ? predicate, a conversion, tap, then, instance_eval (infer_call_inner's rules, in their order) */
static int infer_universal_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, TyKind *out) {
  /* A boxed receiver's `===` can be a Proc's, whose answer is the proc's
     return value rather than a boolean (#3818). Everything else boxed answers
     a boolean, which a poly slot holds just as well. */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 && rt == TY_POLY)
    { *out = TY_POLY; return 1; }
  /* /re/ === str -> match boolean */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode"))
    { *out = TY_BOOL; return 1; }
  /* Class.===(obj) is always bool */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    { *out = TY_BOOL; return 1; }

  if ((sp_streq(name, "-@") || sp_streq(name, "+@")) && recv >= 0 && argc == 0) {
    if (rt == TY_STRING) { *out = TY_STRING; return 1; }  /* +str = mutable copy; -str = frozen self */
    { *out = ty_is_numeric(rt) ? rt : rt == TY_POLY ? TY_POLY : TY_UNKNOWN; return 1; }
  }
  /* unary bitwise complement: ~int / ~poly -> int (poly value coerced via to_i) */
  if (sp_streq(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY))
    { *out = TY_INT; return 1; }
  if (sp_streq(name, "!")) { *out = TY_BOOL; return 1; }
  if (sp_streq(name, "respond_to?") && recv >= 0) { *out = TY_BOOL; return 1; }
  if ((sp_streq(name, "method_defined?") || sp_streq(name, "const_defined?") ||
       sp_streq(name, "public_method_defined?") || sp_streq(name, "private_method_defined?") ||
       sp_streq(name, "protected_method_defined?")) && recv >= 0) { *out = TY_BOOL; return 1; }
  /* const_get(:K) with a literal name resolves to the constant's type (codegen
     emits cst_<K>); a literal name that does not resolve raises NameError at
     runtime, so its value type is poly. A dynamic name is left unresolved. */
  if (sp_streq(name, "const_get") && recv >= 0 && argc >= 1) {
    const char *cgt = nt_type(nt, argv[0]);
    const char *cgn = NULL;
    if (cgt && sp_streq(cgt, "SymbolNode")) cgn = nt_str(nt, argv[0], "value");
    else if (cgt && sp_streq(cgt, "StringNode")) cgn = nt_str(nt, argv[0], "content");
    /* const_get(name, false) searches only the receiver's own constants, so an
       inherited one is a NameError, not that constant's type (#3762) */
    if (cgn && argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "FalseNode")) {
      const char *cg_rty = nt_type(nt, recv);
      const char *cg_rnm = (cg_rty && (sp_streq(cg_rty, "ConstantReadNode") ||
                                       sp_streq(cg_rty, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
      if (cg_rnm && !const_owned_by_class(c, cg_rnm, cgn)) { *out = TY_POLY; return 1; }
    }
    /* a CLASS or module name answers the class object itself (#3969) */
    if (cgn && comp_class_index(c, cgn) >= 0) { *out = TY_CLASS; return 1; }
    if (cgn) { LocalVar *cv = comp_const(c, cgn); if (cv && cv->type != TY_UNKNOWN) { *out = cv->type; return 1; } { *out = TY_POLY; return 1; } }
  }
  if (sp_streq(name, "nil?") && recv >= 0 && argc == 0) { *out = TY_BOOL; return 1; }
  /* A generated READER of this name owns it on a concrete object, as it does
     in CRuby -- Data.define(:object_id) answers the member (CRuby warns and
     defines it). Codegen's reader arm already wins there; typing the call
     Integer here split the two halves and the build stopped (#4190). */
  if ((sp_streq(name, "object_id") || sp_streq(name, "__id__")) && recv >= 0 && argc == 0) {
    if (ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR)
      { /* fall through to the member-read rule below */ }
    else { *out = TY_INT; return 1; }
  }
  /* #hash on a primitive returns an Integer (CRuby's any_hash contract): the
     value is the receiver boxed through sp_rbval_hash_key, the same hashing the
     Hash container uses, so a user `def hash = v.hash` composes consistently. A
     concrete user object is left to its own #hash method dispatch (which may
     return any type -- honored via the sp_obj_hash_hook for keys). */
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 && !ty_is_object(rt)) { *out = TY_INT; return 1; }
  if (sp_streq(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) { *out = TY_BOOL; return 1; }
  /* int & | ^ a Bignum operand promotes (#2422). `&` too: a negative receiver
     is sign-extended forever, so `-1 & 0xFFFFFFFFFFFFFFFF` IS that mask. */
  if (recv >= 0 && argc == 1 &&
      is_bit_op(name) &&
      infer_type(c, recv) == TY_INT && infer_type(c, argv[0]) == TY_BIGINT) { *out = TY_BIGINT; return 1; }
  if ((sp_streq(name, "match?") || sp_streq(name, "!~")) && recv >= 0) { *out = TY_BOOL; return 1; }
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    const char *rrt = nt_type(nt, recv), *art = argc > 0 ? nt_type(nt, argv[0]) : NULL;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")))
      /* the block form evaluates to the block's value (nil on a miss) */
      { *out = nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA; return 1; }
  }
  if (sp_streq(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")) ||
        rt == TY_REGEX || a0t == TY_REGEX) { *out = TY_POLY; return 1; }
  }
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if (rt == TY_REGEX || a0t == TY_REGEX)
      { *out = nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA; return 1; }
    /* String#match with a String pattern (regexp source) -> MatchData */
    if ((rt == TY_STRING || rt == TY_STRBUF) && a0t == TY_STRING)
      { *out = nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA; return 1; }
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode")) {
    if (sp_streq(name, "source")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "options")) { *out = TY_INT; return 1; }
  }

  /* array set operations: &, intersection, |, union, -, difference. The named
     forms are variadic (fold over each argument); the operators are binary. */
  if (recv >= 0 && argc >= 1 &&
      is_set_op(name)) {
    if (ty_is_array(rt) && a0 == rt) { *out = rt; return 1; }
    /* empty array [] arg (TY_UNKNOWN): result is same kind as receiver */
    if (ty_is_array(rt) && a0 == TY_UNKNOWN) { *out = rt; return 1; }
    /* any array receiver with a different-kind (or poly) array argument: the
       codegen boxes both operands to poly and runs the poly set op, so the
       result is a poly array. */
    if (ty_is_array(rt) && ty_is_array(a0) && a0 != rt) { *out = TY_POLY_ARRAY; return 1; }
  }
  /* The variadic set operations with NO argument answer a copy of the
     receiver, and fetch_values with none answers an empty Array; only the
     union form had an arm, so the others were refused outright (#3851). */
  if (recv >= 0 && argc == 0 && ty_is_array(rt) &&
      (sp_streq(name, "intersection") || sp_streq(name, "difference") ||
       sp_streq(name, "union")))
    { *out = rt; return 1; }
  /* Array#intersect?(other) -> bool */
  if (recv >= 0 && argc == 1 && sp_streq(name, "intersect?") && ty_is_array(rt))
    { *out = TY_BOOL; return 1; }
  { TyKind r; if (infer_operator_call(c, id, nt, name, recv, argc, argv, rt, a0, &r)) { *out = r; return 1; } }

  /* a program's own Object method answers what it returns, whatever its
     name says: activesupport's Object#acts_like? gives respond_to?'s
     answer, which a class's own respond_to? may give as any value */
  if (recv >= 0) {
    /* a boxed receiver too: the builtin-only answer the poly dispatch's
       default arm is shaped by is the Object fallback's call */
    const char *ocls = ty_is_object(rt) || rt == TY_POLY ? "Object" : builtin_class_of_type(rt);
    TyKind ort;
    if (ocls && object_reopen_answers(c, ocls, id, &ort)) { *out = ort; return 1; }
  }
  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') { *out = TY_BOOL; return 1; }

  if (sp_streq(name, "to_s") || sp_streq(name, "inspect") ||
      sp_streq(name, "chr") || sp_streq(name, "to_str")) { *out = TY_STRING; return 1; }
  /* a boxed receiver's to_i/to_int in promote: see the universal-table note
     above for why this is the mode-gated one (#4688) */
  if (g_promote_mode && recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "to_i") || sp_streq(name, "to_int"))) { *out = TY_POLY; return 1; }
  if (sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
      sp_streq(name, "length") || sp_streq(name, "size") ||
      sp_streq(name, "count") ||
      sp_streq(name, "ord") || sp_streq(name, "abs")) { *out = TY_INT; return 1; }
  if (sp_streq(name, "to_f")) { *out = TY_FLOAT; return 1; }
  if (sp_streq(name, "to_sym")) { *out = TY_SYMBOL; return 1; }

  if (is_void_call(name) && recv < 0 && !an_bare_call_class_owned(c, id)) { *out = TY_VOID; return 1; }

  /* $stdout/$stderr.puts/print/write return nil (so a value-position use --
     an if/else arm or assignment -- unifies and boxes as nil). */
  if (recv >= 0 && (sp_streq(name, "puts") || sp_streq(name, "print") || sp_streq(name, "write") ||
                    sp_streq(name, "syswrite")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gv = nt_str(nt, recv, "name");
    if (gv && (sp_streq(gv, "$stdout") || sp_streq(gv, "$stderr")))
      { *out = (sp_streq(name, "write") || sp_streq(name, "syswrite")) ? TY_INT : TY_NIL; return 1; }
  }

  /* tap: run block, return self */
  if (sp_streq(name, "tap") && recv >= 0) {
    /* `[].tap { |a| a << x }`: the empty-array-literal receiver has no element
       type of its own (rt stays unknown/poly), so the block param -- typed from
       its pushes -- carries the real container type. Return it so the tap result
       and any chained `.join` (or the `p` that inspects it) agree with the
       container codegen materializes for the receiver (#3200, #3208). */
    const char *rty = nt_type(nt, recv);
    int rel = 0;
    if (rty && sp_streq(rty, "ArrayNode")) nt_arr(nt, recv, "elements", &rel);
    int tblk = nt_ref(nt, id, "block");
    if (rty && sp_streq(rty, "ArrayNode") && rel == 0 && tblk >= 0) {
      const char *bp = block_param_name(c, tblk, 0);
      Scope *bs = bp ? comp_scope_of(c, tblk) : NULL;
      LocalVar *blv = (bs && bp) ? scope_local(bs, bp) : NULL;
      if (blv) {
        /* the tap node may be inferred before the block body typed the param;
           infer the body now so blv->type is settled before we read it. */
        int bdy = nt_ref(nt, tblk, "body");
        int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
        for (int j = 0; j < bbn; j++) infer_subtree(c, bbb[j]);
        if (ty_is_array(blv->type)) { *out = blv->type; return 1; }
      }
    }
    { *out = rt; return 1; }
  }
  /* then / yield_self: run block, return block result */
  if (sp_streq(name, "then") || sp_streq(name, "yield_self")) {
    int blk_id = nt_ref(nt, id, "block");
    /* with NO block it is an enumerator of one element, the receiver (#4028) */
    if (blk_id < 0 && nt_ref(nt, id, "arguments") < 0) { *out = TY_ENUMERATOR; return 1; }
    if (blk_id >= 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) { *out = TY_NIL; return 1; }
      /* Pin block param to receiver type so body inference uses the right type */
      const char *bp0 = block_param_name(c, blk_id, 0);
      Scope *bs = bp0 ? comp_scope_of(c, blk_id) : NULL;
      LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind saved_blv = blv ? blv->type : TY_UNKNOWN;
      if (blv && rt != TY_UNKNOWN) blv->type = rt;
      g_infer_blv_pin++;
      TyKind result = then_block_value_ty(c, bdy, infer_type(c, bbb[bbn - 1]));
      g_infer_blv_pin--;
      if (blv) blv->type = saved_blv;
      { *out = result; return 1; }
    }
  }
  if (sp_streq(name, "instance_eval")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0 && ty_is_object(rt) &&
        comp_method_in_chain(c, ty_object_class(rt), "instance_eval", NULL) < 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) { *out = TY_NIL; return 1; }
      int saved_ie = an_ie_class_id;
      an_ie_class_id = ty_object_class(rt);
      TyKind result = infer_type(c, bbb[bbn - 1]);
      an_ie_class_id = saved_ie;
      { *out = result; return 1; }
    }
    { *out = TY_POLY; return 1; }
  }
  if (sp_streq(name, "instance_exec") && rt == TY_POLY && nt_ref(nt, id, "block") >= 0) { *out = TY_POLY; return 1; }
  return 0;
}

/* An enumerator chain (each_slice, with_index, the each family's value form) and an object array's iterators (infer_call_inner's rules, in their order) */
static int infer_enum_chain_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int args, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* each_slice(n).map/collect { |...| } chain: return array of block result type.
     The blockless each_slice receiver types as TY_ENUMERATOR (a first-class
     enumerator value); the codegen fold still unrolls this chain syntactically,
     so accept that receiver type here too and keep the array result. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) && (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && (sp_streq(nt_str(nt, recv, "name"), "each_slice") ||
                                   sp_streq(nt_str(nt, recv, "name"), "each_cons")) &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_es = nt_ref(nt, id, "block");
    if (blk_es >= 0) {
      int body_es = nt_ref(nt, blk_es, "body");
      int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
      { *out = ty_array_of(bn_es > 0 ? infer_type(c, bb_es[bn_es - 1]) : TY_UNKNOWN); return 1; }
    }
  }

  /* each_cons(n).with_index(off).map/collect { |...| } chain. The receiver used
     to infer TY_UNKNOWN; a blockless enum.with_index is now itself a
     materialized Enumerator, so accept that type here too -- the chain arm must
     keep winning over the generic enumerator surface. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) &&
      (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int blk_wi = nt_ref(nt, id, "block");
      if (blk_wi >= 0) {
        int body_wi = nt_ref(nt, blk_wi, "body");
        int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
        { *out = ty_array_of(bn_wi > 0 ? infer_type(c, bb_wi[bn_wi - 1]) : TY_UNKNOWN); return 1; }
      }
    }
  }

  /* array.{map,each,select,...}.with_index(off) { |x, i| } result: map collects
     the block value (array of body type); each yields the receiver; select/reject
     filter, preserving the receiver's array type. */
  if (recv >= 0 && sp_streq(name, "with_index") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    const char *inner = nt_str(nt, recv, "name");
    int arr_recv = nt_ref(nt, recv, "receiver");
    TyKind arr_t = arr_recv >= 0 ? infer_type(c, arr_recv) : TY_UNKNOWN;
    /* an Integer Range source behaves as an int array (materialized by the
       emitter); each.with_index still yields the Range itself (#3228) */
    TyKind arr_t0 = arr_t;
    if (arr_t == TY_RANGE) arr_t = TY_INT_ARRAY;
    if (inner && ty_is_array(arr_t)) {
      if (sp_streq(inner, "map") || sp_streq(inner, "collect")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          { *out = ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN); return 1; }
        }
      }
      else if (sp_streq(inner, "each") || sp_streq(inner, "select") ||
               sp_streq(inner, "filter") || sp_streq(inner, "reject") ||
               sp_streq(inner, "take_while") || sp_streq(inner, "drop_while") ||
               sp_streq(inner, "map!") || sp_streq(inner, "collect!"))
        { *out = (arr_t0 == TY_RANGE && sp_streq(inner, "each")) ? TY_RANGE
             : arr_t; return 1; }   /* take_while/drop_while keep the element type (subset) */
    }
  }

  /* arr.each.with_index(off).<terminal> / arr.each_with_index.<terminal>:
     a blockless [elem, index]-pair enumerator consumed by the terminal.
     (matz/spinel#1481 inject/reduce result; #1483 others.) */
  if (recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    int chain_arr = an_indexed_each_source(nt, recv);
    TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
    if (ty_is_array(chain_at)) {
      TyKind elem = ty_array_elem(chain_at);
      if (sp_streq(name, "inject") || sp_streq(name, "reduce")) {
        int args = nt_ref(nt, id, "arguments");
        int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
        TyKind acc = (argc > 0 && argv) ? infer_type(c, argv[0]) : elem;
        /* an empty `{}` seed is a general boxed-key/value hash builder, like
           each_with_object({}) -- not the element type (#2958) */
        if (acc == TY_UNKNOWN && argc > 0 && argv && nt_type(nt, argv[0]) &&
            sp_streq(nt_type(nt, argv[0]), "HashNode")) {
          int hn = 0; nt_arr(nt, argv[0], "elements", &hn);
          if (hn == 0) acc = TY_POLY_POLY_HASH;
        }
        if (acc == TY_UNKNOWN) acc = elem;
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          if (bn > 0) {
            TyKind bt = infer_type(c, bb[bn - 1]);
            if (ty_is_numeric(bt)) acc = ty_promote_numeric(acc, bt);
            /* a boxed body (a fold that may promote under
               --int-overflow=promote, a Rational) cannot fold back into a
               numeric seed slot: the accumulator is boxed, as the array
               inject rule has it */
            else if (ty_is_numeric(acc) && (bt == TY_POLY || bt == TY_BIGINT || bt == TY_RATIONAL || bt == TY_COMPLEX))
              acc = TY_POLY;
          }
        }
        { *out = acc; return 1; }
      }
      int blk = nt_ref(nt, id, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      /* The codegen path only handles the |v, i| two-param block form; gate the
         result type on it so single-param forms fall to their normal rules. */
      int two_param = blk >= 0 && !block_param_is_multi(c, blk, 0) &&
                      block_param_name(c, blk, 0) && block_param_name(c, blk, 1);
      if (two_param && (sp_streq(name, "map") || sp_streq(name, "collect")))
        { *out = ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN); return 1; }
      if (sp_streq(name, "to_a") || sp_streq(name, "entries") ||
          (two_param && (sp_streq(name, "select") || sp_streq(name, "filter") || sp_streq(name, "reject"))))
        { *out = TY_POLY_ARRAY; return 1; }   /* an array of [element, index] pairs */
      if (blk < 0 && sp_streq(name, "to_h")) {
        /* an array of [element, index] pairs collected into {element => index};
           the block form instead maps each pair, so leave it to its own rule. */
        TyKind h = ty_hash_of(elem, TY_INT);
        { *out = h != TY_UNKNOWN ? h : TY_POLY_POLY_HASH; return 1; }
      }
      if (two_param && sp_streq(name, "count")) { *out = TY_INT; return 1; }
      if (two_param && (sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?")))
        { *out = TY_BOOL; return 1; }
    }
  }

  /* homogeneous object array (sp_PtrArray of unboxed sp_X*): the typed
     counterpart of the poly-array block below, for the narrowed TY_OBJ_ARRAY
     type. Only the ops narrow_object_arrays admits appear here. */
  if (recv >= 0 && ty_is_obj_array(rt)) {
    int ecls = ty_obj_array_class(rt);
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) { *out = ty_object(ecls); return 1; }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) { *out = ty_object(ecls); return 1; }
    if (sp_streq(name, "[]=") && argc == 2) { *out = ty_object(ecls); return 1; }
    if (is_push_alias(name)) { *out = rt; return 1; }
    if (is_len_alias(name) && argc == 0) { *out = TY_INT; return 1; }
    if (sp_streq(name, "empty?") && argc == 0) { *out = TY_BOOL; return 1; }
    /* no-block comparisons (admitted by the narrowing pass only for element
       classes with `<=>`): sort keeps the array type, min/max yield an
       element (NULL-encoded nil when empty). */
    if ((sp_streq(name, "sort") || sp_streq(name, "sort!")) && argc == 0) { *out = rt; return 1; }
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) { *out = ty_object(ecls); return 1; }
    /* the block iterators the narrowing pass admits (nested_row_iter_call):
       each and its kin answer the receiver, map the block's values (#4846) */
    { int oblk = nt_ref(nt, id, "block");
      if (oblk >= 0 && nt_kind(nt, oblk) == NK_BlockNode) {
        if ((sp_streq(name, "each") || sp_streq(name, "reverse_each") ||
             sp_streq(name, "each_entry") || sp_streq(name, "each_with_index")) && argc == 0)
          { *out = rt; return 1; }
        if ((sp_streq(name, "map") || sp_streq(name, "collect")) && argc == 0)
          { *out = infer_map_block_ty(c, id, oblk); return 1; }
      } }
  }
  return 0;
}

/* An exception receiver: an exception class's methods and an exception-shaped instance's queries (infer_call_inner's rules, in their order) */
static int infer_exception_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt, TyKind *out) {
  /* Exception class-level methods: Cls.exception(msg) is Cls.new (#2740);
     Exception.to_tty? answers whether stderr is a terminal (#2757). */
  if (recv >= 0 && nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
                                          (sp_streq(nt_type(nt, recv), "ConstantPathNode") &&
                                           comp_class_index(c, nt_str(nt, recv, "name")) < 0))) {
    /* by its whole path: Errno::ENOENT.exception */
    const char *ecn = superclass_builtin_exc_name(nt, recv);
    if (ecn && is_builtin_exception_name(ecn)) {
      if (sp_streq(name, "exception")) { *out = TY_EXCEPTION; return 1; }
      if (sp_streq(name, "to_tty?")) { *out = TY_BOOL; return 1; }
    }
  }

  /* exception receiver methods */
  /* A specialized rescue var is typed as the exception subclass object, but
     its exception-shaped queries still answer as on a base exception, unless
     the subclass defines its own override (#1415). */
  /* a method a reopening of a builtin exception class defined, reached on a
     base-typed exception or on a user subclass instance whose own chain
     lacks it (`class Exception; def as_json`) */
  if (recv >= 0 && (rt == TY_EXCEPTION ||
                    (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)) &&
                     comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0))) {
    /* the runtime class picks among several definers, so the call answers
       what they all do, or a boxed value when they disagree */
    int xr[8];
    int xn = exc_reopen_definers(c, name, xr, 8);
    if (xn > 0) {
      TyKind xt = an_user_call(c, id, comp_method_in_chain(c, xr[0], name, NULL), UC_REOPEN, xr[0]);
      for (int q = 1; q < xn; q++)
        if (method_call_ret(c, comp_method_in_chain(c, xr[q], name, NULL), id) != xt) { *out = TY_POLY; return 1; }
      { *out = xt; return 1; }
    }
  }
  int exc_shaped = rt == TY_EXCEPTION ||
                   (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)) &&
                    comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (recv >= 0 && exc_shaped) {
    /* Exception#message is #to_s, so an override answering something other
       than a String carries that value out: the string-typed helper answered
       the stored message (the class name) instead (#3868). */
    if (sp_streq(name, "message") && ty_is_object(rt)) {
      int mi8 = comp_method_in_chain(c, ty_object_class(rt), "to_s", NULL);
      if (mi8 >= 0 && (TyKind)c->scopes[mi8].ret != TY_STRING &&
          (TyKind)c->scopes[mi8].ret != TY_UNKNOWN)
        { *out = (TyKind)c->scopes[mi8].ret; return 1; }
    }
    /* On a receiver whose class is only known at run time, any exception in the
       program may be the one answering: when some subclass carries a non-String
       out of #message, the query is a union and rides the boxed dispatcher. */
    if (rt == TY_EXCEPTION && (sp_streq(name, "message") || sp_streq(name, "to_s")) &&
        exc_has_nonstring_msg_override(c))
      { *out = TY_POLY; return 1; }
    if (sp_streq(name, "message") || sp_streq(name, "to_s") ||
        sp_streq(name, "to_str") || sp_streq(name, "inspect") ||
        sp_streq(name, "full_message") || sp_streq(name, "detailed_message"))
      { *out = TY_STRING; return 1; }
    /* #exception answers an instance of the receiver's own class -- itself
       with no argument, a copy carrying the new message with one; #== is the
       value comparison. Neither had an arm for a user subclass instance, so
       the value was discarded into nil (#3870). */
    if (sp_streq(name, "exception") && argc <= 1) { *out = rt; return 1; }
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "class")) { *out = TY_CLASS; return 1; }  /* a Class object, carried by name */
    if (sp_streq(name, "backtrace")) { *out = TY_STR_ARRAY; return 1; }  /* empty: no frames captured */
    if (sp_streq(name, "cause")) { *out = TY_EXCEPTION; return 1; }      /* the threaded cause, nil if none */
    if (sp_streq(name, "result")) { *out = TY_POLY; return 1; }          /* StopIteration#result, nil otherwise */
    if (sp_streq(name, "errno")) { *out = TY_POLY; return 1; }           /* SystemCallError#errno: the Errno:: class's number, nil for the parent (#4560) */
    if (sp_streq(name, "name")) { *out = TY_POLY; return 1; }            /* NameError#name, nil otherwise */
    if (sp_streq(name, "path")) { *out = TY_POLY; return 1; }            /* LoadError#path: nil (no runtime-raised LoadError carries one) */
    if (sp_streq(name, "dup") || sp_streq(name, "clone")) { *out = rt; return 1; }  /* a copy keeps the (subclass) type */
    if (sp_streq(name, "key") || sp_streq(name, "receiver") || sp_streq(name, "args") ||
        sp_streq(name, "reason") || sp_streq(name, "exit_value") ||
        sp_streq(name, "tag") || sp_streq(name, "value"))
      { *out = TY_POLY; return 1; }   /* class-gated introspection accessors (#2753-#2756, #2770) */
    if (sp_streq(name, "private_call?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "status")) { *out = TY_INT; return 1; }       /* SystemExit#status */
    if (sp_streq(name, "success?")) { *out = TY_BOOL; return 1; }    /* SystemExit#success? */
    if (sp_streq(name, "signo")) { *out = TY_INT; return 1; }        /* SignalException#signo */
    if (sp_streq(name, "signm")) { *out = TY_STRING; return 1; }     /* SignalException#signm */
    if (rt == TY_EXCEPTION && sp_streq(name, "exception")) { *out = TY_EXCEPTION; return 1; }  /* self, or a copy carrying a new message */
    if (sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "equal?"))
      { *out = TY_BOOL; return 1; }
  }
  return 0;
}

/* A runtime value receiver, typed or boxed: Fiber, Thread, Queue, Mutex, a native object's protocol, Process::Tms and Status, OpenStruct, Enumerator (infer_call_inner's rules, in their order) */
static int infer_runtime_value_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* Fiber: builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && rt == TY_FIBER) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }

  /* Object's identity protocol on the native kinds: typed from the same
     decision the codegen arm (emit_native_object_protocol) emits from, so the
     two cannot drift. A user exception subclass's own definition wins, as it
     does in the arm. */
  if (recv >= 0 && nt_ref(c->nt, id, "block") < 0 &&
      ty_object_protocol_answers(rt, argc == 1 ? infer_type(c, argv[0]) : TY_UNKNOWN, name, argc) &&
      !(rt == TY_EXCEPTION && exc_subclass_defines(c, name))) {
    if (sp_streq(name, "freeze")) { *out = rt; return 1; }
    if (sp_streq(name, "to_s")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "<=>")) { *out = TY_POLY; return 1; }   /* 0 or nil, boxed */
    { *out = TY_BOOL; return 1; }
  }

  /* Thread, Queue, Mutex and ConditionVariable: builtin-op rows
     (builtin_ops.c), the universal queries (#3124) among them. Queue's,
     Mutex's and ConditionVariable's rules sat below the TY_POLY rules
     that follow, which no handle reaches. */
  if (recv >= 0 && (rt == TY_THREAD || rt == TY_QUEUE || rt == TY_MUTEX ||
                    rt == TY_CONDVAR)) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }

  /* Array#push / #append / #unshift / #prepend answer the RECEIVER, and on a
     boxed receiver that is the poly value itself. Leaving the call untyped made
     the poly dispatch declare an sp_int result temp whose push arm (which only
     assigns for a poly result) never wrote it, so the answer was the temp's
     zero seed: `->(acc) { acc.push(1) }` answered nil (#4320). Poly is also the
     right answer when a user class owns the name -- it is the union of that
     return and the builtin one, which is what every other name on this surface
     already widens to. */
  if (recv >= 0 && rt == TY_POLY && (argc >= 1 || sp_streq(name, "push")) &&
      (sp_streq(name, "push") || sp_streq(name, "append") ||
       sp_streq(name, "unshift") || sp_streq(name, "prepend")))
    { *out = TY_POLY; return 1; }

  /* Array#pop(n) / #shift(n) on a boxed array answer an Array of the removed
     elements (#3613) -- unless a program class also answers the name, when
     the dispatch writes that class's result into the same slot, and only a
     boxed one holds both (#4831; the push rule above for the same reason). */
  /* ...but pop(true) / shift(false) is Queue#pop(non_block): an Array's
     pop(true) is a TypeError, so only a Queue answers it, with one value */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (sp_streq(name, "pop") || sp_streq(name, "shift")) &&
      (infer_type(c, argv[0]) == TY_BOOL || infer_type(c, argv[0]) == TY_POLY))
    { *out = TY_POLY; return 1; }   /* a boxed argument: a Queue's flag or an Array's count */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (sp_streq(name, "pop") || sp_streq(name, "shift"))) {
    if (c->poly_builtin_ty && id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN)
      c->poly_builtin_ty[id] = TY_POLY_ARRAY;   /* the dispatch's builtin default */
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, name, argc)) { *out = TY_POLY; return 1; }
    { *out = TY_POLY_ARRAY; return 1; }
  }

  /* String#to_i(base) is the only builtin to_i that takes an argument, and
     the poly emitter answers it as an sp_int (the String arm, else
     ArgumentError). A program class's zero-arity to_i does not reach that
     call, so its return must not widen this one: widened, the consumer
     boxed a value the emitter had produced unboxed -- `buf << s.to_i(16)`
     passed an intptr_t where an sp_RbVal was expected. Poly only when a
     program class does answer a one-argument to_i. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "to_i")) {
    /* what a String answers, for the dispatch's String arm when a class's
       own to_i makes the call poly (emit_poly_str_prearm) */
    if (c->poly_builtin_ty && id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN)
      c->poly_builtin_ty[id] = TY_INT;
    for (int k = 0; k < c->nclasses; k++) {
      if (c->classes[k].is_native_class) {
        if (comp_poly_arm_defines_n(c, k, name, argc)) { *out = TY_POLY; return 1; }
        continue;
      }
      int mi = comp_method_in_chain(c, k, name, NULL);
      if (mi >= 0 && (c->scopes[mi].nparams >= 1 || c->scopes[mi].rest_idx >= 0))
        { *out = TY_POLY; return 1; }
    }
    { *out = TY_INT; return 1; }
  }

  /* Process::Tms: builtin-op rows (builtin_ops.c), looked up where its
     rule sat, so the rules above still claim first */
  if (recv >= 0 && rt == TY_TMS) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }
  /* Process::Status readers: builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && rt == TY_PROCESS_STATUS) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }
  /* The same names on a BOXED status -- which is how one normally arrives,
     since waitpid2 answers an Array and its second element is read out of a
     poly container. emit_poly_builtin_method already emits the unboxed scalar
     for exactly these seven, and a Process::Tms's four times (behind a runtime
     cls_id check), so without the
     matching rule here the two sides disagreed: `"exit #{st.exitstatus}"`
     asked sp_poly_to_s for a poly the emitter had produced as an sp_int. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      !an_user_defines_or_reads(c, name)) {
    int blk = nt_ref(nt, id, "block") >= 0;
    const BuiltinOp *op = bop_find_boxed(TY_PROCESS_STATUS, name, argc, blk);
    /* and a boxed Process::Tms's four CPU times, as on a typed one */
    if (!op) op = bop_find_boxed(TY_TMS, name, argc, blk);
    if (op) { *out = op->result; return 1; }
  }
  /* OpenStruct: dynamic members. A member read (any name, arg-less, no
     writer) or `[sym]` returns a boxed value; a writer / `[]=` returns the
     assigned value; the rest is a small fixed surface (#3135). */
  if (recv >= 0 && rt == TY_OPENSTRUCT) {
    if (sp_streq(name, "to_h") && argc == 0) { *out = TY_SYM_POLY_HASH; return 1; }
    if (sp_streq(name, "respond_to?")) { *out = TY_BOOL; return 1; }
    if (is_kind_query(name)) { *out = TY_BOOL; return 1; }
    if ((sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "!=")) && argc == 1) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "class") && argc == 0) { *out = TY_CLASS; return 1; }
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) { *out = TY_STRING; return 1; }
    if ((sp_streq(name, "[]=") || sp_streq(name, "[]")) ) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "frozen?") || sp_streq(name, "nil?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "dig")) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "each_pair") || sp_streq(name, "freeze") ||
        sp_streq(name, "itself")) { *out = TY_OPENSTRUCT; return 1; }
    /* any other arg-less name (or a `name=` writer) is a dynamic member */
    { *out = TY_POLY; return 1; }
  }
  /* TY_ENUMERATOR instance methods */
  if (recv >= 0 && rt == TY_ENUMERATOR) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    if (sp_streq(name, "+") && argc == 1 && infer_type(c, argv[0]) == TY_ENUMERATOR) { *out = TY_ENUMERATOR; return 1; }  /* #2481 */
    /* Stored-enumerator block form returns the underlying each return (the
       boxed source). Immediate chains (arr.each.with_index { } and the
       map/select shapes) keep their own typed arms below -- skip a blockless
       iter-shaped CallNode receiver over an array. */
    if (sp_streq(name, "with_index") && argc <= 1 && nt_ref(nt, id, "block") >= 0) {
      int wchain = 0;
      if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
          nt_ref(nt, recv, "block") < 0) {
        const char *wnm = nt_str(nt, recv, "name");
        int wrcv = nt_ref(nt, recv, "receiver");
        if (wnm && wrcv >= 0 &&
            (sp_streq(wnm, "each") || ty_iter_shape(wnm) != TY_ITER_NONE) &&
            ty_is_array(infer_type(c, wrcv)))
          wchain = 1;
      }
      if (!wchain) { *out = TY_POLY; return 1; }
    }
  }
  return 0;
}

/* A nil receiver, and an array conversion at the end of a chain: cycle.first or take, chunk_while or slice_when .to_a, to_a over an enumerator (infer_call_inner's rules, in their order) */
static int infer_nil_chain_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, TyKind rt, TyKind *out) {
  if (recv >= 0 && rt == TY_NIL) {
    if (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
        sp_streq(name, "===") || sp_streq(name, "equal?") || sp_streq(name, "eql?") ||
        sp_streq(name, "!~")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "=~")) { *out = TY_NIL; return 1; }
    if (sp_streq(name, "rationalize")) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "tap")) { *out = TY_POLY; return 1; }  /* the (boxed) nil receiver */
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) &&
        nt_ref(nt, id, "block") < 0) { *out = TY_ENUMERATOR; return 1; }
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk9 = nt_ref(nt, id, "block");
      int bd9 = nt_ref(nt, blk9, "body");
      int bn9 = 0; const int *bb9 = bd9 >= 0 ? nt_arr(nt, bd9, "body", &bn9) : NULL;
      if (bn9 >= 1) {
        TyKind bt9 = then_block_value_ty(c, bd9, infer_type(c, bb9[bn9 - 1]));
        { *out = bt9 == TY_NIL ? TY_POLY : bt9; return 1; }
      }
    }
    if (sp_streq(name, "to_c")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "to_f")) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "to_r")) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "to_a")) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "to_h")) { *out = TY_SYM_POLY_HASH; return 1; }
    if (sp_streq(name, "respond_to?")) { *out = TY_BOOL; return 1; }
  }

  /* <array>.cycle.first(n) / .take(n) -> same array kind (bounded consumer of the
     infinite cycle; the unbounded forms stay a loud reject). */
  if (recv >= 0 && (sp_streq(name, "first") || sp_streq(name, "take")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "cycle") &&
      nt_ref(nt, recv, "block") < 0) {
    int cargs = nt_ref(nt, recv, "arguments");
    int cac = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &cac);
    int pr = nt_ref(nt, recv, "receiver");
    if (cac == 0 && pr >= 0) { TyKind rt2 = infer_type(c, pr); if (ty_is_array(rt2)) { *out = rt2; return 1; } }
  }

  /* int_array.chunk_while/slice_when/chunk { |...| } .to_a -> a poly array (runs / pairs) */
  if (recv >= 0 && sp_streq(name, "to_a") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "chunk_while") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_when") ||
       sp_streq(nt_str(nt, recv, "name"), "chunk") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_before") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_after")) &&
      nt_ref(nt, recv, "block") >= 0 &&
      (nt_ref(nt, recv, "arguments") < 0 ||
       (!sp_streq(nt_str(nt, recv, "name"), "slice_before") &&
        !sp_streq(nt_str(nt, recv, "name"), "slice_after")))) {
    int pr = nt_ref(nt, recv, "receiver");
    if (pr >= 0) {
      TyKind prt = infer_type(c, pr);
      if (prt == TY_INT_ARRAY || prt == TY_POLY_ARRAY || prt == TY_STR_ARRAY ||
          prt == TY_FLOAT_ARRAY ||
          /* a boxed receiver walks whatever sp_poly_arr_recv renders -- a
             hash's [key, value] pairs, an array's elements (#3451) */
          prt == TY_POLY ||
          (prt == TY_UNKNOWN && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "ArrayNode")) ||
          (prt == TY_RANGE && range_enum_redispatch(c, recv)))
        { *out = TY_POLY_ARRAY; return 1; }
      /* hash.chunk { |k, v| key }.to_a materializes the same way (the chunk
         first-class emitter iterates the hash directly); gated to the named
         two-param block shape the emitter serves. */
      if (ty_is_hash(prt) && sp_streq(nt_str(nt, recv, "name"), "chunk") &&
          block_param_name(c, nt_ref(nt, recv, "block"), 0) &&
          block_param_name(c, nt_ref(nt, recv, "block"), 1))
        { *out = TY_POLY_ARRAY; return 1; }
    }
  }

  /* chunk_while/slice_when/chunk { } standing on its own (no .to_a terminal,
     which the arm above claims first): a first-class Enumerator over the
     eagerly materialized runs */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "chunk_while") || sp_streq(name, "slice_when") ||
       sp_streq(name, "chunk") ||
       ((sp_streq(name, "slice_before") || sp_streq(name, "slice_after")) &&
        nt_ref(nt, id, "arguments") < 0))) {
    TyKind crt = infer_type(c, recv);
    if (crt == TY_POLY_ARRAY || crt == TY_INT_ARRAY || crt == TY_STR_ARRAY ||
        (crt == TY_RANGE && range_enum_redispatch(c, id))) {
      if (!an_chunk_family_to_a(c, id)) { *out = TY_ENUMERATOR; return 1; }
    }
  }
  return 0;
}

/* A Kernel method that runs its block: loop, catch and throw, instance_eval and instance_exec, a trampoline's block (infer_call_inner's rules, in their order) */
static int infer_block_kernel_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, TyKind rt, TyKind *out) {
  /* loop { break val } -> the type of the break value */
  if (recv < 0 && sp_streq(name, "loop") && !an_bare_call_class_owned(c, id)) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        TyKind bt = scan_break_type(c, body, 0);
        if (bt != TY_UNKNOWN) { *out = bt; return 1; }
      }
      /* A loop ended by StopIteration answers that exception's #result -- the
         exhausted enumerator. Only a body that pulls from one can end that
         way; every other break-less loop keeps its nil (#3588). */
      if (body >= 0 && an_subtree_calls_enum_next(c, body)) { *out = TY_POLY; return 1; }
      { *out = TY_NIL; return 1; }
    }
    /* blockless `loop` is an infinite Enumerator yielding nil (#3236) */
    { *out = TY_ENUMERATOR; return 1; }
  }

  /* catch(:tag) { ... } -> unify the block's last value with every throw
     value that can target the tag, across method boundaries (the throw need
     not be syntactically inside the body). */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "catch")) {
    int blk = nt_ref(nt, id, "block");
    TyKind result = TY_UNKNOWN;
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
        if (bn > 0) result = infer_type(c, bb[bn - 1]);
      }
      const char *tag = NULL;
      int targ = nt_ref(nt, id, "arguments");
      int tac = 0; const int *tav = targ >= 0 ? nt_arr(nt, targ, "arguments", &tac) : NULL;
      if (tac >= 1 && nt_type(nt, tav[0])) {
        if (sp_streq(nt_type(nt, tav[0]), "SymbolNode")) tag = nt_str(nt, tav[0], "value");
        else if (sp_streq(nt_type(nt, tav[0]), "StringNode")) tag = nt_str(nt, tav[0], "unescaped");
      }
      TyKind tt = scan_throw_type(c, tag);
      if (tt != TY_UNKNOWN) result = ty_unify(result, tt);
    }
    { *out = result == TY_UNKNOWN ? TY_NIL : result; return 1; }
  }

  /* recv.instance_eval/exec { ... } -> the block's last-expression type
     (bare calls inside resolve via the ie node->class map). A trampoline
     method `recv.M { ... }` resolves the same way. */
  int ie_kind = (recv >= 0 && (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec")) &&
                 ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  /* a non-object receiver (nil, a scalar) is served by the non-object splice;
     type it by the block's last-expression the same way (#2956) */
  if (!ie_kind && recv >= 0 && !ty_is_object(rt) &&
      (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec"))) {
    int nblk = nt_ref(nt, id, "block");
    if (nblk >= 0 && nt_type(nt, nblk) && sp_streq(nt_type(nt, nblk), "BlockNode")) ie_kind = 1;
  }
  if (!ie_kind && recv >= 0 && ty_is_object(rt) && nt_ref(nt, id, "block") >= 0)
    ie_kind = comp_trampoline_kind(c, ty_object_class(rt), name, NULL) != 0;
  /* receiverless instance_eval/exec inside an instance method resolves to self */
  if (!ie_kind && recv < 0 && ie_implicit_self_class(c, id) >= 0) ie_kind = 1;
  if (ie_kind) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      const char *bty = nt_type(nt, blk);
      if (bty && sp_streq(bty, "BlockArgumentNode")) {
        /* `instance_exec(args, &b)` forwards the enclosing method's block; the
           value it produces is that method's own forwarded-block value across
           call sites (the method inlines per site, splicing the literal). */
        Scope *encl = comp_scope_of(c, id);
        int emi = encl ? (int)(encl - c->scopes) : -1;
        if (emi >= 0) {
          TyKind ft = yield_value_type(c, emi);
          if (ft != TY_UNKNOWN && ft != TY_VOID) { *out = ft; return 1; }
        }
        /* The forwarded block's value isn't statically pinned here (its call
           sites may not be typed yet during the fixpoint). It is a real boxed
           value, not nil -- poly keeps the result a scalar carrier so the
           enclosing method stays inlinable and the splice yields it per site. */
        { *out = TY_POLY; return 1; }
      }
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn > 0) {
        TyKind bt = infer_type(c, bb[bn - 1]);
        if (bt == TY_VOID) bt = TY_NIL;
        /* A value-carrying break/next can widen the result past the last
           expression (e.g. `next val + 1` poly vs trailing `999` int). */
        TyKind bnt = ie_block_break_next_ty(c, body);
        if (bnt != TY_UNKNOWN)
          bt = (bt == TY_NIL || bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
        { *out = bt; return 1; }
      }
      { *out = TY_NIL; return 1; }
    }
  }
  return 0;
}

/* The last resorts: a safe-navigation call, a reopened Array, Numeric or Object's own methods, a boxed receiver's face (infer_call_inner's rules, in their order) */
static int infer_last_resort_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt, TyKind *out) {
  /* safe navigation &. with unresolved type: return poly (receiver may be nil at runtime) */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.")) { *out = TY_POLY; return 1; }
  }

  /* Builtin class reopening: look up user-defined methods on Array/Numeric/Object
     receivers where no builtin method matched. */
  if (recv >= 0) {
    /* Array reopening: any array-typed receiver */
    if (ty_is_array(rt)) {
      int oc_mi = an_reopen_method(c, "Array", name);
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* Hash reopening: any hash-typed receiver */
    if (ty_is_hash(rt)) {
      int oc_mi = an_reopen_method(c, "Hash", name);
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* Numeric reopening: integers and floats */
    if (rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) {
      int oc_mi = an_reopen_method(c, "Numeric", name);
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* FalseClass methods (TrueClass already checked earlier for TY_BOOL) */
    if (rt == TY_BOOL) {
      int oc_mi = an_reopen_method(c, "FalseClass", name);
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* NilClass methods on a receiver known to be nil */
    if (rt == TY_NIL) {
      int oc_mi = an_reopen_method(c, "NilClass", name);
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* Object reopening: universal fallback for any receiver type */
    {
      int oc_mi = an_reopen_method(c, "Object", name);
      /* a yielding one given a block answers what its proc form does,
         a boxed value (#5779) */
      if (oc_mi >= 0 && c->scopes[oc_mi].yields && nt_ref(c->nt, id, "block") >= 0) { *out = TY_POLY; return 1; }
      if (oc_mi >= 0) { *out = c->scopes[oc_mi].ret; return 1; }
    }
    /* A poly receiver may hold a Class at runtime, where `name` is a class
       method (`def self.name`) -- codegen dispatches it on the class tag (#3215).
       Type the call poly (not unknown) so the result flows as a value instead of
       being discarded as a void unresolved call. */
    if (rt == TY_POLY && name) {
      int ncc = 0;
      comp_cmethod_candidates(c, name, &ncc);
      if (ncc > 0) { *out = TY_POLY; return 1; }
      /* Thread.current / Fiber.current through a class held in a poly slot:
         a boxed handle (the codegen's gate answers it) */
      if (argc == 0 && sp_streq(name, "current") && !an_user_defines_method(c, name)) { *out = TY_POLY; return 1; }
    }
  }

  /* A boxed HANDLE answering one of its own exclusive names: type the call as
     if the receiver were that handle. Codegen unboxes it back to exactly that
     before re-dispatching, and checks the runtime cls_id first, so a value of
     any other kind still raises NoMethodError (#4158 follow-up). */
  if (recv >= 0 && rt == TY_POLY && !face_active() && argc == 0 &&
      ty_poly_handle_face(name) != TY_UNKNOWN &&
      !an_user_defines_or_reads(c, name)) {
    an_face_push(recv, ty_poly_handle_face(name));
    TyKind kt = infer_call(c, id);
    an_face_pop();
    if (kt != TY_UNKNOWN) { *out = kt; return 1; }
  }
  /* Last resort for a boxed receiver: the face table. Answer as the typed
     call would with the receiver pinned to each owner kind in turn -- codegen
     unboxes to exactly that kind before re-entering the typed emitter, so
     both sides agree on the result slot -- and unify the owners' answers: one
     owner gives the typed call's own type, owners that disagree give poly and
     the emission boxes each arm. A receiver of no owner's kind raises
     NoMethodError there, as it did before (#3449). */
  if (recv >= 0 && rt == TY_POLY && !face_active() && !an_user_defines_or_reads(c, name)) {
    int blk = nt_ref(nt, id, "block") >= 0;
    unsigned own = an_zero_arg_builtin_shadowed(c, name, argc) ? 0
                   : ty_poly_face_owners(name, argc, blk, nt_call_args_plain(nt, id), 1) & PF_OWNERS;
    if (own) {
      TyKind r = TY_UNKNOWN;
      for (unsigned bit = 1; bit & PF_OWNERS; bit <<= 1) {
        if (!(own & bit)) continue;
        an_face_push(recv, ty_poly_face_kind(bit));
        TyKind ht = infer_call(c, id);
        an_face_pop();
        if (ht == TY_UNKNOWN) continue;
        /* a mutator that answers its receiver answers the box, not the copy
           its emitter worked on (see emit_face_arm) */
        if (ty_poly_face_owner_flags(name, argc, blk, nt_call_args_plain(nt, id), bit) & PF_VAL_SELF) ht = TY_POLY;
        r = r == TY_UNKNOWN ? ht : ty_unify(r, ht);
      }
      if (r != TY_UNKNOWN) { *out = r; return 1; }
    }
  }
  return 0;
}

/* A block-taking iterator: the each family's value form, select, filter and reject on a boxed receiver, uniq!, a times chain, find_all (infer_call_inner's rules, in their order) */
static int infer_block_iter_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt, TyKind *out) {
  /* `each_slice(n) { } / each_cons(n) { }` answer the receiver. When the
     receiver is the marked `to_a` hop above, that is the original Hash or
     Range, not the pair array the loop walked (#3842). */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode") &&
      ((argc == 1 && (sp_streq(name, "each_slice") || sp_streq(name, "each_cons"))) ||
       /* each_entry answers the receiver too, and the value emitter yields it:
          left on the pair array's type the two disagreed and the C compiler
          was handed a hash where an array was declared (#3895). It is
          renamed to each before this point. reverse_each over an Enumerator
          or a Hash reaches the array machinery through the same marked hop,
          and answers that receiver, not the array it walked (#4325). */
       (argc == 0 && is_each_walk(name))) &&
      nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "enum_recv")) {
    int orecv = nt_ref(nt, recv, "receiver");
    if (orecv >= 0) { *out = infer_type(c, orecv); return 1; }
  }
  /* A forwarded proc takes the receiver-valued runtime path. Keep this
     structural rule here: builtin rows cannot distinguish a block argument
     from a literal block. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && sp_streq(name, "each") &&
      nt_kind(nt, nt_ref(nt, id, "block")) == NK_BlockArgumentNode)
    { *out = TY_POLY; return 1; }
  /* A block each-family call returns its receiver (each, each_value/each_key/
     each_pair, each_with_index, reverse_each), so the value form composes:
     r = arr.each { }; arr.each { }.map { }. Gated to receivers that define
     the method -- the respond_to? machinery probes a synthesized
     `recv.m { }`, and an unconditional arm would make every type "respond"
     to each. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode")) {
    /* `obj.__enum_to_a.<m> { }` (m in each / each_with_index / reverse_each) is
       the desugared each-family call on a user Enumerable / Struct receiver:
       Ruby returns the enumerable itself, so type it as the original receiver
       obj, not the intermediate member array (#2546/#2547). The codegen value
       form (emit_iter_value_expr) yields obj to match. */
    if ((sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      int orecv = rnm && sp_streq(rnm, "__enum_to_a") ? nt_ref(nt, recv, "receiver") : -1;
      if (orecv >= 0) { *out = infer_type(c, orecv); return 1; }
    }
    int enumerable_recv = ty_is_array(rt) || ty_is_hash(rt) ||
                          rt == TY_RANGE || rt == TY_ENUMERATOR;
    /* each_entry belongs here too: Enumerable#each_entry answers the receiver
       exactly as #each does, and typing it nil left `arr.each_entry { }.class`
       reading NilClass (#3395). */
    if (enumerable_recv &&
        (sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")))
      { *out = rt; return 1; }
    /* A poly receiver answers `each` with itself, exactly as the typed kinds
       do -- that is the receiver's own type, so claiming it says nothing the
       receiver did not already say. Left UNKNOWN, a chained
       `h.each { }.length` had nothing to dispatch on and was refused at
       compile time, naming nothing: `JSON.parse("[]").each { }.size` reached
       that even though the value is an Array at run time (#3987). A receiver
       with no `each` raises where it always did, from the run-time dispatch.
       The value form of the iterator (emit_iter_value_expr) yields the
       receiver for a poly one too, so the two agree. */
    if (rt == TY_POLY && nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry") ||
         /* each_index's poly arm hands back the boxed receiver as well;
            untyped, its value read as nil */
         sp_streq(name, "each_index") ||
         /* the three Hash walks take the same poly iterator emission, which
            hands back the boxed receiver; typed through the face table they
            read as a Hash and the chained inspect was handed an sp_RbVal,
            a C error (#4485) */
         sp_streq(name, "each_pair") || sp_streq(name, "each_key") ||
         sp_streq(name, "each_value")))
      { *out = TY_POLY; return 1; }
    if (ty_is_hash(rt) &&
        (sp_streq(name, "each_value") || sp_streq(name, "each_key") ||
         sp_streq(name, "each_pair")))
      { *out = rt; return 1; }
  }
  /* `recv.sort_by { }` always returns a new Array. For a typed array receiver
     it stays that array's type (arm below); for a hash / poly / not-yet-settled
     receiver it is a generic Array. Without this a hash sort_by whose receiver
     was still poly when the local was typed settled to poly, and a downstream
     `.first(n).each` then had no array to iterate (#2876). */
  if (recv >= 0 && sp_streq(name, "sort_by") && nt_ref(nt, id, "block") >= 0) {
    /* A hash / poly / not-yet-settled receiver yields a generic Array. A typed
       array or a Range keep their own element-typed sort path below. */
    TyKind srt = infer_type(c, recv);
    if (ty_is_hash(srt) || srt == TY_POLY || srt == TY_UNKNOWN) { *out = TY_POLY_ARRAY; return 1; }
  }
  /* `poly.members` on a Struct/Data read out of a container: the field-name
     symbols as a generic Array. #deconstruct is the member values (like to_a).
     A class that reads a value of its own as `members` -- an attr_reader, a
     Struct/Data member of that name, or an alias of one -- answers it
     through the dispatch, as the emitter's #members arm stands aside for it
     (poly_name_user_claimed with readers), so the answer is not the names. */
  if (recv >= 0 && (sp_streq(name, "members") || sp_streq(name, "deconstruct")) &&
      argc == 0 && nt_ref(nt, id, "block") < 0 && infer_type(c, recv) == TY_POLY &&
      !an_user_recv_defines_method(c, name)) {
    if (!sp_streq(name, "members") || !an_class_reads_name(c, name)) { *out = TY_POLY_ARRAY; return 1; }
    /* ...and when a class does read it, a Struct or Data value in the same
       container (an instance without such a member, or a class) still
       answers its member names: either can come back, boxed. */
    if (an_program_has_struct(c)) { *out = TY_POLY; return 1; }
  }
  /* `poly.reject/select/filter { }` on a value only known to be an array at
     runtime (read out of a poly container): a filtered generic Array. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "map!") || sp_streq(name, "collect!")) &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY_ARRAY; return 1; }
  /* The filtering siblings answer whatever kind the receiver is -- Hash#select
     is a Hash, Array#select an Array -- and only the runtime value says which,
     so the result rides boxed (#3449). */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter")) &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY; return 1; }
  /* `poly.uniq { }` answers a new Array (the boxed emitter hands it back
     boxed); `uniq! { }` answers the receiver or nil. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "uniq") || sp_streq(name, "uniq!")) &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY; return 1; }
  /* `poly.times { }` / `upto(n) { }` / `downto(n) { }` answer the receiver,
     which codegen unboxes to an sp_int before handing the call to the typed
     emitters. step is left out: a Float owns it too. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      ((argc == 0 && sp_streq(name, "times")) ||
       (argc == 1 && (sp_streq(name, "upto") || sp_streq(name, "downto")))) &&
      infer_type(c, recv) == TY_POLY && !an_user_defines_or_reads(c, name))
    { *out = TY_INT; return 1; }
  /* The blockless forms answer the same range-shaped enumerator the typed
     Integer arm answers below, so a chain on them (`n.times.map { }`) keeps
     its array type. Without this arm a boxed receiver -- every Integer
     parameter under promote mode -- left the chain unknown, and the call on
     its answer was emitted as a NoMethodError (#4677). */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 &&
      ((argc == 0 && sp_streq(name, "times")) ||
       (argc == 1 && (sp_streq(name, "upto") || sp_streq(name, "downto")))) &&
      infer_type(c, recv) == TY_POLY && !an_user_defines_or_reads(c, name))
    { *out = TY_RANGE; return 1; }
  /* `poly.find { }` / `detect { }` answer the winning ELEMENT, boxed. Without
     an arm here they fell through to the last-resort Hash face below, which
     types them as the winning [k, v] pair -- and the emitter, which answers
     the element either way, then had its already-boxed value boxed a second
     time under that array type. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "find") || sp_streq(name, "detect")) &&
      infer_type(c, recv) == TY_POLY && !an_user_defines_or_reads(c, name))
    { *out = TY_POLY; return 1; }
  /* find_all is NOT the third spelling of select: Hash#select answers a Hash,
     Hash#find_all the [k, v] pairs as an Array, whatever the receiver turns
     out to be. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      sp_streq(name, "find_all") && infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY_ARRAY; return 1; }
  /* `poly.each_slice(n) { }` / `each_cons(n) { }` answer the receiver, whatever
     kind it turns out to be -- an Array for an Array, the Hash itself for a
     Hash -- so the result rides boxed, like the filtering siblings above. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 1 &&
      (sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY; return 1; }
  /* `poly.zip(other) { }` / `poly.cycle(n) { }` answer nil, as they do for a
     typed receiver. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 1 &&
      (sp_streq(name, "zip") || sp_streq(name, "cycle")) &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_NIL; return 1; }
  /* `poly.zip(other...)` on a value only known to be an array at runtime
     (e.g. a row that is a block param of an outer nested-array iterator):
     a poly array of tuples, matching the array-receiver form (#3190). */
  if (recv >= 0 && sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0 &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY_ARRAY; return 1; }
  /* `poly_recv.sum { }` (a group_by bucket, a case-merged local) folds the
     block result with sp_poly_add, so the accumulation is a boxed poly value.
     Concrete typed arrays keep their int/float/string sum arms below. (#2872) */
  if (recv >= 0 && sp_streq(name, "sum") && nt_ref(nt, id, "block") >= 0 &&
      infer_type(c, recv) == TY_POLY)
    { *out = TY_POLY; return 1; }
  return 0;
}

/* Regexp and MatchData: Regexp.compile and the class methods, a Regexp receiver, a MatchData receiver (infer_call_inner's rules, in their order) */
static int infer_regexp_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind *out) {
  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && sp_streq(name, "compile")) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && sp_streq(cn, "Regexp")) { *out = TY_REGEX; return 1; }
    }
  }

  /* StringScanner instance methods */
  /* StringScanner: a native-bound class (packages/strscan); no arms here. */

  /* Regexp class methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if ((sp_streq(name, "escape") || sp_streq(name, "quote")) && argc >= 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "union")) { *out = TY_REGEX; return 1; }  /* argc 0 = the never-matching /(?!)/ */
    if (sp_streq(name, "last_match") && argc == 0) { *out = TY_MATCHDATA; return 1; }
    if (sp_streq(name, "last_match") && argc == 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "linear_time?") && argc == 1) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "try_convert") && argc == 1) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "timeout") && argc == 0) { *out = TY_POLY; return 1; }   /* nil */
    if (sp_streq(name, "timeout") && argc == 1) { *out = TY_POLY; return 1; }   /* timeout= returns its arg */
    if (sp_streq(name, "timeout=") && argc == 1) { *out = TY_POLY; return 1; }
  }

  /* Regexp instance methods */
  if (recv >= 0 && rt == TY_REGEX) {
    const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
    if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
  }

  /* MatchData instance methods */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) { *out = op->result; return 1; }
    }
    if (sp_streq(name, "[]") && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_RANGE ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode"))))
      { *out = TY_POLY_ARRAY; return 1; }   /* md[range] (#2532) */
    if (sp_streq(name, "[]") && argc == 1) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "[]") && argc == 2) { *out = TY_POLY_ARRAY; return 1; }   /* md[start, length] (#2507) */
    if (sp_streq(name, "named_captures") && argc == 1) {
      /* symbolize_names: false asks for the string keys (#3640) */
      int kv = kwh_lookup(nt, argv[0], "symbolize_names");
      const char *kvt = kv >= 0 ? nt_type(nt, kv) : NULL;
      if (kvt && sp_streq(kvt, "FalseNode")) { *out = TY_STR_POLY_HASH; return 1; }
      { *out = TY_SYM_POLY_HASH; return 1; }   /* symbolize (#2530) */
    }
  }
  return 0;
}

/* A query on a class constant (a Struct class's members and keyword_init?, Math.sqrt, the try_converts) and the container-read pre-arms (infer_call_inner's rules, in their order) */
static int infer_constant_query_call(Compiler *c, int id, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind *out) {
  /* <StructClass>.members at the class level: symbol array */
  if (recv >= 0 && sp_streq(name, "members") && argc == 0) {
    const char *mrty = nt_type(nt, recv);
    int mci = -1;
    if (mrty && (sp_streq(mrty, "ConstantReadNode") || sp_streq(mrty, "ConstantPathNode")))
      mci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (mrty && (sp_streq(mrty, "LocalVariableReadNode") ||
                      (sp_streq(mrty, "CallNode") && is_struct_call(c, recv))))
      mci = class_var_static_ci(c, recv);
    if (mci >= 0 && c->classes[mci].is_struct &&
        comp_cmethod_in_chain(c, mci, "members", NULL) < 0) { *out = TY_POLY_ARRAY; return 1; }
  }
  if (recv >= 0 && sp_streq(name, "keyword_init?") && argc == 0) {
    const char *krty = nt_type(nt, recv);
    int kci = -1;
    if (krty && (sp_streq(krty, "ConstantReadNode") || sp_streq(krty, "ConstantPathNode")))
      kci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (krty && sp_streq(krty, "LocalVariableReadNode"))
      kci = class_var_static_ci(c, recv);
    if (kci >= 0 && c->classes[kci].is_struct &&
        comp_cmethod_in_chain(c, kci, "keyword_init?", NULL) < 0) { *out = TY_POLY; return 1; }  /* nil/true/false */
  }
  /* Integer.sqrt(Bignum) -> Bignum (#2420) */
  if (recv >= 0 && sp_streq(name, "sqrt") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      infer_type(c, argv[0]) == TY_BIGINT) { *out = TY_BIGINT; return 1; }
  /* Hash[k: v] desugared to a bare hash literal: transparent passthrough */
  if (recv >= 0 && sp_streq(name, "__hash_brackets_kw")) { *out = infer_type(c, recv); return 1; }
  /* Hash[] with no arguments: an empty hash (same C type as a bare {}) */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash"))
    { *out = TY_STR_POLY_HASH; return 1; }
  /* Array/Integer/String/IO.try_convert(x) -> the value or nil (poly)
     (#2325, #2585) */
  if (recv >= 0 && name && sp_streq(name, "try_convert") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "Array") || sp_streq(nt_str(nt, recv, "name"), "Integer") ||
       sp_streq(nt_str(nt, recv, "name"), "String") || sp_streq(nt_str(nt, recv, "name"), "IO")))
    { *out = TY_POLY; return 1; }
  if (recv >= 0 && name && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash")) {
    if (sp_streq(name, "try_convert")) { *out = TY_POLY; return 1; }
  }
  /* container-read builtin pre-arms (#3234). The name/argc gates run
     FIRST: the infer_type(recv) probe recurses through the receiver's
     own call chain, and paying it for EVERY one-arg call re-infers each
     chain suffix once more per level -- exponential on deep chains (a
     Rails-scale tree went 23s -> >10min under it). Gated this way only
     the arm names below ever pay the probe. */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "cover?") || sp_streq(name, "gcdlcm") ||
       sp_streq(name, "sum") || sp_streq(name, "inject") ||
       sp_streq(name, "reduce")) &&
      infer_type(c, recv) == TY_POLY) {
    if (sp_streq(name, "cover?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "gcdlcm")) { *out = TY_POLY_ARRAY; return 1; }   /* a Bignum pair stays boxed (#4665) */
    if (sp_streq(name, "sum") && nt_ref(nt, id, "block") < 0) { *out = TY_POLY; return 1; }
    if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) &&
        nt_ref(nt, id, "block") < 0 && infer_type(c, argv[0]) == TY_SYMBOL) { *out = TY_POLY; return 1; }
  }
  return 0;
}

static TyKind infer_call_inner(Compiler *c, int id) {
  /* the call is inferred afresh: only the row this pass answers with counts */
  /* the builtin-only re-derivation (an_builtin_answer) asks what the call
     would be with no user method: not the call's answer, so it neither
     clears nor makes the call's records */
  if (g_plan_check && !an_builtin_only && id >= 0 && id < c->node_cap) {
    c->bop_inf[id] = NULL;
    c->ucall_inf[id].via = UC_NONE;
  }

  /* a yielder push (`y << v` inside an Enumerator.new generator) lowers to a
     Fiber.yield, whose value is boxed -- never the array append it looks like */
  if (nt_int(c->nt, id, "yielder_push", 0)) return TY_POLY;
  /* the rewrite recorded that this call answers nil whatever it drives (#3589) */
  if (nt_int(c->nt, id, "nil_result", 0)) return TY_NIL;
  /* the redirect recorded that this call yields its original receiver (#2981) */
  {
    int sr = nt_int(c->nt, id, "enum_self_result", -1);
    if (sr >= 0) return infer_type(c, sr);
  }
  const NodeTable *nt = c->nt;
  /* `e.each { }` over an Enumerator, walked through its marked `to_a` hop:
     it answers the generator's body value or the collection the Enumerator
     was made from, not the hop's array. */
  {
    const char *en = nt_str(nt, id, "name");
    int er = nt_ref(nt, id, "receiver");
    if (en && (sp_streq(en, "each") || sp_streq(en, "each_with_index")) &&
        nt_ref(nt, id, "block") >= 0 && er >= 0 && nt_kind(nt, er) == NK_CallNode &&
        nt_str(nt, er, "enum_each_wrap") && nt_ref(nt, er, "receiver") >= 0 &&
        infer_type(c, nt_ref(nt, er, "receiver")) == TY_ENUMERATOR)
      return TY_POLY;
  }
  /* a dynamic send lowered to a name-dispatch (desugar_dynamic_send) yields one
     of several boxed method results -> poly. */
  { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) return TY_POLY; }
  /* and a runtime-name respond_to? (desugar_dynamic_respond_to): true or false */
  { int dn = 0; nt_arr(nt, id, "dyn_rto_arms", &dn); if (dn > 0) return TY_BOOL; }
  /* likewise a runtime-name const_get (desugar_dynamic_const_get): a class
     or a value, whichever arm the name selects */
  { int dn = 0; nt_arr(nt, id, "dyn_cget_arms", &dn); if (dn > 0) return TY_POLY; }
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) return TY_UNKNOWN;
  /* a boxed value's enum_for no generator serves (desugar_to_enum) */
  if (sp_streq(name, "__poly_enum_for")) return TY_ENUMERATOR;

  /* Array#to_a and #to_ary answer the receiver itself, and a boxed typed
     array can only hand out a converted copy as an sp_PolyArray. Where that
     answer is mutated in place, keep it boxed, so the mutation reaches the
     array the receiver holds. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "to_a") || sp_streq(name, "to_ary")) &&
      nt_ref(nt, id, "block") < 0 && infer_type(c, recv) == TY_POLY &&
      an_to_a_result_mutated(c, id)) {
    if (!an_user_poly_arm(c, name, argc)) return TY_POLY;
  }

  /* `alias new old` / `alias_method :new, :old` in a reopened primitive
     (`class String; alias starts_with? start_with?`). The reopen's ClassInfo
     carries the alias, but a call on a String-typed receiver is resolved by
     NAME against the builtin surface, which has never heard of `new`: the
     emitter raised NoMethodError for a builtin target, and named a C
     function that does not exist (sp_String_yell) for a target the reopen
     itself defined. Resolve through the alias table once, on the node, so
     every later pass and the emitter see the target name -- a builtin
     method dispatches as the builtin, a reopen's own def as that def.
     Receiverless inside the reopen (`def shout = up + "!"`) the class is the
     scope's own. Only the primitives the reopen dispatch covers can reach
     here: builtin_class_of_type names no other. */
  {
    int aci = -1;
    if (recv >= 0) {
      const char *bcn = builtin_class_of_type(infer_type(c, recv));
      if (bcn) aci = comp_class_index(c, bcn);
    }
    else {
      int si = id < c->nt->count ? c->nscope[id] : -1;
      int k = si >= 0 ? c->scopes[si].class_id : -1;
      if (k >= 0 && k < c->nclasses && c->classes[k].name &&
          (sp_streq(c->classes[k].name, "String") || sp_streq(c->classes[k].name, "Integer") ||
           sp_streq(c->classes[k].name, "Float") || sp_streq(c->classes[k].name, "Symbol") ||
           sp_streq(c->classes[k].name, "Time")))
        aci = k;
    }
    /* a call already resolved to a captured builtin keeps it: its name is
       the builtin's now, which the class may alias to its own method */
    if (aci >= 0 && c->classes[aci].naliases > 0 && !nt_int(nt, id, "builtin_only", 0)) {
      int bi = 0;
      const char *rn = comp_resolve_alias_ex(c, aci, name, NULL, &bi);
      if (rn && !sp_streq(rn, name)) {
        nt_node_set_str((NodeTable *)nt, id, "name", rn);
        name = nt_str(nt, id, "name");
        /* the alias captured the builtin: the class's own method of that
           name, defined or aliased after it, is not this call's */
        if (bi) nt_node_set_int((NodeTable *)nt, id, "builtin_only", 1);
      }
    }
  }

  /* `Module.accessor.cmethod(...)` where the singleton accessor statically
     folds to a constant: dispatch as that constant's class method. This has to
     come BEFORE anything that decides by the receiver's type, because the
     receiver -- a `class << self; attr_reader` read -- types poly (its slot
     holds a class value), and every rule that answers a poly receiver answers
     poly. It sat below them, and was never reached: the emitter, which asks
     the same fold, devirtualized the call to the class method's C function
     while the site expected an sp_RbVal, and clang refused the seam (#4426).
     The hand-written `def self.adapter; @adapter; end` never had the problem
     because that reader is a class method with a typed return, not a slot. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int mi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
      if (mi >= 0) return an_user_call(c, id, mi, UC_CMETH, fold_ci);
    }
    /* Stage-2: accessor holds one of several constants; unify their cmethod returns. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      TyKind r = TY_UNKNOWN;
      for (int k = 0; k < ncand; k++) {
        int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
        if (mi >= 0) r = ty_unify(r, method_call_ret(c, mi, id));
      }
      if (r != TY_UNKNOWN) return r;
    }
  }

  /* The universal predicates and conversions the poly runtime answers as a RAW
     C scalar. The emitters have always known this; the TYPE did not say it, so
     `x&.frozen?` was inferred poly while its value arm rendered an sp_bool, and
     the safe-navigation emitter carried a hand-kept table of these names to
     put the two back together. A table of what a call ANSWERS belongs in the
     inference, where every consumer reads it -- and it went stale as tables do
     (#4070 follow-up: `begin`, `end`, `count` and `bytes` were missing and
     `infinite?` was listed with the wrong type). Guarded the way the emitter
     guarded its table: a user class owning the name answers for itself. */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 && !an_user_defines_or_reads(c, name) &&
      infer_type(c, recv) == TY_POLY) {
    /* to_i widens in promote only. #4665 answers the boxed Integer surface
       ungated (pred, pow, gcd, lcm, ceildiv), but deliberately left to_i
       raising -- test/poly_bignum_integer_surface.rb pins
       `poly(2**64-1).to_i` to RangeError and the commit calls promoting that
       slot "the wider question of #2024". promote mode is where the answer
       is already promised, so it widens there and nowhere else (#4688). */
    if (g_promote_mode && argc == 0 && sp_streq(name, "to_i")) return TY_POLY;
    for (int q = 0; AN_POLY_RAW[q].n; q++)
      if (AN_POLY_RAW[q].ac == argc && sp_streq(name, AN_POLY_RAW[q].n)) return AN_POLY_RAW[q].t;
    /* the exception accessors the emitter answers on a poly receiver
       through sp_poly_exc_acc (a rescued exception in a union-typed local, a
       `case msg when ::Exception` subject): a boxed value each, nil when
       the exception has none -- `if msg.backtrace` in the logger gem's
       Formatter was refused as a non-bool condition because the call had
       no type at all */
    if (argc == 0 && (sp_streq(name, "backtrace") || sp_streq(name, "result") ||
                      sp_streq(name, "errno") || sp_streq(name, "key") || sp_streq(name, "receiver")))
      return TY_POLY;
  }

  /* A retargeted `x.send(:m)` reaching a top-level def: see the codegen twin. */
  if (nt_str(nt, id, "send_blind") && recv >= 0 && nt_ref(nt, id, "block") < 0) {
    int smi = comp_method_index(c, name);
    if (smi >= 0 && !(smi < c->nscopes && c->scopes[smi].yields)) {
      if (!send_blind_recv_owns(c, recv, infer_type(c, recv), name))
        return an_user_call(c, id, smi, UC_SEND_BLIND, -1);
    }
  }

  /* A call with NO receiver resolves the way CRuby's ancestry does: the
     enclosing scope's own chain first, then Object -- where a top-level `def`
     lands -- and only then a Kernel builtin. The user-method arm further down
     said the second part already, but the Kernel arms are reached by POSITION
     and several sit above it, so `caller`, `p`, `puts` and `loop` were typed
     from the builtin while codegen called the user's method: the local was
     declared sp_StrArray * and assigned a const char *. One rule, asked before
     any builtin arm, keeps the two halves of the compiler on the same method. */
  if (recv < 0 && nt_ref(nt, id, "block") < 0 &&
      !nt_str(nt, id, "vis_enforce")) {
    int bmi = comp_method_index(c, name), bvia = UC_TOP;
    if (bmi < 0) { bmi = comp_included_method_index(c, name, id); bvia = UC_INCLUDED; }
    if (bmi >= 0) {
      Scope *bsc = comp_scope_of(c, id);
      int bcls = bsc ? bsc->class_id : -1;
      int shadowed = bcls >= 0 && bcls < c->nclasses &&
                     (bsc->is_cmethod
                        ? comp_cmethod_in_chain(c, bcls, name, NULL) >= 0
                        : (comp_method_in_chain(c, bcls, name, NULL) >= 0 ||
                           comp_is_reader(&c->classes[bcls], name)));
      /* A callee that yields carries a block parameter in its C signature,
         which the arm below is what establishes: answering here left the call
         site and the definition disagreeing about the signature. */
      if (!shadowed && !(bmi < c->nscopes && c->scopes[bmi].yields))
        return an_user_call(c, id, bmi, bvia, bvia == UC_TOP ? -1 : c->scopes[bmi].class_id);
    }
    /* ...and the first part of that sentence, which only the per-arm
       `!an_bare_call_class_owned` guards said, one name at a time: the
       enclosing chain's OWN method answers before any Kernel builtin, with
       no top-level def of the name anywhere. A module's sibling call is the
       shape that has none -- `module_function; def trap(m) = raise(...)`
       beside a `def div` that calls `trap(...)` -- and the Kernel arms below
       claimed it (#4592). Same ownership test the codegen twin makes. */
    if (bmi < 0 && ie_class_of(c, id) < 0 && an_bare_call_class_owned(c, id)) {
      Scope *osc = comp_scope_of(c, id);
      int ocls = osc ? osc->class_id : -1;
      int omi = osc && osc->is_cmethod ? comp_cmethod_in_chain(c, ocls, name, NULL)
                                       : comp_method_in_chain(c, ocls, name, NULL);
      /* the same answer the implicit-self arm below gives, overrides
         included: codegen's dispatch switches over every descendant's
         override, so the type must hold each of their returns. Answering
         the base's declared return alone typed `@snap = attributes` a
         Hash where the switch answered sp_RbVal (#4600). */
      if (omi >= 0 && omi < c->nscopes && !c->scopes[omi].yields && osc)
        return an_self_call_ret(c, osc, name, omi, id);
    }
  }

  /* the accessors every exception carries, on an instance of a user subclass
     (#3732): #exception is self, #cause another exception, #backtrace the
     frames, and the two message renderings strings */
  if (recv >= 0 && argc == 0 && ty_is_object(infer_type(c, recv)) &&
      class_is_exc_subclass(c, ty_object_class(infer_type(c, recv))) &&
      comp_method_in_chain(c, ty_object_class(infer_type(c, recv)), name, NULL) < 0) {
    if (sp_streq(name, "exception")) return infer_type(c, recv);
    if (sp_streq(name, "cause")) return TY_EXCEPTION;
    if (sp_streq(name, "backtrace")) return TY_STR_ARRAY;
    if (sp_streq(name, "full_message") || sp_streq(name, "detailed_message")) return TY_STRING;
  }

  /* #dup drops the receiver's singleton methods, so the copy is an instance of
     the class Ruby sees rather than the synthesized singleton one (#3739) */
  if (recv >= 0 && sp_streq(name, "dup") && argc == 0) {
    TyKind drt0 = infer_type(c, recv);
    if (ty_is_object(drt0) &&
        comp_resolve_member(c, ty_object_class(drt0), "dup", 0, NULL, NULL) == SP_MEMBER_NONE) {
      int dci0 = ty_object_class(drt0);
      int vis = singleton_visible_ci(c, dci0);
      if (vis != dci0) return ty_object(vis);
    }
  }

  /* A copy of a value whose type is only known at run time is itself only known
     at run time: answering UNKNOWN left the slot open to the usage rules, and an
     int-keyed `copy[i] = v` then typed the copy of an Array as a HASH -- the
     method answered `{}` where CRuby answered an Array (#3952). */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "dup") || sp_streq(name, "clone")) &&
      infer_type(c, recv) == TY_POLY) {
    if (c->poly_builtin_ty && id < c->node_cap) c->poly_builtin_ty[id] = TY_POLY;
    return TY_POLY;
  }

  /* `!`, `!=` and `==` are ordinary methods a class may override, so the
     result is whatever its definition answers, not a bool (#3740). `==` joins
     them: a class whose `==` answers something else had every call site typed
     bool, and the value was rendered as true/false. */
  if (recv >= 0 && (sp_streq(name, "!") || sp_streq(name, "!=") || sp_streq(name, "==")) &&
      argc == (sp_streq(name, "!") ? 0 : 1)) {
    TyKind nrt = infer_type(c, recv);
    if (ty_is_object(nrt)) {
      int nmi = comp_method_in_chain(c, ty_object_class(nrt), name, NULL);
      if (nmi >= 0) return (TyKind)c->scopes[nmi].ret;
    }
  }

  /* $~'s MatchData face over the match registers (codegen reads the same
     backing the back-references use): nullable strings. */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "GlobalVariableReadNode") ||
       sp_streq(nt_type(nt, recv), "BackReferenceReadNode")) &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "$~") &&
      (sp_streq(name, "pre_match") || sp_streq(name, "post_match") || sp_streq(name, "to_s")))
    return TY_STRING;

  /* A block with a top-level `break <v>` makes this call return <v>, so its
     result is the union of the normal result and the break value -- poly. The
     break wrapper suppresses this (g_infer_ignore_brk) to recover the normal
     result type for the inner emission. */
  if (!g_infer_ignore_brk && call_breaks(c, id)) return TY_POLY;

  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  /* --plan-check: a boxed receiver's call is dispatched over every user
     class's method of the name; the first candidate stands for that union
     (an arm below that binds one method records it instead) */
  if (g_plan_check && recv >= 0 && rt == TY_POLY && name && !an_builtin_only) {
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    for (int pi = 0; pi < npc; pi++) {
      if (c->classes[pcs[pi].cls].is_native_class) continue;
      /* a reader candidate written as a method (`def x = @x`) is that method */
      int pmi = pcs[pi].mi >= 0 ? pcs[pi].mi : comp_method_in_chain(c, pcs[pi].cls, name, NULL);
      if (pmi >= 0) { an_user_call_record(c, id, pmi, UC_POLY, pcs[pi].cls); break; }
    }
  }
  /* respond_to? on a boxed receiver is the compile-time fold
     (emit_call_body) unless a class that can be the value overrides it
     (respond_to_user_defined: an instantiated class with respond_to? in its
     chain), when the dispatch has that override's arm and its answer is the
     call's. Without such a class the fold answers, a Boolean; typed by the
     union with a class's override that is never made, the call read the
     fold's Boolean as a boxed value. */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 && name && sp_streq(name, "respond_to?")) {
    int own = 0;
    for (int k = 0; k < c->nclasses && !own; k++)
      if (c->classes[k].instantiated && comp_method_in_chain(c, k, name, NULL) >= 0) own = 1;
    if (!own) return TY_BOOL;
  }
  /* The call on that widened yield is a boxed carrier too, whatever the
     builtin answers on a poly receiver: each site emits its own kind's
     result (an sp_IntArray from one site's sort, an sp_StrArray from the
     other's) and boxes it. Left to the poly-receiver rows, `yield.sort` typed
     an sp_PolyArray slot, which neither site's array fits, and the tail was
     dropped for the slot's nil. */
  if (rt == TY_POLY && yield_recv_builtin_every_site(c, id)) return TY_POLY;
  /* A read marked to hand out the shared handle (a reader call a mutation
     reaches through, `c.name.setbyte(0, 90)`) is still a String receiver:
     the handle is only its storage. Left as TY_STRBUF it matched none of the
     String rows, and setbyte's Integer answer typed as the receiver. The
     appenders that answer the receiver itself keep the handle: their value
     is that same object, and the in-place append arms are chosen on it. */
  if (rt == TY_STRBUF && !(name && (sp_streq(name, "<<") || sp_streq(name, "concat") ||
                                    sp_streq(name, "prepend") || sp_streq(name, "replace") ||
                                    sp_streq(name, "insert") || sp_streq(name, "clear"))))
    rt = TY_STRING;
  /* The appenders keep the handle as the RECEIVER type above, so the in-place
     arms are chosen; the call's own value is still the ordinary String read
     (`sp_String_cstr(...)`), which is what the value-form emitter writes. Left
     out of the String rows by that exclusion, `obj.buf << x` in value position
     inferred TY_UNKNOWN and was boxed as `(expr, sp_box_nil())` -- the append
     ran and the method answered nil. */
  if (rt == TY_STRBUF && recv >= 0 && name &&
      ((argc == 1 && (sp_streq(name, "<<") || sp_streq(name, "concat") ||
                      sp_streq(name, "prepend") || sp_streq(name, "replace"))) ||
       (argc == 2 && sp_streq(name, "insert")) ||
       (argc == 0 && sp_streq(name, "clear"))))
    return TY_STRING;
  /* A program's own method on Range / Time / File / Class, ahead of the
     builtin rules for those receivers: the Time block below answers Integer
     for a name it does not know, which typed `t.stamp` as an int slot around
     a String-returning reopen. The scalar reopens (String, Integer, ...) keep
     their place further down, where their rules have long been ordered. */
  if (recv >= 0 && (rt == TY_RANGE || rt == TY_TIME || rt == TY_IO || rt == TY_CLASS) &&
      !nt_int(nt, id, "builtin_only", 0)) {
    const char *ecn = rt == TY_RANGE ? "Range" : rt == TY_TIME ? "Time" : "Class";
    int eci = rt == TY_IO ? io_reopen_class(c, name) : comp_class_index(c, ecn);
    int emi = eci >= 0 ? comp_method_in_chain(c, eci, name, NULL) : -1;
    if (emi >= 0 && rt == TY_IO && io_reopen_ret_mixed(c, name)) return TY_POLY;
    /* a kind no reopening serves gets the builtin's answer: boxed when the
       two answer different types */
    if (emi >= 0 && rt == TY_IO && io_reopen_leaves_builtin(c, name)) {
      TyKind ur = an_user_call(c, id, emi, UC_REOPEN, eci);
      int sv_skip = g_io_skip_reopen, sv_skip_node = g_io_skip_node;
      g_io_skip_reopen = 1; g_io_skip_node = id;
      TyKind bt = an_builtin_answer(c, id);
      g_io_skip_reopen = sv_skip; g_io_skip_node = sv_skip_node;
      if (bt != TY_UNKNOWN && ur != TY_UNKNOWN && bt != ur) return TY_POLY;
      return ur;
    }
    if (emi >= 0) return an_user_call(c, id, emi, UC_REOPEN, eci);
  }
  /* An Array or Hash reopen that defines a builtin's name owns it, as the
     scalar reopens do: `class Array; def first = "arr"; end` makes
     `[1, 2].first` answer "arr" in CRuby. The builtin rows below answered the
     element type, and codegen, which reached the reopen only for a name no
     builtin arm takes, called the builtin. Only a method the reopen itself
     defines: an Object reopen's `first` does not displace Array#first. */
  if (recv >= 0 && name && (ty_is_array(rt) || ty_is_obj_array(rt) || ty_is_hash(rt)) &&
      nt_ref(nt, id, "block") < 0 && !nt_int(nt, id, "builtin_only", 0)) {
    int aci = comp_class_index(c, ty_is_hash(rt) ? "Hash" : "Array");
    int adc = -1, ami = aci >= 0 ? comp_method_in_chain(c, aci, name, &adc) : -1;
    /* the reopen's own method under this very name: an alias taken before the
       reopen (`alias orig_first first`) still names the builtin */
    if (ami >= 0 && adc == aci && c->scopes[ami].name && sp_streq(c->scopes[ami].name, name))
      return an_user_call(c, id, ami, UC_REOPEN, aci);
  }
  /* A boxed-value hash whose values are all one class: its value reads are
     that class (nil included, as a NULL pointer), and `values` an array of it
     (#4846). */
  if (recv >= 0 && (rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH || rt == TY_POLY_POLY_HASH)) {
    const char *hn = nt_str(nt, id, "name");
    int hargs = nt_ref(nt, id, "arguments");
    int hac = 0; if (hargs >= 0) nt_arr(nt, hargs, "arguments", &hac);
    if (hn && nt_ref(nt, id, "block") < 0 &&
        (((sp_streq(hn, "[]") || sp_streq(hn, "fetch") || sp_streq(hn, "delete")) && hac == 1) ||
         (sp_streq(hn, "values") && hac == 0))) {
      int hcls = hv_value_class(c, recv);
      if (hcls >= 0) return sp_streq(hn, "values") ? ty_obj_array(hcls) : ty_object(hcls);
    }
  }
  /* A block call on a poly receiver whose candidates include a YIELDING method
     is served by that method's proc-form clone, which answers poly uniformly
     (its yield is a call on a real proc). This has to precede every
     `poly.<builtin name> { }` rule below: those type the call from the builtin
     alone, so a candidate answering an Integer array landed in a generic-array
     slot with no conversion and the value was read as the wrong container
     (#3399, #3409). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 &&
      poly_enum_op_for(name)) {
    for (int k = 0; k < c->nclasses; k++) {
      if (!c->classes[k].ctor_reachable) continue;
      if (comp_method_in_chain(c, k, name, NULL) >= 0) return TY_POLY;
    }
  }
  /* Same, for a name outside that surface: only a yielding candidate makes
     the call a dispatch there, since a non-yielding one leaves a
     block-carrying call on the builtin path entirely. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0) {
    for (int k = 0; k < c->nclasses; k++) {
      if (!c->classes[k].ctor_reachable) continue;
      int ymi = comp_method_in_chain(c, k, name, NULL);
      if (ymi >= 0 && c->scopes[ymi].yields) return TY_POLY;
    }
  }
  /* Range receivers (analyze_infer_recv.c). */
  { TyKind rr; if (infer_range_call(c, id, rt, &rr)) return rr; }
  /* A Range Enumerable method spinel serves by materializing to an int array:
     infer it as the array version (the array arms below key on `rt`). */
  if (rt == TY_RANGE && range_enum_redispatch(c, id)) rt = TY_INT_ARRAY;
  /* the block form of each_with_index returns the receiver hash (#2417) */
  if (ty_is_hash(rt) && sp_streq(name, "each_with_index") &&
      nt_ref(nt, id, "block") >= 0) return rt;
  if (ty_is_hash(rt) && hash_enum_redispatch(c, id)) rt = TY_POLY_ARRAY;
  { TyKind r; if (infer_block_iter_call(c, id, nt, name, recv, argc, rt, &r)) return r; }
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
  /* Object#itself is the receiver, whatever its type -- the scattered per-type
     arms below predate this and remain harmless. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "itself") &&
      nt_ref(nt, id, "block") < 0 && !an_user_defines_or_reads(c, "itself"))
    return infer_type(c, recv);

  /* A literal integer power whose result exceeds int64 (`10 ** 30`, `2 ** 70`)
     is a Bignum in every overflow mode (CRuby). Type it bigint so codegen emits
     sp_bigint_pow rather than the saturating float sp_int_pow. */
  if (sp_streq(name, "**") && recv >= 0 && argc == 1) {
    long long base, exp;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &exp) &&
        infer_int_pow_overflows(base, exp))
      return TY_BIGINT;
  }
  /* Integer ** <negative literal>: a Rational in CRuby, typed statically. A
     runtime exponent keeps the static Integer result (typing it poly would
     cascade through every int-arithmetic consumer -- see limitations.md); the
     negative case raises loudly, and the poly-dispatched path (promote-mode
     params) resolves the class at runtime in sp_poly_pow. */
  /* The cheap operand-type and name gates come BEFORE infer_type(c, recv): that
     receiver re-inference recurses through infer_call, so calling it for every
     1-arg call node makes a wide call graph explode (#2707). */
  if (a0 == TY_INT && sp_streq(name, "**") && recv >= 0 && argc == 1 &&
      infer_type(c, recv) == TY_INT) {
    long long base, exp;
    int cexp = infer_const_int_node(nt, argv[0], &exp);
    if (cexp && exp < 0) return TY_RATIONAL;
    /* --int-overflow=promote: an int `**` whose operands are not both known
       constants can escape the word at run time and promote to a Bignum
       (codegen lowers it to sp_poly_pow), the `<<` rule below. A const-const
       pair is exact -- an overflowing one answered TY_BIGINT above, the rest
       fit an int. A typed exponent -- a block parameter, an element -- kept
       the int helper, which raises in the mode whose contract is to promote
       (#4679). */
    if (g_promote_mode && !(cexp && infer_const_int_node(nt, recv, &base)))
      return TY_POLY;
  }
  /* Integer with a Rational/Complex operand: ** Complex is Complex; ** Rational
     is a Float (by design, see codegen); fdiv is Float, div is the Integer floor. */
  if (recv >= 0 && argc == 1 && (a0 == TY_COMPLEX || a0 == TY_RATIONAL) &&
      (sp_streq(name, "**") || sp_streq(name, "fdiv") || sp_streq(name, "div")) &&
      infer_type(c, recv) == TY_INT) {
    if (sp_streq(name, "**") && a0 == TY_COMPLEX) return TY_COMPLEX;
    if (sp_streq(name, "**") && a0 == TY_RATIONAL) return TY_FLOAT;
    if (sp_streq(name, "fdiv") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_FLOAT;
    if (sp_streq(name, "div") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_INT;
  }
  /* A literal left shift whose result exceeds int64 (`1 << 64`, the 2**64 mask)
     is a Bignum -- type it bigint so codegen emits a bigint shift, not a UB C
     `1LL << 64LL`. */
  if (sp_streq(name, "<<") && recv >= 0 && argc == 1) {
    long long base, amount;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &amount) &&
        infer_int_shl_overflows(base, amount))
      return TY_BIGINT;
  }

  /* `@table[i][j]` on a poly-array dispatch table of int-returning entries
     yields an int (a method/peek table). Resolves the optcarrot CPU's
     fetch/peek/store, which would otherwise run boxed-poly. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_double_index_int(c, id))
    return TY_INT;
  /* `@nested[i]` on a poly array of int arrays yields an int array (nested
     array of int arrays -- @chr_banks / @nmt_mem). Without this the element
     read is a boxed poly and cascades poly through the PPU. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_index_int_array(c, id))
    return TY_INT_ARRAY;
  /* `CONST[i]` on a poly-array constant of int arrays (WAVE_FORM / TILE_LUT
     shapes) yields an int array; codegen unboxes the poly element. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && const_poly_index_int_array(c, id))
    return TY_INT_ARRAY;

  /* Complex / Rational value types. */
  /* A class-tagged poly value answers #name/#to_s/#inspect with its class name
     (Base.subclasses / #ancestors hand back boxed classes, #2656). Only when no
     user class defines the method -- then it dispatches to that instead. */
  /* Boxed (poly) receivers: the run of poly-face arms of infer_call (analyze_infer_recv.c). */
  /* A boxed receiver ordered against a class: Module#< and friends answer
     nil for unrelated classes, so the result stays boxed as it does for a
     class-typed receiver. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && infer_type(c, argv[0]) == TY_CLASS &&
      (is_cmp_op(name) || sp_streq(name, "<=>"))) return TY_POLY;
  { TyKind rr; if (infer_poly_call(c, id, rt, &rr)) return rr; }
  /* bool/nil <=> : 0 for an equal immediate pair, nil otherwise (#2733) */
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>") &&
      (rt == TY_BOOL || rt == TY_NIL)) return TY_POLY;
  /* __enum_chain(arr): the desugared Enumerable#chain / Enumerator#+ (#2545) */
  if (recv < 0 && sp_streq(name, "__enum_chain") && argc == 1) return TY_ENUMERATOR;
  if (recv < 0 && sp_streq(name, "__enum_pairs") && argc == 1) return TY_ENUMERATOR;
  /* Dir surface (#2823, #2828, #2830) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
    if (sp_streq(name, "empty?") && argc == 1) return TY_BOOL;
    if (sp_streq(name, "home") && argc == 1) return TY_STRING;
    if (sp_streq(name, "glob") && (argc == 1 || argc == 2)) return TY_STR_ARRAY;
    if (sp_streq(name, "for_fd") && argc == 1) return TY_DIR;
    if (sp_streq(name, "fchdir") && argc == 1) return TY_INT;
  }
  /* the desugared ENV snapshot (#2742) */
  if (recv < 0 && sp_streq(name, "__env_to_h") && argc == 0) return TY_STR_STR_HASH;
  /* Exception-suppressed String or nullable conversion can answer nil, so it
     cannot use the unboxed numeric value type (#3893). */
  if (recv < 0 && (sp_streq(name, "Complex") || sp_streq(name, "Rational")) &&
      argc == 2 && (infer_type(c, argv[0]) == TY_STRING ||
                    ((infer_type(c, argv[0]) == TY_NIL || infer_type(c, argv[0]) == TY_POLY ||
                      ((infer_type(c, argv[0]) == TY_INT || infer_type(c, argv[0]) == TY_FLOAT) &&
                       nullable_int_value(c, argv[0]))) &&
                     nt_kind(nt, argv[1]) == NK_KeywordHashNode)) &&
      nt_type(nt, argv[1]) &&
      (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
       sp_streq(nt_type(nt, argv[1]), "HashNode"))) {
    int kn9 = 0; const int *els9 = nt_arr(nt, argv[1], "elements", &kn9);
    int only_exc9 = kn9 > 0;
    for (int e = 0; e < kn9 && only_exc9; e++) {
      int key9 = els9 ? nt_ref(nt, els9[e], "key") : -1;
      const char *kt9 = key9 >= 0 ? nt_type(nt, key9) : NULL;
      const char *kn9s = (kt9 && sp_streq(kt9, "SymbolNode")) ? nt_str(nt, key9, "value") : NULL;
      if (!kn9s || !sp_streq(kn9s, "exception")) only_exc9 = 0;
    }
    if (only_exc9) return TY_POLY;
  }
  if (recv < 0 && sp_streq(name, "Complex")) return TY_COMPLEX;
  if (recv < 0 && sp_streq(name, "Rational") && (argc == 1 || argc == 2)) {
    /* a bignum operand needs the big Rational, which is a boxed value */
    if (infer_type(c, argv[0]) == TY_BIGINT ||
        (argc == 2 && infer_type(c, argv[1]) == TY_BIGINT)) return TY_POLY;
    return TY_RATIONAL;
  }
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    if (rrty && sp_streq(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Complex") &&
        (sp_streq(name, "polar") || sp_streq(name, "rect") || sp_streq(name, "rectangular")))
      return TY_COMPLEX;
  }
  /* Numeric receivers: the Complex and Rational faces, the mixed Integer/Float x Complex operators, and the curried-Proc accumulator (analyze_infer_recv.c). */
  { TyKind rr; if (infer_numeric_call(c, id, rt, &rr)) return rr; }
  /* nil's to_a, to_h, =~ and !~ (and a Float's &, | and ^), which the
     receiver's class lacks: typed as nil's answer, boxed, whether or not the
     slot turns out able to hold nil. Typed only once the marking had settled
     (after the fixpoint), the call was still unknown to every expression
     around it, and `b.to_a.inspect` raised NoMethodError for the nil. */
  if (scalar_nil_only_call(c, id, rt)) return sp_streq(name, "!~") ? TY_BOOL : TY_POLY;

  /* Safe navigation &. : nil receiver always short-circuits to nil */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.") && rt == TY_NIL) return TY_NIL;
  }

  /* nil receiver: type inference for NilClass methods */
  if (recv >= 0 && sp_streq(name, "display") && argc == 0 &&
      !(ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR))
    return TY_NIL;
  if (recv >= 0 && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      ty_is_object(rt)) return TY_BOOL;
  if (recv >= 0 && rt == TY_SYMBOL && argc == 0 && sp_streq(name, "encoding"))
    return TY_POLY;  /* a boxed Encoding value */
  if (recv >= 0 && (rt == TY_BOOL || rt == TY_SYMBOL || rt == TY_FLOAT) && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?"))) return TY_BOOL;
  if (recv >= 0 && rt == TY_FLOAT && argc == 1 && sp_streq(name, "===")) return TY_BOOL;  /* (#2400) */
  { TyKind r; if (infer_nil_chain_call(c, id, nt, name, recv, rt, &r)) return r; }

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last of an empty array is nil, boxed to poly (codegen emits
           sp_box_nil). min/max/pop/shift/sample keep the historical int-0
           shortcut pending their own nil arms. */
        if ((sp_streq(name, "first") || sp_streq(name, "last") ||
             sp_streq(name, "sample")) && argc == 0) return TY_POLY;  /* nil, boxed (#2322) */
        if ((sp_streq(name, "nil?") || sp_streq(name, "empty?")) && argc == 0) return TY_BOOL;
        /* [] + X / [] - X / set-ops keep the OTHER operand's array kind (the
           codegen emits sp_<kind>Array_<op>(NULL, X)); the result type must
           agree with that kind, not the default poly array. */
        if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
             sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
             sp_streq(name, "intersection")) && argc == 1) {
          TyKind at = infer_type(c, argv[0]);
          if (ty_is_array(at) || at == TY_POLY_ARRAY) return at;
        }
        if ((sp_streq(name, "min") || sp_streq(name, "max") ||
             sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) return TY_INT;
        rt = TY_POLY_ARRAY;
      }
    }
    /* mirror for an empty HASH literal receiver (`{}.freeze`): without this
       the identity `freeze` stays unresolved, so `CONST = {}.freeze` never
       gets a type and the constant is dropped from codegen entirely -- reads
       then raise "uninitialized constant" or break the C build when the
       constant is a typed ivar's || fallback (#1758). Deliberately
       TY_STR_POLY_HASH, the same C type codegen emits for a bare {}, so the
       declaration, initializer, and readers agree (POLY_POLY would disagree
       with sp_StrPolyHash_new and produce garbage). */
    else if (rty && (sp_streq(rty, "HashNode") || sp_streq(rty, "KeywordHashNode"))) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* object query methods on a bare {} fold to a bool (see codegen) */
        if (argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "frozen?") ||
                          sp_streq(name, "empty?") || sp_streq(name, "any?"))) return TY_BOOL;
        if (argc == 1 && is_kind_query(name)) return TY_BOOL;
        if (argc == 0 && sp_streq(name, "to_h")) return TY_STR_POLY_HASH;   /* (#2410) */
        if (argc <= 1 && sp_streq(name, "sum")) {
          /* the empty collection answers the SEED itself, so the call is the
             seed's type -- except that nil and true/false have no scalar slot
             to be answered in, and ride boxed (`{}.sum(nil)` is nil) */
          TyKind st0 = argc == 1 ? infer_type(c, argv[0]) : TY_INT;         /* (#2416) */
          if (st0 == TY_NIL || st0 == TY_BOOL || st0 == TY_VOID) st0 = TY_POLY;
          return st0;
        }
        if (argc == 0 && (sp_streq(name, "min") || sp_streq(name, "max"))) return TY_POLY;
        if (argc == 0 && sp_streq(name, "minmax")) return TY_POLY_ARRAY;    /* (#2406) */
        if (argc == 1 && is_cmp_op(name)) return TY_BOOL;  /* (#2399) */
        rt = TY_STR_POLY_HASH;
      }
    }
  }

  /* `<&block-param>.call(...)` inside a yielding method: the explicit-call form
     of yield. Its value is the call-site block's value (resolved like yield). */
  {
    int emi = (int)(comp_scope_of(c, id) - c->scopes);
    if (emi > 0 && is_blk_param_call(c, id, emi)) {
      /* In a proc form the block is a real proc supplied per call site, so the
         call answers poly uniformly -- the same reason the YieldNode arm does.
         Resolving it against one site's block fixes the clone to that site,
         and a method declaring `&blk` reaches the dispatch this way rather
         than through a yield (#3408). */
      if (c->scopes[emi].is_proc_form) return TY_POLY;
      /* The per-site answer is the FIRST concrete block's type, but one body
         is emitted per site and a `&block` method reached from two sites whose
         blocks answer differently (Enumerable's collector and its generator)
         then boxes one of them against the other's type -- the C compiler saw
         an sp_RbVal cast to a pointer. When the sites disagree the value is
         only known at run time (#3793). */
      TyKind bfirst = yield_value_type(c, emi);
      { int sv_ua = g_yvt_unify_all;
        g_yvt_unify_all = 1;
        TyKind ball = yield_value_type(c, emi);
        g_yvt_unify_all = sv_ua;
        if (ball != bfirst && ball != TY_UNKNOWN) return TY_POLY; }
      /* No call site could say what the block answers -- a method that
         declares `&blk` and calls it without ever yielding has none of the
         yield machinery's routes to one, and a module method reached through
         `extend` is called with a proc built at the call site rather than
         spliced into it. Unknown is not an answer here: it degrades, and the
         degrade is silent, `blk.call.to_s` folding to the empty string and
         `blk.call.inspect` to "[]" (the nil-degrade placeholders in
         emit_call). The value is decided at run time, which is poly. */
      if (bfirst == TY_UNKNOWN || bfirst == TY_VOID) return TY_POLY;
      return bfirst;
    }
  }

  /* __dir__ -> the source directory (a string) */
  if (recv < 0 && sp_streq(name, "__dir__") && argc == 0) return TY_STRING;

  /* Kernel#caller -> the call stack as strings (empty in release builds) */
  if (recv < 0 && sp_streq(name, "caller") && argc <= 2 &&
      !an_bare_call_class_owned(c, id)) return TY_STR_ARRAY;
  /* caller_locations -> an array of Backtrace::Location objects. AOT builds keep
     no runtime frame stack (like `caller`, which is empty in release), so this is
     an empty array. Returning a poly array (not nil) keeps `&.first&.label`,
     `.each`, `.map`, and `.is_a?(Array)` well-typed and nil-safe. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "caller_locations") && argc <= 2) return TY_POLY_ARRAY;

  /* bare `name` inside a class method body -> the class name string */
  if (recv < 0 && sp_streq(name, "name") && argc == 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }
  /* `self.name` / `self.to_s` inside a class method -> the class name string */
  if (recv >= 0 && argc == 0 &&
      is_name_reader(name) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }

  { TyKind r; if (infer_block_kernel_call(c, id, nt, name, recv, rt, &r)) return r; }

  { TyKind r; if (infer_constant_query_call(c, id, nt, name, recv, argc, argv, &r)) return r; }
  { TyKind r; if (infer_method_proc_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }
  /* Hash#default_proc: the stored Hash.new{} block as a first-class Proc
     (NULL-encoded nil when the hash has none) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 0 && sp_streq(name, "default_proc"))
    return TY_PROC;
  /* default_proc= installs the lambda; the assignment's value is the proc (#2371) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 1 && sp_streq(name, "default_proc="))
    return TY_PROC;

  { TyKind r; if (infer_class_module_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  { TyKind r; if (infer_new_call(c, id, nt, name, recv, argc, argv, &r)) return r; }

  { TyKind r; if (infer_regexp_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  { TyKind r; if (infer_builtin_cmethod_call(c, id, nt, name, recv, argc, argv, &r)) return r; }

  { TyKind r; if (infer_runtime_value_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  /* Kernel#p returns its argument (one arg; several return the array), so it
     composes as an expression: x = p(y), f(p(y)). Statement-position p keeps
     its own emitter; this types the value form. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc >= 2)
    return TY_POLY_ARRAY;   /* p(a, b, ...) returns the array of its arguments */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc == 1)
    return infer_type(c, argv[0]);
  /* Object#instance_variables: a static symbol list for a typed object */
  if (recv >= 0 && ty_is_object(rt) && argc == 0 &&
      sp_streq(name, "instance_variables"))
    return TY_POLY_ARRAY;
  if (recv >= 0 && ty_is_object(rt) && argc == 0 &&
      (sp_streq(name, "methods") || sp_streq(name, "public_methods") ||
       sp_streq(name, "singleton_methods")) &&
      an_object_methods_listable(c, ty_object_class(rt), name))
    return TY_POLY_ARRAY;

  /* Kernel#puts / #print return nil; typed so the value form composes. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "puts") || sp_streq(name, "print")) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#warn / #printf / p() (no args) return nil in value position. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "warn") || sp_streq(name, "printf") ||
                   ((sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 0)) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#putc returns its argument. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "putc") && argc == 1 && nt_ref(nt, id, "block") < 0)
    return infer_type(c, argv[0]);

  { TyKind r; if (infer_handle_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  { TyKind r; if (infer_user_method_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  /* Object receivers: the user-object face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_object_call(c, id, rt, &rr)) return rr; }

  { TyKind r; if (infer_receiverless_call(c, id, nt, name, recv, argc, argv, &r)) return r; }
  /* Kernel.sleep(seconds) / ::Kernel.sleep -> Integer seconds slept */
  if (recv >= 0 && sp_streq(name, "sleep") && argc <= 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *rname = nt_str(nt, recv, "name");
      if (rname && sp_streq(rname, "Kernel")) return TY_INT;
    }
  }
  /* Signal.trap / Signal.list / Signal.signame; Process.kill (#2735, #2750) */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *rname = nt_str(nt, recv, "name");
    if (rname && sp_streq(rname, "ENV")) {
      if (sp_streq(name, "class")) return TY_CLASS;
      if (sp_streq(name, "frozen?")) return TY_BOOL;
      if (sp_streq(name, "shift")) return TY_POLY;
      if (sp_streq(name, "reject!") || sp_streq(name, "select!") || sp_streq(name, "filter!"))
        return TY_POLY;   /* nil when nothing changed (#2844) */
      if (sp_streq(name, "clear") || sp_streq(name, "update") || sp_streq(name, "merge!") ||
          sp_streq(name, "replace") || sp_streq(name, "delete_if") || sp_streq(name, "keep_if"))
        return TY_STR_STR_HASH;   /* filter-block params seed in mark_proc_captures */
    }
    if (rname && sp_streq(rname, "Signal")) {
      if (sp_streq(name, "trap") && argc >= 1) return TY_POLY;
      if (sp_streq(name, "list") && argc == 0) return TY_STR_INT_HASH;
      if (sp_streq(name, "signame") && argc == 1) return TY_STRING;
    }
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "kill") && argc >= 2)
      return TY_INT;
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "spawn") && argc >= 1)
      return TY_INT;
    if (rname && sp_streq(rname, "Process") &&
        (sp_streq(name, "waitpid2") || sp_streq(name, "wait2")) && argc <= 1)
      return TY_POLY_ARRAY;
    /* Process.wait / waitpid answer the pid they reaped and set $? */
    if (rname && sp_streq(rname, "Process") &&
        (sp_streq(name, "wait") || sp_streq(name, "waitpid")) && argc <= 1)
      return TY_INT;
  }

  /* Fiber storage: Fiber[:k] and Fiber.current[:k] -> poly */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && fiber_storage_recv(nt, recv)) return TY_POLY;
  /* Fiber[:k] = v -> returns v's type */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2 && fiber_storage_recv(nt, recv)) return infer_type(c, argv[1]);
  /* ENV[key] -> string or nil (use TY_STRING; null means nil). ENV.fetch
     answers its default on a miss, so the call is the union of String with
     the default's type: a String or nil default keeps the nullable string,
     anything else boxes. Typed String alone, an Array default was placed in
     the const char * slot as it stood. (The block form is the ENV snapshot's
     Hash#fetch, #2742, and types there.) */
  if (recv >= 0 && argc >= 1 && (sp_streq(name, "[]") || sp_streq(name, "fetch"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        if (sp_streq(name, "fetch") && argc >= 2) {
          TyKind dt = infer_type(c, argv[1]);
          if (dt == TY_UNKNOWN || dt == TY_NIL || dt == TY_VOID) return TY_STRING;
          return ty_unify(TY_STRING, dt);
        }
        return TY_STRING;
      }
    }
  }
  /* ENV.key?/has_key?/include?/member?(key) -> bool */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "key?") || sp_streq(name, "has_key?") ||
       sp_streq(name, "include?") || sp_streq(name, "member?"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) return TY_BOOL;
    }
  }
  /* ENV direct arms (#2743, #2746): typed results so p/interp accept them */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *rn = nt_str(nt, recv, "name");
    if (rn && sp_streq(rn, "ENV")) {
      if (sp_streq(name, "to_s")) return TY_STRING;
      /* a non-String key raises TypeError; type the (diverging) call poly */
      if (argc >= 1 &&
          (sp_streq(name, "[]") || sp_streq(name, "fetch") || sp_streq(name, "key?") ||
           sp_streq(name, "has_key?") || sp_streq(name, "include?") || sp_streq(name, "member?") ||
           sp_streq(name, "delete") || sp_streq(name, "store"))) {
        TyKind kt0 = infer_type(c, argv[0]);
        if (kt0 == TY_SYMBOL || kt0 == TY_INT || kt0 == TY_FLOAT ||
            kt0 == TY_BOOL || kt0 == TY_NIL) return TY_POLY;
      }
      /* wrong arity raises ArgumentError; poly likewise */
      if (sp_streq(name, "[]") && argc != 1) return TY_POLY;
      if (sp_streq(name, "fetch") && (argc == 0 || argc > 2)) return TY_POLY;
      if (sp_streq(name, "delete")) return TY_STRING;   /* nullable string: NULL is nil */
      if (sp_streq(name, "store") || sp_streq(name, "[]=")) return TY_STRING;
      if (sp_streq(name, "dup") || sp_streq(name, "clone") || sp_streq(name, "freeze")) return TY_POLY;
    }
  }

  { TyKind r; if (infer_enum_chain_call(c, id, nt, name, recv, args, argc, argv, rt, &r)) return r; }

  /* array receiver methods */
  /* Array receivers: the array face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_array_call(c, id, rt, &rr)) return rr; }

  { TyKind r; if (infer_exception_call(c, id, nt, name, recv, argc, rt, &r)) return r; }

  { TyKind r; if (infer_poly_operand_call(c, id, nt, name, recv, argc, argv, rt, a0, &r)) return r; }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    {
      const BuiltinOp *op = an_bop_find(c, id, rt, name, argc, nt_ref(nt, id, "block") >= 0);
      if (op && op->result != TY_UNKNOWN) return op->result;
    }
    /* Symbol#<=> is defined only between Symbols; String included, any other
       operand is not comparable and the result is nil (#3081) */
    if (sp_streq(name, "<=>") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_SYMBOL) return TY_INT;
      if (at != TY_POLY && at != TY_UNKNOWN) return TY_NIL;
    }
    /* casecmp/casecmp? against a non-symbol operand answer nil */
    if (sp_streq(name, "casecmp") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_INT : TY_NIL;
    if (sp_streq(name, "casecmp?") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_BOOL : TY_NIL;
  }

  { TyKind r; if (infer_range_lazy_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  /* hash receiver methods */
  if (recv >= 0 && sp_streq(name, "default") && argc <= 1 &&
      nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "HashNode") ||
                             sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
    return TY_POLY; /* {}.default -> nil (poly nil) */
  }
  /* a literal (or single-assignment variable) Hash.new(d) receiver that
     never narrowed: .default and [] both yield the default (no write can
     have reached it). The variable form widens to poly, hence both gates. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      ((sp_streq(name, "default") && argc <= 1) ||
       (sp_streq(name, "[]") && argc == 1))) {
    int dn = hash_new_default_arg(c, recv);
    if (dn >= 0) return infer_type(c, dn);
  }
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      sp_streq(name, "values_at") && argc >= 1 &&
      hash_new_default_arg(c, recv) >= 0) return TY_POLY_ARRAY;  /* (#2408) */
  /* h.default = v as a value: the assigned value (works for the untyped {}
     literal receiver too) */
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && ty_is_hash(rt))
    return infer_type(c, argv[0]);
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && rt == TY_UNKNOWN &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")))
    return infer_type(c, argv[0]);
  /* fetch(key, default) on an unknown/empty hash: return the default type */
  if (recv >= 0 && sp_streq(name, "fetch") && argc >= 2 && !ty_is_hash(rt)) {
    TyKind dt = infer_type(c, argv[1]);
    if (dt != TY_UNKNOWN) return dt;
  }
  /* Hash receivers: the hash face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_hash_call(c, id, rt, &rr)) return rr; }

  /* <str>.encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  { TyKind r; if (infer_string_recv_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }
  /* <int_array>.product(<int_array>)[.to_a].inspect -> a string */
  if (sp_streq(name, "inspect") && argc == 0 && recv >= 0) {
    int pr = recv;
    if (nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") &&
        nt_str(nt, pr, "name") && sp_streq(nt_str(nt, pr, "name"), "to_a"))
      pr = nt_ref(nt, pr, "receiver");
    if (pr >= 0 && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") && nt_str(nt, pr, "name") &&
        (sp_streq(nt_str(nt, pr, "name"), "product") || sp_streq(nt_str(nt, pr, "name"), "slice_before") ||
         sp_streq(nt_str(nt, pr, "name"), "slice_after") || sp_streq(nt_str(nt, pr, "name"), "slice_when") ||
         sp_streq(nt_str(nt, pr, "name"), "chunk")))
      return TY_STRING;
  }

  { TyKind r; if (infer_int_float_recv_call(c, id, nt, name, recv, argc, argv, rt, &r)) return r; }

  { TyKind r; if (infer_universal_call(c, id, nt, name, recv, argc, argv, rt, a0, &r)) return r; }

  { TyKind r; if (infer_last_resort_call(c, id, nt, name, recv, argc, rt, &r)) return r; }

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

/* A branch whose last statement is a bare raise / throw / exit never produces
   a value, so unifying its type into the surrounding case or if widens the
   result for nothing: `case o when Integer then 7 when String then 12 else
   raise end` is an Integer, not a poly. The BeginNode arm has applied the same
   rule to a diverging body since #2739; this generalizes it to the branch
   forms. */
static int stmts_diverge(Compiler *c, int st) {
  const NodeTable *nt = c->nt;
  if (st < 0) return 0;
  int n = 0; const int *b = nt_arr(nt, st, "body", &n);
  if (n <= 0 || !b) return 0;
  int last = b[n - 1];
  if (nt_kind(nt, last) != NK_CallNode || nt_ref(nt, last, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, last, "name");
  return nm && is_diverging_call(nm);
}
/* Whether call `w`, named `wn`, can run a method the program defines: a
   receiverless call (or one on self) when some user method has the name; a
   user object whose class chain defines it; a class or module whose chain
   has it as a class method; or a receiver the analysis cannot pin (poly,
   unknown), whose dispatch may land on one. A builtin-typed receiver (an
   Array's push, a String's +) runs the builtin, whatever else in the program
   shares the name. The receiver's type is the one recorded so far, not
   inferred afresh: this is asked while a yield is being typed, and inferring
   a receiver that depends on that yield recursed until the stack ran out. */
static int call_may_reach_user_method(Compiler *c, int w, const char *wn) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, w, "receiver");
  if (r < 0 || nt_kind(nt, r) == NK_SelfNode) return an_user_defines_method(c, wn);
  NodeKind rk = nt_kind(nt, r);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = nt_str(nt, r, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) return comp_cmethod_in_chain(c, ci, wn, NULL) >= 0;
  }
  TyKind rt = comp_ntype(c, r);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), wn, NULL) >= 0;
  if (rt == TY_POLY || rt == TY_UNKNOWN) return an_user_defines_method(c, wn);
  return 0;
}

/* Whether `v` answers the value of `node` itself: `v` is `node`, or a
   parenthesized, conditional, `&&`/`||` or statement-list expression one of
   whose value arms is. */
static int value_arm_is(const NodeTable *nt, int v, int node) {
  if (v < 0) return 0;
  if (v == node) return 1;
  switch (nt_kind(nt, v)) {
  case NK_StatementsNode: {
    int n = 0; const int *s = nt_arr(nt, v, "body", &n);
    return n > 0 && value_arm_is(nt, s[n - 1], node);
  }
  case NK_ParenthesesNode: return value_arm_is(nt, nt_ref(nt, v, "body"), node);
  case NK_ElseNode: return value_arm_is(nt, nt_ref(nt, v, "statements"), node);
  case NK_IfNode:
    return value_arm_is(nt, nt_ref(nt, v, "statements"), node) ||
           value_arm_is(nt, nt_ref(nt, v, "subsequent"), node);
  case NK_UnlessNode:
    return value_arm_is(nt, nt_ref(nt, v, "statements"), node) ||
           value_arm_is(nt, nt_ref(nt, v, "else_clause"), node);
  case NK_AndNode: case NK_OrNode:
    return value_arm_is(nt, nt_ref(nt, v, "left"), node) ||
           value_arm_is(nt, nt_ref(nt, v, "right"), node);
  default: return 0;
  }
}

/* The places a yield's value can settle a type from the first call site
   alone (see the YieldNode arm of infer_uncached): a variable write, an
   array literal's element, an argument, an operator's receiver and the
   last statement of a rescue or ensure frame. Each diverging yield walked
   the whole table for each of them, every round, and #6185's argument walk
   asked call_may_reach_user_method of every call on the way, so the
   typing grew with yields times nodes. One pass per round lists, for each
   yield, the nodes that hold it in one of those places, through the value
   arms value_arm_is follows; the arm then checks only those. The list is
   rebuilt per fixpoint iteration and when the table grows, like the
   receiver set above, and each node on it is checked as the walk did, so
   one that no longer holds the yield answers no. */
enum { YU_WRITE, YU_ELEMENT, YU_ARGUMENT, YU_RECEIVER, YU_FRAME, YU_BLOCK };
static const NodeKind yu_write_kinds[] = {
  NK_LocalVariableWriteNode, NK_LocalVariableOperatorWriteNode,
  NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode,
  NK_InstanceVariableWriteNode, NK_InstanceVariableOperatorWriteNode,
  NK_InstanceVariableOrWriteNode, NK_InstanceVariableAndWriteNode,
  NK_GlobalVariableWriteNode, NK_GlobalVariableOperatorWriteNode,
  NK_GlobalVariableOrWriteNode, NK_GlobalVariableAndWriteNode,
  NK_ClassVariableWriteNode, NK_ClassVariableOperatorWriteNode,
  NK_ClassVariableOrWriteNode, NK_ClassVariableAndWriteNode };
static int *g_yu_head, *g_yu_next, *g_yu_node;
static unsigned char *g_yu_kind;
static int g_yu_n, g_yu_cap, g_yu_cnt = -1;
static unsigned g_yu_gen;
static const NodeTable *g_yu_nt;
static void yu_add(int y, int w, int kind) {
  if (g_yu_n == g_yu_cap) {
    g_yu_cap = g_yu_cap ? g_yu_cap * 2 : 256;
    g_yu_next = realloc(g_yu_next, sizeof(int) * (size_t)g_yu_cap);
    g_yu_node = realloc(g_yu_node, sizeof(int) * (size_t)g_yu_cap);
    g_yu_kind = realloc(g_yu_kind, (size_t)g_yu_cap);
    if (!g_yu_next || !g_yu_node || !g_yu_kind) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  g_yu_node[g_yu_n] = w; g_yu_kind[g_yu_n] = (unsigned char)kind;
  g_yu_next[g_yu_n] = g_yu_head[y]; g_yu_head[y] = g_yu_n++;
}
/* Lists `w` for each yield y that value_arm_is(nt, v, y) answers yes for,
   down the same arms. */
static void yu_collect(const NodeTable *nt, int v, int w, int kind) {
  if (v < 0 || v >= nt->count) return;
  switch (nt_kind(nt, v)) {
  case NK_YieldNode: yu_add(v, w, kind); return;
  case NK_StatementsNode: {
    int n = 0; const int *st = nt_arr(nt, v, "body", &n);
    if (n > 0) yu_collect(nt, st[n - 1], w, kind);
    return;
  }
  case NK_ParenthesesNode: yu_collect(nt, nt_ref(nt, v, "body"), w, kind); return;
  case NK_ElseNode: yu_collect(nt, nt_ref(nt, v, "statements"), w, kind); return;
  case NK_IfNode:
    yu_collect(nt, nt_ref(nt, v, "statements"), w, kind);
    yu_collect(nt, nt_ref(nt, v, "subsequent"), w, kind);
    return;
  case NK_UnlessNode:
    yu_collect(nt, nt_ref(nt, v, "statements"), w, kind);
    yu_collect(nt, nt_ref(nt, v, "else_clause"), w, kind);
    return;
  case NK_AndNode: case NK_OrNode:
    yu_collect(nt, nt_ref(nt, v, "left"), w, kind);
    yu_collect(nt, nt_ref(nt, v, "right"), w, kind);
    return;
  default: return;
  }
}
/* The first entry for yield `y` (follow g_yu_next), -1 for none. */
static int yield_uses(Compiler *c, int y) {
  const NodeTable *nt = c->nt;
  if (g_yu_gen != g_narrow_gen || g_yu_nt != nt || g_yu_cnt != nt->count) {
    free(g_yu_head);
    g_yu_head = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    if (!g_yu_head) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < nt->count; i++) g_yu_head[i] = -1;
    g_yu_n = 0;
    for (int wk = 0; wk < (int)(sizeof yu_write_kinds / sizeof yu_write_kinds[0]); wk++)
      NT_FOREACH_KIND(nt, yu_write_kinds[wk], w) yu_collect(nt, nt_ref(nt, w, "value"), w, YU_WRITE);
    NT_FOREACH_KIND(nt, NK_ArrayNode, w) {
      int en = 0; const int *ev = nt_arr(nt, w, "elements", &en);
      for (int e = 0; e < en; e++) yu_collect(nt, ev[e], w, YU_ELEMENT);
    }
    NT_FOREACH_KIND(nt, NK_CallNode, w) {
      int r = nt_ref(nt, w, "receiver");
      if (r >= 0 && r < nt->count && nt_kind(nt, r) == NK_YieldNode) yu_add(r, w, YU_RECEIVER);
      int bk = nt_ref(nt, w, "block");
      if (bk >= 0 && bk < nt->count && nt_kind(nt, bk) == NK_BlockNode)
        yu_collect(nt, nt_ref(nt, bk, "body"), w, YU_BLOCK);
      int an = nt_ref(nt, w, "arguments");
      if (an < 0) continue;
      int ac = 0; const int *av = nt_arr(nt, an, "arguments", &ac);
      for (int e = 0; e < ac; e++) yu_collect(nt, av[e], w, YU_ARGUMENT);
    }
    NT_FOREACH_KIND(nt, NK_BeginNode, w) {
      int st = nt_ref(nt, w, "statements");
      if (st < 0) continue;
      int bn = 0; const int *bs = nt_arr(nt, st, "body", &bn);
      if (bs && bn > 0 && bs[bn - 1] >= 0 && bs[bn - 1] < nt->count &&
          nt_kind(nt, bs[bn - 1]) == NK_YieldNode) yu_add(bs[bn - 1], w, YU_FRAME);
    }
    g_yu_gen = g_narrow_gen; g_yu_nt = nt; g_yu_cnt = nt->count;
  }
  return y >= 0 && y < g_yu_cnt ? g_yu_head[y] : -1;
}

/* Whether a branch node produces no value: a statement list that ends in a
   raise, or an `elsif` chain EVERY arm of which does -- including the arm
   that is not written, so a chain without an `else` never diverges as a
   whole, its fall-through being the implicit nil. Judging the chain by its
   first arm's statements alone typed `if a then 1 elsif b then raise end` as
   the Integer of the `if` arm, and the fall-through emitted a boxed nil
   into that Integer's slot (#4464). */
static int branch_diverges(Compiler *c, int b) {
  if (b < 0) return 0;
  NodeKind k = nt_kind(c->nt, b);
  if (k == NK_ElseNode) return stmts_diverge(c, nt_ref(c->nt, b, "statements"));
  if (k == NK_IfNode || k == NK_UnlessNode) {
    int sub = nt_ref(c->nt, b, k == NK_UnlessNode ? "else_clause" : "subsequent");
    return stmts_diverge(c, nt_ref(c->nt, b, "statements")) && branch_diverges(c, sub);
  }
  return k == NK_StatementsNode ? stmts_diverge(c, b) : 0;
}

/* `Array.new` answers 1 and `Hash.new` (bare, with a default or a capacity,
   or with a default block) 2: like the empty literals they start with no element type
   of their own, and infer to TY_UNKNOWN until their use settles one. */
static int an_empty_container_new(Compiler *c, int b) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, b, "name");
  int r = nt_ref(nt, b, "receiver");
  if (!nm || !sp_streq(nm, "new") || r < 0 || nt_kind(nt, r) != NK_ConstantReadNode) return 0;
  const char *rn = nt_str(nt, r, "name");
  int a = nt_ref(nt, b, "arguments");
  int an = 0; if (a >= 0) nt_arr(nt, a, "arguments", &an);
  if (rn && sp_streq(rn, "Array"))
    return an == 0 && nt_ref(nt, b, "block") < 0 ? 1 : 0;
  if (!rn || !sp_streq(rn, "Hash")) return 0;
  return an <= 1 ? 2 : 0;
}

/* An empty `[]` / `{}` carries no element type of its own, so it caches
   TY_UNKNOWN, and unifying an arm that ends in one DROPS it: the branch then
   answers whatever the other arms said, and the literal's construction is
   emitted into that slot. `if true then [] else 1 end` typed Integer and the
   C compiler refused the program; `... else [] end` typed nil and answered
   nil where CRuby answers []. Answer the container the literal is instead,
   and let the unify see it. TY_UNKNOWN here means "not an empty literal
   tail", which leaves the caller's own answer alone. */
static TyKind an_empty_container_tail(Compiler *c, int stmts) {
  if (stmts < 0) return TY_UNKNOWN;
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = nt_arr(nt, stmts, "body", &n);
  int tail = (bb && n > 0) ? bb[n - 1] : stmts;
  const char *ty = nt_type(nt, tail);
  int len = 0;
  if (ty && sp_streq(ty, "ArrayNode")) {
    nt_arr(nt, tail, "elements", &len);
    if (len == 0) return TY_POLY_ARRAY;
  }
  else if (ty && sp_streq(ty, "HashNode")) {
    nt_arr(nt, tail, "elements", &len);
    if (len == 0) return TY_STR_POLY_HASH;
  }
  else if (ty && sp_streq(ty, "CallNode")) {
    int k = an_empty_container_new(c, tail);
    if (k) return k == 1 ? TY_POLY_ARRAY : TY_STR_POLY_HASH;
  }
  return TY_UNKNOWN;
}

/* infer_type for one branch arm, with the empty-literal tail above given its
   container type. */
static TyKind an_branch_ty(Compiler *c, int stmts) {
  TyKind t = stmts >= 0 ? infer_type(c, stmts) : TY_NIL;
  if (t == TY_UNKNOWN) {
    TyKind e = an_empty_container_tail(c, stmts);
    if (e != TY_UNKNOWN) return e;
  }
  return t;
}

/* 1 when a value (a branch arm, a write's right side, a returned value) is
   an empty `[]` literal or `Array.new`, 2 for an empty `{}` or `Hash.new`. */
int an_empty_container_kind(Compiler *c, int b) {
  const NodeTable *nt = c->nt;
  while (b >= 0) {
    NodeKind k = nt_kind(nt, b);
    if (k == NK_ElseNode || k == NK_ParenthesesNode) { b = nt_ref(nt, b, k == NK_ElseNode ? "statements" : "body"); continue; }
    if (k == NK_StatementsNode) {
      int n = 0; const int *ids = nt_arr(nt, b, "body", &n);
      if (n <= 0) return 0;
      b = ids[n - 1];
      continue;
    }
    int n = 0;
    if (k == NK_ArrayNode) { nt_arr(nt, b, "elements", &n); return n == 0 ? 1 : 0; }
    if (k == NK_HashNode) { nt_arr(nt, b, "elements", &n); return n == 0 ? 2 : 0; }
    return k == NK_CallNode ? an_empty_container_new(c, b) : 0;
  }
  return 0;
}
/* `x || {}` / `x || Hash.new` with a nil (or not yet typed) left: the nil-guard
   fallback, whose value is the empty hash on the right. That producer, else
   -1. */
int an_or_empty_hash_fallback(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_OrNode) return -1;
  int r = nt_ref(nt, node, "right");
  if (r < 0 || nt_kind(nt, r) == NK_ArrayNode || !node_is_empty_container(nt, r)) return -1;
  if (nt_kind(nt, r) == NK_CallNode && an_empty_container_new(c, r) != 2) return -1;
  TyKind lt = infer_type(c, nt_ref(nt, node, "left"));
  return (lt == TY_NIL || lt == TY_UNKNOWN) ? r : -1;
}

/* the other arm's (or the slot's other writes') settled type is not the
   empty literal's kind of container */
int an_empty_container_disagrees(int kind, TyKind other) {
  if (!kind || other == TY_UNKNOWN || other == TY_NIL || other == TY_VOID || other == TY_POLY) return 0;
  if (kind == 1) return !ty_is_array(other) && !ty_is_obj_array(other);
  return !ty_is_hash(other);
}

/* The value of the `super` node id in s that reaches method mi. */
static TyKind super_target_ret(Compiler *c, Scope *s, int mi, int id) {
  /* `super(x) { }`: the literal block is spliced into the yielding parent
     like a call's, so the super answers what that call would */
  int sblk = nt_ref(c->nt, id, "block");
  if (c->scopes[mi].yields && sblk >= 0 && nt_kind(c->nt, sblk) == NK_BlockNode)
    return method_call_ret(c, mi, id);
  TyKind sret = (TyKind)c->scopes[mi].ret;
  /* A yielding parent's return is whatever its yield produces, decided per
     call site, so its own `ret` stays unknown. The block reaching it is the
     one this method is called with, so take that value's type. */
  if ((sret == TY_UNKNOWN || sret == TY_VOID) && c->scopes[mi].yields) {
    int smi = (int)(s - c->scopes);
    TyKind yt = yield_value_type(c, smi);
    /* a middle link in a super chain has no call sites of its own */
    if (yt == TY_UNKNOWN || yt == TY_VOID) yt = yield_value_type_via_super(c, smi);
    if (yt != TY_UNKNOWN && yt != TY_VOID) return yt;
  }
  return sret;
}

/* A ConstantPathNode (A::B) (infer_uncached's arms, in their order) */
static int infer_constant_path(Compiler *c, int id, const NodeTable *nt, NodeKind nk, TyKind *out) {
  if (!(nk == NK_ConstantPathNode)) return 0;
  /* M::CONST -> resolve by the final path component (constants register
     under their unqualified name) */
  const char *nm = nt_str(nt, id, "name");
  /* An ffi_const is parent-qualified; resolve it BEFORE the leaf-keyed
     plain-constant table, or a same-leaf plain constant in another module
     silently claims the reference (and its type). */
  {
    int fpar = nt_ref(nt, id, "parent");
    const char *fpty = fpar >= 0 ? nt_type(nt, fpar) : NULL;
    /* the qualifying module is the parent's LEAF name, so a nested path
       (Outer::CSql::TEXT) qualifies by CSql -- the same unqualified name
       the ffi decl registered under */
    const char *fpnm = (fpty && (sp_streq(fpty, "ConstantReadNode") ||
                                 sp_streq(fpty, "ConstantPathNode")))
                       ? nt_str(nt, fpar, "name") : NULL;
    if (fpnm && nm)
      for (int fci = 0; fci < c->n_ffi_consts; fci++)
        if (sp_streq(c->ffi_consts[fci].mod, fpnm) &&
            sp_streq(c->ffi_consts[fci].name, nm))
          { *out = TY_INT; return 1; }
  }
  /* `klass::CONST` on a class VALUE: the constant is whichever the runtime
     class owns, so the type is the one they agree on -- not the leaf-named
     constant's, which is a different constant entirely (#4257). Same
     candidate search the emitter makes. */
  {
    int dpar = nt_ref(nt, id, "parent");
    const char *dpty = dpar >= 0 ? nt_type(nt, dpar) : NULL;
    int dyn = dpar >= 0 && !(dpty && (sp_streq(dpty, "ConstantReadNode") ||
                                      sp_streq(dpty, "ConstantPathNode")));
    /* ...and a constant that HOLDS a class rather than naming one is a
       dynamic receiver too (#4259) */
    if (!dyn && dpar >= 0 && dpty && sp_streq(dpty, "ConstantReadNode")) {
      const char *pn = nt_str(nt, dpar, "name");
      if (pn && comp_class_index(c, pn) < 0 && !is_builtin_class_name(pn) &&
          comp_const(c, pn)) dyn = 1;
    }
    TyKind prt = dyn ? infer_type(c, dpar) : TY_UNKNOWN;
    if (nm && dyn && (prt == TY_CLASS || prt == TY_POLY)) {
      TyKind ct = TY_UNKNOWN; int nc = 0, uniform = 1;
      for (int k = 0; k < c->nclasses; k++) {
        const char *kn = c->classes[k].c_name;
        if (!kn) continue;
        char tail[512];
        snprintf(tail, sizeof tail, "%s__%s", kn, nm);
        size_t tl = strlen(tail);
        for (int ci2 = 0; ci2 < c->nconsts; ci2++) {
          const char *cn2 = c->consts[ci2].name;
          size_t l2 = strlen(cn2);
          if (l2 < tl || strcmp(cn2 + l2 - tl, tail) != 0) continue;
          if (l2 > tl && strncmp(cn2 + l2 - tl - 2, "__", 2) != 0) continue;
          if (nc == 0) ct = c->consts[ci2].type;
          else if (c->consts[ci2].type != ct) uniform = 0;
          nc++;
          break;
        }
      }
      if (nc > 0 && uniform && ct != TY_UNKNOWN) { *out = ct; return 1; }
    }
  }
  LocalVar *lv = nm ? comp_const(c, nm) : NULL;
  /* Same guard the bare ConstantReadNode carries: a registered constant
     whose type never settled (Block = Struct.new(:kind), which registers
     the name before the anonymous class exists) must not shadow the
     class-table fallback below. Without it `Probe::Block` read as a value
     is TY_UNKNOWN, the slot is sp_RbVal, and boxing an unknown kind takes
     the nil tail -- so the class id is emitted and then discarded by a
     comma expression. The bare form was already guarded; only the
     qualified path was not (#4271). */
  if (lv && lv->type != TY_UNKNOWN) { *out = lv->type; return 1; }
  /* A top-level scoped constant `::Name` (no parent) names the same thing as
     the bare constant `Name`; resolve it as a class when it is one so is_a?,
     case/when, etc. treat `::Integer` exactly like `Integer` (#2683). */
  if (nm && nt_ref(nt, id, "parent") < 0 &&
      (comp_class_index(c, nm) >= 0 || is_builtin_class_name(nm) ||
       is_builtin_exception_name(nm)))
    { *out = TY_CLASS; return 1; }
  if (nm && sp_streq(nm, "ARGV")) { *out = TY_STR_ARRAY; return 1; }
  if (nm && sp_streq(nm, "ARGF")) { *out = TY_ARGF; return 1; }
  /* well-known module constants */
  int par_id = nt_ref(nt, id, "parent");
  /* a ::-scoped BUILTIN class (Math::DomainError, Process::Status) is a
     first-class Class value like its bare siblings (#2840) */
  {
    int qpar = par_id;
    const char *qpty = qpar >= 0 ? nt_type(nt, qpar) : NULL;
    const char *qpnm = (qpty && (sp_streq(qpty, "ConstantReadNode") ||
                                 sp_streq(qpty, "ConstantPathNode")))
                       ? nt_str(nt, qpar, "name") : NULL;
    if (qpnm && nm) {
      char qbuf[160];
      snprintf(qbuf, sizeof qbuf, "%s::%s", qpnm, nm);
      /* id-backed (Math::DomainError) or name-backed: the Errno:: family
         is open, so any Errno::X is an exception Class value */
      if (builtin_class_id(qbuf) != 0 || is_builtin_exception_name(qbuf)) { *out = TY_CLASS; return 1; }
    }
    /* Errno::ENOENT::Errno: the number of the class, read at run time
       since the numbers differ by platform (#4560) */
    if (nm && sp_streq(nm, "Errno") && qpnm && qpty && sp_streq(qpty, "ConstantPathNode")) {
      int gp = nt_ref(nt, qpar, "parent");
      const char *gpty = gp >= 0 ? nt_type(nt, gp) : NULL;
      const char *gpnm = gpty && sp_streq(gpty, "ConstantReadNode") ? nt_str(nt, gp, "name") : NULL;
      char eq[160];
      snprintf(eq, sizeof eq, "Errno::%s", qpnm);
      if (gpnm && sp_streq(gpnm, "Errno") && is_builtin_exception_name(eq)) { *out = TY_INT; return 1; }
    }
  }
  const char *par_ty = par_id >= 0 ? nt_type(nt, par_id) : NULL;
  /* the qualifying module is the parent's leaf name, so a nested / root
     path (`::Float::MAX`) qualifies by `Float` too -- match codegen's
     par_nmc, which already accepts a ConstantPathNode parent here */
  const char *par_nm = (par_ty && (sp_streq(par_ty, "ConstantReadNode") ||
                                   sp_streq(par_ty, "ConstantPathNode")))
                       ? nt_str(nt, par_id, "name") : NULL;
  if (par_nm && sp_streq(par_nm, "Float")) {
    if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN") || sp_streq(nm, "EPSILON") ||
               sp_streq(nm, "INFINITY") || sp_streq(nm, "NAN"))) { *out = TY_FLOAT; return 1; }
    /* DIG/MANT_DIG/RADIX and the exponent limits are Integer constants */
    if (nm && (sp_streq(nm, "DIG") || sp_streq(nm, "MANT_DIG") || sp_streq(nm, "RADIX") ||
               sp_streq(nm, "MAX_EXP") || sp_streq(nm, "MIN_EXP") ||
               sp_streq(nm, "MAX_10_EXP") || sp_streq(nm, "MIN_10_EXP"))) { *out = TY_INT; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "Math")) {
    if (nm && (sp_streq(nm, "PI") || sp_streq(nm, "E"))) { *out = TY_FLOAT; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "Regexp")) {
    if (nm && (sp_streq(nm, "IGNORECASE") || sp_streq(nm, "EXTENDED") ||
               sp_streq(nm, "MULTILINE") || sp_streq(nm, "FIXEDENCODING") ||
               sp_streq(nm, "NOENCODING"))) { *out = TY_INT; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "Encoding") && nm) {
    /* every ALL_CAPS Encoding constant is a boxed Encoding value: the two
       the runtime transcodes and every other name it only carries (see the
       codegen arm) */
    int caps = 1;
    for (const char *q = nm; *q; q++)
      if (!((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) { caps = 0; break; }
    if (caps && *nm) { *out = TY_POLY; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "File")) {
    if (nm && (sp_streq(nm, "SEPARATOR") || sp_streq(nm, "PATH_SEPARATOR") ||
               sp_streq(nm, "ALT_SEPARATOR") || sp_streq(nm, "NULL"))) { *out = TY_STRING; return 1; }
    if (nm && (sp_streq(nm, "RDONLY") || sp_streq(nm, "WRONLY") || sp_streq(nm, "RDWR") ||
               sp_streq(nm, "CREAT") || sp_streq(nm, "EXCL") || sp_streq(nm, "TRUNC") ||
               sp_streq(nm, "APPEND") || sp_streq(nm, "NONBLOCK") || sp_streq(nm, "BINARY") ||
               sp_streq(nm, "LOCK_SH") || sp_streq(nm, "LOCK_EX") || sp_streq(nm, "LOCK_UN") ||
               sp_streq(nm, "LOCK_NB")))
      { *out = TY_INT; return 1; }   /* the open(2)/flock(2) flag constants (#2788, #2808) */
  }
  if (par_nm && (sp_streq(par_nm, "IO") || sp_streq(par_nm, "File"))) {
    /* IO#seek whence constants (File inherits them from IO) */
    if (nm && (sp_streq(nm, "SEEK_SET") || sp_streq(nm, "SEEK_CUR") ||
               sp_streq(nm, "SEEK_END"))) { *out = TY_INT; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "Process")) {
    /* clock ids (codegen emits their integer values) */
    if (nm && (sp_streq(nm, "CLOCK_MONOTONIC") || sp_streq(nm, "CLOCK_REALTIME") ||
               sp_streq(nm, "CLOCK_PROCESS_CPUTIME_ID") || sp_streq(nm, "CLOCK_THREAD_CPUTIME_ID") ||
               sp_streq(nm, "PRIO_PROCESS") || sp_streq(nm, "PRIO_PGRP") || sp_streq(nm, "PRIO_USER")))
      { *out = TY_INT; return 1; }
  }
  if (par_nm && sp_streq(par_nm, "Integer")) {
    if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) { *out = TY_UNKNOWN; return 1; } /* raises NameError */
  }
  /* Socket::<CONST> is an Integer flag; the runtime resolves its value, so
     an unknown name raises NameError there rather than typing here. */
  if (par_nm && sp_streq(par_nm, "Socket") && sp_feature_required("socket") && nm)
    { *out = TY_INT; return 1; }
  if (nm && comp_class_index(c, nm) >= 0) { *out = TY_CLASS; return 1; }
  if (nm && is_builtin_class_name(nm)) { *out = TY_CLASS; return 1; }
  /* a builtin class named by its path (Enumerator::Chain), a Class value
     carried by that name */
  if (par_nm && nm) {
    char qn[256]; snprintf(qn, sizeof qn, "%s::%s", par_nm, nm);
    if (is_builtin_class_name(qn) || is_builtin_module_name(qn)) { *out = TY_CLASS; return 1; }
  }
  /* an exception class with no cls_id of its own (SystemCallError,
     LoadError) is still a first-class Class value, carried by name */
  if (nm && is_builtin_exception_name(nm)) { *out = TY_CLASS; return 1; }
  /* FFI const: Module::NAME -> int */
  if (par_nm && nm) {
    for (int fci = 0; fci < c->n_ffi_consts; fci++) {
      if (sp_streq(c->ffi_consts[fci].mod, par_nm) &&
          sp_streq(c->ffi_consts[fci].name, nm))
        { *out = TY_INT; return 1; }
    }
  }
  { *out = TY_UNKNOWN; return 1; }
  return 0;
}

/* A YieldNode: the block's value at its call sites (infer_uncached's arms, in their order) */
static int infer_yield_node(Compiler *c, int id, const NodeTable *nt, NodeKind nk, TyKind *out) {
  if (!(nk == NK_YieldNode)) return 0;
  int ymi = (int)(comp_scope_of(c, id) - c->scopes);
  /* In a proc form the block is a real proc, so the yield is a call on it:
     poly, uniformly, whatever any individual call site's block answers. That
     is the whole point of the clone -- everything the yield feeds widens with
     it, so one body serves every site (#3399). */
  if (getenv("SP_DBG_PF2")) fprintf(stderr, "[y] node=%d scope=%d pf=%d name=%s\n", id, ymi, (ymi>=0&&ymi<c->nscopes)?c->scopes[ymi].is_proc_form:-1, (ymi>=0&&ymi<c->nscopes&&c->scopes[ymi].name)?c->scopes[ymi].name:"?");
  if (ymi >= 0 && ymi < c->nscopes && c->scopes[ymi].is_proc_form) { *out = TY_POLY; return 1; }
  /* A lowered method's block is a proc as well, and its yield is a call
     on it: poly. Without this a lowered method no site ever gave a block
     (its `if block_given?` arm is dead at every site, but compiled) had an
     UNKNOWN yield in a condition, which the emitter refused. */
  if (ymi >= 0 && ymi < c->nscopes && c->scopes[ymi].is_lowered_yield) { *out = TY_POLY; return 1; }
  /* When the block value diverges across call sites (string block at one,
     int at another) AND this yield is the value of an assignment (its result
     flows into a LOCAL), the local settles its type from the first site and
     the other site miscompiles into that slot. Type the yield poly so the
     local is a boxed carrier and each inlined site boxes its own value. A
     bare-yield tail is handled per-site by emit_block_invoke_coerced /
     method_call_ret and must keep its concrete first-site type. */
  if (yield_value_diverges(c, ymi)) {
    for (int u = yield_uses(c, id); u >= 0; u = g_yu_next[u]) {
      int w = g_yu_node[u];
      switch (g_yu_kind[u]) {
      case YU_WRITE:
        /* An instance, global or class variable written from the yield, or
           from a conditional one of whose arms is the yield (`@y =
           block_given? ? yield(x) : "nil"`), is in the same position as a
           local: the variable takes one type, the first site's, and the
           other site's value was emitted into it (`@y = yield(x)` with a
           String block, then an Integer one, stopped the build). */
        if (value_arm_is(nt, nt_ref(nt, w, "value"), id)) { *out = TY_POLY; return 1; }
        break;
      case YU_ELEMENT: {
        /* An array literal's element likewise: `[yield(x)]` built its
           array from the first site's element type. */
        int en = 0; const int *ev = nt_arr(nt, w, "elements", &en);
        for (int e = 0; e < en; e++)
          if (value_arm_is(nt, ev[e], id)) { *out = TY_POLY; return 1; }
        break;
      }
      case YU_ARGUMENT: {
        /* An argument to a method the program defines likewise: its
           parameter took the first site's type, and the other site's value
           was converted to it at run time. `show(yield)` with a String
           block at one site and a Float block at another raised TypeError
           where CRuby prints both. Only a call that can reach a user
           method (call_may_reach_user_method): a builtin on the yield
           (`yield + yield`, an Array's `push(yield)`) is lowered per site
           to its concrete form, which a poly operand does not fit, and an
           unrelated class defining a method of the same name does not
           change that. */
        int an = nt_ref(nt, w, "arguments");
        if (an < 0) break;
        const char *wn = nt_str(nt, w, "name");
        if (!wn || !call_may_reach_user_method(c, w, wn)) break;
        int ac = 0; const int *av = nt_arr(nt, an, "arguments", &ac);
        for (int e = 0; e < ac; e++)
          if (value_arm_is(nt, av[e], id)) { *out = TY_POLY; return 1; }
        break;
      }
      case YU_BLOCK: {
        /* The value of a block handed to a method the program defines
           likewise: that method's own yield takes it into a slot typed
           from the first site, so `def run2(x) = run(x) { |u| yield u }`
           with an Integer block at one site and a String one at another
           emitted the String into an sp_int. Poly boxes each site's. */
        const char *wn = nt_str(nt, w, "name");
        if (wn && call_may_reach_user_method(c, w, wn)) { *out = TY_POLY; return 1; }
        break;
      }
      case YU_RECEIVER: {
        /* The receiver of a builtin arithmetic operator too: `yield +
           yield` typed its `+`, and the method's return, from the first
           site's block, so a String block at one site and a Float one at
           another put the Float into a `const char *`. Poly makes the
           result a boxed carrier, and codegen types each site's operator
           from that site's block (yield_operator_site_type), so the value
           it emits is boxed into it. Only the operators that helper types:
           for another method on the yield, codegen still emits one site's
           concrete result into the slot unboxed. */
        if (nt_ref(nt, w, "receiver") != id || nt_ref(nt, w, "block") >= 0) break;
        const char *op = nt_str(nt, w, "name");
        int an = nt_ref(nt, w, "arguments"), ac = 0;
        if (an >= 0) nt_arr(nt, an, "arguments", &ac);
        if (ac == 1 && op && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") ||
                              sp_streq(op, "/") || sp_streq(op, "%")))
          { *out = TY_POLY; return 1; }
        /* A builtin whose result follows the receiver's kind (yield.abs,
           yield.first, yield.dup): the same mismatch, the slot typed from
           the first site's receiver and the other site's concrete result
           (a double from fabs, a const char * from a String Array's first)
           stored into it. yield_builtin_method_site_type types each site's
           call from ty_recv_builtin_result, so widen only where that table
           answers every site's block kind: a site it does not answer would
           emit its concrete value into the poly slot unboxed, which turned
           a wrong value into C that does not compile when this arm widened
           every method. */
        if (yield_recv_builtin_every_site(c, w)) { *out = TY_POLY; return 1; }
        break;
      }
      case YU_FRAME: {
        /* A yield whose value leaves through an ENSURE frame is in the
           same position as one written to a local, for the same reason:
           the frame carries the value in a slot of its own, and that slot
           settles its type at whichever call site is analyzed first. The
           per-site coercion the comment above relies on handles the
           method's own tail, and does not reach a tail one frame in. So
           `def run; begin; yield 7; ensure; nil; end; end` answered its
           first site's type at its second -- `p run { |x| x == 7 }` then
           `p run { |x| x * 3 }` printed true twice, where CRuby says true
           and 21, and a pair whose types do not share a C slot stopped the
           build instead. Poly makes the slot a boxed carrier, and each
           inlined site boxes its own value. A rescue frame the same: its
           value slot takes the body's value or the rescue arm's, `def
           guarded; yield; rescue; "rescued"; end`. */
        if (nt_ref(nt, w, "ensure_clause") < 0 && nt_ref(nt, w, "rescue_clause") < 0) break;
        int st = nt_ref(nt, w, "statements");
        if (st < 0) break;
        int bn = 0; const int *bs = nt_arr(nt, st, "body", &bn);
        if (bs && bn > 0 && bs[bn - 1] == id) { *out = TY_POLY; return 1; }
        break;
      }
      }
    }
  }
  { *out = yield_value_type(c, ymi); return 1; }
  return 0;
}

TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  NodeKind nk = nt_kind(nt, id);
  /* A read marked strbuf_box yields the shared sp_String* HANDLE, whatever
     the node kind: a container-stored local read, a demanded string literal
     element, or a demanded call-result store (#3227). */
  if (c->strbuf_box[id]) return TY_STRBUF;
  /* a multiple assignment's element, read back for desugar_masgn_store_evidence:
     a method returning a fixed tuple answers that element's own type (a
     negative position counts from the end) */
  if (nk == NK_CallNode) {
    long long pos = nt_int(nt, id, "masgn_elem", LLONG_MIN);
    TyKind elems[16];
    int en = pos != LLONG_MIN ? multi_return_elem_types(c, nt_ref(nt, id, "receiver"), elems, 16) : 0;
    if (pos < 0) pos += en;
    if (pos >= 0 && pos < en) return elems[pos];
    /* `x = pair[1]` reads the boxed tuple's element back at its own type */
    TyKind et = tuple_elem_read_unboxed(c, id);
    if (et != TY_UNKNOWN && infer_type(c, nt_ref(nt, id, "receiver")) == TY_POLY_ARRAY) return et;
  }

  if (nk == NK_IntegerNode)             return nt_str(nt, id, "bigval") ? TY_BIGINT : TY_INT;
  if (nk == NK_FloatNode)               return TY_FLOAT;
  if (nk == NK_ImaginaryNode)           return TY_COMPLEX;
  if (nk == NK_RationalNode)            return TY_RATIONAL;
  if (nk == NK_StringNode)              return TY_STRING;
  if (nk == NK_SourceFileNode)          return TY_STRING;
  if (nk == NK_SourceLineNode)          return TY_INT;
  if (nk == NK_SourceEncodingNode)      return TY_POLY;
  if (nk == NK_RegularExpressionNode ||
      nk == NK_InterpolatedRegularExpressionNode) return TY_REGEX;
  /* `/(?<n>..)/ =~ str` -- the value is the `=~` result (match index or nil). */
  if (sp_streq(ty, "MatchWriteNode")) return TY_POLY;
  if (nk == NK_InterpolatedStringNode)  return TY_STRING;
  if (nk == NK_XStringNode || nk == NK_InterpolatedXStringNode) return TY_STRING;
  if (nk == NK_InterpolatedSymbolNode)  return TY_SYMBOL;
  if (nk == NK_SymbolNode)              return TY_SYMBOL;
  if (nk == NK_TrueNode)                return TY_BOOL;
  if (nk == NK_FalseNode)               return TY_BOOL;
  if (nk == NK_NilNode)                 return TY_NIL;
  /* A while/until loop in value position evaluates to nil (a valued `break`
     is a separate gap); type it as poly so the slot holds a boxed nil. */
  if (nk == NK_WhileNode || nk == NK_UntilNode || nk == NK_ForNode) return TY_POLY;
  if (nk == NK_RangeNode) {
    /* (:a..:e): symbols enumerate by name succession -- the whole range
       lowers to a poly array of boxed symbols (see codegen_expr) */
    {
      int slo = nt_ref(nt, id, "left"), shi = nt_ref(nt, id, "right");
      const char *slt = slo >= 0 ? nt_type(nt, slo) : NULL;
      const char *sht = shi >= 0 ? nt_type(nt, shi) : NULL;
      if (slt && sht && sp_streq(slt, "SymbolNode") && sp_streq(sht, "SymbolNode"))
        return TY_POLY_ARRAY;
    }
    /* infer the bounds so codegen can tell an int range from a string range */
    int lo = nt_ref(nt, id, "left"), hi = nt_ref(nt, id, "right");
    TyKind lt = lo >= 0 ? infer_type(c, lo) : TY_UNKNOWN;
    TyKind ht = hi >= 0 ? infer_type(c, hi) : TY_UNKNOWN;
    /* (1.0..3.0): a BOUNDED range with both endpoints float lowers to the
       distinct sp_FloatRange type. A finite mixed int/float range (1..3.0)
       stays on the ordinary int TY_RANGE path, which serves it well (its
       #to_a, #sum and #cover? are all right there and its iteration is the
       integer one CRuby performs).
       An INFINITE bound is the exception: sp_int has no value for it, so the
       int range can only record "unbounded" and #begin / #end then answer nil
       where CRuby answers +/-Infinity. Such a range takes the float
       representation, which holds the infinity -- at the cost of reporting the
       other (finite) bound as a Float (see docs/limitations.md). #3670 */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_FLOAT)
      return TY_FLOAT_RANGE;
    /* A FLOAT begin with an Integer end (1.5..5) is a Float range too: it
       cannot be iterated from a Float in CRuby either, and the integer
       representation truncated the begin, which every reader then answered
       (#3896). The mirror case (1..5.5) keeps the integer representation --
       CRuby does iterate it -- and its end readers answer the literal. */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_INT)
      return TY_FLOAT_RANGE;
    /* A Float begin before an end only known at run time (a boxed value,
       which may be nil): (f..x). The integer representation truncated the
       begin and read a nil end as 0. Such a range is never iterated in
       CRuby, whatever the end turns out to be, so the Float representation
       loses nothing; its end decides at run time whether the range is
       endless. A boxed begin before a Float end may hold an Integer that
       CRuby does iterate from, and keeps the integer representation. */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_POLY)
      return TY_FLOAT_RANGE;
    /* A Float bound beside an absent or nil one -- (1.5..), (..2.5),
       (1.5..nil), (nil..2.5) -- is a half-open Float range. The integer
       representation truncated the Float bound: (1.5..) inspected as
       "1..9223372036854775807" and (..2.5).include?(2.2) answered false. */
    {
      int lo_open = lo < 0 || lt == TY_NIL;
      int hi_open = hi < 0 || ht == TY_NIL;
      if ((lt == TY_FLOAT && hi_open) || (ht == TY_FLOAT && lo_open))
        return TY_FLOAT_RANGE;
    }
    /* Only when the BEGIN is the infinite one: `(2..Float::INFINITY)` is the
       canonical lazy source and its integer enumeration is what the fused
       pipeline walks, so that shape keeps the int representation and reports
       its end through the literal arm instead. */
    if (lo >= 0 && hi >= 0 &&
        (lt == TY_FLOAT || lt == TY_INT) && (ht == TY_FLOAT || ht == TY_INT) &&
        infer_endpoint_is_infinite(c, lo))
      return TY_FLOAT_RANGE;
    /* A range that only matches (mark_match_ranges) compares a Float bound
       as written, which only the Float representation holds. */
    if (nt_int(nt, id, "match_only", 0) && (lt == TY_FLOAT || ht == TY_FLOAT) &&
        (lo < 0 || lt == TY_INT || lt == TY_FLOAT) && (hi < 0 || ht == TY_INT || ht == TY_FLOAT))
      return TY_FLOAT_RANGE;
    /* ("a".."e"): both endpoints strings -> the distinct sp_StrRange, so a
       range held in a variable stays a Range rather than materializing into
       its element array (#3064). An endless ("a"..) or beginless (.."e") one
       is a String range too, its missing bound a nil (NULL) endpoint. */
    if ((lo >= 0 || hi >= 0) && (lo < 0 || lt == TY_STRING) && (hi < 0 || ht == TY_STRING))
      return TY_STR_RANGE;
    return TY_RANGE;
  }
  /* A splat inside an array literal (`[*0..10]`, `[*arr]`) contributes the
     element type of the splatted collection, so the literal stays a typed
     array instead of widening to poly_array. The value returned here is the
     would-be element type, which the ArrayNode arm unifies in. */
  if (nk == NK_SplatNode) {
    int inner = nt_ref(nt, id, "expression");
    if (inner < 0) return TY_UNKNOWN;
    const char *ity = nt_type(nt, inner);
    if (ity && sp_streq(ity, "RangeNode")) {
      int lo = nt_ref(nt, inner, "left");
      return (lo >= 0 && infer_type(c, lo) == TY_STRING) ? TY_STRING : TY_INT;
    }
    TyKind it = infer_type(c, inner);
    if (ty_is_array(it)) return ty_array_elem(it);
    return TY_POLY;
  }
  if (nk == NK_LambdaNode)              return TY_PROC;
  /* an assignment expression evaluates to the assigned value -- but codegen
     lowers `x = expr` to `({ lv_x = ...; lv_x; })`, so the chain value IS the
     slot. Return the local's slot type (when known) so a chained `a = b = expr`
     boxes consistently with the slot, mirroring the ivar-write rule below. */
  if (nk == NK_LocalVariableWriteNode) {
    const char *lwn = nt_str(nt, id, "name");
    Scope *lws = comp_scope_of(c, id);
    LocalVar *lwv = lwn ? scope_local(lws, lwn) : NULL;
    if (lwv && lwv->type != TY_UNKNOWN) return lwv->type;
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_InstanceVariableWriteNode ||
      nk == NK_InstanceVariableOrWriteNode ||
      nk == NK_InstanceVariableAndWriteNode ||
      nk == NK_InstanceVariableOperatorWriteNode) {
    /* expression evaluates to the ivar slot's type (same as a read): codegen
       lowers `@a = expr` to `({ iv_a = ...; iv_a; })`, so the chain value IS the
       slot, and inference must match to keep `@x = @a = expr` boxing consistent. */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* inside an instance_eval/exec splice the block scope has no class_id; the
       ivar belongs to the rebound receiver class (an_ie_class_id). */
    int wcls = s->class_id >= 0 ? s->class_id : an_ie_class_id;
    /* a toplevel method's `@x ||= v` / `@x &&= v` / `@x += v` answers the
       Toplevel slot, which the value alone does not type (`(@a ||= []) << 1`,
       a boxed `@i += 1` read back as the slot); so does a plain write of an
       untyped value (`y = (@a = [])`, where a later write made the slot a
       poly array) */
    if (wcls < 0 && !s->is_cmethod && id < c->node_cap && c->node_cbody[id] < 0 &&
        (nk == NK_InstanceVariableOrWriteNode || nk == NK_InstanceVariableAndWriteNode ||
         nk == NK_InstanceVariableOperatorWriteNode ||
         (nk == NK_InstanceVariableWriteNode && infer_type(c, nt_ref(nt, id, "value")) == TY_UNKNOWN))) {
      int tl = comp_class_index(c, "Toplevel");
      int tiv = tl >= 0 && nm ? comp_ivar_index(&c->classes[tl], nm) : -1;
      TyKind tt = tiv >= 0 ? ivar_value_ty(&c->classes[tl], tiv) : TY_UNKNOWN;
      if (tt != TY_UNKNOWN) return tt;
    }
    if (wcls < 0) return infer_type(c, nt_ref(nt, id, "value"));
    ClassInfo *ci = &c->classes[wcls];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    /* a strbuf slot's write, like its read, is the String face (the handle
       rides _sp_ret_strbuf): the raw STRBUF typed a method whose body is the
       write `sp_String *` while its callers read the face (#4567) */
    return ivar_value_ty(ci, iv);
  }
  if (nk == NK_LocalVariableOperatorWriteNode) {
    const char *nm2 = nt_str(nt, id, "name");
    Scope *s2 = comp_scope_of(c, id);
    LocalVar *lv2 = nm2 ? scope_local(s2, nm2) : NULL;
    TyKind ct2 = lv2 ? lv2->type : TY_UNKNOWN;
    TyKind vt2 = infer_type(c, nt_ref(nt, id, "value"));
    if (ct2 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct2) && ty_is_numeric(vt2))
      return (ct2 == TY_FLOAT || vt2 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct2 != TY_UNKNOWN ? ct2 : vt2;
  }
  if (nk == NK_LocalVariableOrWriteNode || nk == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
  }

  if (nk == NK_LocalVariableReadNode) {
    /* a `return .. if p.nil?`-guarded param read: the non-nil type (#1661) */
    if (c->nilnarrow[id] != TY_UNKNOWN) return c->nilnarrow[id];
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* &block param that escapes (not yield-inlined): the LocalVar slot type is
       TY_UNKNOWN, but the value is a Proc object when the method does not inline
       the block (yields==0). Return TY_PROC so callers can type the return value. */
    if (nm && s && s->blk_param && s->blk_param[0] && sp_streq(nm, s->blk_param)
        && !s->yields)
      return TY_PROC;
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    /* an UNMARKED read of a (phase-3-promoted) mutable string demotes to the
       plain string type: ordinary consumers see the copy-read value exactly
       as before promotion (#3227) */
    if (lv) {
      TyKind lt = lv->type == TY_STRBUF ? TY_STRING : lv->type;
      /* Reset-and-re-derive leaves a local UNKNOWN until its own write is
         reached, so a read that precedes it in node order answered UNKNOWN and
         the enclosing unify dropped the arm: `v = if c then 9 else (t = s.upcase; t) end`
         typed v Integer, and the String was converted into an int slot. The
         previous round's answer is the honest one to carry here; hoisting the
         same write out of the arm already gave the right type, which is what
         made the defect depend on node order rather than on the program. */
      /* Only a CONCRETE carry. A stale poly would widen where the old
         behaviour let the other arm's concrete type stand, and that is a
         direction this must not move in: two optcarrot float locals went from
         sp_float to sp_RbVal on it. UNKNOWN is what those sites answered
         before, so declining leaves them exactly as they were. */
      if (lt == TY_UNKNOWN && g_infer_write_round && !lv->is_param &&
          !lv->is_block_param && (TyKind)lv->gc_root != TY_UNKNOWN &&
          (TyKind)lv->gc_root != TY_POLY)
        return (TyKind)lv->gc_root;
      return lt;
    }
    return TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    /* $stdin is the IO handle over the C stdin stream (gets/read/tty?/... via
       the TY_IO dispatch). $stdout/$stderr keep their dedicated AST-shape
       emission arm and stay untyped here. */
    if (nm && (sp_streq(nm, "$stdin") || sp_streq(nm, "$stdout") ||
               sp_streq(nm, "$stderr"))) return TY_IO;   /* the C stream handles (#2818) */
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && sp_streq(nm, "$/")) return TY_STRING;
    /* the Process::Status of the last child waited for, NULL (nil) before
       any; it was an Integer, and `$?.exitstatus` raised NoMethodError */
    if (nm && sp_streq(nm, "$?")) return TY_PROCESS_STATUS;
    if (nm && (sp_streq(nm, "$PROGRAM_NAME") || sp_streq(nm, "$0"))) return TY_STRING;
    if (nm && sp_streq(nm, "$!")) return TY_EXCEPTION;  /* the exception being handled, or nil (NULL) outside a rescue */
    if (nm && (sp_streq(nm, "$;") || sp_streq(nm, "$,"))) return TY_NIL;
    /* regex match globals: $~ is the last MatchData (NULL = nil); the
       text back-references are nullable strings */
    if (nm && sp_streq(nm, "$~")) return TY_MATCHDATA;
    if (nm && (sp_streq(nm, "$&") || sp_streq(nm, "$`") ||
               sp_streq(nm, "$'") || sp_streq(nm, "$+"))) return TY_STRING;
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableOperatorWriteNode) {
    /* `$g += v` evaluates to the updated value (the local/ivar op-write forms
       above already do; #1484). */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (ct == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct) && ty_is_numeric(vt))
      return (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct != TY_UNKNOWN ? ct : vt;
  }
  if (nk == NK_GlobalVariableWriteNode || nk == NK_GlobalVariableOrWriteNode || nk == NK_GlobalVariableAndWriteNode) {
    /* `$g = v` evaluates to the stored value, which codegen reads back from
       the slot -- the slot's type, as for the ivar and cvar writes above */
    /* `$g ||= v` evaluates to the slot after the guarded write */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ct != TY_UNKNOWN ? ct : infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_ConstantReadNode) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    /* a registered constant whose type never settled (e.g. an anonymous
       Struct class assignment) must not shadow the class-table fallbacks
       below -- Pt = Struct.new(:x) reads as the class value, not unknown */
    if (lv && lv->type != TY_UNKNOWN) return lv->type;
    /* `include Math` exposes bare PI/E as Float constants (#2600) */
    if (c->has_include_math && !lv && nm && (sp_streq(nm, "PI") || sp_streq(nm, "E")))
      return TY_FLOAT;
    if (nm && (sp_streq(nm, "RUBY_DESCRIPTION") || sp_streq(nm, "RUBY_VERSION") ||
               sp_streq(nm, "RUBY_PLATFORM") || sp_streq(nm, "RUBY_ENGINE") ||
               sp_streq(nm, "RUBY_ENGINE_VERSION") || sp_streq(nm, "RUBY_RELEASE_DATE") ||
               sp_streq(nm, "RUBY_REVISION") || sp_streq(nm, "RUBY_COPYRIGHT"))) return TY_STRING;
    if (nm && sp_streq(nm, "RUBY_PATCHLEVEL")) return TY_INT;
    if (nm && sp_streq(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && sp_streq(nm, "ARGF")) return TY_ARGF;
    /* STDOUT/STDERR/STDIN are IO handles wrapping the C standard streams, so
       puts/print/write/flush -- and gets/read/tty? for STDIN -- route through
       the existing TY_IO dispatch. */
    if (nm && (sp_streq(nm, "STDOUT") || sp_streq(nm, "STDERR") ||
               sp_streq(nm, "STDIN"))) return TY_IO;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* an exception class with no cls_id of its own (SystemCallError,
       LoadError) is still a first-class Class value, carried by name */
    if (nm && is_builtin_exception_name(nm)) return TY_CLASS;
    /* `OpenStruct` as a value (o.class == OpenStruct) resolves only under
       require "ostruct" (#3155). */
    if (nm && sp_streq(nm, "OpenStruct") && sp_feature_required("ostruct")) return TY_CLASS;
    return TY_UNKNOWN;
  }
  if (nk == NK_DefinedNode) return TY_STRING;  /* a label string, or nil (NULL) */
  if (nk == NK_NumberedReferenceReadNode) return TY_STRING;  /* $1..$9: capture, or nil (NULL) */
  if (nk == NK_BackReferenceReadNode) return TY_STRING;  /* $&/$`/$'/$~/$+: nullable string */
  { TyKind r; if (infer_constant_path(c, id, nt, nk, &r)) return r; }
  if (nk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    int self_cls = s->class_id;
    /* inside a class method, bare `self` is the Class object (#2443) --
       unless an instance_eval/exec block there rebinds it to an object */
    if (self_cls >= 0 && s->is_cmethod) {
      int iec = ie_class_of(c, id);
      if (iec < 0) return TY_CLASS;
      self_cls = iec;
    }
    /* `self` inside an instance_eval/exec block is the rebound receiver. */
    if (self_cls < 0) self_cls = (an_ie_class_id >= 0) ? an_ie_class_id : ie_class_of(c, id);
    /* a statement of a class or module body: the class object itself */
    if (self_cls < 0 && self_class_body(c, id) >= 0) return TY_CLASS;
    if (self_cls < 0) return TY_POLY;   /* main, a boxed Object (#4926), or a boxed receiver */
    const char *cn = c->classes[self_cls].name;
    if (sp_streq(cn, "String"))  return TY_STRING;
    if (sp_streq(cn, "Integer")) return TY_INT;
    if (sp_streq(cn, "Float"))   return TY_FLOAT;
    if (sp_streq(cn, "Symbol"))  return TY_SYMBOL;
    if (sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass") || sp_streq(cn, "NilClass")) return TY_BOOL;
    /* an Array / Hash reopen takes self boxed (sp_RbVal): the receiver may
       be any element kind. Typing it as the unboxed poly array handed
       sp_PolyArray_length an sp_RbVal (#blank.rb) */
    if (sp_streq(cn, "Array"))   return TY_POLY;
    if (sp_streq(cn, "Hash"))    return TY_POLY;
    if (sp_streq(cn, "Object"))  return TY_POLY;  /* dynamic: called on any receiver type */
    /* a Numeric reopen is called on any number -- an Integer, a Float --
       and takes self boxed, the arithmetic on it dispatching on the value */
    if (sp_streq(cn, "Numeric")) return TY_POLY;
    if (sp_streq(cn, "Range"))   return TY_RANGE;
    if (sp_streq(cn, "Time"))    return TY_TIME;
    if (sp_streq(cn, "Thread"))  return TY_THREAD;
    if (sp_streq(cn, "Fiber"))   return TY_FIBER;
    if (io_family_class(c, self_cls)) return TY_IO;   /* File, IO, the sockets: one handle type */
    if (sp_streq(cn, "Class"))   return TY_CLASS;
    if (is_builtin_exception_name(cn)) return TY_EXCEPTION;
    return ty_object(self_cls);
  }
  if (nk == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cls_id = (s->class_id >= 0) ? s->class_id : an_ie_class_id;
    if (cls_id < 0) cls_id = ie_class_of(c, id);
    /* an instance_eval/exec body in a method reads the receiver's ivar */
    if (ie_class_of(c, id) >= 0) cls_id = ie_class_of(c, id);
    /* ... and on a boxed receiver, the ivar of each class it can hold */
    if (ie_class_of(c, id) < -1 && nm) {
      int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
      TyKind pr = TY_UNKNOWN;
      for (int i = 0; i < npk; i++) {
        ClassInfo *pci = &c->classes[pk[i]];
        int piv = comp_ivar_index(pci, nm);
        if (piv < 0) continue;
        TyKind t = pci->ivar_types[piv] == TY_STRBUF ? TY_STRING : pci->ivar_types[piv];
        pr = pr == TY_UNKNOWN ? t : ty_unify(pr, t);
      }
      if (npk > 0) return pr;
    }
    if (cls_id < 0) cls_id = comp_class_index(c, "Toplevel");
    if (cls_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    if (iv < 0) {
      /* an instance_eval/exec body reads its receiver's: one the receiver's
         class never writes is nil there (emit_expr reads it so) */
      int ie = s->class_id < 0 && (an_ie_class_id >= 0 || ie_class_of(c, id) >= 0);
      for (int k = ci->parent; ie && nm && k >= 0; k = c->classes[k].parent)
        if (comp_ivar_index(&c->classes[k], nm) >= 0) ie = 0;
      return ie && nm ? TY_NIL : TY_UNKNOWN;
    }
    /* an UNMARKED read of a shared-mutable string slot demotes to the plain
       string type (copy-read), mirroring the local-read demotion (#3227) */
    if (ci->ivar_types[iv] == TY_STRBUF) return TY_STRING;
    return ci->ivar_types[iv];
  }
  if (nk == NK_ClassVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id;
    if (cid < 0 && id < c->node_cap) cid = c->node_cbody[id];
    /* a body-level read in a (reopened) class body: the body's class */
    if (cid < 0 && c->node_cbody && id < c->node_cap) cid = c->node_cbody[id];
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) return TY_UNKNOWN;
    cid = comp_cvar_owner(c, cid, nm);
    int idx = nm ? comp_cvar_index(&c->classes[cid], nm) : -1;
    return idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableOperatorWriteNode || nk == NK_ClassVariableWriteNode ||
      nk == NK_ClassVariableOrWriteNode || nk == NK_ClassVariableAndWriteNode) {
    /* `@@x op= v` / `@@x = v` / `@@x ||= v` / `@@x &&= v` write the cvar and yield
       the stored value, which the codegen coerces to the cvar's slot type (poly
       when widened under promote) -- so the expression's type is the cvar's, not
       v's. (For a non-widened cvar this equals v's type, so default mode is
       unchanged.) */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s ? s->class_id : -1;
    if (cid < 0 && c->node_cbody && id < c->node_cap) cid = c->node_cbody[id];
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    cid = comp_cvar_owner(c, cid, nm);
    int idx = (cid >= 0 && nm) ? comp_cvar_index(&c->classes[cid], nm) : -1;
    if (idx >= 0) return c->classes[cid].cvar_types[idx];
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_IndexOrWriteNode || nk == NK_IndexAndWriteNode ||
      nk == NK_IndexOperatorWriteNode) {
    /* all three yield the slot's (post-write) value, so the expression's type
       is the slot type -- op-write included (`a[i] += x` used as a value, e.g.
       the tail of a block whose proc result is consumed). */
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) return TY_UNKNOWN;
    TyKind rt = infer_type(c, recv);
    if (ty_is_array(rt)) return ty_array_elem(rt);
    if (ty_is_hash(rt)) return ty_hash_val(rt);
    return TY_POLY;
  }
  if (nk == NK_ParenthesesNode) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_NIL;
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_StatementsNode) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_CaseNode) {
    /* value = unify of each when's body; a missing else means a no-match
       falls through to nil */
    int nw = 0; const int *whens = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, whens[w], "statements");
      if (stmts_diverge(c, st)) continue;
      r = ty_unify(r, an_branch_ty(c, st));
    }
    if (else_c >= 0) {
      int st = nt_ref(nt, else_c, "statements");
      if (!stmts_diverge(c, st)) r = ty_unify(r, an_branch_ty(c, st));
    }
    else r = ty_unify(r, TY_NIL);
    return r;
  }
  if (nk == NK_CaseMatchNode) {
    /* case X; in PATTERN; ... -- value = unify of each arm's body (+ else). */
    int nw = 0; const int *conds = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, conds[w], "statements");
      if (stmts_diverge(c, st)) continue;
      r = ty_unify(r, an_branch_ty(c, st));
    }
    if (else_c >= 0) {
      int st = nt_ref(nt, else_c, "statements");
      if (!stmts_diverge(c, st)) r = ty_unify(r, an_branch_ty(c, st));
    }
    return r;
  }
  if (nk == NK_IfNode || nk == NK_UnlessNode) {
    int is_unless = nk == NK_UnlessNode;
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    /* a statically-answered defined? predicate types as its live arm alone
       (the dead arm is never emitted -- see the codegen folds) */
    int dpred = nt_ref(nt, id, "predicate");
    int df = comp_defined_guard_false(c, dpred);
    int dt = df ? 0 : comp_defined_guard_true(c, dpred);
    if (df || dt) {
      int take_then = is_unless ? df : dt;
      if (take_then) return then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
      return else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    }
    int tdiv = branch_diverges(c, then_b);
    int ediv = branch_diverges(c, else_b);
    /* both arms diverging leaves nothing to type: fall through to the plain
       unify rather than answering UNKNOWN out of nowhere */
    if (tdiv && !ediv) return an_branch_ty(c, else_b);
    if (ediv && !tdiv) return an_branch_ty(c, then_b);
    TyKind tt = an_branch_ty(c, then_b);
    TyKind et = an_branch_ty(c, else_b);
    /* An empty `[]` / `{}` arm is still UNKNOWN (its kind comes from its
       use), and the unify took the other arm's type for it: an object, a
       String, a number. It is a container all the same, so a non-container
       other arm makes the answer boxed; `if u then Rel.new else [] end` built
       an sp_IntArray into an sp_Rel slot (#4938). */
    { int tk = an_empty_container_kind(c, then_b), ek = an_empty_container_kind(c, else_b);
      if (tt == TY_UNKNOWN && tk && ek == 0 && an_empty_container_disagrees(tk, et)) return TY_POLY;
      if (et == TY_UNKNOWN && ek && tk == 0 && an_empty_container_disagrees(ek, tt)) return TY_POLY;
      /* ...and against an arm that settles on no type at all (a call nothing
         answers raises), once the rounds are no longer optimistic, the empty
         literal is the value: `if q then Parser.new.parse(q) else [] end`
         answered nil (#4938) */
      if (!g_infer_optimistic && tt == TY_UNKNOWN && et == TY_UNKNOWN && (tk != 0) != (ek != 0))
        return (tk ? tk : ek) == 1 ? TY_POLY_ARRAY : TY_POLY_POLY_HASH; }
    return ty_unify(tt, et);
  }
  if (nk == NK_ElseNode) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (nk == NK_ArrayNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* An empty `[]` consumed directly as a receiver or interpolation has no
         writes to infer from, so mark_empty_array_operands types it as a poly
         array. An argument may instead take a specific layout through arr_want.
         Elsewhere it stays UNKNOWN so `x = []; x << 1` can back-fill its kind. */
      /* A SPECIFIC kind fixed by the use context outranks the generic poly
         layout: the row of an `Array.new(n) { [] }` table is a block tail
         (marked as a receiver-style empty) and is built as an sp_FloatArray
         once narrow_object_arrays has decided the table's kind (#4484). */
      if (c->arr_want && id < c->node_cap && ty_is_array(c->arr_want[id]) &&
          c->arr_want[id] != TY_POLY_ARRAY)
        return c->arr_want[id];
      if (c->empty_arr_recv && id < c->node_cap && c->empty_arr_recv[id])
        return TY_POLY_ARRAY;
      /* kind fixed by the use context (mark_empty_array_operands) */
      if (c->arr_want && id < c->node_cap && ty_is_array(c->arr_want[id]))
        return c->arr_want[id];
      return TY_UNKNOWN;  /* empty: element type comes from usage */
    }
    /* A callee stores elements of another kind into the literal it is
       passed (widen_arg_array). */
    if (c->arr_want && id < c->node_cap && c->arr_want[id] == TY_POLY_ARRAY)
      return TY_POLY_ARRAY;
    TyKind e = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      TyKind et = infer_type(c, els[k]);
      /* A nested container literal whose own type is still open (an empty
         `[]` / `{}` element) is a non-scalar value all the same: treat it as
         poly, exactly like the HashNode arm below does for `{}`. Otherwise
         UNKNOWN unifies away and `[[], 1]` collapses to an IntArray, whose
         emit pushes the nested array POINTER as an int element (silent
         garbage). A bare `Array.new` / `Hash.new` is that same empty
         container (node_is_empty_container). */
      if (et == TY_UNKNOWN) {
        const char *ety = nt_type(nt, els[k]);
        if ((ety && (sp_streq(ety, "ArrayNode") || sp_streq(ety, "HashNode") ||
                     sp_streq(ety, "KeywordHashNode"))) ||
            node_is_empty_container(nt, els[k]))
          et = TY_POLY;
      }
      /* a nil element keeps the literal a boxed container, as it always was:
         the String join now absorbs nil (#4567), and a typed array or hash
         is not where a written nil belongs */
      if (et == TY_NIL) et = TY_POLY;
      e = ty_unify(e, et);
    }
    /* ty_array_of holds an all-unknown element type at bottom while the
       fixpoint runs; see its TY_UNKNOWN case. */
    return ty_array_of(e);
  }
  if (nk == NK_HashNode || nk == NK_KeywordHashNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* a bare `{}` used directly as a hash block-method receiver dispatches as
         the STR_POLY hash (mirrors the empty-array-receiver mark) (#2336). */
      if (c->empty_hash_recv && id < c->node_cap && c->empty_hash_recv[id])
        return TY_STR_POLY_HASH;
      /* an empty literal whose use context fixes a variant (compared against a
         hash-typed peer) adopts it, so both sides share a representation and
         the comparison is a real content check (#3040). */
      /* A literal that is ALSO passed to a user method has to be wide enough
         for whatever the callee writes, which the key context cannot see. The
         arg rule therefore wins over the key context rather than the other way
         round: narrowing here left the caller's `{}` and the callee's
         parameter naming different C structs for the same object (#3386). */
      int eh_arg = c->empty_hash_arg && id < c->node_cap && c->empty_hash_arg[id];
      if (eh_arg) return TY_POLY_POLY_HASH;
      if (c->hash_want && id < c->node_cap && ty_is_hash(c->hash_want[id]))
        return c->hash_want[id];
      return TY_UNKNOWN;
    }
    /* A literal whose local is later given a key of another type has to hold
       both, exactly as a mixed-key literal does (#3927). */
    if (c->hash_want && id < c->node_cap && c->hash_want[id] == TY_POLY_POLY_HASH)
      return TY_POLY_POLY_HASH;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    int empty_spread = 0;
    TyKind spread_ty = TY_UNKNOWN;   /* a typed hash spread in, when there is one */
    int spread_mixed = 0;            /* spreads of two typed variants */
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (aty && sp_streq(aty, "AssocSplatNode")) {
        /* `{ **h, ... }`: merge the spread source's key/value types so the
           rebuilt literal keeps a concrete typed-hash variant instead of
           erasing to UNKNOWN. */
        int src = nt_ref(nt, els[k], "value");
        TyKind sh = src >= 0 ? infer_type(c, src) : TY_UNKNOWN;
        /* `**{}` spreads nothing, so it leaves the key/value types alone */
        int nsrc = -1;
        if (sh == TY_UNKNOWN && src >= 0 && nt_kind(nt, src) == NK_HashNode) nt_arr(nt, src, "elements", &nsrc);
        if (nsrc == 0) { empty_spread = 1; continue; }
        /* A local that only ever holds nil is boxed once inference is done
           (the TY_NIL backstop), and its spread merges at run time into the
           hash of any key, as a poly source's does. Read as spreading nothing
           until then, the literal settled on its pairs' variant -- a String
           to Integer hash -- and the parameter it bound took that type, which
           the merged hash then was not: `m(1, "s" => 2, **h)` with `h = nil`
           did not compile into `def m(*r, a)`, and crashed once splatted. */
        if (sh == TY_NIL && src >= 0 && nt_kind(nt, src) == NK_LocalVariableReadNode) sh = TY_POLY;
        /* nor does an operand of another class: nil carries no keywords, and
           `**true`, `**1` or an object without #to_hash raise CRuby's
           TypeError where they stand (a slot that may be nil, only when it
           is not). A boxed operand, or an object that may still answer
           #to_hash through method_missing, is known only at run time. */
        if (!ty_is_hash(sh) && sh != TY_POLY && sh != TY_UNKNOWN && sh != TY_VOID &&
            !(ty_is_object(sh) && (ty_object_class(sh) < 0 ||
              comp_method_in_chain(c, ty_object_class(sh), "to_hash", NULL) >= 0 ||
              comp_method_in_chain(c, ty_object_class(sh), "method_missing", NULL) >= 0))) {
          empty_spread = 1;
          continue;
        }
        if (ty_is_hash(sh)) {
          kt = ty_unify(kt, ty_hash_key(sh));
          vt = ty_unify(vt, ty_hash_val(sh));
          if (spread_ty != TY_UNKNOWN && spread_ty != sh) spread_mixed = 1;
          spread_ty = sh;
        }
        else if (sh == TY_POLY) {
          /* a poly spread source (a hash reached through a poly binding) merges
             at runtime into a fully-poly hash. */
          kt = ty_unify(kt, TY_POLY);
          vt = ty_unify(vt, TY_POLY);
        }
        else {
          return TY_UNKNOWN;  /* unresolved or non-hash splat */
        }
        continue;
      }
      if (!aty || !sp_streq(aty, "AssocNode")) return TY_UNKNOWN;
      {
        TyKind kt_elem = infer_type(c, nt_ref(nt, els[k], "key"));
        if (kt_elem == TY_NIL) kt_elem = TY_POLY;   /* a nil key keeps the hash poly-keyed (see the value below) */
        kt = ty_unify(kt, kt_elem);
      }
      int vnode = nt_ref(nt, els[k], "value");
      TyKind vt_elem = infer_type(c, vnode);
      /* A nested hash/array literal whose element kind is unresolved (a bare
         `{}` or `[]`) is still a non-scalar value; treat it as poly so the
         outer hash promotes to a poly-valued variant rather than erasing the
         whole hash to UNKNOWN (which would reject `{ "k" => [] }`). */
      if (vt_elem == TY_UNKNOWN) {
        const char *vnode_ty = nt_type(nt, vnode);
        if (vnode_ty && (sp_streq(vnode_ty, "HashNode") || sp_streq(vnode_ty, "KeywordHashNode") ||
                         sp_streq(vnode_ty, "ArrayNode")))
          vt_elem = TY_POLY;
      }
      if (vt_elem == TY_NIL) vt_elem = TY_POLY;   /* a nil value keeps the hash poly-valued (see the array literal) */
      vt = ty_unify(vt, vt_elem);
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    /* No scalar (key,val) variant: the value is poly-stored (a nested hash/
       array/object, or a mix). The key type still selects the hash variant --
       string keys stay a str-keyed poly hash rather than collapsing to a
       fully-poly-keyed one, so the literal matches a `Hash[String, untyped]`
       (StrPolyHash) parameter without a layout-mismatching pointer cast. */
    if (hv == TY_UNKNOWN && vt != TY_UNKNOWN) hv = kt == TY_STRING ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
    /* A typed hash spread in merges directly only into a literal of its own
       variant; the hash of any key and value merges every variant at run
       time. `{**h, "s" => nil}` with `h = {"s" => 2}` came out a String-keyed
       poly hash, which a String-to-Integer one cannot be merged into, and was
       refused -- a lambda's keywords, `f.call(**h, "s" => nil)`, too. */
    if (hv != TY_UNKNOWN && spread_ty != TY_UNKNOWN && (spread_mixed || spread_ty != hv))
      return TY_POLY_POLY_HASH;
    if (hv != TY_UNKNOWN) return hv;
    /* keyword arguments are symbol-keyed */
    if (empty_spread && kt == TY_UNKNOWN) return nk == NK_KeywordHashNode ? TY_SYM_POLY_HASH : TY_POLY_POLY_HASH;
    return hv;
  }
  if (nk == NK_DefNode) return TY_SYMBOL;  /* `def` evaluates to :name */
  if (nk == NK_CallOrWriteNode || nk == NK_CallAndWriteNode) {
    /* `a.v ||= x` evaluates to the attribute's (assigned-or-existing) value:
       the backing ivar's type when the receiver class is known. */
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    TyKind rt2 = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
    if (attr && ty_is_object(rt2)) {
      int cid2 = ty_object_class(rt2);
      /* An explicit `def` reader or writer is a method call, not an ivar
         touch: the value is the reader's answer or the assigned value, and
         neither need be the backing ivar's type (`def v=(x); @v = x * 10; end`
         stores an Integer while the expression answers what was assigned).
         Unify the two arms, the way the emitter does (#4148). */
      int rmi2 = -1, wmi2 = -1;
      int rk2 = comp_resolve_member(c, cid2, attr, 0, NULL, &rmi2);
      int wk2 = comp_resolve_member(c, cid2, attr, 1, NULL, &wmi2);
      if (rk2 == SP_MEMBER_METHOD || wk2 == SP_MEMBER_METHOD) {
        int v3 = nt_ref(nt, id, "value");
        TyKind at = v3 >= 0 ? infer_type(c, v3) : TY_UNKNOWN;
        TyKind rr = (rk2 == SP_MEMBER_METHOD && rmi2 >= 0) ? (TyKind)c->scopes[rmi2].ret : TY_UNKNOWN;
        if (rr == TY_UNKNOWN || rr == TY_VOID) return at;
        if (at == TY_UNKNOWN) return rr;
        return ty_unify(rr, at);
      }
      char ivn2[300]; snprintf(ivn2, sizeof ivn2, "@%s", attr);
      int ii2 = comp_ivar_index(&c->classes[cid2], ivn2);
      if (ii2 >= 0) return ivar_value_ty(&c->classes[cid2], ii2);
    }
    int v2 = nt_ref(nt, id, "value");
    return v2 >= 0 ? infer_type(c, v2) : TY_UNKNOWN;
  }
  if (nk == NK_NextNode) {
    /* `next v` produces the BLOCK's value: its type is v's type (nil when
       bare). Leaving it untyped made a yield whose block ends in `next v`
       infer nil, so the delivered value was discarded at the call site. */
    int nargs = nt_ref(nt, id, "arguments");
    int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
    if (nvc > 0) {
      const char *aty = nt_type(nt, nv[0]);
      /* `next *x` delivers the splat-built ARRAY (the SplatNode arm above
         answers with the ELEMENT type, for array-literal splices). */
      if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
      return infer_type(c, nv[0]);
    }
    return TY_NIL;
  }
  { TyKind r; if (infer_yield_node(c, id, nt, nk, &r)) return r; }
  if (nk == NK_SuperNode || nk == NK_ForwardingSuperNode) {
    if (g_plan_check && id >= 0 && id < c->node_cap) c->ucall_inf[id].via = UC_NONE;
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    const char *shadow = comp_super_shadow(c, s);
    if (shadow) {
      int mi = s->is_cmethod ? comp_cmethod_in_class(c, s->class_id, shadow)
                         : comp_method_in_class(c, s->class_id, shadow);
      if (mi < 0) return TY_UNKNOWN;
      /* a yielding shadow is spliced as a parent is */
      an_user_call_record(c, id, mi, UC_SUPER, s->class_id);
      return super_target_ret(c, s, mi, id);
    }
    /* Class#new: an instance of the receiving class, which is this one or,
       through an inherited `self.new`, any descendant -- then boxed */
    if (comp_super_is_class_new(c, id)) {
      for (int k = 0; k < c->nclasses; k++)
        if (k != s->class_id && is_descendant(c, k, s->class_id)) return TY_POLY;
      return ty_object(s->class_id);
    }
    int p = comp_super_parent(c, s->class_id, s->is_cmethod);
    const char *uname = comp_super_name(c, p, s->name, s->is_cmethod);
    /* `super` in a respond_to? override no ancestor defines is Object's
       respond_to?: a boolean (see emit_super_respond_to) */
    int rto_super = !s->is_cmethod && uname && sp_streq(uname, "respond_to?");
    if (p < 0) return rto_super ? TY_BOOL : TY_UNKNOWN;
    /* super inside a class method resolves through the parent's CLASS-method
       chain (the instance chain would miss `def self.x` entirely). */
    int mi = s->is_cmethod ? comp_cmethod_in_chain(c, p, uname, NULL)
                           : comp_method_in_chain(c, p, uname, NULL);
    if (mi < 0) return rto_super ? TY_BOOL : TY_UNKNOWN;
    an_user_call_record(c, id, mi, UC_SUPER, p);
    return super_target_ret(c, s, mi, id);
  }
  if (nk == NK_AndNode || nk == NK_OrNode) {
    int lnd = nt_ref(nt, id, "left"), rnd = nt_ref(nt, id, "right");
    TyKind lt = infer_type(c, lnd);
    TyKind rt = infer_type(c, rnd);
    if (lt == TY_BOOL && rt == TY_BOOL) return TY_BOOL;
    /* An empty `[]` / `{}` on the right carries no element type of its own, so
       it caches UNKNOWN and ty_unify() drops it, answering the LEFT's type.
       That is what typed `true && []` as BOOL: the array construction was
       then emitted into a bool slot, which the C compiler met as a
       pointer/integer mismatch and, for an Integer left, refused outright.
       Give the literal the container type it is before unifying.

       With a nil (or not-yet-typed) left the answer is the container itself
       rather than the union: `nil_valued || []` is the nil-guard fallback,
       where the literal is the value the expression yields and unifying it
       with nil typed the whole thing nil, so `.size` on it raised
       NoMethodError (#3462). An empty `{}` whose slot's key writes chose its
       variant is the same fallback. */
    if (ty_is_hash(rt) && (lt == TY_NIL || lt == TY_UNKNOWN) && an_empty_container_kind(c, rnd) == 2)
      return rt;
    if (rt == TY_UNKNOWN && rnd >= 0) {
      const char *rty = nt_type(nt, rnd);
      int rn = 0;
      if (rty && sp_streq(rty, "ArrayNode")) {
        nt_arr(nt, rnd, "elements", &rn);
        if (rn == 0) rt = TY_POLY_ARRAY;
      }
      else if (rty && sp_streq(rty, "HashNode")) {
        nt_arr(nt, rnd, "elements", &rn);
        if (rn == 0) rt = TY_STR_POLY_HASH;
      }
      if (rt != TY_UNKNOWN && (lt == TY_NIL || lt == TY_UNKNOWN)) return rt;
      /* `Array.new` / `Hash.new` are as untyped as the literals: the nil-guard
         fallback of `||` is the container, and beside a left of another kind
         the answer boxes */
      if (rt == TY_UNKNOWN) {
        int ek = an_empty_container_kind(c, rnd);
        if (ek && nk == NK_OrNode && (lt == TY_NIL || lt == TY_UNKNOWN)) {
          /* the variant the slot's key writes chose (mark_empty_hash_key_ctx) */
          if (ek == 2 && c->hash_want && rnd < c->node_cap && ty_is_hash(c->hash_want[rnd]))
            return c->hash_want[rnd];
          return ek == 1 ? TY_POLY_ARRAY : TY_STR_POLY_HASH;
        }
        if (an_empty_container_disagrees(ek, lt)) return TY_POLY;
      }
    }
    return ty_unify(lt, rt);  /* value form: a || b -> common type */
  }
  if (nk == NK_BeginNode) {
    /* value = body value unified with each rescue handler's value. With an
       `else` clause the else's value REPLACES the body's on success, so the
       else is the value source; the body's divergence still counts, since a
       body that raises never reaches the else. Typed from the body alone,
       `begin; :sym; rescue; else; false; end` held false in a Symbol slot and
       read it back as symbol number 0. */
    int body = nt_ref(nt, id, "statements");
    int else_c = nt_ref(nt, id, "else_clause");
    int else_st = else_c >= 0 ? nt_ref(nt, else_c, "statements") : -1;
    int vsrc = else_st >= 0 ? else_st : body;
    TyKind r = vsrc >= 0 ? infer_type(c, vsrc) : TY_NIL;
    if (else_c >= 0 && else_st < 0) r = TY_NIL;   /* an empty else is the value: nil */
    TyKind body_t = r;
    /* a body whose last statement is a bare raise diverges: the begin's value
       comes from the rescue arms alone, so their type must not widen (#2739) */
    if (body >= 0 && nt_ref(nt, id, "rescue_clause") >= 0) {
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      if (bn > 0 && bs) {
        const char *lt = nt_type(nt, bs[bn - 1]);
        const char *lnm = (lt && sp_streq(lt, "CallNode")) ? nt_str(nt, bs[bn - 1], "name") : NULL;
        if (lnm && (sp_streq(lnm, "raise") || sp_streq(lnm, "fail")) &&
            nt_ref(nt, bs[bn - 1], "receiver") < 0)
          r = TY_VOID;
      }
    }
    /* An empty container body reads UNKNOWN for want of an element type, not
       because it diverges. Left as "no value" the begin took the handler's
       type alone, and the array pointer went into an sp_int slot (#3496).
       It carries a value, so the two arms are a union: poly. */
    if (r == TY_UNKNOWN && vsrc >= 0) {
      int bn2 = 0; const int *bs2 = nt_arr(nt, vsrc, "body", &bn2);
      if (bs2 && bn2 > 0 && node_is_empty_container(nt, bs2[bn2 - 1])) r = TY_POLY;
    }
    /* A body that carries a value of a type not settled yet is not the same as
       a body that carries none: taking the handler's type alone made the whole
       begin an Integer when the body answered an array whose local had not been
       typed at this point in the walk, and the array was coerced into an int
       slot (#3708). Stay UNKNOWN and let a later round settle it. */
    if (r == TY_UNKNOWN && vsrc >= 0) {
      int bn3 = 0; const int *bs3 = nt_arr(nt, vsrc, "body", &bn3);
      /* only for a local read: its slot is typed by a write that comes later
         in the walk, so within this round it reads UNKNOWN however concrete it
         really is. Anything else that answers UNKNOWN here genuinely has no
         value, and the handler's type is the right answer for it. */
      if (bs3 && bn3 > 0 && nt_kind(nt, bs3[bn3 - 1]) == NK_LocalVariableReadNode)
        return TY_UNKNOWN;
      /* A call ON such a local reads UNKNOWN for the same reason: `a << 2`
         answers the array `a`, whose slot the write earlier in this very body
         types only later in the walk. Taking the handler's type made the whole
         begin a Class, and the boxed value was assigned to an sp_Class (#3867).
         The two arms are a union, so poly is the honest answer -- and it is
         also right for the unresolved call this shape covers, whose raise
         leaves only the handler's (boxed-compatible) value. */
      if (bs3 && bn3 > 0 && nt_kind(nt, bs3[bn3 - 1]) == NK_CallNode) {
        int lrcv = nt_ref(nt, bs3[bn3 - 1], "receiver");
        if (lrcv >= 0 && nt_kind(nt, lrcv) == NK_LocalVariableReadNode) r = TY_POLY;
      }
    }
    int have = !(r == TY_UNKNOWN || r == TY_VOID);
    for (int rs = nt_ref(nt, id, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent")) {
      int st = nt_ref(nt, rs, "statements");
      TyKind at = st >= 0 ? infer_type(c, st) : TY_NIL;
      /* the same on the handler side: an empty container arm carries a value */
      if (at == TY_UNKNOWN && st >= 0) {
        int an2 = 0; const int *as2 = nt_arr(nt, st, "body", &an2);
        if (as2 && an2 > 0 && node_is_empty_container(nt, as2[an2 - 1])) at = TY_POLY;
      }
      if (at == TY_UNKNOWN || at == TY_VOID) continue;  /* this arm diverges too */
      r = have ? ty_unify(r, at) : at;
      have = 1;
    }
    return have ? r : body_t;
  }
  if (nk == NK_CallNode) return infer_call(c, id);

  if (nk == NK_RescueModifierNode) {
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    TyKind et = e >= 0 ? infer_type(c, e) : TY_NIL;
    TyKind rt = r >= 0 ? infer_type(c, r) : TY_NIL;
    /* a diverging expression (raise, a void writer) yields only the rescue
       arm's value: its type stands. A still-UNRESOLVED try (a mid-fixpoint
       estimate) is different -- committing to the rescue arm's concrete type
       here let a local pin to sp_Class while the try later settled on Integer
       (#3130). Answer the boxed union until the try resolves; if it never
       does (the NoMethodError gate's raise-all token), poly still holds the
       rescue arm's value correctly. */
    if (et == TY_VOID || et == TY_NIL) return rt;
    if (et == TY_UNKNOWN) {
      /* an empty container carries a value; the union of the two arms is poly */
      if (node_is_empty_container(nt, e)) return TY_POLY;
      /* a KNOWN diverging form never produces a try value however the
         fixpoint settles: keep the rescue arm's type for it */
      const char *ety = e >= 0 ? nt_type(nt, e) : NULL;
      if (ety && sp_streq(ety, "CallNode") && nt_ref(nt, e, "receiver") < 0) {
        const char *en = nt_str(nt, e, "name");
        if (en && (sp_streq(en, "throw") || sp_streq(en, "raise") ||
                   sp_streq(en, "exit") || sp_streq(en, "abort") ||
                   sp_streq(en, "exit!")))
          return rt;
      }
      return (rt == TY_UNKNOWN || rt == TY_VOID) ? rt : TY_POLY;
    }
    return ty_unify(et, rt);
  }

  /* MultiWriteNode as expression: value is the RHS array. */
  if (nk == NK_MultiWriteNode) {
    int v = nt_ref(nt, id, "value");
    TyKind vt = infer_type(c, v);
    /* an empty `[]` there is built as a poly array (mark_empty_array_operands) */
    if (vt == TY_UNKNOWN && nt_kind(nt, v) == NK_ArrayNode) {
      int en = 0; nt_arr(nt, v, "elements", &en);
      if (en == 0) return TY_POLY_ARRAY;
    }
    return vt;
  }

  return TY_UNKNOWN;
}

/* Where node `id`, just typed `t`, got a degraded type from: the slot's why
   for a read of a local or parameter, the callee's return for a call bound
   to a user def whose return degraded, else the first child carrying a
   degraded type (the tail statement, for a body), else the node itself --
   the poly was born here. -1 when `t` is not degraded. One rule at the one
   place every node's type lands, rather than a note at each of the rules
   that answer poly (#4509: node-origin, not slot-based). */
/* The value of ivar write `wid` when it writes `nm` in class `cid` and that
   value's type is degraded; -1 otherwise. */
static int why_ivar_write_of(Compiler *c, int wid, const char *nm, int cid) {
  const NodeTable *nt = c->nt;
  const char *wn = nt_str(nt, wid, "name");
  if (!wn || !sp_streq(wn, nm)) return -1;
  Scope *ws = comp_scope_of(c, wid);
  if (!ws || ws->class_id != cid) return -1;
  int v = nt_ref(nt, wid, "value");
  return (v >= 0 && v < c->node_cap && ty_degraded(c->ntype[v])) ? v : -1;
}

static int why_node_origin(Compiler *c, int id, TyKind t) {
  if (!ty_degraded(t)) return -1;
  const NodeTable *nt = c->nt;
  NodeKind nk = nt_kind(nt, id);
  if (nk == NK_LocalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = (nm && s) ? scope_local(s, nm) : NULL;
    if (lv && ty_degraded(lv->type) && lv->why.node >= 0 && lv->why.node != id) return lv->why.node;
    return id;
  }
  if (nk == NK_StatementsNode) {
    int n = 0; const int *body = nt_arr(nt, id, "body", &n);
    if (body && n > 0 && body[n - 1] >= 0 && body[n - 1] < c->node_cap && ty_degraded(c->ntype[body[n - 1]])) return body[n - 1];
  }
  if (nk == NK_CallNode) {
    /* a call bound to a user def whose return degraded: the value it
       returns. The receiver: none (self), an object, or a constant naming
       a class or module (a singleton method). */
    int recv = nt_ref(nt, id, "receiver");
    const char *name = nt_str(nt, id, "name");
    if (name && (recv < 0 || !(recv < c->node_cap && ty_degraded(c->ntype[recv])))) {
      int cid = -1, want_cm = 0;
      if (recv >= 0 && recv < c->node_cap && ty_is_object(c->ntype[recv])) cid = ty_object_class(c->ntype[recv]);
      else if (recv >= 0 && (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode)) {
        const char *cn = nt_str(nt, recv, "name");
        cid = cn ? comp_class_index(c, cn) : -1; want_cm = 1;
      }
      else if (recv < 0) { Scope *cs = comp_scope_of(c, id); cid = cs ? cs->class_id : -1; want_cm = cs ? cs->is_cmethod : 0; }
      for (int si = 1; si < c->nscopes; si++) {
        Scope *m = &c->scopes[si];
        if (!m->name || m->def_node < 0 || !sp_streq(m->name, name)) continue;
        if (m->class_id != cid || (m->is_cmethod != 0) != (want_cm != 0)) continue;
        if (ty_degraded(m->ret) && m->ret_why.node >= 0) return m->ret_why.node;
        break;
      }
    }
  }
  if (nk == NK_InstanceVariableReadNode) {
    /* an instance variable is typed from its writes: the first write of the
       name, in this class, whose value is degraded; the read itself when no
       write is (the ivar's kind is the writes' meeting, or a rule's) */
    const char *nm = nt_str(nt, id, "name");
    Scope *rs = comp_scope_of(c, id);
    int cid = rs ? rs->class_id : -1;
    if (nm) {
      /* the writes of the name: through the shared index (a hash bucket,
         so the name is still checked) while the scope index is frozen --
         the passes that own the fixpoint -- and by the scan elsewhere,
         where the index would rebuild on every ask */
      if (comp_scope_index_is_frozen()) {
        int best = -1, best_w = -1;   /* the bucket is not in table order: the first write is the lowest id */
        for (int r = ivw_shared_first(c, nm); r >= 0; r = ivw_shared_next(r)) {
          int wid = ivw_shared_node(r);
          if (nt_kind(nt, wid) != NK_InstanceVariableWriteNode) continue;
          if (best_w >= 0 && wid > best_w) continue;
          int v = why_ivar_write_of(c, wid, nm, cid);
          if (v >= 0) { best = v; best_w = wid; }
        }
        if (best >= 0) return best;
      }
      else {
        NT_FOREACH_KIND(nt, NK_InstanceVariableWriteNode, wid) {
          int v = why_ivar_write_of(c, wid, nm, cid);
          if (v >= 0) return v;
        }
      }
    }
    return id;
  }
  /* the first child carrying a degraded type -- among the children the
     node's own type is derived from: an `if`'s, `while`'s or `case`'s
     predicate and a `when`'s conditions decide which arm, not what the
     arm is worth, and a poly there is not where this poly came from */
  const SpNode *n = &nt->nodes[id];
  for (int i = 0; i < n->nr; i++) {
    int ch = n->r[i].ref;
    if (n->r[i].key && sp_streq(n->r[i].key, "predicate")) continue;
    if (ch >= 0 && ch < c->node_cap && ch != id && ty_degraded(c->ntype[ch])) return ch;
  }
  for (int i = 0; i < n->na; i++) {
    if (n->a[i].key && sp_streq(n->a[i].key, "conditions")) continue;
    for (int k = 0; k < n->a[i].n; k++) {
      int ch = n->a[i].ids[k];
      if (ch >= 0 && ch < c->node_cap && ch != id && ty_degraded(c->ntype[ch])) return ch;
    }
  }
  return id;
}

/* Does the program make a bare Object (`Object.new`)? Asked once. */
int an_program_news_object(Compiler *c) {
  static const Compiler *memo_c = NULL;
  static int memo = -1;
  if (memo_c == c && memo >= 0) return memo;
  const NodeTable *nt = c->nt;
  int found = 0;
  for (int n = comp_kind_first(c, NK_CallNode); n >= 0 && !found; n = comp_kind_next(c, n)) {
    if (nt_kind(nt, n) != NK_CallNode || !nt_str(nt, n, "name") || !sp_streq(nt_str(nt, n, "name"), "new")) continue;
    int r = nt_ref(nt, n, "receiver");
    found = r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && nt_str(nt, r, "name") &&
            sp_streq(nt_str(nt, r, "name"), "Object");
  }
  memo_c = c; memo = found;
  return found;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  if (g_io_skip_reopen && id != g_io_skip_node) {
    g_io_skip_reopen = 0;
    TyKind t = infer_type(c, id);
    g_io_skip_reopen = 1;
    return t;
  }
  /* The face re-inference (infer_call's last resort, and codegen's re-entry)
     asks what one call would be with this receiver pinned to one concrete
     kind (the face table in types.h). Only that receiver node, only for the
     duration, and the cache is left untouched so the receiver's own type is
     unaffected. */
  { TyKind fk = face_of(id); if (fk != TY_UNKNOWN) return fk; }
  /* see an_builtin_answer: the asked call's receiver keeps its own type */
  if (an_builtin_only && id == an_bo_recv && id < c->node_cap && c->ntype[id] != TY_UNKNOWN)
    return c->ntype[id];
  /* A depth-0 entry opens a fresh memo generation: nothing carries over from
     the previous tree, so the fixpoint sees every table change. */
  if (g_infer_depth == 0 && ++g_imemo_gen == 0) {
    if (g_imemo_stamp)
      memset(g_imemo_stamp, 0, sizeof(unsigned) * (size_t)g_imemo_cap);
    g_imemo_gen = 1;
  }
  int memo_ok = !face_active() && !an_builtin_only && an_ie_class_id < 0 &&
                g_infer_blv_pin == 0;
  if (memo_ok && id < g_imemo_cap && g_imemo_stamp[id] == g_imemo_gen)
    return g_imemo_val[id];
  g_infer_depth++;
  TyKind t = infer_uncached(c, id);
  g_infer_depth--;
  /* The builtin-only re-derivation (see an_builtin_only) asks what this call
     would be if no user class owned the name. That answer is not the node's
     real type, and neither are the child types derived under it, so the cache
     must not record any of them -- leaving them in made the whole analysis
     order-sensitive (#3459). */
  /* `x&.pred?` on a receiver that may be nil answers nil OR a boolean, and a
     C bool has no nil: the two arms of the emitted guard then disagreed about
     their type and the program did not build (#3899). Such a call is poly.
     A bool is not the only such answer -- a Symbol, a Class, a Rational and a
     Complex have no C nil either, and the guard boxed nil into their slot all
     the same. The two rules in infer_uncached that call a `&.` poly are placed
     by POSITION in the inference chain, so a name resolved before them
     (`class`, `to_sym`) never reached one; this one runs after every arm and
     asks the property that actually decides it: can the answer's C type hold
     a nil? Integer and Float have their sentinels, String and the arrays and
     the reference objects have NULL, so those keep their concrete type and
     the guard uses that nil (the array trio of #3461 is this same rule).
     The receiver's type does not narrow this. A miss on a specialized
     container is the element type's C nil -- a NULL string, SP_INT_NIL --
     not a poly nil, so `h["zz"]&.empty?` reached the guard with a concrete
     receiver and answered `false` where CRuby answers nil (#4070). `&.` is
     the program saying nil is possible; the answer has to be able to hold
     one. */
  if (!an_ty_holds_nil(t) && nt_kind(c->nt, id) == NK_CallNode) {
    const char *sn_op = nt_str(c->nt, id, "call_operator");
    int sn_recv = nt_ref(c->nt, id, "receiver");
    if (sn_op && sp_streq(sn_op, "&.") && sn_recv >= 0) t = TY_POLY;
  }
  if (!an_builtin_only) {
    c->ntype[id] = t;
    /* the origin only when a consumer asked (--warn-widen, --emit-types):
       its ivar arm walks the program's ivar writes per read and its call
       arm scans the scopes by name, +10% analysis on optcarrot and +23% on
       a 2000-ivar class when it ran on every plain compile */
    static int want = -1;
    if (want < 0) { const char *ww = getenv("SPINEL_WARN_WIDEN"), *et = getenv("SPINEL_EMIT_TYPES"); want = ((ww && *ww) || (et && *et)) ? 1 : 0; }
    if (want && id < c->node_cap) c->norigin[id] = why_node_origin(c, id, t);
  }
  /* memo_ok still holds here: every mode section inside infer_uncached
     restores its flag before returning, so t is the mode-free answer. */
  if (memo_ok) {
    if (id >= g_imemo_cap) {
      int ncap = c->nt->count > id + 1 ? c->nt->count : id + 1;
      unsigned *nstamp = realloc(g_imemo_stamp, sizeof(unsigned) * (size_t)ncap);
      TyKind *nval = realloc(g_imemo_val, sizeof(TyKind) * (size_t)ncap);
      /* a grown side is kept either way (the old pointer is gone); the cap
         only moves when both grew, and on failure the memo -- an optimization
         -- just does not record this node */
      if (nstamp) g_imemo_stamp = nstamp;
      if (nval) g_imemo_val = nval;
      if (!nstamp || !nval) return t;
      memset(g_imemo_stamp + g_imemo_cap, 0,
             sizeof(unsigned) * (size_t)(ncap - g_imemo_cap));
      g_imemo_cap = ncap;
    }
    g_imemo_stamp[id] = g_imemo_gen;
    g_imemo_val[id] = t;
  }
  return t;
}

/* See analyze.h. Children first, so a parent whose inference reads child
   caches (comp_ntype) sees the refreshed values. */
void infer_subtree(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) infer_subtree(c, nt_ref_at(nt, id, i));
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) infer_subtree(c, ids[k]);
  }
  infer_type(c, id);
}

/* ---- scope assignment ---- */

void scope_add_param(Scope *s, const char *name, int defnode) {
  if (s->nparams % 8 == 0) {
    s->pnames = realloc(s->pnames, sizeof(char *) * (size_t)(s->nparams + 8));
    s->pdefault = realloc(s->pdefault, sizeof(int) * (size_t)(s->nparams + 8));
  }
  s->pdefault[s->nparams] = defnode;
  s->pnames[s->nparams++] = strdup(name);
  if (defnode < 0) s->nrequired = s->nparams;
  LocalVar *lv = scope_local_intern(s, name);
  lv->is_param = 1;
}

/* Collect parameters from a DefNode into scope s. */
