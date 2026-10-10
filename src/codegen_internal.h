/* Internal shared state and declarations for the split code generator.
 * The generator was one 19k-line file; it is now split by emission stage
 * (util / fold / call / expr / stmt / decl+driver). Everything here was
 * file-static in the single file and is shared between the parts. */
#ifndef SPINEL_CODEGEN_INTERNAL_H
#define SPINEL_CODEGEN_INTERNAL_H
#include "ffi_spec.h"
/* M2 code generator: the M1 scalar/control-flow subset plus user-defined
 * methods (required params, inferred param/return types, recursion, tail-
 * position implicit returns). Emits the same runtime ABI as the legacy
 * generator. Unsupported constructs abort loudly.
 */
#include "codegen.h"
#include "compiler.h"
#include "analyze.h"
#include "decide.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

/* ---- output buffer ---- */

typedef struct { char *p; size_t len, cap; } Buf;

/* Buffer ops (defined in codegen_util.c). Declared here, before the
   inline emit_indent below uses buf_puts -- otherwise the inline body
   references an undeclared function (clang errors, gcc warns). */
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_erase(Buf *b, size_t off, size_t n);
extern int g_no_root_elision;
extern int g_no_root_frame;
extern int g_inline_hot;
extern int g_no_write_barrier;
extern const char *g_ruby_description;
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline void emit_indent(Buf *b, int n) { for (int i = 0; i < n; i++) buf_puts(b, "  "); }

/* The class argument of is_a?/kind_of?/instance_of?/=== may be a bare constant
   (`Integer`) or a top-level scoped constant (`::Integer`); both name the same
   class. Returns the name, or NULL for a nested path or a non-constant. */
static inline const char *isa_const_name(const NodeTable *nt, int arg) {
  const char *t = arg >= 0 ? nt_type(nt, arg) : NULL;
  if (!t) return NULL;
  if (sp_streq(t, "ConstantReadNode")) return nt_str(nt, arg, "name");
  /* A ConstantPathNode names a class by its last segment, whether root-scoped
     (`::Integer`, parent < 0) or namespace-qualified (`Outer::Thing`, parent
     >= 0). Resolving the latter too lets is_a? answer a namespace-qualified
     class instead of falling to the dynamic path, which raised on a builtin
     receiver where CRuby returns false (#3258). */
  if (sp_streq(t, "ConstantPathNode"))
    return nt_str(nt, arg, "name");
  return NULL;
}

/* Fully-qualified name of a constant argument ("PG::Error"), built by walking
   a ConstantPathNode's parent chain into buf. Exception-class names are
   registered qualified, so an is_a? compare against the flat leaf name never
   matched a namespaced exception class (#3260). Returns the flat name for a
   bare or root-anchored (`::X`) constant, buf for a nested path, or NULL for
   a non-constant / overlong path. */
static inline const char *isa_const_qualname(const NodeTable *nt, int arg, char *buf, size_t bufsz) {
  const char *t = arg >= 0 ? nt_type(nt, arg) : NULL;
  if (!t) return NULL;
  if (sp_streq(t, "ConstantReadNode")) return nt_str(nt, arg, "name");
  if (!sp_streq(t, "ConstantPathNode")) return NULL;
  const char *leaf = nt_str(nt, arg, "name");
  if (!leaf) return NULL;
  char segs[8][64]; int nseg = 0; int ok = 1;
  int qpar = nt_ref(nt, arg, "parent");
  while (qpar >= 0 && nseg < 8) {
    const char *pty = nt_type(nt, qpar);
    const char *pn = nt_str(nt, qpar, "name");
    if (!pty || !pn) { ok = 0; break; }
    if (sp_streq(pty, "ConstantReadNode")) { snprintf(segs[nseg++], sizeof segs[0], "%s", pn); break; }
    if (sp_streq(pty, "ConstantPathNode")) { snprintf(segs[nseg++], sizeof segs[0], "%s", pn); qpar = nt_ref(nt, qpar, "parent"); continue; }
    ok = 0; break;
  }
  if (!ok) return NULL;
  if (nseg == 0) return leaf;   /* root-anchored ::X */
  size_t o = 0;
  for (int si = nseg - 1; si >= 0; si--) {
    int w = snprintf(buf + o, bufsz - o, "%s::", segs[si]);
    if (w < 0 || (size_t)w >= bufsz - o) return NULL;
    o += (size_t)w;
  }
  snprintf(buf + o, bufsz - o, "%s", leaf);
  return buf;
}

/* Statement prelude: some expressions (array/hash literals) lower to
   temp-variable construction that must run before the statement that
   uses them. While a statement line is being built, g_pre collects those
   setup lines at g_indent; the statement wrapper flushes g_pre before the
   line. g_tmp hands out unique temp ids. */
extern Buf *g_pre;
extern int  g_indent;
extern int  g_tmp;

/* Inlining a yielding method: method-local names are renamed (to avoid
   clashing with the call site's locals), and yield emits the active
   block's body. g_block_id is the current BlockNode for yield (-1 if
   none). The rename map holds only the inlined method's locals. */
#define MAX_RENAME 128
extern char g_ren_from[MAX_RENAME][96];
extern char g_ren_to[MAX_RENAME][112];
typedef struct { int sv, from, n; char (*f)[96]; char (*t)[112]; int fence; } RenPark;
RenPark ren_park(int from);
void ren_unpark(RenPark *p);
const char *strbuf_local_name(Compiler *c, int recv);
int ivar_global_slot(Compiler *c, int node, char *out, size_t cap);
int gvar_global_slot(Compiler *c, int node, char *out, size_t cap);
int cvar_global_slot(Compiler *c, int node, char *out, size_t cap);
int lent_global_slot_rebound(Compiler *c, int arg, const char *slot);
void refuse_lent_global_rebound(Compiler *c, int arg, const char *slot, const char *target, const char *pname);
int strbuf_ivar_owner(Compiler *c, int node);
/* Is an object's ivar set: 0 always, 1 when not nil, 2 cannot tell, 3 explicit flag (codegen_util.c) */
int ivar_set_kind(Compiler *c, int cid, const char *ivn);
/* ... and whether the family keeps the rank of each ivar's first assignment in place of the flag (codegen_util.c) */
int ivar_ranked(Compiler *c, int cid);
int *ivar_listing_order_new(Compiler *c, int cid); /* malloc'd; the caller frees */
void emit_ivar_order_open(Compiler *c, int cid, const char *obj, Buf *b);
void emit_ivar_order_close(Buf *b);
void emit_ivar_list_fill(Compiler *c, int k, const char *obj, const char *arr, int create, Buf *b);
const char *ivar_set_test(Compiler *c, int cid, const char *ivn, const char *expr, char *buf, size_t cap);
/* ... and the statement that marks one of kind 3 assigned after a store */
const char *ivar_set_mark(Compiler *c, int cid, const char *ivn, const char *obj, const char *acc,
                          char *buf, size_t cap);
const char *ivar_write_mark(Compiler *c, int w, char *buf, size_t cap);
int ie_class_of(Compiler *c, int node);
/* The shared-mutable shim (codegen_stmt.c) re-runs a value-semantics mutator
   arm against a plain shadow copy, then swaps the handle's bytes for it. A
   LOCAL receiver is redirected into the shadow by the rename table; an ivar
   has no name to rename, so the shim publishes the slot it is shadowing here
   and the ivar emitter resolves reads AND the arm's write-back to the shadow
   -- the same substitution one level down (#4363). */
extern const char *g_sb_iv_name;   /* "@bt" while a shim is open, else NULL */
extern int         g_sb_iv_cid;
extern char        g_sb_iv_repl[64];
int strbuf_slot_ref(Compiler *c, int recv, char *out, size_t cap);
int strbuf_bang_self_local(const Compiler *c, int v);
void emit_strbuf_param_bind(Compiler *c, const LocalVar *pv, TyKind want, const char *src, Buf *b);
/* `REF ||= v` / `REF &&= v` on a shared-handle String slot (codegen_expr.c) */
void emit_strbuf_orw_guard(Compiler *c, const char *ref, int v, int is_or, Buf *b);
/* The value a write hands a shared-handle String slot `lv` (codegen_stmt.c) */
void emit_strbuf_value(Compiler *c, LocalVar *lv, int v, Buf *b);
/* The handle such a slot takes from value v: its own, or a new one */
void emit_strbuf_handle_of(Compiler *c, int v, Buf *b);
const char *strbuf_new_func(Compiler *c, int v);
/* --share-strings: the handle a value route that answers the String it is
   handed (`+s`, `String(s)`, `s.then { |x| x }`) hands on: 1, or 0 with
   nothing emitted */
int emit_strbuf_route(Compiler *c, int v, Buf *b);
/* --share-strings: does codegen hand the shared handle along flow kind
   `kind` (ShareFlowKind) at node site, from value node v?
   The routes codegen carries, which the seal requires of every flow into a
   shared class (repr_share_flows_check). fm holds the answers asked once
   per method, for one check. */
typedef struct {
  signed char *yield_ok;   /* per method scope: its yields carry the handle
                              (1), do not (0), not asked yet (-1) */
} StrbufFlowMemo;
int strbuf_flow_carries(Compiler *c, StrbufFlowMemo *fm, int kind, int site, int v);
int strbuf_call_picks_up(Compiler *c, int id);
int strbuf_self_reader_slot(Compiler *c, int id);
int strbuf_self_reader_handle(Compiler *c, int id);
int strbuf_call_reads_handle(Compiler *c, int recv);
int strbuf_route_reader(Compiler *c, int v);
const NativeMethod *strbuf_native_answer(Compiler *c, int n);
int strbuf_io_outbuf(Compiler *c, int id);
int emit_io_read_nonblock_outbuf(Compiler *c, int id, Buf *b);
int emit_strbuf_io_read(Compiler *c, int id, Buf *b);
void emit_str_concat_handle(Compiler *c, const char *sref, int argc, const int *argv, Buf *b, int indent);
int emit_bang_self_handle(Compiler *c, int v, Buf *b);
int strbuf_chain_over_handle(Compiler *c, int v);
int strbuf_narrowed_box_mutator(Compiler *c, int id);
void emit_narrowed_box_mutator(Compiler *c, int id, Buf *b);
int strbuf_opwrite_handle(Compiler *c, int v, char *out, size_t cap);
int strbuf_poly_to_s(Compiler *c, int v);
int strbuf_boxed_local(Compiler *c, int v);
/* Is a variable holding the shared handle (or, --share-strings, a route or
   a box) one of the values conditional v can hand over (codegen_stmt.c)? */
int strbuf_cond_has_handle_leaf(Compiler *c, int v, int depth);
/* String mutator id's receiver recv as the handle it changes: 1 a slot, 2
   a route (read once, nil raising NoMethodError for id), 0 neither
   (codegen_stmt.c) */
int strbuf_recv_handle(Compiler *c, int id, int recv, char *out, size_t cap);
/* Root receiver handle _t<t> when strbuf_recv_handle answered a route (hr
   2) and something can allocate while it is live: the mutator (argv NULL)
   or one of its argc ty operands argv; a slot's needs none (codegen_stmt.c) */
void emit_route_recv_root(Compiler *c, int hr, int t, int argc, const int *argv, TyKind ty, Buf *b);
/* --share-strings: is recv a deep-return pickup whose method can answer
   nil? (codegen_stmt.c) */
int strbuf_pickup_may_nil(Compiler *c, int recv);
/* --share-strings: can the deep-return pickup `id` answer nil?
   (codegen_call.c) */
int strbuf_pickup_answers_nil(Compiler *c, int id);
/* --share-strings: the handle slot a variable's read names, 0 for any
   other node (codegen_stmt.c) */
int strbuf_var_handle(Compiler *c, int n, char *out, size_t cap);
/* --share-strings: the String identity comparison of two operands through
   their boxes (codegen_call_operator.c), and an inline-spliced call whose
   result is the handle itself (codegen_stmt.c) */
void emit_string_identity(Compiler *c, int recv, int arg, Buf *b);
int string_identity_inline(Compiler *c, int recv, int arg);
int strbuf_route_inline_call(Compiler *c, int v);
/* --share-strings: does value v hand over a shared String as its handle
   (a variable, a route, a conditional with such an arm)? (codegen_stmt.c) */
int strbuf_value_carries(Compiler *c, int v);
int strbuf_builtin_tail(Compiler *c, int v);
int strbuf_hash_default_arg(Compiler *c, int v);
/* A builtin String clamp's receiver and two bounds, or absent endpoints. */
int strbuf_route_clamp(Compiler *c, int v, int *ops);
/* `String(x)` or `+x` over a variable whose slot holds the handle: that
   slot's text, *uplus for `+x` (codegen_stmt.c). Returns the operand node
   plus one, or zero when there is no route. */
int strbuf_self_route_slot(Compiler *c, int v, int *uplus, char *out, size_t cap);
/* --share-strings: an exception's message argument that reads a variable
   holding the handle, which the exception then holds
   (codegen_call_exception.c) */
int exc_msg_handle(Compiler *c, int arg, char *href, size_t cap);
/* --share-strings: `e.message` over a variable's exception (codegen_stmt.c) */
int exc_message_identity_operand(Compiler *c, int v);
void emit_exc_message_idbox(Compiler *c, int v, Buf *b);
int emit_exc_message_equal(Compiler *c, int recv, int arg, Buf *b);
int strbuf_route_exc_message(Compiler *c, int v);
int strbuf_route_srange_end(Compiler *c, int v);
void emit_slot_expr(Compiler *c, int id, Buf *b);
/* A `next` value a block's boxed answer slot takes: a shared String as its
   handle's box under --share-strings (codegen_stmt.c) */
void emit_boxed_next_value(Compiler *c, int v, Buf *b);
/* The value a Struct constructor or an attribute writer stores into a String
   ivar slot (codegen_stmt.c) */
void emit_strbuf_ivar_store(Compiler *c, int shared, int v, Buf *b);
/* A read of a shared-mutable String slot `sref` at node `id` (codegen_expr.c) */
void emit_strbuf_node_read(Compiler *c, int id, const char *sref, Buf *b);
int emit_strbuf_ivar_write_handle(Compiler *c, int v, Buf *b);
/* A write whose slot holds the --share-strings handle, where a handle is
   taken: the write, valued as that handle (codegen_expr.c) */
int emit_strbuf_write_handle(Compiler *c, int v, Buf *b);
int operand_may_allocate(Compiler *c, int id);
/* Can operand `v` allocate where it stands? operand_may_allocate, less
   what builds nothing: a pure read (subtree_is_pure_read: a typed Array's
   element, a plain field), and a shared String slot's read that hands its
   consumer the live buffer (strbuf_read_raw). */
int operand_allocates_beside(Compiler *c, int v);
/* Is `v` a read of a shared String slot (a local's, an ivar's, a global's
   or a constant's) whose value form is a fresh copy of its bytes
   (sp_strbuf_read_pub), held by nothing until its consumer takes it? A
   read that hands out the handle or the live buffer copies nothing, and
   one that ran already is its temp. */
int strbuf_read_copies(Compiler *c, int v);
/* The same shim over a READER call that hands out the handle
   (`obj.name[0] = "X"`): no name to rename and no ivar node, so the call node
   itself reads as the shadow through the argument-override table. */
/* what sb_reader_shim_open lifted: the node's marks and type as they were,
   and the views (ntok of them from tok) that hold the lifted ones */
typedef struct { unsigned char box, demand; TyKind ty; int tok, ntok; } SbReaderSave;
int sb_reader_shim_open(Compiler *c, int recv, char *sref, size_t cap, SbReaderSave *sv);
void sb_reader_shim_close(Compiler *c, int recv, const SbReaderSave *sv);
int sb_shadowed_reader(int node);
int sb_shim_args_first(Compiler *c, int id, Buf *pre, int indent);
int str_mut_var_recv(Compiler *c, int recv);
void emit_str_frozen_check(Compiler *c, int recv, Buf *b);
int strbuf_boxed_elem_read(Compiler *c, int v);
int emit_strbuf_read_ref(Compiler *c, int recv, Buf *b);
int strbuf_object_ref(Compiler *c, int recv, Buf *b);
extern int g_block_nren;
extern int g_yield_block_fallback_nren;
/* Paired 1:1 with g_block_nren / g_yield_block_fallback_nren: the &block
   parameter name of the SCOPE THAT OWNS the block g_block_id (resp.
   g_yield_block_fallback) currently names, so that when that block's body is
   finally spliced (emit_block_invoke), `<that scope's own &block>.call(...)`
   inside it still resolves -- g_block_param_name itself names the CURRENT
   callee's own &block instead (see codegen_util.c), which is a different
   scope once a block is spliced through more than one inlined callee. */
extern const char *g_block_owner_param_name;
extern const char *g_yield_block_fallback_param_name;
extern int  g_nren;
/* hide the renames from base up and put them back (codegen_util.c) */
typedef struct { int base, top; char (*from)[96]; char (*to)[112]; } RenHide;
void ren_hide(RenHide *h, int base);
void ren_unhide(RenHide *h);
extern int  g_block_id;
int builtin_method_known(const char *cls, const char *m);
int builtin_cmethod_known(const char *cls, const char *name);
int builtin_super_cmethod_known(const char *cls, const char *name);
int builtin_instance_name_known(const char *m);
int builtin_arity_violation(Compiler *c, int id);
int builtin_object_method_known(const char *m);
int name_is_enumerable_module_method(const char *m);
int emit_object_methods_reflection(Compiler *c, int recv, int cid, const char *name, int all,
                                   int arg, Buf *b);
int scope_reads_callee(Compiler *c, int si);
int sp_yield_site_type(const Compiler *c, int id, TyKind *out);
TyKind block_next_value_ntype(const Compiler *c, int node);
/* Argument-hoist overrides (see emit_args_filled): node id -> rooted temp
   name substituted by emit_expr. A site checks for room against
   MAX_ARG_OVERRIDE and falls back when there is none; the table always
   holds that many past its fill (argov_reserve). */
#define MAX_ARG_OVERRIDE 64
extern int  *g_argov_node;
/* The text a bound node is written as (view_bind), per slot. A class name in it (the
   receiver cast of a poly arm, "((sp_<Class> *)_t0.v.p)") made 32 bytes too few for a
   name of 15 characters or more, and the vsnprintf cut the text short without a word:
   the C did not build (#7604). view_bind checks the length now. */
#define ARGOV_TEXT_LEN 160
extern char (*g_argov_text)[ARGOV_TEXT_LEN];
extern unsigned char *g_argov_oint;   /* the bound text is an oint (view_bind_o) */
int view_bind_o(int node, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern int  g_n_argov;
/* Room for one more override whatever the fill, for a site that must run
   every argument of a call ahead of it, however many there are
   (emit_args_run and its kin): the table grows, keeping
   MAX_ARG_OVERRIDE entries free past the fill for the sites that check. */
void argov_reserve(void);
/* The setter call (`obj.x = v`) emit_stmt is lowering: nothing reads its value,
   so emit_object_call leaves the value temp out (see setter_value_open). */
extern int  g_setter_stmt_id;
extern int  g_sn_skip;   /* safe-nav re-entry marker (see codegen_util.c) */
extern int  g_cls_tag_skip;   /* poly-dispatch builtin-arm re-entry marker */
/* Ask subtree_may_allocate before leaving something unrooted across `id`:
   its "no" is a keyed decision (src/decide.c). subtree_allocates is the
   bare fact, for a caller whose answer licenses no such omission. */
int subtree_may_allocate(const NodeTable *nt, int id);
int subtree_allocates(const NodeTable *nt, int id);
int subtree_has_side_effect(Compiler *c, int id);
int loop_has_valued_break(Compiler *c, int root);
/* Can evaluating the subtree store into an ivar, class variable or global? A
   write to one, or anything that runs Ruby code it does not show; not scalar
   arithmetic, a typed-array read or a plain field read (codegen_call.c). */
int subtree_may_reassign_state(Compiler *c, int id);
/* Can evaluating the subtree run a proc or block of the program's? A
   yield, a super, a block, a call of a method the program defines (or of a
   name it defines on any class), and a builtin handed anything but numbers,
   Strings, Symbols, their ranges and typed Arrays can; a builtin over those
   alone, with no block, cannot: `total + i.to_s` (codegen_call.c). */
int subtree_may_run_proc(Compiler *c, int id);
/* Does evaluating the subtree run no code, store nothing and allocate nothing
   -- variable and literal reads, scalar arithmetic, typed-array reads and
   plain field reads, all the way down (codegen_call.c)? */
int subtree_is_pure_read(Compiler *c, int id);
/* A pure read, or one of the forms beside that list which run no call of the
   program's and make nothing either: a global's read; a unary minus, `abs`
   or an operator on numbers; `!` on a boolean; `size` or `length` of a
   String, an Array or a Hash; `&&`, `||` and a conditional of such values.
   An index, an operator, a reader or one of those calls counts only while it
   is provably the builtin's (plain_name_is_builtin, codegen_call.c). */
int subtree_is_plain_value(Compiler *c, int id);
/* The uses by which a program can hold a method the node table does not
   show (plain_uses_walk, codegen_call.c), asked of the table as the parser
   wrote it: called ahead of analyze_program, whose desugars may rewrite one. */
void plain_note_parsed(Compiler *c);
int operand_is_held_read(Compiler *c, int id);
/* Is the call a reader the emitter lowers to a plain field read? *allocates is
   set when the read builds a copy (a shared String slot). codegen_call.c */
int call_is_field_read(Compiler *c, int id, int *allocates);
/* Is the receiverless call a class's own attr reader, lowered to a plain
   read of self's slot? codegen_call.c */
int implicit_self_is_field_read(Compiler *c, int id);
/* Typed-array headers cached across an innermost loop (codegen_stmt.c, see
   emit_while). hc_array / hc_string answer 1 and the names of the cached
   header locals when the receiver is cached in the loop being emitted;
   hc_mark is the text a write's or a safepoint's slow path appends, after
   which the cached headers are read again ("" outside such a loop). */
int hc_array(Compiler *c, int recv, int is_float, char *d, char *l, char *w, size_t cap);
void hc_read_len(const char *hd, char *out, size_t cap);
int hc_index_in_range(Compiler *c, int recv, int idx);
extern int g_loop_polls_in_cond;   /* the next emit_loop_body leaves its polls to the loop's condition */
int hc_string(Compiler *c, int recv, char *d, char *l, size_t cap);
/* hc_array for a Float array whose in-range elements the reader needs to
   be no nil: *n names a length that is 0 while the array may hold one, or
   while `guard` (a Float local the loop does not assign; -1 for none) is
   nil. Answers 2 when it took the guard. */
int hc_array_nilfree(Compiler *c, int recv, int guard, char *d, char *n, size_t cap);
const char *hc_mark(void);
/* recv is read in the loop being emitted the way hc_array caches it: no
   code runs between two of its reads in one pass, so they agree */
int hc_recv_cached(Compiler *c, int recv);
/* the nil test a cached array read writes in its out-of-range branch for
   a receiver seen as Repr.nil_cold (emit_nil_target_cold, codegen_call.c) */
void emit_nil_cold_test(Compiler *c, int id, int r, Buf *b);
int call_is_scalar_op(Compiler *c, int id);   /* a builtin operator over scalars */
/* Whether the subtree at `id` assigns the local `nm`: a write, an op-write
   or a multiple-assignment target by that name. */
int subtree_writes_local(Compiler *c, int id, const char *nm);
int iter_recv_bind_once(Compiler *c, int node);
/* When a yielding method is inlined, g_yield_block_fallback holds the block
   that was active in the CALLER's context so nested `yield`s inside the
   passed block can chain back to the outermost caller's block. */
extern int  g_yield_block_fallback;
int yield_block_out(int k);   /* codegen_iter.c */
int yield_target_push(int block, int saved_block, const char *owner, int nren,
                      const char *brk, int brk_ebase);   /* codegen_iter.c */
void yield_target_pop(int pushed, int saved_cur);
int yield_target_cur(void);
extern const char *g_yield_self_fallback;        /* see codegen_util.c */
extern const char *g_yield_self_fallback2;
extern const char *g_yield_self_deref_fallback2;
extern int g_yield_emitting_class_fallback2;
extern const char *g_yield_self_deref_fallback;
extern int g_yield_emitting_class_fallback;
/* Name of the `&block` parameter of the method currently being inlined, so
   `<blk>.call(args)` inside it expands the active block like `yield args`. */
extern const char *g_block_param_name;
extern const char *g_yielder_name;
/* Result temp for a do{}while(0)-wrapped instance_exec splice; a top-level
   `next <v>` captures into it before continuing out. NULL otherwise. */
extern const char *g_ie_next_var;
/* The C type of the slot g_ie_next_var names, when it is a container kind: an
   empty `[]` / `{}` handed to `next` has no kind of its own and would be built
   at its own default, which the destination then reads as the wrong struct
   (#3978). TY_UNKNOWN when unknown or not a container. */
extern TyKind g_ie_next_ty;
typedef struct EmitUnitState EmitUnitState;
EmitUnitState *emit_state_snapshot(void);
void emit_state_release(EmitUnitState *s, int rollback);
extern TyKind g_bv_dest_ty;
extern int g_c_loop_depth;   /* C-loop nesting inside the current fn body */
extern int g_in_proc_body;   /* emitting a _proc_N function body */
/* Set while the wrapped splice's result temp is poly, so a value-carrying
   break/next boxes a scalar value to match. */
extern int g_ie_res_poly;
/* The C expression for `self` (a pointer). Overridden while inlining an
   instance method at a call site (where there is no real `self` param). */
extern const char *g_self;
extern const char *g_self_deref;
extern const char *g_inline_recv_expr;
void emit_into_pre_line(Compiler *c, void (*fn)(Compiler *, int, Buf *), int node);
extern int g_inline_recv_class;
/* When emitting class/module body statements, the class index (-1 outside). */
extern int g_class_body_id;
/* Class id of the scope currently being emitted (-1 if none). Used to resolve
   implicit self calls in included-module methods to the including class. */
extern int g_emitting_class_id;
extern int g_scopes_settled;   /* analysis done: scope_is_shadowed caches (codegen_util.c) */
/* While emitting a compile-time-unrolled define_method body: the loop-var
   name to substitute and the literal node to emit in its place (-1 = none). */
extern const char *g_dm_subst_name;
extern int g_dm_subst_node;
/* When inside an instance_eval block, the class id of the receiver (-1 outside).
   Used so InstanceVariableReadNode/WriteNode use g_self->iv_X instead of civ_Toplevel_X. */
extern int g_ie_class_id;
/* Set while emitting an instance_eval/exec splice in statement position: the
   block's value is discarded, so the last statement emits as a statement
   (not coerced to an expression, which would fail for e.g. a trailing puts). */
extern int g_ie_discard_value;
/* an instance_eval/exec body run on a receiver without ivars: they read nil
   (the call id + 1, 0 when off) */
extern int g_ie_nil_ivars;
/* While emitting a rescue handler: the C var names holding the caught
   exception's class/message, so a bare `raise` can re-raise. */
extern const char *g_rescue_cls, *g_rescue_msg;
/* When inside a rescue handler that can `retry`, holds the goto label for the
   retry target (just before `sp_exc_top++`). NULL otherwise. */
extern const char *g_retry_label;
/* Redo label stack: each enclosing loop that contains a `redo` pushes a fresh
   C label id; a RedoNode emits `goto _redo_<top>` to re-run the current
   iteration without re-testing the guard or advancing the iterator. */
extern int g_redo_stack[64];
extern int g_redo_depth;
/* A redo label the next emit_stmts of a block body places after the body's
   setup (its locals' reset and block_param_rebind_len's statements); 0 when
   none is pending. */
extern int g_redo_pending;
/* The body whose own `redo`s each label serves (subtree_owns_redo). */
extern int g_redo_owner[64];
int redo_label_push(int owner);
void emit_redo_unwind(Buf *b);   /* pops what a redo leaves on its way to the label */
int subtree_owns_redo(const NodeTable *nt, int body, int redo);
int block_of_body(Compiler *c, int body);
int block_param_rebind_len(const NodeTable *nt, int body);

/* When set inside a loop-as-expression, BreakNode assigns its value here. */
extern const char *g_loop_break_var;
/* Valued-break-from-block state (see codegen_util.c). */
extern const char *g_brk_ser_var;
extern int g_brk_ensure_base;
extern const char *g_block_brk_var;
extern const char *g_yield_blk_brk_fallback;
extern int g_block_brk_ebase;
extern int g_yield_blk_brk_efallback;
extern int g_proc_body_kind;
extern const char *g_proc_brk_home;
extern int g_brk_skip_id;
extern const char *g_hoist_len_var;
extern const char *g_hoist_len_recv;
/* When set, tail positions assign to this var instead of `return`ing
   (used to give a begin/rescue a value). */
extern const char *g_result_var;
/* When g_result_var is set, whether that result slot is poly (so a scalar
   tail value must be boxed into it). */
extern int g_result_poly;
extern TyKind g_result_ty;
/* Non-lambda proc `return`: a method owning a proc-return frame routes every
   `return` to a single exit (g_method_pr_label) that pops the frame, storing
   the value in g_method_pr_var; a returning proc's body longjmps to the home
   frame named by g_proc_return_home (a C expr reading the proc's capture). */
extern const char *g_method_pr_label;
extern const char *g_method_pr_var;
extern const char *g_proc_return_home;
int cmethod_takes_self_cls(Compiler *c, int si);
const char *emit_cmethod_self_cls_arg(Compiler *c, int mi, int recv_cls, Buf *b);
int reopen_self_defaults(Compiler *c, int mi);
int ctor_needs_self_defaults(Compiler *c, int initm, int argc);
void emit_ctor_alloc_init(Compiler *c, int cid, int initm, int argsNode, int call_id, Buf *b);
void emit_ctor_alloc_init_argv(Compiler *c, int cid, int initm, const int *argv, int argc, int argsNode,
                               Buf *b);
void emit_super_class_new(Compiler *c, int id, Buf *b); /* super in `self.new` */
extern const char *g_ctor_self;
extern const char *g_ctor_self_deref;
extern const char *g_arm_self;
extern const Scope *g_arm_scope;
extern int g_arm_depth;
extern const Scope *g_inl_dflt_scope;
extern int g_inl_dflt_nren;
extern const char *g_inl_dflt_self;
extern const char *g_inl_dflt_deref;
extern int g_inl_dflt_class;
extern int g_inl_dflt_depth;
typedef struct { const Scope *scope; int nren, cls, depth; const char *self, *deref; } InlDflt;
InlDflt inl_dflt_enter(const Scope *m, int nren, const char *self, const char *deref, int cls);
void inl_dflt_leave(InlDflt saved);
extern int g_expr_depth;
extern int g_proc_toplevel_return;
extern int g_exc_frame_depth;      /* live begin/rescue setjmp frames (see codegen_util.c) */
extern int g_method_pr_exc_depth;
extern int g_method_pr_ensure_depth;  /* g_ensure_depth at the return-funnel target (see codegen_util.c) */
extern int g_loop_exc_base;
extern int g_loop_rescue_base;
extern int g_loop_ensure_base;  /* g_ensure_depth at the innermost C-loop entry:
   a `next` crossing ensure regions opened INSIDE the loop defers through them
   (runs their bodies) before the C continue */
extern int g_brk_exc_base;
extern int g_block_brk_exc_base;
extern int g_brk_rescue_base;
extern int g_block_brk_rescue_base;
/* Return type of the method currently being emitted, so a tail/return value
   can be boxed when the method returns poly but the value is concrete. */
extern TyKind g_ret_type;
extern int g_c_ret_void;   /* the C function returns void (a fiber body) */
extern int g_c_ret_void;   /* the C function returns void (a fiber body) */
extern int g_fiber_body;   /* that fiber body's statements, or -1 */
extern const char *g_fn_pr_label;   /* real function's return funnel (see codegen_util.c) */
extern const char *g_fn_pr_var;
extern TyKind g_fn_ret_type;
const char *proc_ret_slot(void);
/* Set while emitting a self-recursive yield method (is_lowered_yield=1).
   Persists into inner proc literal bodies so { yield } forwards the block
   param (g_lowered_blk_name, or the synthetic __yblk__). */
extern int g_current_scope_is_lowered;
extern int g_ret_seeded;
extern const char *g_lowered_blk_name;
extern int g_yblk_celled;
extern int g_yield_lowered_fallback;
/* the enclosing inline's forwarded proc block (g_yield_proc_ref) and its
   slot type, parked for the spliced caller block the same way */
extern const char *g_yield_proc_ref_fallback;
extern TyKind g_yield_slot_ty_fallback;
extern const char *g_yield_lowered_blk_fallback;
extern const char *g_yield_proc_ref;
extern int g_yield_proc_expr;
extern int g_yield_proc_method;
extern const char *g_yield_proc_expr_ref;
void refuse_yield_string_copies(Compiler *c, int yargc, const int *yargv);
extern TyKind g_yield_slot_ty;
/* the forwarded proc one level further out still, the same pairing
   g_yield_self_fallback2 keeps for self: a literal block (B) handed to an
   inlined callee (M1) that itself hands B on into a second inlined callee
   (M2, `{ |x| yield x }`) needs B's OWN g_yield_proc_ref (the proc M1 was
   given, if M1 forwards ITS block by name via `.call`) to survive M2's own
   entry, which otherwise overwrites g_yield_proc_ref_fallback with M1's
   (irrelevant) value before B's body is ever spliced. */
extern const char *g_yield_proc_ref_fallback2;
extern TyKind g_yield_slot_ty_fallback2;

/* When set (SPINEL_LINE_MAP / SPINEL_DEBUG), emit `#line N "file"` directives
   at statement boundaries so a C compile error is reported against the
   original Ruby source line. Set once by codegen_program. */
extern int g_line_map;
void emit_current_line_directive(Compiler *c, Buf *b);
extern int g_debug;
extern int g_check_stores;
extern int g_gate_raise;  /* SPINEL_GATE_RAISE: raise NoMethodError at the
                             unresolved-call gate instead of a silent default. */
/* Emit a `#line` directive for node `id` into `b`, deduped against the last
   one emitted. No-op when g_line_map is off or the node has no line stamp. */
void emit_line_directive(Compiler *c, int id, Buf *b);

/* Ensure context stack for deferred `return` inside begin..ensure.
   When `return` appears in the body of a begin..ensure block, the return
   is deferred until after the ensure clause runs.  Each ensure clause
   pushes a context on this stack; emit_return uses the top to emit a
   deferred goto instead of a bare C `return`. */
#define MAX_ENSURE_DEPTH 32
/* retv_ty is the TYPE OF THE FRAME'S OWN SLOT, which is not always the
   enclosing method's return type: an inlined method's ensure frame is
   declared while g_ret_type is the INLINE's, and the block spliced at its
   yield then defers a `return` belonging to the OUTER method into it. Reading
   g_ret_type at the store site therefore asked the wrong function whether to
   box, and a String went into an sp_RbVal slot unboxed. */
typedef struct { int lid; int has_retval; int exc_base; TyKind retv_ty; int rescue_base; int catches;
                 int retv_o;   /* the frame's slot holds its nil beside the value (sp_oint) */ } EnsureCtx;
extern EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
extern int       g_ensure_depth;

/* One entry per rescue body currently being emitted. exc_base records
   g_exc_frame_depth at that body's entry so a non-local exit can tell which
   rescue bodies it crosses (those with exc_base >= the exit's frame base) and
   pop their sp_exc_handling entries (sp_rescue_sp). */
typedef struct { int exc_base; } RescueSave;
extern RescueSave g_rescue_save_stack[MAX_ENSURE_DEPTH];
extern int        g_rescue_save_depth;
/* Emit the pop that leaves the exception frames above pop_base AND pops the
   sp_rescue_sp handler for each rescue body crossed. Replaces the bare
   `sp_exc_top -= N;` emission at every non-local-exit site. When guard != NULL
   (deferred return), both are wrapped in `if (guard) { ... }`. Returns 1 if it
   emitted anything. */
void emit_rescue_pops_to(Buf *b, int rescue_base);
/* A retry's pops back to the body of the begin it restarts (codegen_stmt.c) */
void emit_retry_unwind(Buf *b);
/* The pops of a deferred return handed on to an enclosing ensure (codegen_stmt.c) */
void emit_ensure_chain_pops(Buf *b, const EnsureCtx *outer);
void emit_loop_unwind(Buf *b);
int emit_frame_unwind(Buf *b, int pop_base, const char *guard);
int rescues_crossed(int pop_base);
/* Pop the sp_rescue_sp handlers crossed (no frame pop), for the begin..ensure
   deferred return whose frame-pop text is special. */
void emit_cur_exc_restore(Buf *b, int pop_base);

/* First-class Proc support: each `proc {}` / `lambda {}` / `->{}` literal
   lowers to a standalone `static sp_int _proc_N(void *cap, sp_int *args)`
   function (the ABI sp_proc_call expects). Definitions accumulate in g_procs
   and prototypes in g_proc_protos during the main emission pass, then are
   flushed ahead of the method/main bodies that reference them. */
extern Buf g_procs;
extern Buf g_proc_protos;
/* Out-of-line poly dispatch functions (see pd_hoist in codegen_call.c):
   their prototypes beside the procs', their bodies after the procs. */
extern Buf g_pd_protos;
extern Buf g_pd_defs;
void pd_emit_used(const char *const *texts, int ntexts, Buf *protos, Buf *defs);
extern int g_proc_counter;
extern int g_needs_proc_poly_argslot; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
extern int g_fiber_counter;

/* Static regex-literal table: each distinct (source, flags) pair compiles once
   to an sp_re_pat_<i> global initialized in sp_tu_init(). */
extern char **g_re_src;
extern int *g_re_flg;
extern int g_re_count, g_re_cap;


/* A set of local names (borrowed pointers into the node table). */
typedef struct { const char **v; int n, cap; } NameSet;
/* While emitting a capturing proc's body: the cap struct's C type name and the
   set of captured names, so a read/write of a captured var routes to the cell
   held in `_cap` instead of a (non-existent) local. NULL outside such a body. */
extern const char *g_cap_struct;
extern NameSet *g_cap_names;
/* set when the program registers an at_exit hook; main()'s tail then calls
   sp_at_exit_run(), which runs them in reverse registration order (the
   runtime calls the same helper on the exit / abort / uncaught-raise paths). */
extern int g_needs_at_exit;
/* set when the program may use class-introspection machinery (user classes, or
   .class / is_a? / kind_of? / instance_of? / ancestors / superclass / === on
   builtins, or a builtin class constant used as a value). When clear, the
   sp_class_* / sp_poly_is_a / sp_user_exc_parent helper bank is not emitted --
   a minimal program like `p 42` carries none of it. */
extern int g_needs_class_machinery;
/* Set when sp_mark_user_globals marks at least one heap-typed user
   global/constant/class-ivar. When 0 the generated marker is identical to the
   runtime default (sp_re_mark_globals, installed by a constructor before main),
   so it -- and the sp_tu_init hook override -- are skipped. */
extern int g_has_user_global_marks;
/* Does this statement list leave the function at its tail -- a `return`, or a
   bare `raise` / `throw`? A splice of it into a value position has to supply a
   value of its own, because the C statement expression is then void. */
int stmts_diverge(Compiler *c, int stmts);
/* Distinct out-of-int64 integer literals, shared per TU (see codegen_util.c). */
extern char **g_bigl_val;
extern int g_bigl_n;
int bigl_intern(const char *v);
/* Whole-program feature presence, computed once before main is emitted, so the
   main() prologue can skip setup a trivial program never needs:
   g_emit_sym_rt -> sp_tu_init sets sp_sym_name_fn; g_uses_regex -> sp_tu_init
   wires the regex error handler; g_uses_argv -> the sp_argv copy loop runs.
   g_re_init_needed is the OR of the
   conditions that give sp_tu_init a body (symbols/regex/class-machinery/user
   global marks); when 0, neither sp_tu_init nor its call is emitted. */
extern int g_uses_symbols;
extern int g_uses_marshal;
extern int g_emit_sym_rt;      /* emit sp_dyn_syms / sp_sym_to_s / sp_sym_intern */
extern int g_emit_class_names; /* emit sp_class_to_s (the class-name table) */
extern int g_emit_obj_dispatch;/* emit sp_obj_inspect_sw / sp_obj_to_s_sw (user classes exist) */
extern int g_uses_program_name;/* $0 / $PROGRAM_NAME read somewhere */
/* `$~`, `$1`..`$9`, `$&`, `` $` ``, `$'`, `$+` or Regexp.last_match read
   somewhere. Only such a program has gsub / sub / scan record their last
   match (sp_re_track_last), which costs a copy of the match and its groups
   per call; one that never reads them keeps the plain scans. */
extern int g_reads_match_regs;
/* A sub! / gsub! beside which, or in whose receiver, argument or block, a
   sub of another call may run (sub_bang_reenters): the bang arm names its
   call here. Its plain form enters the runtime through the `_own` wrapper,
   and a block form (emit_gsub_block_expr) answers with the C local that
   says whether its loop found a match. */
extern int g_sub_bang_id, g_sub_bang_tm;
extern int g_stmt_cur;         /* codegen_stmt.c: the statement being emitted */
extern int g_gen_obj_hash;
extern int g_gen_obj_to_json;  /* a package wants obj reflection + >=1 user #to_json */  /* a package wants obj reflection + >=1 struct: emit+install sp_obj_to_hash */
extern int g_gen_obj_struct_values;  /* >=1 instantiated Struct (not Data): emit+install sp_obj_struct_values (poly member array) */
extern int g_gen_obj_to_h;  /* >=1 instantiated Struct/Data: emit+install sym-keyed sp_obj_to_h (poly #to_h) */
extern int g_gen_cls_answers;  /* a class-side name called on a boxed receiver: emit the sp_cls_* answers */
extern int g_gen_obj_with;  /* >=1 instantiated Data: emit+install sp_obj_with (poly Data#with) */
extern int g_uses_regex;
extern int g_uses_argv;
extern int g_uses_threads;
extern int g_uses_finalizers;
extern int g_uses_ensure;   /* the program holds an ensure clause: only then is an exception ever in flight (sp_inflight_cause) */
extern int g_has_user_cmp;
extern int g_has_user_binop;
extern int g_has_user_aset;
extern int g_has_user_coerce;
/* 1 if class k defines a #coerce this TU emits and can call: one parameter,
   no rest, an array return -- the [other, self] pair, poly or homogeneously
   typed. See analyze_util.c. */
int class_coerce_emittable(Compiler *c, int k);
int class_has_coerce_shape(Compiler *c, int k);
int class_has_to_str_shape(Compiler *c, int k);
int is_numeric_coerce_op(const char *op);
extern int g_has_user_to_io;
extern int g_gen_obj_hashkey; /* >=1 instantiated class defines #hash + #eql?: emit + install the obj hash/eql key hooks */
extern int g_gen_obj_valeq;   /* >=1 instantiated Struct/Data class: emit + install the value-== hook so containers compare them by value */
extern int g_re_init_needed;

const char *rename_local(const char *nm);


void emit_expr(Compiler *c, int id, Buf *b);
void emit_constant_slot(Compiler *c, int id, Buf *b);
void emit_expr_slot(Compiler *c, int node, TyKind slot, Buf *b);
void emit_tail_value(Compiler *c, int node, Buf *b);
void emit_typed_sink_text(Compiler *c, int node, TyKind slot, const char *text, Buf *b);
/* The store check (--check-stores): see codegen_util.c. */
TyKind store_value_kind(Compiler *c, int node);
int store_fits(Compiler *c, TyKind from, TyKind to);
void store_check(Compiler *c, int node, TyKind slot, const char *what, Buf *b);
void store_check_kind(Compiler *c, int node, TyKind from, TyKind slot, const char *what, Buf *b);
/* How emit_coerce may convert: see codegen_util.c. */
enum { CO_HOLD, CO_CONVERT };
void emit_coerce(Compiler *c, int node, TyKind slot, int how, const char *what, Buf *b);
void emit_coerce_text(Compiler *c, int node, TyKind from, TyKind slot, int how,
                      const char *text, const char *what, Buf *b);

/* ---- forward decls ---- */

int emit_bm_flat_args(Compiler *c, const int *argv, int argc, Buf *b);
int is_builtin_reopen(const char *name);
int is_exc_name(const char *n);
int class_is_exc_subclass(Compiler *c, int ci);
int class_is_exc_reopen(Compiler *c, int ci);
int class_has_exc_name(Compiler *c, int ci);
int any_exc_reopen(Compiler *c);
char **dsend_candidates(Compiler *c, int *out_n);
int emit_super_respond_to(Compiler *c, int id, Scope *s, Buf *b);
int exc_reopen_definers(Compiler *c, const char *mname, int *out, int max);
int emit_exc_reopen_pick_head(Compiler *c, const int *xr, int xn, const char *cls_expr, Buf *b);
int class_has_subclass(Compiler *c, int ocid);
int exc_has_user_msg_override(Compiler *c);
int exc_text_override_calls_super(Compiler *c);
int scope_calls_super(Compiler *c, int s);
int exc_has_nonstring_msg_override(Compiler *c);
int fi_fiber_stack_risk(Compiler *c);
const char *class_ruby_name(Compiler *c, int ci);
void obj_default_name(Compiler *c, int ci, char *name, size_t nn, char *arg, size_t na);
int anon_class_text(Compiler *c, int ci, const char *head, Buf *b);
int scope_def_line(Compiler *c, Scope *s);
const char *scope_def_file(Compiler *c, Scope *s);
const char *obj_str_cname(Compiler *c, int cid, int want_inspect);
const char *obj_str_mname(Compiler *c, int cid, int want_inspect);
int obj_str_ret_poly(Compiler *c, int cid, int want_inspect);
const char *exc_builtin_parent(Compiler *c, int ci);
void emit_method_cname(Compiler *c, Scope *s, Buf *b);
void emit_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_stmts(Compiler *c, int id, Buf *b, int indent);
void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar);
int  emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent);
int  emit_output_call(Compiler *c, int id, Buf *b, int indent);
TyKind emit_range_step_array(Compiler *c, int id, Buf *b);
int  emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_loop_body(Compiler *c, int body, Buf *b, int indent);
int  subtree_has_own_redo(const NodeTable *nt, int id);
int  subtree_has_own_next(const NodeTable *nt, int id);
int  subtree_owns_next(const NodeTable *nt, int body, int next);
int  next_is_block_value(Compiler *c, int next);
int  subtree_reads_local(const NodeTable *nt, int id, const char *name);
int  emit_inline_call(Compiler *c, int id, Buf *b, int indent);
int  emit_inline_expr(Compiler *c, int id, Buf *b);
void emit_cond(Compiler *c, int id, Buf *b);
void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen, int size_node);
int  needs_root(TyKind t);
int  kw_flag_static(Compiler *c, int node);
void emit_kw_flag(Compiler *c, int node, Buf *out);
int  emit_vis_refusal(Compiler *c, int id, Buf *b);
void emit_poly_vis_precheck(Compiler *c, int id, int tv, Buf *b);
/* The per-class `case` arms that store `src` into each candidate class's
   `base` writer slot through the object pointer text `objp` (codegen_stmt.c). */
void emit_boxed_writer_arms(Compiler *c, const char *base, const char *nm,
                            const char *objp, const char *src, TyKind at, int vnode, Buf *b);
int  method_is_void(Scope *s);
void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
void emit_boxed(Compiler *c, int node, Buf *b);
void emit_recv_rooted(Compiler *c, int recv, int t, const char *rootm, Buf *b);
int  push_recv_in_slot(Compiler *c, int recv, int argc, const int *argv, TyKind art);
void emit_rat_coerce(Compiler *c, int node, Buf *b);
void emit_super(Compiler *c, int id, Buf *b);
int  emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
void emit_callee_block_arg(Compiler *c, int id, const Scope *m, Buf *b);
void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);
/* emit_args_filled over the arguments `argv[0..argc)`, a run of some call's
   arguments (`raise Cls, msg` passes Cls.new the message alone); `argsNode`
   is the node a refusal names, -1 for none. */
void emit_args_filled_argv(Compiler *c, int callee_idx, const int *argv, int argc, int argsNode,
                           const char *lead, Buf *out);
void kw_plan(Compiler *c, Scope *m, int kwh, KwPlan *P);
/* The keyword error a plan finds statically, in CRuby's order, into `msg`;
   0 when it finds none. */
int kw_plan_error(const KwPlan *P, char *msg, size_t n);
/* A callee's positional count, keywords apart: required, and all of them. */
void positional_arity(Compiler *c, Scope *m, int *required, int *total);
void emit_unreached_splat_count(Compiler *c, Scope *m, const int *argv, int argc, int pos_argc,
                                const KwPlan *P);
/* Every argument of a call run ahead of it, in source order, each `**`
   operand converted where it stands (codegen_fold.c): a call planned so
   (KwPlan.args_first) or whose keywords run ahead (kwh_runs_ahead). Only
   the positionals, the keywords' reads went unguarded against a `**` that
   rewrites them, and a nil value, skipped as if it were a raise, ran after
   the keywords, or never when their check raised. */
void emit_args_run(Compiler *c, const int *argv, int argc);
/* Has the argument `node` run already, into the temp an override from
   the `from`th on names? */
int arg_ran_first(int node, int from);
/* The handle temp a shared String slot's argument took when it ran first
   (emit_arg_temp), -1 when there is none. */
int ran_first_handle(int node);
/* Bind node v to its value temp _t<t>, with th its handle's temp or -1
   (codegen_fold.c) */
void ran_first_bind(int v, int t, int th);
/* The temp the nil arm's head (emit_nil_target_head) ran operand `node`
   into, while the call it heads is emitted (Repr.head_held); -1 for any
   other node, or one the head ran for its effect alone (a nil). */
int head_held_temp(Compiler *c, int node);
/* That temp, when `text` (the operand as an arm rendered it) is the temp
   itself, unconverted: the arm reads it in place of a hold of its own. -1
   otherwise. */
int head_held_read(Compiler *c, int node, const char *text);
/* Is operand `node` one the head ran, boxed by a wrap of the head's temp
   (an immediate, or a pointer that temp roots, repr_box_form)? A hold of
   that box needs no root of its own. */
int head_held_box(Compiler *c, int node);
/* An operand an arm holds in a temp of its own ahead of what it runs next
   (its other operands, a box, its raise): `<ctype> _t<t> = <operand>;`
   and, when `root`, its root (sp_RbVal when `boxed` or ty is TY_POLY,
   else ty's C type), then `sep`, into b; t is the temp. An operand the nil arm's head
   ran (head_held_temp) is held by the head's temp already: read
   unconverted it is that temp, and nothing is written; boxed by a wrap of
   it (an immediate, a pointer the head's temp roots), the box is declared
   without a root of its own. Answers the temp the arm reads. */
int hold_operand(Compiler *c, int node, TyKind ty, int boxed, int t, int root, const char *sep, Buf *b);
/* hold_operand as a statement of its own in g_pre, at the current indent. */
int hold_operand_pre(Compiler *c, int node, TyKind ty, int boxed, int t, int root);
int emit_splat_gather(Compiler *c, Scope *m, const int *argv, const ArgLayout *L);
/* Does parameter i take the argument written at index i ahead of the first
   splat, however long the splats run? */
int gather_lead_placed(Compiler *c, Scope *m, const int *argv, int argc, int i);
void emit_gather_arity_check(Compiler *c, Scope *m, int ct);
void emit_gathered_param(Compiler *c, Scope *m, int i, int ct, Buf *out);
/* A byref parameter's value with no caller slot to lend: a rooted temp's address. */
void emit_lent_temp(const char *val, Buf *out);
/* The slot a String local lends a byref parameter; 0 when it has none. */
int emit_lent_local(LocalVar *lv, const char *vn, Buf *out);
/* The parameter of s a bare super in s hands pm's parameter j, or -1. */
int zsuper_param_source(Compiler *c, Scope *s, Scope *pm, int j);
int gathered_param_index(Compiler *c, Scope *m, int i, const char *len, char *idx, size_t cap,
                         int *npost_out);
extern unsigned g_yield_live_mask;   /* emit_proc_yield: positions whose targets take live bytes */
void refuse_yield_handle_args(Compiler *c, int id);
int emit_handle_var_ref(Compiler *c, int a, Buf *b);
unsigned inline_alias_params(Compiler *c, int mi, const int *argv, int pargc, const ArgLayout *L, int blk,
                             int tag, int *splice_tok);
void inline_alias_release(Compiler *c, Scope *m, unsigned alias_mask, int splice_tok);
int splice_store_open(Compiler *c, int w, Buf *b);
void splice_store_close(Compiler *c, int w, int n, Buf *b);
void emit_splice_store_text(Compiler *c, int w, const char *lhs, const char *val, int str, Buf *b);
void emit_inline_locals_aliased(Compiler *c, int mi, int tag, unsigned alias_mask, Buf *b, int din);
void emit_inline_alias_arg(Compiler *c, int av, Buf *b);
void emit_inline_bind_params(Compiler *c, Scope *m, int args, const int *argv, int argc,
                             const ArgLayout *L, unsigned alias_mask, int tag, int saved_nren,
                             int din, Buf *b);
/* A splat operand whose static type is nil or a scalar: Ruby spreads nil to
   nothing and any of the others to itself. */
int splat_operand_is_scalar(TyKind t);
/* A keyword key the callee has no parameter for: emits the ArgumentError and
   returns 1. Shared by emit_args_filled and the INLINE parameter binding, which
   walks parameters looking for keys and so could not see an unclaimed one
   (#4419). */
void emit_call_arity_check(Compiler *c, Scope *m, int argc, const int *argv);
/* The arity ArgumentError, in CRuby's words, for every binder (codegen_util.c):
   the expected range (max < 0 for no upper bound), the whole message, the
   callee's "; required keyword(s): ..." suffix, and the run-time raise and
   check of a count only the run time knows (sp_raise_arity). */
void arity_expected(char *out, size_t n, int min, int max);
void arity_message(char *out, size_t n, int given, int min, int max, const char *kw);
void arity_kw_suffix(const NodeTable *nt, int params, char *out, size_t n);
void scope_arity_kw_suffix(Compiler *c, const Scope *m, char *out, size_t n);
void emit_arity_raise(Buf *b, const char *given, int min, int max, const char *kw);
void emit_arity_check(Buf *b, const char *given, int min, int max, const char *kw);
int scope_refuses_keywords(Compiler *c, const Scope *m);
/* The keyword ArgumentErrors, `kind` "missing" or "unknown", naming `count`
   keywords already inspected and joined in `names` (codegen_util.c). */
void kw_error_message(char *out, size_t n, const char *kind, int count, const char *names);
void kw_names_add(char *list, size_t n, int *count, const char *inspected);
/* A literal Symbol (is_sym) or String key as #inspect writes it, and whether
   a Symbol's shows bare. */
void kw_key_inspect(const char *kn, int is_sym, char *out, size_t n);
int sym_name_plain(const char *s);
/* True when `m`'s `**kwrest` takes a key of any class (TY_POLY_POLY_HASH):
   some call may bring it one that is no Symbol (bind_call_params). */
int kwrest_any_key(Compiler *c, const Scope *m);
/* Collect a call's keywords no declared keyword parameter takes into a fresh
   temp of the hash `m`'s `**kwrest` is (sp_SymPolyHash, or sp_PolyPolyHash
   when kwrest_any_key); returns the temp id. */
int emit_kwrest_collect(Compiler *c, Scope *m, int kwh, int ds_hash_tmp, TyKind ds_hash_type, int argsNode);
/* The `unknown keyword` raise for a call's keyword hash, the last of its
   arguments `argv`, into `m`, once those have run (emit_call_arity_check). */
int emit_unknown_kwarg_raise(Compiler *c, Scope *m, const int *argv, int argc);
/* The argument of a String append (`<<` / `concat`), rendered for the append.
   An Integer -- typed OR boxed -- is a CODEPOINT, not its decimal digits. Shared
   because the rule was written twice and the second copy only had the typed half
   (#4425). */
void emit_str_append_arg(Compiler *c, int arg, const char *rtext, Buf *b);
void emit_str_force_encoding(Compiler *c, const char *name, const char *r, const int *argv, int argc, Buf *b);
int rest_shortfall_required(Compiler *c, Scope *m);
/* Emit a hash key, unboxing a poly value to the typed-hash's key type. */
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
/* The ENV name check's text around a key desugar_env_enum marked. */
const char *env_key_open(const NodeTable *nt, int n);
const char *env_key_close(const NodeTable *nt, int n);
int hash_key_misses(Compiler *c, int key, TyKind kt);
int hash_okey_miss(Compiler *c, int key, TyKind kt);
int hash_okey_form(Compiler *c, int key, TyKind kt);
void emit_hash_okey(Compiler *c, int key, Buf *b);
const char *hash_key_ctype(Compiler *c, int key, TyKind kt);
const char *hash_okey_sfx(Compiler *c, int key, TyKind kt);
void emit_hash_key_o(Compiler *c, int key, TyKind kt, Buf *b);
int hash_nil_key_stored(Compiler *c, int key, TyKind kt);
const char *conv_wrong_cls_name(TyKind t);
const char *conv_cls_name_of(Compiler *c, TyKind t);
TyKind obj_container_conv(Compiler *c, TyKind t, const char *conv, int *def);
int conv_takes_no_args(Compiler *c, int mi);
void emit_conv_dflt_args(Compiler *c, int mi, Buf *b);
void emit_str_pattern_expr(Compiler *c, int node, Buf *b);
void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b);
/* the form --repr-check records when emit_boxed_text boxes a kind t (RF_*) */
int emit_boxed_text_form(Compiler *c, TyKind t);
int hold_recv_open(Compiler *c, int recv, int boxed, const char *ctype, const char *rootm,
                   Buf *b, Buf *rb);
void emit_yielder_yield(Compiler *c, int id, const char *cn, Buf *b);
int emit_iter_bind_rest(Compiler *c, int block, int np, TyKind elem_t, const char *elem_src, Buf *b, int indent);
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b);
/* For a reference-backed builtin type (a genuinely nilable C pointer that can
   be NULL), return the name of its SP_BUILTIN_* class-id constant; else NULL.
   Such a value must box via sp_box_nullable_obj so a NULL becomes SP_TAG_NIL. */
const char *ty_nullable_builtin_id(TyKind t);
/* 1 iff a value of type t is a C pointer whose NULL is nil. */
int ty_null_is_nil(TyKind t);
void ty_traits_dump(Compiler *c);
int ty_traits_check(Compiler *c);
/* the per-kind spellings the boxers and the unboxers write (see ty_traits) */
const char *ty_box_fn(TyKind t);
const char *ty_box_nil_fn(TyKind t);
const char *poly_rhs_unbox_fn(TyKind slot);
const char *poly_sink_unbox_fn(TyKind slot);
const char *token_unbox_fmt(TyKind target);
/* A node of such a type may hold NULL: not a literal, not self. */
int node_may_be_null_nil(Compiler *c, int node);
/* A value stored where a String handle is wanted may be nil: one the
   never-nil list (ivs_never_nil) does not prove. */
int stored_value_may_be_nil(Compiler *c, int v);
/* `fn(recv)` with recv evaluated once, answering nil_c for a NULL recv. */
void emit_null_guarded_call(Compiler *c, int recv, TyKind rt, const char *fn, const char *nil_c, Buf *b);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
/* emit_unbox_text, but a nil-tagged poly lands on the slot's own nil (an int?
   or float? oint's nil flag) instead of the zero payload under the tag
   (#3412). */
void emit_unbox_nilable_text(Compiler *c, TyKind t, const char *expr, Buf *b);
/* `recv.attr ||= v` / `&&=` where the reader or the writer is a real `def`:
   emits the reader/writer pair as an expression, or answers 0 to leave the
   caller's direct-ivar shapes alone. See codegen_expr.c. */
void emit_orw_guard(Compiler *c, int v, TyKind slot, const char *cond, const char *lhs, int value_form, int indent, Buf *b);
void emit_slot_orw_value(Compiler *c, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                         const char *mark, Buf *b);
/* the same for an Integer / Float slot that holds its nil: `niltest` (its
   nil's C test, NULL = never nil), and a field's class / ivar / prefix so
   the store keeps the nil bit (codegen_expr.c) */
void emit_slot_orw_value_o(Compiler *c, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                           const char *niltest, int cid, int iv, const char *pfx, const char *mark, Buf *b);
/* `@x ||= v` at write node id; `obj.x ||= v` through the field (codegen_util.c);
   `mark` as emit_slot_orw_value's */
void emit_ivar_orw_value(Compiler *c, int id, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                         const char *mark, Buf *b);
void emit_attr_orw_value(Compiler *c, int cid, int iv, const char *pfx, TyKind t, int elems_handle,
                         const char *ref, int v, int is_or, const char *mark, Buf *b);
int ivar_orw_niltest(Compiler *c, int id, const char *ref, char *out, size_t cap, int *cid, int *iv, char *pfx, size_t pcap);
int emit_empty_literal_as(Compiler *c, int v, TyKind slot, Buf *b);
int emit_call_or_write_via_methods(Compiler *c, int id, int is_or, Buf *b);
/* Wrap a boxed expression in the --rbs seed assertion (a no-op macro without
   -DSP_RBS_CHECK) before it narrows into a seeded slot. */
int seeded_array_kind_mismatch(TyKind slot, TyKind vt);
void emit_array_store_value(Compiler *c, TyKind slot, int v, Buf *b);
void emit_rbs_checked_text(Compiler *c, TyKind slot, const char *slotname,
                           const char *expr, Buf *b);
void emit_block_param_share(Compiler *c, Scope *bs, LocalVar *lv, const char *name, Buf *pb, int rooted);
void emit_proc_literal(Compiler *c, int create, Buf *b);
int proc_slot_is_direct(TyKind t);
const char *proc_rest_name(Compiler *c, int create);
int proc_post_count(Compiler *c, int create);
const char *proc_post_name(Compiler *c, int create, int idx);
int proc_opt_count(Compiler *c, int create);
const char *proc_opt_name(Compiler *c, int create, int idx);
int proc_opt_value(Compiler *c, int create, int idx);
int proc_numbered_max(const NameSet *used);
int proc_has_rest(Compiler *c, int create);
void emit_hash_pairs_expr(Compiler *c, int recv, TyKind rt, const char *hn, Buf *b);
/* push the key of entry _t<ti> of hash _t<th> (key kind kt), boxed, onto
   the PolyArray _t<dest> */
void emit_push_hash_key(TyKind kt, int dest, int th, int ti, Buf *b);
TyKind comp_recv_type(Compiler *c, int recv);
int is_empty_array_lit(const NodeTable *nt, int id);
int proc_slot_is_ptr(TyKind t);
int proc_slot_via_poly(Compiler *c, TyKind t);
int cell_is_typed_ptr(Compiler *c, LocalVar *lv);
int call_returns_nullable_int(Compiler *c, int node);
int box_nullable_arg(Compiler *c, int v);
int ivar_assigned_in_initialize(Compiler *c, int k, const char *ivn);
int recv_may_be_sentinel(Compiler *c, int node);
int nil_answers_name(const char *n);
int emit_nullable_scalar_nil_only(Compiler *c, int id, Buf *b);
void emit_sg_activate(Compiler *c, int node, int recv, Buf *b, int indent);
int sg_activates_ci(Compiler *c, int node);
int subtree_has_param_named_pub(const NodeTable *nt, int id, const char *nm);
const char *past_open_parens(const char *s);
int text_diverges(const char *txt);
int inlined_local_needs_volatile(Compiler *c, LocalVar *lv);
int proc_local_needs_volatile(Compiler *c, int create, LocalVar *lv);
void emit_inlined_local_decl(Compiler *c, LocalVar *lv, const char *rn, Buf *b, int din);
/* A parameter a closure captures is a heap cell: the `lv_<uniq>` a call
   binds an argument to, for a later default to read, gets that cell too. */
void emit_pd_cell_alias_into(Compiler *c, LocalVar *plv, const char *uniq, Buf *b, int indent);
void emit_inlined_locals(Compiler *c, Scope *m, int tag, Buf *b, int din);
void emit_retf_return(int eid, int has_retval, Buf *b);
void emit_main_exit(Buf *b);
/* The assignment target for an inlined method's parameter, spelled by the same
   rule that declared it (a cell-promoted one is `(*_cell_x)`). See codegen.c. */
void emit_inlined_param_target(Compiler *c, Scope *m, const char *pname,
                               const char *rn, Buf *b);
const char *cell_scan_fn(TyKind t);
const char *cell_value_struct(TyKind t);
const char *cell_value_struct_empty(TyKind t);
const char *cell_value_struct_scan(TyKind t);
void emit_cell_elem_type(Compiler *c, LocalVar *lv, Buf *b);
const char *borrowed_string_type(const LocalVar *lv);
int splat_string_var(Compiler *c, const int *av, int ac, int *fs);
void refuse_super_splat(Compiler *c, int id, int target);
void refuse_yield_splat(Compiler *c, int blk, int yc, const int *yv);
void emit_proc_call_args(Compiler *c, int call, int argc, const int *argv, Buf *b, int force_poly);
/* the rooted holder of a by-value struct's box handed to a Proc, and the box
   written through it (codegen_call.c) */
int proc_arg_box_hold(Compiler *c, TyKind at, Buf *decl, int indent);
void emit_proc_arg_boxed(Compiler *c, TyKind at, const char *tn, int hold, Buf *b);
int call_args_need_spread(const NodeTable *nt, const int *argv, int argc);
int emit_spread_args(Compiler *c, const int *argv, int argc);
int emit_spread_args_kw(Compiler *c, const int *argv, int argc, char *kwpos, size_t kwsz);
int emit_spread_args_into(Compiler *c, const int *argv, int argc, const char *kwflag);
void emit_proc_yield(Compiler *c, const char *ref, int yargc, const int *yargv, Buf *b);
/* Unbox the boxed proc result (_sp_proc_poly_ret) to a call's inferred type. */
void emit_proc_ret_unbox(Compiler *c, TyKind rty, Buf *b);
void emit_case_expr(Compiler *c, int id, Buf *b);


/* ---- cross-part function declarations (generated by the split) ---- */
/* buf_putn / buf_puts / buf_printf are declared earlier (next to Buf),
   so the inline emit_indent can use buf_puts. */
/* Map Prism regex flag bits (IGNORE_CASE=4, EXTENDED=8, MULTI_LINE=16) to the
   engine's RE_FLAG_* (IGNORECASE=1, MULTILINE=2, DOTALL=4, EXTENDED=8); Ruby's
   /m means dot-matches-newline -> MULTILINE|DOTALL = 6. */
int re_engine_flags(int pf);
/* Find or add a RegularExpressionNode literal; returns its table index, or
   -1 if the node isn't a static regex literal. */
int re_lit_index(Compiler *c, int nid);
int re_lit_node(Compiler *c, int nid);
/* Codegen's whole-program lookups, built once and rebuilt when the node or
   scope count changes (#4966). cg_scope_nodes: the node ids whose nscope is
   `si`, ascending. cg_block_owner: the lowest node whose "block" ref is
   `blk`, or -1. A name-keyed int memo for answers fixed by the node table. */
const int *cg_scope_nodes(Compiler *c, int si, int *n);
int cg_block_owner(Compiler *c, int blk);
typedef struct CgMemoEnt CgMemoEnt;
typedef struct {
  CgMemoEnt **tab; const NodeTable *nt; int count; int nscopes;
  int (*touches)(Compiler *c, int id);   /* can an appended node change an answer? */
} CgMemo;
int cg_memo_get(Compiler *c, CgMemo *m, const char *key, int tag, int *val);
void cg_memo_put(CgMemo *m, const char *key, int tag, int val);
/* The unescaped source of a regex literal or a constant bound to one (for
   capture detection). Returns NULL when nid is not a resolvable regex. */
const char *re_lit_src(Compiler *c, int nid);
void emit_interp(Compiler *c, int id, Buf *b);
int interp_is_literal_fold(const NodeTable *nt, int id);
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b);
int nameset_has(NameSet *s, const char *nm);
void nameset_add(NameSet *s, const char *nm);
/* Emit the C lvalue for local `name` in the current emission context: a
   captured var inside a proc body -> the cell in _cap; a cell local in its
   enclosing scope -> `(*_cell_x)`; otherwise the plain `lv_x`. Reads and
   writes share this (a cell deref is a valid lvalue). */
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b);
void emit_poly_lift_ref(const char *ref, Buf *b);
int strbuf_marked_yields_handle(Compiler *c, int v);
void emit_scope_local_ref(Compiler *c, Scope *s, const char *name, Buf *b);
void emit_typed_elem_value(Compiler *c, int node, TyKind et, Buf *b);
void emit_block_locals_reset(Compiler *c, int blk, Buf *b, int indent);
const char *resolve_class_alias(Compiler *c, const char *cname);
/* Emit `sp_Proc *` reference to the synthetic __yblk__ param of a lowered
   self-recursive yield method.  If we are inside an inner proc literal that
   captures __yblk__ via a cell, cast back from the sp_int cell slot. */
void emit_yblk_ref(Buf *b);
/* Emit the lead of a tail value: `return ` or `<result> = `. */
void emit_tail_lead(Buf *b);
const char *rename_local(const char *nm);
const char *rename_local_cell(const char *nm);
int sb_shim_shadow(const char *name, const char *rn);
typedef struct { TyKind type, shim_ty; int shim_lift; } SbShimSave;
SbShimSave sb_shim_enter(LocalVar *lv);
void sb_shim_leave(LocalVar *lv, SbShimSave sv);
void sb_shim_lift(Compiler *c, int node);
void sb_shim_drop(Compiler *c, int node);
/* `unsupported` never returns: it longjmps to the codegen driver's per-unit
   recovery (see g_unsup_recover) when one is armed, else exits. Marked noreturn so every caller's
   "this construct is unsupported" guard correctly treats the code after it as
   unreachable. */
int collect_mode(void);            /* 1: refusals are collected per unit (always) */
int collect_emit_anyway(void);     /* 1 under SP_COLLECT_ERRORS: emit the rest, exit 0 */
/* What codegen decided at a node (--emit-types only, #4522). A call is
   ND_DIRECT (one statically bound C call or an inline builtin), ND_SWITCH
   (a switch over the classes or tags the receiver can hold) or ND_BOXED
   (a runtime helper over the boxed value, or an unresolved call); a block
   is ND_BLOCK_PROC when it became a function of its own (a proc, a fiber
   or thread body), else it was spliced in place. */
enum { ND_NONE = 0, ND_DIRECT, ND_SWITCH, ND_BOXED, ND_BLOCK_PROC };
extern unsigned char *g_ndecide;
extern int g_ndecide_cap;
extern int g_nd_call_id;    /* the CallNode being emitted, for emitters without the id */
void nd_stamp(int id, int kind);
/* The def a call bound to (--emit-types, #4557): `mi` the callee scope,
   `owner_ci` its class or -1 for a top-level def; `add` appends a switch arm
   (deduplicated) where 0 records the one direct target. */
extern char **g_ndtarget;
extern int g_ndtarget_cap;
void nd_callee(Compiler *c, int id, int mi, int owner_ci, int add);
/* --plan-check: a user-method binding codegen made at node id (nd_callee
   reports every one; the splices and super, which stamp nothing for
   --emit-types, call it directly), and the end-of-compile report comparing
   them with inference's (codegen_util.c) */
void ucall_observe(Compiler *c, int id, int mi, int owner_ci, int add);
void ucall_report(Compiler *c);
/* --nil-check (#7444): the calls nil_recv_guard decided, and the
   end-of-compile report holding the analysis's nil fact against the codegen
   helpers' answers there (codegen_call.c) */
void nil_check_seen(int id);
void nil_check_report(Compiler *c);
/* --plan-check: codegen emitted the call node id (whatever it bound) */
void ucall_emitted(int id);
/* --plan-check: codegen emitted the visibility refusal for node id */
void ucall_refused(int id);
/* how deep emit_inline_call_x is in spliced bodies (codegen_iter.c) */
int inline_splice_depth(void);
/* One refusal: where and what. Recorded in order for --emit-types. */
typedef struct { const char *file; int line; const char *msg; } SpDiag;
extern SpDiag *g_diags;
extern int g_ndiags;
extern jmp_buf g_unsup_recover;    /* per-unit recovery point, armed by the driver */
extern int g_unsup_armed;          /* nonzero while a recovery point is live */
extern int g_unsup_quiet;          /* record a refusal without printing it (codegen.c decides) */
int defer_refusals(void);
int time_parse_refusal(Compiler *c, int id, char *msg, size_t cap);
int emit_stmt_or_defer(Compiler *c, int st, Buf *b, int indent);
extern int g_unsup_probe;          /* silent emittability probe (drop a dynamic-send arm) */
extern int g_open_defaults;        /* parameter defaults being emitted, innermost last */
/* The compiled conversion method a statically-typed user object reaches at a
   typed slot (CRuby's implicit conversion protocol), or -1: `conv` is "to_str"
   or "to_int" and `want` the slot's type, which the method's declared return
   must be. *def_out receives the defining class. Shared by the emitter and the
   native-argument check so both agree on which objects may cross. */
int obj_conv_method(Compiler *c, TyKind t, const char *conv, TyKind want, int *def_out);
/* A String comparison's operand: the shape test both the type rules and the
   arms ask, the conversion emitted on a spilled temp, and the prologue the
   arms share. See codegen.c. */
int str_cmp_conv_shape(Compiler *c, int node);
int str_cmp_bound_foreign(Compiler *c, int n);
/* a call on a boxed receiver dispatched by the receiver's class (codegen_call.c) */
int emit_poly_method_dispatch(Compiler *c, int id, Buf *b);
void emit_str_cmp_conv(Compiler *c, int node, int tmp, Buf *b);
void emit_str_cmp_prologue(Compiler *c, const char *rtxt, int operand,
                           int *tr, int *to, int *ts, Buf *b);
/* 1 iff any class defines a usable #to_int / #to_str -- see codegen.c. */
int prog_has_conv_method(Compiler *c, const char *conv, TyKind want);

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what);
int unsup_message(Compiler *c, int id, const char *what, int self_ci, char *msg, size_t cap);
void refuse_from_plan(Compiler *c, int id, int from, const char *site);
/* --defer-refusals: the plan refusals the passes ahead of emission make
   otherwise, raised where call `id` is emitted */
void refuse_prepass_at_emit(Compiler *c, int id);
/* a `def recv.m` reading self on a receiver no user class's instance holds
   (analyze_scope marks it): refused where the def or its body is emitted */
void sg_refused_def(Compiler *c, int def);
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg);

/* Compile a regexp literal with the engine and throw the result away, to
   refuse at COMPILE time a pattern that would only have failed at the
   program's startup. Returns the engine's own message, or NULL when the
   pattern reads. Defined in src/re_lit_check.c, which is what links the
   engine into the compiler. */
const char *sp_re_literal_error(const char *src, int len, int flags);
/* Returns a negative cls_id for well-known builtin class/module names,
   or 0 if the name is not a recognized builtin class. */
/* The boxed side channel's slot count, as lib/sp_proc.h defines it: the
   emitters that publish arguments into it and the arity they decline past
   must agree with the runtime, not carry their own copy of the number. */
#ifndef SP_PROC_ARG_SLOTS
#define SP_PROC_ARG_SLOTS 64
#endif
int builtin_class_id(const char *name);
int builtin_class_parent_id(int id);   /* analyze_util.c */
int is_builtin_class_name(const char *n);
int is_builtin_module_name(const char *n);
int builtin_module_at(int i, const char **name, int *id);
int is_builtin_exception_name(const char *n);
const char *superclass_builtin_exc_name(const NodeTable *nt, int sc);   /* analyze_util.c */
const char *errno_canonical_name(const char *n);   /* analyze_util.c */
int is_syserr_family_name(const char *n);           /* analyze_util.c */
int class_is_syserr(Compiler *c, int ci);          /* codegen_call.c */
void emit_syserr_call(Compiler *c, int id, const char *fn, const char *lead,
                      int argc, const int *argv, Buf *b);   /* codegen_call.c */
int class_inherits_builtin_exception(Compiler *c, int ci);  /* analyze_util.c */
/* The class name a runtime match (is_a?/===/when) should test against: the
   QUALIFIED path name when it names a known builtin (exception) class --
   raised exceptions carry their qualified name, so "Errno::ENOENT" must not
   shrink to "ENOENT" -- and the leaf otherwise (user classes register by
   leaf). */
static inline const char *isa_match_name(const NodeTable *nt, int arg, char *buf, size_t bufsz) {
  const char *l = isa_const_name(nt, arg);
  const char *q = isa_const_qualname(nt, arg, buf, bufsz);
  if (q && l && !sp_streq(q, l) &&
      (is_builtin_exception_name(q) || builtin_class_id(q) != 0)) return q;
  return l;
}
const char *c_type_name(TyKind t);
int is_scalar_ret(TyKind t);
/* A constant whose slot has a presence flag, cst_NAME__set (LocalVar.const_early):
   every assignment sets it (emit_const_flag_set) and the reads that see
   whether the constant exists test it. */
int const_has_flag(const LocalVar *lv);
void emit_const_flag_set(const LocalVar *lv, const char *key, Buf *b, int indent);
/* The test of a read of node id that raises NameError while the constant is
   unset, as a `(test, ...)` prefix around the read: open before it, close
   after. They emit nothing for a constant with no flag. */
void emit_const_check_open(Compiler *c, int id, const LocalVar *lv, Buf *b);
void emit_const_check_close(Compiler *c, int id, const LocalVar *lv, Buf *b);
int emit_const_recv_wrap(Compiler *c, int id, Buf *b);
int emit_const_recv_stmt_wrap(Compiler *c, int id, Buf *b, int indent, int tail);
void emit_const_slot_read(Compiler *c, int id, const LocalVar *lv, const char *key, int slot, int own, Buf *b);
const char *ffi_c_type(const char *spec);
/* Map an FFI type spec string to the C type used in extern prototypes.
   Uses standard C types to avoid conflicting with system headers. */
const char *ffi_cb_arg_ctype(const char *spec);
void ffi_extern_name(Compiler *c, int fi, Buf *out);
int ty_is_struct_valued(TyKind t);   /* see codegen_util.c: struct passed by value */
const char *native_c_type(const char *spec);
const char *default_value(TyKind t);
const char *default_value_from_compiler(Compiler *c, TyKind t);
void emit_slot_truthy(TyKind t, const char *ref, Buf *b);
void emit_sentinel_bind(Compiler *c, TyKind t, int node, char *ref, size_t cap, Buf *b);
const char *typed_elem_box_fn(TyKind t);
const char *nil_store_sfx(Compiler *c, const char *k, int node);
#define NIL_STORE_BOXED (-2)   /* nil_store_sfx's node for a boxed element */
int enum_builtin_node(Compiler *c, int node);
const char *nomethod_head(const char *name);
const char *enum_walk_name(Compiler *c, int id, int recv, const char *name);
void emit_walk_arity_raise(Compiler *c, int id, int recv, const char *name, int t, int indent, Buf *b);
int typed_array_lit_flag_free(Compiler *c, int node);
void emit_may_nil_text(Compiler *c, int node, TyKind t, const char *arr, Buf *b);
const char *raise_tail_value(TyKind t);
/* A builtin type that certainly has no #to_ary, and its class name for the
   "no implicit conversion of X into Array" TypeError (codegen_call_recv.c). */
int conv_to_ary_impossible(TyKind t);
const char *conv_builtin_class_name(TyKind t);
void ty_traits_render(const char *cell, const char *expr, Buf *b);
const char *raise_tail_value_c(Compiler *c, TyKind t);
const char *array_times_type_error(TyKind at);
void emit_bigint_operand_ext(Compiler *c, int node, Buf *b);
const char *nil_value(TyKind t);
int cvar_defined_probed(Compiler *c, const char *nm);
void emit_cvar_set_flag(Compiler *c, int cid, const char *nm, int as_expr, Buf *b);
void emit_cvar_set_flag_after(Compiler *c, int cid, const char *nm, Buf *b);
extern int g_ivar_nil_guarded_id;
int ivar_nil_recv_guard(Compiler *c, int id, int *recv_out);
/* the same for any receiver the call guards: an ivar as above, or a param or
   local that can hold nil (codegen_call.c) */
int nil_recv_guard(Compiler *c, int id, int *recv_out);
void emit_ivar_nil_guard(Compiler *c, int id, int recv, Buf *b, int indent);
int emit_ivar_nil_guarded(Compiler *c, int id, Buf *b, int indent,
                          int (*fn)(Compiler *, int, Buf *, int));
/* a statement-position call on a builtin receiver that may be nil, behind
   its nil arm (cplan_nil, #7444); 0 when it has none */
int emit_nil_target_stmt(Compiler *c, int id, Buf *b, int indent);
const char *local_init_value(Compiler *c, LocalVar *lv);
int local_nil_test(Compiler *c, LocalVar *lv, const char *ref, Buf *out);
/* Append the C type name for `t` to `b` (objects need the class name). */
const char *class_ctype(Compiler *c, int cid);
void emit_native_rest_args(Compiler *c, const NativeMethod *m, int argc, const int *argv, Buf *b);
void native_arg_check(Compiler *c, int id, const char *what, NativeMethod *m,
                      int argc, const int *argv);
int emit_native_splat_call(Compiler *c, int id, int cid, const char *name, int recv,
                           int argc, const int *argv, Buf *b);
int emit_native_count_mismatch(Compiler *c, int id, int cid, const char *name, int kind,
                               int recv, int argc, const int *argv, Buf *b);
void emit_ctype(Compiler *c, TyKind t, Buf *b);
const char *ptr_array_stamp(Compiler *c, TyKind t);   /* "SP_PTR_ELEM_x, cls" for sp_box_ptr_array_k (#4486) */
/* "Int" / "Str" / "Float" for the sp_<K>Array_* runtime family. */
const char *array_kind(TyKind t);
/* "Poly" / "Ptr" / array_kind, for a loop that walks the container. "Ptr" is
   a nested numeric table (sp_PtrArray of row pointers). */
const char *array_iter_kind(TyKind t);
const char *array_to_poly_fn(TyKind t);
int call_never_returns(Compiler *c, int id);
/* comp_ntype for a fold seed, with an empty `[]` / `{}` literal resolved to
   its container kind rather than left TY_UNKNOWN (see types.c). */
TyKind fold_seed_ntype(Compiler *c, int node);
/* `sum(seed)` through sp_poly_sum_seed, with both operands boxed into rooted
   temporaries in receiver-then-seed order (see codegen_util.c). A negative
   seed denotes the implicit Integer zero. */
void emit_poly_sum_seed(Compiler *c, int recv, int seed, Buf *b);
void emit_c_escaped_n(Buf *b, const char *s, size_t len);
void emit_c_escaped(Buf *b, const char *s);
/* A poly RHS assigned into a scalar slot needs an unbox. The statement form
   (emit_assign) and the expression form (`x = v` in value position) share the
   rule; #3303 was the expression form missing it. Returns 1 when it emitted. */
int emit_poly_rhs_coerced(Compiler *c, TyKind slot, int v, Buf *b);
/* An empty `[]` / `{}` into a typed slot builds at the slot's representation
   rather than the literal's default (#4054). Returns 1 when it emitted. */
int emit_empty_container_for_slot(Compiler *c, int v, TyKind slot, Buf *b);
/* A frozen literal from its C-escaped bytes: a reference to the one file-scope
   object for that content, whose definition fzl_emit_defs writes. */
void emit_frozen_literal(Buf *b, const char *esc, size_t esc_len, size_t raw_len);
void fzl_emit_defs(const char *t, Buf *out, int handles);
/* Emit a Ruby string literal. len is the true byte count (may exceed strlen
   when the string contains embedded NUL bytes). */
/* What a `round`-family call's trailing keyword hash says, as far as it can
   be read at compile time. `half` is the node the tie-break mode was written
   as; a `**` source's keys are only known at run time and are marked splat;
   `unknown` is the ArgumentError message for the keys that are neither --
   `round` takes no keyword but `half:`, and CRuby names every other one. A
   key spelled some other way leaves the set unreadable, and nothing may be
   called an unknown keyword on the strength of what cannot be read. */
typedef struct {
  int node;                   /* the KeywordHashNode itself */
  int half;                   /* value node of the last literal `half:`, or -1 */
  int nelem;
  int nsplat;                 /* how many `**` sources it carries */
  char unknown[256];          /* the ArgumentError message, or empty */
  int nunknown;
} RoundKw;
/* What one element is: its value node, and which of the three kinds of key it
   was written with. Read from the node each time rather than cached in the
   struct, so a call may carry any number of keywords -- a fixed cap meant a
   hash past it was read as empty, which silently dropped its `half:` and let
   an unknown keyword through. */
void round_kw_read(Compiler *c, int kwh, RoundKw *o);
void emit_round_kw_effects(Compiler *c, const RoundKw *kw, Buf *b);
int emit_round_kw_binds(Compiler *c, const RoundKw *kw, Buf *b);

void emit_str_literal_n(Buf *b, const char *content, size_t len, int frozen);
void emit_str_literal(Buf *b, const char *content);
void emit_str_literal_src(Buf *b, const char *content, size_t len, int frozen);
/* Emit a catch/throw tag (a Symbol or String literal) as a `const char *`.
   The same literal text is produced for both catch and throw sites so the
   runtime's strcmp tag match succeeds. Falls back to a runtime string expr. */
int emit_catch_tag(Compiler *c, int id, Buf *b);
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
/* Strip ParenthesesNode wrappers to reach the inner expression. */
int unwrap_parens(Compiler *c, int id);
int hoisted_value_is_stmt(Compiler *c, int id);
/* Collect a String `<<` chain's args outermost-first (max 64); *base gets
   the node the chain bottoms out at. Returns the link count. */
int str_append_chain(Compiler *c, int recv, int *chain, int *base);
int emit_string_handle_append(Compiler *c, int id, Buf *b, const char *name, int recv, int argc, const int *argv);
int emit_str_append_chain_handle(Compiler *c, int id, Buf *b);
int kwh_only_spreads(const NodeTable *nt, int kwh);
void emit_str_array_handles(Compiler *c, int v, Buf *b);
int iter_src_as_handles(Compiler *c, int n);
int iter_filter_src_as_handles(Compiler *c, int id, int recv);
void emit_boxed_iter_src(Compiler *c, int n, Buf *b);
int elem_param_is_handle(const LocalVar *lv, TyKind et);
const char *int_arith_fn(const char *op);
const char *bigint_arith_fn(const char *op);
/* Mangle a Ruby method name into a C identifier: `?`->_p, `!`->_bang,
   `=`->_set, anything else non-identifier -> `_`. Returns a static buffer
   (one live result at a time -- fine since each use is consumed inline). */
const char *mc(const char *name);
/* The class stem to compose a REOPENED builtin's method name with: the
   plain c_name, or one with an `_oc` suffix where the runtime already has a
   function of that exact spelling (sp_String_length). */
const char *mc_reopen_cls(Compiler *c, int class_id, const char *mname);
const char *mc_top(Compiler *c, const char *name);
const char *iv_c(const char *name);  /* ivar/member name -> valid C field id (#3110) */
#define IV_C_MAX 64  /* iv_c's longest result; a longer name is shortened with a hash */
/* A method scope is shadowed (and must not be emitted) when a later
   scope redefines the same (class, name, is_cmethod) -- a reopened class
   where the last definition wins, matching comp_method_in_class. A top-level
   `def` is shadowed by a later top-level `def` of the same name. */
int scope_is_shadowed(Compiler *c, int s);
int  scope_needs_proc_form(Compiler *c, int s);
int  scope_proc_form_of(Compiler *c, int s);
int  expr_is_held_ref(Compiler *c, int node);   /* a read of a held object: no root needed */
/* Reads kept by their existing frame slot across later argument evaluation. */
int read_of_fixed_param(Compiler *c, int node);
int read_unbound_in_stmt(Compiler *c, int node);
int  typed_array_src_held(Compiler *c, int node);   /* a typed Array held while it is boxed */
int  proc_form_live(Compiler *c, int s);
int  proc_form_source(Compiler *c, int s);
int  ctor_site_on_cycle(Compiler *c, int id, int initm);
int  ctor_init_proc_form(Compiler *c, int cid);
int scope_has_callable_symbol(Compiler *c, int s);
int scope_toplevel_included(Compiler *c, int s);
int scope_uses_ivars(Compiler *c, int mi);
int emit_forwarded_proc_arg(Compiler *c, int blk_node, Buf *b);
const char *forwarded_real_proc(int blk0, int blk);
int emit_block_arg_proc(Compiler *c, int fe, Buf *b);
void emit_obj_dispatch_key(Compiler *c, int cid, const char *selfptr, Buf *b);
int struct_kwarg_value(Compiler *c, int kwh, const char *name);
/* Value-equality family: operands in the same nonzero family compare by value;
   different nonzero families are never == (Ruby does no cross-type coercion,
   except int/float which share family 1). 0 = not a simple comparable type. */
int eq_family(TyKind t);
/* Compile-time `is_a?` for a concrete builtin receiver type: 1 yes, 0 no,
   -1 not determinable here. `exact` is instance_of? (no ancestor match). */
int ty_matches_class(TyKind t, const char *cn, int exact);
int builtin_reopen_includes_module(Compiler *c, int mod);
void emit_method_call(Compiler *c, int id, Buf *b);
/* A receiverless call the enclosing class's own chain answers (see
   codegen_call.c): the Kernel arms must stand down for it. */
int bare_call_class_owned(Compiler *c, int id);
/* Resolve a forwarded `&blk` (a BlockArgumentNode handing on the active block
   param) to the caller's already-inlined block g_block_id (-1 when no block was
   given, so the forward becomes a nil block). Any other block node is returned
   unchanged. Lets a forwarded block be materialized by emit_proc_literal. */
int resolve_forwarded_block(Compiler *c, int block);
int emit_hash_collect_expr(Compiler *c, int id, Buf *b);
int emit_iter_autosplat(Compiler *c, int block, TyKind rt, const char *elem_src, int indent);
int block_tail_is_unresolved(Compiler *c, int node);
int emit_iter_value_expr(Compiler *c, int id, Buf *b);
void set_enum_walk_result(int tmp);
int iter_value_answers_recv(Compiler *c, int id);
int sn_guard_pending(Compiler *c, int id);
int emit_takewhile_with_index(Compiler *c, int id, Buf *b);
int emit_transform_hash_expr(Compiler *c, int id, Buf *b);
int emit_bsearch_expr(Compiler *c, int id, Buf *b);
int emit_poly_uniq_block(Compiler *c, int id, Buf *b);
int emit_gsub_block_expr(Compiler *c, int id, Buf *b);
int subtree_reads_match_globals(Compiler *c, int root);
int emit_sum_block_expr(Compiler *c, int id, Buf *b);
int emit_sum_block_poly_expr(Compiler *c, int id, Buf *b);
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b);
int emit_product_inspect_expr(Compiler *c, int id, Buf *b);
int emit_step_array_expr(Compiler *c, int id, Buf *b);
int emit_inject_expr(Compiler *c, int id, Buf *b);
int emit_reduce_block_expr(Compiler *c, int id, Buf *b);
int emit_sortby_expr(Compiler *c, int id, Buf *b);
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b);
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b);
int emit_lazy_class_expr(Compiler *c, int id, Buf *b);
int emit_lazy_pipeline_expr(Compiler *c, int id, Buf *b);
int lazy_alias_write_suppressible(Compiler *c, int write);  /* lazy-alias write whose uses all force it */
int emit_lazy_size_expr(Compiler *c, int id, Buf *b);
int emit_native_ctor(Compiler *c, int id, int ci, int argc, const int *argv, Buf *b);
void emit_block_value_into(Compiler *c, int block, const char *dest,
                           int want_poly, int indent);
int emit_block_cond_next(Compiler *c, int block, int indent, Buf *out);
int fold_body_has_next(Compiler *c, int node);  /* a `next` of the body's own, not a nested block's */
int iter_step_needs_frame(Compiler *c, int block);
int emit_iter_step_stmts(Compiler *c, int body, Buf *b, int indent, const char *sep);
void emit_iter_step_body(Compiler *c, int block, Buf *b, int indent);
void emit_iter_loop_stmts(Compiler *c, int body, Buf *b, int indent);
/* One step of a builtin iterator's block (emit_iter_step_open). */
typedef struct { int block, want_poly, slot; TyKind slot_ty; } IterStep;
void emit_iter_step_open(Compiler *c, int block, int want_poly, int indent, IterStep *st);
TyKind emit_iter_step_tail(Compiler *c, const IterStep *st, Buf *vb);
void emit_iter_step_cond(Compiler *c, const IterStep *st, int raw, Buf *cb);
int emit_collect_expr(Compiler *c, int id, Buf *b);
int emit_with_index_expr(Compiler *c, int id, Buf *b);
int emit_enum_with_index_expr(Compiler *c, int id, Buf *b);
int emit_enum_find_expr(Compiler *c, int id, Buf *b);
int emit_each_with_index_chain(Compiler *c, int id, Buf *b);
int emit_each_with_index_terminal(Compiler *c, int id, Buf *b);
int emit_chunk_while_expr(Compiler *c, int id, Buf *b);
int emit_chunk_family_poly_expr(Compiler *c, int id, Buf *b);
int emit_chunk_family_enum_expr(Compiler *c, int id, Buf *b);
int lazy_endpoint_is_infinite(Compiler *c, int right); /* endless / Float::INFINITY literal end */
int emit_chunk_first_class_expr(Compiler *c, int id, Buf *b);
int emit_cycle_bounded_expr(Compiler *c, int id, Buf *b);
int emit_predicate_expr(Compiler *c, int id, Buf *b);
int emit_find_index_poly_expr(Compiler *c, int id, Buf *b);
void emit_autosplat_params(Compiler *c, int block, int np, int elem_temp, int indent);
int emit_tuple_block_params(Compiler *c, int id, int block, const char *tuple_src, Buf *out);
int poly_block_call_needs_dispatch(Compiler *c, int id);
void emit_obj_alloc_expr(Compiler *c, int cid, Buf *b);
void emit_own_class_alloc(Compiler *c, int id, int base, Buf *b);
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out);
int declare_default_locals(Compiler *c, Scope *m, int dnode);
int arg_wants_root(Compiler *c, TyKind pt, int provided);
int emit_nilfree_operand(Compiler *c, int v, const char *op, int left, const char *lhs, Buf *b);
int emit_nilfree_cmp_operand(Compiler *c, int v, const char *op, int left, const char *lhs, Buf *b);
void emit_rooted_operand(Compiler *c, TyKind pt, int provided, const char *expr, Buf *out);
int arg_read_converts(Compiler *c, TyKind pt, int provided);
void emit_rooted_conversion(Compiler *c, TyKind pt, const char *expr, Buf *out);
int prelude_is_held_decls(const char *p);
int arg_slot_for_param(Compiler *c, Scope *m, int idx, int argc);
/* 1 when a parameter default reads an earlier parameter: it must be evaluated
   with that parameter bound (see emit_args_filled). */
int default_refs_earlier_param(Compiler *c, Scope *m);
int opt_before_required(Compiler *c, Scope *m);
void emit_pd_param_bind(Compiler *c, Scope *m, int i, int uid, const char *val, char *name, size_t cap);
/* `(sp_Parent *)` when an object value flows into an ancestor-typed slot; the
   layouts match by construction, but C needs the cast spelled (#3418). */
void emit_obj_upcast_prefix(Compiler *c, TyKind slot, TyKind val, Buf *b);
/* Value node for keyword `name` inside a KeywordHashNode, the last when the
   key is written twice, or -1. */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname);
int callee_has_kwarg(Compiler *c, Scope *m, const char *name);
int callee_param_is_declared_kwarg(Compiler *c, Scope *m, const char *name);
int rest_packable_arm(Compiler *c, Scope *s);
int callee_declares_kwargs(Compiler *c, Scope *m);
int is_fresh_array(Compiler *c, int v);
/* True when the keyword hash `kwh` passes keywords from more than one source
   that may carry the same key -- two `**` operands, a literal Symbol key
   ahead of one, or a literal key written twice -- and every literal key is a
   Symbol. Its keywords then bind from one hash merging every source in
   order, a later key winning, as CRuby binds them; a lone `**` with literal
   keys only after it binds each keyword from its own source, since a literal
   after it always wins. */
int kwh_sources_overlap(const NodeTable *nt, int kwh);
/* True when element `e` of the braceless hash `kwh` is dropped from the hash
   it builds: CRuby compiles a hash of Symbol keys alone with each key once,
   at the place it is last written, so an earlier pair's value runs for its
   effect alone (emit_dropped_value). With a `**` or another kind of key the
   hash is built at run time, and a key keeps its first place. */
int kwh_elem_dropped(const NodeTable *nt, int kwh, int e);
/* The value `v` of such a pair, evaluated into `b` for its effect. */
void emit_dropped_value(Compiler *c, int v, Buf *b);
/* True when the keyword hash `kwh` has a `**` operand. */
int kwh_has_splat(const NodeTable *nt, int kwh);
/* kwh_sources_overlap, for a call into `m`, which declares keyword params. */
int kwh_merged(Compiler *c, Scope *m, int kwh);
/* The keywords of such a `kwh` merged into one fresh, rooted SymPolyHash in
   source order, a later key replacing an earlier one, the way
   emit_kwrest_collect merges them: each value and `**` operand is evaluated
   once, where it stands, and an operand of another class raises there, as a
   lone one does in emit_ds_hash_materialize. Returns the temp id and sets
   *out_type to TY_SYM_POLY_HASH -- or, with `any_key`, merges into a
   PolyPolyHash that keeps a String or other key, literal or an operand's,
   as a Data or Struct construction and a `**kwrest` of any key
   (kwrest_any_key) need (TY_POLY_POLY_HASH). */
int emit_ds_hash_merge(Compiler *c, int kwh, int any_key, TyKind *out_type);
/* True when emit_ds_hash_materialize runs keyword code with an effect ahead
   of the call's positionals: a kwh_merged call's merged hash, or a first
   `**` operand with a side effect that it evaluates. */
int kwh_runs_ahead(Compiler *c, Scope *m, int kwh);
/* True when the literal keys of the keyword hash `kwh` into `m` do not come
   in the order `m` binds them: a key names a keyword parameter ahead of an
   earlier key's, or the one before it again, or a keyword parameter after a
   key the `**kw` rest takes. The bindings run in parameter order, so such a
   call's arguments run first (emit_args_in_source_order). */
int kwh_out_of_order(Compiler *c, Scope *m, int kwh);
/* The arguments of a call that binds them in parameter order -- a
   Method#call, an inlined yielding initialize, or one a static check refuses
   -- evaluated in source order, keywords included and each value of a key
   written twice, into rooted temps written into `b` and pushed onto the
   g_argov overrides, for the caller to pop once it has bound the call. A
   nil value runs for its effect alone and reads as 0. A read of a variable
   a later value can give another value is taken too, as CRuby reads it at
   its place. */
void emit_args_in_source_order(Compiler *c, const int *argv, int argc, Buf *b);
/* The arguments of a call whose binding runs under another self -- an
   instance_exec's, bound where self is already its receiver -- evaluated in
   source order ahead of the switch, as emit_args_in_source_order runs them:
   each one with an effect, each that reads self (an ivar, self, a
   receiverless call), which CRuby reads as the caller's, and each read a
   later argument rebinds (`instance_exec(x, (x = 2))` binds the 1). */
void emit_args_off_self(Compiler *c, const int *argv, int argc, Buf *b);
/* emit_args_in_source_order, where the nodes `after` also run ahead of the
   binding reading the values -- a block's defaults -- and so can change what
   a read among them reads. */
void emit_args_before(Compiler *c, const int *argv, int argc, const int *after, int nafter, Buf *b);
/* Would a binding that renders each value in place, in parameter order,
   read one out of CRuby's order? Two values with an effect -- one's setup
   drains ahead of the other's bind -- or a read a later value, or a node of
   `after`, can change. */
int args_order_matters(Compiler *c, const int *argv, int argc, const int *after, int nafter);
/* The arguments of a call into `m` (NULL: none known), run first, in source
   order, into `b` as emit_args_before runs them, when its binding would run
   one out of CRuby's order; 1 when they ran, for the caller to pop the
   overrides once it has bound the call. A binder renders each value, and
   each default it fills at the call site, at its parameter's slot, and
   hoists the ones the slot roots ahead of the call statement, where they
   ran before the arguments left in place. So the arguments run first when
   its keywords are out of the parameters' order (kwh_out_of_order), when a
   read among them is one a later value or such a default can change
   (read_rebound_by: `m(@v, k: f(2))`, `m(v, k: (v = 2))`), when such a
   default reads a variable an argument can change (`g(@y = 2)` into
   `def g(a = @y, b)`), when a value with an effect sits beside a default
   with one (the default runs after every argument), and when a call with
   keywords, bound by name, passes two values with an effect (`m(lg(1),
   k: ls(2))`). Positionals alone are sequenced where emit_args_filled
   hoists them. A call whose keywords run ahead (kwh_runs_ahead) runs its
   arguments first already. */
int emit_args_before_binding(Compiler *c, Scope *m, const int *argv, int argc, Buf *b);
/* Can the node `after` give a variable the value `x` reads another value?
   A local by assigning it, or, when a proc or block assigns its cell
   (LocalVar.proc_rebinds), by anything that may call that proc
   (subtree_may_run_proc); an
   instance, global or class variable by any effect. A value built of reads
   (`[x, 2]`) asks it of each; a block reads when it runs. */
int read_rebound_by(Compiler *c, int x, int after);
/* Whether the key or value of `x[key] = val` may give the local or instance
   variable `x` reads another value (read_rebound_by) and cannot mutate it,
   so a store reads the receiver first and leaves the new binding; 0 for
   any other receiver (codegen_call_recv.c). */
int aset_recv_rebinds_only(Compiler *c, int recv, int key, int val);
/* Whether the key or value of `x[key] = val` may mutate the local or
   instance variable `x` reads, or move its String into a shared handle: it
   reads the variable, an instance variable's makes a call, or a proc that
   captures the local may run. */
int aset_recv_may_mutate(Compiler *c, int recv, int key, int val);
/* The receiver of such a store, read into a rooted temp in g_pre, ahead of
   any prelude its key or value moves there (1); inline without g_pre (0). */
int emit_aset_recv_read(Compiler *c, int recv, Buf *b);
int emit_ds_hash_materialize(Compiler *c, Scope *m, int kwh, TyKind *out_type);
/* The TypeError CRuby raises for a `**` operand that is neither a Hash, nil
   nor convertible with #to_hash, emitted into g_pre ahead of any keyword
   check: a settled kind of another class raises outright, and a boxed one
   (TY_POLY) is checked at run time on `val`, its evaluated value, which a
   user object's #to_hash converts for its effect alone. */
void emit_kw_splat_conv_check(Compiler *c, TyKind t, const char *val);
/* The boxed `**` operand held in the temp named `tmp` converted where it
   stands, the temp then holding what binds: nil, a Hash, or the Hash a user
   object's #to_hash answered; anything else raises the TypeError. */
void emit_kw_splat_conv_temp(Compiler *c, const char *tmp);
/* Can a boxed `**` operand be a user object whose #to_hash converts it:
   some class of the program defines one? Its answer is a new Hash, which
   the temp holding the operand roots. */
int kw_splat_user_to_hash(Compiler *c);
/* A `**` operand of a kind that is no Hash but can still be nil at run time
   -- a nilable Integer or Float slot's nil, a pointer-backed kind's
   NULL -- or that is true or false, which CRuby's TypeError names by value:
   its conversion is checked on the boxed value, as a boxed one is. */
int kw_splat_checked_boxed(Compiler *c, int node);
/* A `**` operand that raises its TypeError whatever it holds: true or
   false, or a kind that is no Hash and cannot be nil. */
int kw_splat_raises(Compiler *c, int node);
/* The same conversion inline, as a statement of an enclosing `({ ... })`:
   evaluates the `**` operand `node` into `b` and checks it there, for a
   site that evaluates its arguments in its own order. */
void emit_kw_splat_operand_inline(Compiler *c, int node, Buf *b);
void emit_ds_kwarg_check(Compiler *c, Scope *m, int kwh, int ds_hash_tmp, TyKind ds_hash_type);
void emit_kwhash_verify(Compiler *c, Scope *m, int hash_tmp, TyKind hash_type, Buf *out);
void emit_ds_param_extract(Compiler *c, Scope *m, int i, int ds_hash_tmp,
                           TyKind ds_hash_type, Buf *out);
/* analyze-side helpers also called from codegen (defined in analyze_util.c /
   analyze_scope.c; canonical declarations live in analyze_internal.h) */
int is_arith_op(const char *op);
int is_cmp_op(const char *op);
const char *int_shift_fn(Compiler *c, const char *op, int v);
int class_def_body(Compiler *c, int def_node);
int class_body_list(Compiler *c, int **out_ci, int **out_body);
int local_all_writes_empty_array(Compiler *c, Scope *sc, const char *name);
TyKind an_builtin_answer(Compiler *c, int id);
int an_yield_site_builtin_answer(Compiler *c, int id, TyKind kind, TyKind *out);
int node_is_empty_container(const NodeTable *nt, int node);
int an_empty_container_kind(Compiler *c, int b);
TyKind ffi_spec_to_ty(const char *spec);
int local_sole_range_node(Compiler *c, int recv);
int range_float_begin(Compiler *c, int recv);
void emit_block_param_from_boxed(Compiler *c, const char *pname, TyKind pt, const char *src, Buf *b);
void emit_rest_pack_kwh(Compiler *c, int from, int pos_argc, const int *argv, int kwh, Buf *b);
int rest_kwh_tail(Compiler *c, Scope *m, int kwh, int pos_argc);
int rest_bind_argc(Compiler *c, Scope *m, int kwh, int pos_argc);
int yield_value_diverges(Compiler *c, int mi);
int node_is_scope_tail(Compiler *c, int mi, int id);
int kwh_gathers(Compiler *c, Scope *m, int kwh, const int *argv, int pos_argc);
int emit_kwh_spread_arg(Compiler *c, int kwh, Buf *b);
int kwh_positional_slot(Compiler *c, Scope *m, int kwh, int pos_argc);
int kwh_arg_param(Compiler *c, Scope *m, int pos_argc);
/* An anonymous `*` forwarding the enclosing method's rest: its C expression. */
int emit_anon_rest_ref(Compiler *c, int splat, Buf *buf);
int splat_operand_ok(Compiler *c, int node);
void emit_splat_operand_array(Compiler *c, int node, Buf *b);
void emit_one_arg(Compiler *c, int arg, int boxed, Buf *b);
void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b);
void emit_array_elem_sure(TyKind at, int tmp, int elem_idx, Buf *b);
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx, Compiler *c, int argv_from, int pos_argc, const int *argv, Buf *b);
int is_descendant(Compiler *c, int k, int anc);
int class_builtin_superclass(Compiler *c, int i);   /* codegen.c */
const char *arysub_array_ctype(Compiler *c, int cid);   /* codegen.c: an Array subclass's Array struct (#7449) */
void emit_arysub_alloc(Compiler *c, ClassInfo *ci, Buf *b);
void arysub_box_id(Compiler *c, TyKind t, Buf *b);   /* the cls_id an object's box carries */
int program_has_arysub(Compiler *c);
void emit_arysub_machinery(Compiler *c, Buf *b);
/* a call Array answers on an Array subclass instance (#7449), as an
   expression or as a statement (codegen_call_array.c) */
int emit_arysub_call(Compiler *c, int id, Buf *b);
int emit_arysub_call_stmt(Compiler *c, int id, Buf *b, int indent);
const char *class_builtin_superclass_name(Compiler *c, int i);   /* codegen.c */
int class_builtin_parent(Compiler *c, int cid);      /* codegen.c */
int class_includes_module_named(Compiler *c, int cid, const char *mod_name);
int class_isa_user(Compiler *c, int k, int cid, const char *cn);  /* codegen_call.c */
int dispatch_impl_count(Compiler *c, int cid, const char *name);
/* do a dispatch switch's arms bind the call's arguments differently (each
   arm then lays them out itself)? the plan's CP_PER_ARM */
int dispatch_arms_disagree(Compiler *c, int cid, const char *name);
/* Do `given` positionals not fit method `m`? Answers 1 with CRuby's
   ArgumentError message in `msg`; 0 when they fit, or when the method's
   arity is not judged (arity_unjudged). The one count rule of the direct
   call and the dispatch arm. (codegen_fold.c) */
int arity_count_error(Compiler *c, Scope *m, int given, char *msg, size_t n);
/* The parameter of user method `m` that its one positional argument binds
   to (not always the first: an optional may come before a required one),
   or -1 when none does. (codegen_fold.c) */
int arm_arg_param(Compiler *c, Scope *m);
/* The arguments after self of a dispatch arm's call of user method `mi`,
   each led by ", ", when the C texts argv[0..argc) are its positionals.
   The layout of the call (arg_layout) places them. A parameter it leaves
   empty takes its default, run with `armself` (a pointer to the arm's
   object) as self, and a block slot takes NULL. What a default runs first
   goes to `pre`. `builds`, when not NULL, is 1 if the arm builds anything
   before its call (a default, a rest's Array, a `pre` statement): an arm
   of a function with unrooted operands roots them then. Answers 0, and
   emits no arguments, when argc arguments do not bind: `raise` then holds
   the statement that raises CRuby's ArgumentError in place of the call.
   An arm that cannot place the arguments (a parameter with no name, or a
   placement the layout does not give a positional call) is refused at
   compile time. A rest takes its arguments boxed, as a PolyArray: each
   text in argv must be a boxed value then. (codegen_fold.c) */
int emit_arm_args_text(Compiler *c, int mi, const char *armself, const char *const *argv, int argc,
                       Buf *pre, Buf *out, int *builds, char *raise, size_t rn);
/* Can running the node `id` assign self's instance variable `iv`, self an
   instance of class `cls` (-1: none known) or of one below it? `depth`
   counts the self calls followed into their methods (0 at the call site);
   1 when it cannot tell. */
int subtree_may_write_ivar(Compiler *c, int id, const char *iv, int cls, int depth);
int block_call_takes_class_dispatch(Compiler *c, int id);
void emit_dispatch(Compiler *c, int cid, const char *name, const char *selfptr, int argsNode, int blk_node, Buf *b);
int emit_reader_override_dispatch(Compiler *c, int id, int cid, const char *name, const char *selfptr, const char *reader, TyKind reader_ty, Buf *b);
int reader_override_arms(Compiler *c, int id, int cid, const char *name, TyKind reader_ty);
TyKind reader_override_ty(Compiler *c, int id, int cid, const char *name);
int emit_tap_then_expr(Compiler *c, int id, Buf *b);
int recv_is_const(const NodeTable *nt, int recv, const char *name);
int sp_is_fiber_storage_recv(const NodeTable *nt, int recv);
int emit_ctor_yield_inline(Compiler *c, int id, int ci, Buf *b);
int emit_env_args_before(Compiler *c, int id);
void emit_call(Compiler *c, int id, Buf *b);
/* A builtin call the builtin-op table covers (codegen_ops.c): 1 when it
   emitted the call, 0 to keep falling through the chain. */
struct BuiltinOp;
typedef struct {
  int id, recv, argc;
  TyKind rt;
  const char *name;
  const struct BuiltinOp *op;
  const char *rtext;   /* the receiver's C when the caller already rendered it */
  int t0;              /* a temp the caller took for the family ($T), or 0 */
} BopCtx;
int emit_builtin_op(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b);
/* the same, the receiver already rendered as rtext by a family that renders
   it once before its own arms */
int emit_builtin_op_text(Compiler *c, int id, int recv, TyKind rt, const char *name,
                         const char *rtext, Buf *b);
/* the same, with the temp t0 the family took before its arms ($T) */
int emit_builtin_op_tmp(Compiler *c, int id, int recv, TyKind rt, const char *name,
                        int t0, Buf *b);
/* emit_builtin_op over the rows of one stage (BuiltinOp.stage): a family
   whose arms sit at several places in the chain looks each place's rows up
   there; every other lookup reads stage 0 */
int emit_builtin_op_stage(Compiler *c, int id, int recv, TyKind rt, const char *name,
                          int stage, Buf *b);

/* codegen_view.c: a node's cached type overridden for one nested emission.
   view_push answers a token for the matching view_pop; a recovery point
   saves view_mark() and view_unwind()s back to it after a refusal.
   view_depth() counts the views of nodes open. */
int view_push(Compiler *c, int id, TyKind t);
void view_pop(Compiler *c, int tok);
int view_depth(void);
int view_mark(void);
/* The arm context a poly dispatch's builtin arm re-enters the call under,
   pushed and popped like a view (view_push_arm / view_pop) and put back by
   view_unwind: the node whose dispatch declines its own re-entry (the
   method dispatch's g_pd_skip, the block dispatch's g_prbd_skip), and
   g_poly_builtin_arm, under which no user class owns a name. send_split
   prevents a boxed send's class arm from splitting the same call again. */
typedef struct { int pd_skip, prbd_skip, builtin_arm, send_split; } ArmCtx;
extern ArmCtx g_arm;
#define g_pd_skip (g_arm.pd_skip)
#define g_prbd_skip (g_arm.prbd_skip)
#define g_poly_builtin_arm (g_arm.builtin_arm)
int view_push_arm(int pd_skip, int prbd_skip, int builtin_arm);
/* A node pinned to one face kind for the inference asked under it,
   pushed and popped like a view (view_pop) and put back by view_unwind
   (face_of, analyze.h). */
int view_push_face(int node, TyKind kind);
/* the statement whose prelude buffer `pre` is open (emit_with_prelude), and
   the innermost such statement: 0 when none is open. view_push_stmt answers
   -1 (nothing to pop) when the stack is too deep to take one. */
int view_push_stmt(int node, const void *pre);
int view_stmt_top(int *node, const void **pre);
/* Whether anything in statement `stmt` can bind the local `name` that the
   read `read` (inside it) names: 1 also when the statement is unknown (< 0),
   binds a local the walk cannot name, or the read is not in its subtree. */
int stmt_may_rebind_local(Compiler *c, int stmt, int read, const char *name);
/* A node bound to the text emit_expr writes for it instead (g_argov_*):
   view_bind answers the binding's slot; view_unbind(n) drops every binding
   from slot n up. */
int view_bind(int node, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void view_unbind(int n);
/* One representation flag of node id seen as v for one nested emission,
   restored by view_pop (or view_unwind on a refusal) like a type view. */
enum { VR_STRBUF_BOX, VR_HANDLE_DEMAND, VR_POLY_LIFT, VR_NILNARROW, VR_NIL_TESTED, VR_HEAD_HELD };
int view_push_repr(Compiler *c, int id, int flag, int v);
/* A splice's alias of a caller's variable that the call's block assigns
   (argument node arg, splice tag, parameter name), and a fence for a C
   function emitted inside it (ren_park(0) pushes it, ren_unpark pops it);
   both popped by view_pop. view_splice_next
   answers the next open entry below position `from` (-1: from the top)
   and above the innermost fence, or -1. */
int view_push_splice(int arg, int tag, const char *pname);
int view_push_fn(void);
int view_splice_next(int from, int *arg, int *tag, const char **pname);
/* bumped by every view push, pop and unwind: a per-node memo of a decision
   that reads the flags or the type keys on it */
unsigned view_epoch(void);
void view_unwind(int mark);   /* back to a view_mark(): views, arm contexts and bindings */
int emit_op_ivar_reflection(Compiler *c, const BopCtx *x, Buf *b);
/* The concurrency handles' row emitters (codegen_call_concurrency.c) */
int emit_op_thread_set_report(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_thread_raise(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_thread_tls(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_mutex_sleep(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_condvar_wait(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_queue_push(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_queue_pop(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_fiber_resume(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_fiber_transfer(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_fiber_raise(Compiler *c, const BopCtx *x, Buf *b);
/* Complex and Rational row emitters (codegen_call_numeric.c) */
int emit_op_rational_round(Compiler *c, const BopCtx *x, Buf *b);
/* String row emitters (codegen_call_recv.c) */
int emit_op_str_set_n(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_str_affix_any(Compiler *c, const BopCtx *x, Buf *b);
/* Hash row emitters (codegen_call_hash.c) */
int emit_op_hash_pattern(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_pattern_all(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_default_proc(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_to_proc(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_aref(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_has_key(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_key(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_default(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_keys(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_fetch(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_to_s(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_compact_bang(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_rehash(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_replace(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_set_default(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_merge_bang_many(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_shift(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_delete(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_invert(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_flatten(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_to_a(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_sort(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_first(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_take(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_drop(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_assoc(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_hash_compact(Compiler *c, const BopCtx *x, Buf *b);
/* Range row emitters (codegen_call_numeric.c) */
int emit_op_range_clone(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_range_freeze(Compiler *c, const BopCtx *x, Buf *b);
/* Array row emitters (codegen_call_array.c) */
int emit_op_array_shift_n(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_cycle_n(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_last(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_join(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_pack_buffer(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_sort_bang(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_slice_bang_range(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_plus(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_setop(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_intersect_p(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_replace(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_minmax(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_sort(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_uniq(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_nmin(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_sum0(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_compact_bang(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_flatten(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_push(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_insert_n(Compiler *c, const BopCtx *x, Buf *b);
int emit_scalar_array_transpose(Compiler *c, int id, int recv, TyKind rt,
                                const char *name, int argc, Buf *b);
int emit_op_float_rationalize(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_string_scan_checked(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_string_slice(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_string_scrub_block(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_transpose(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_assoc(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_combination(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_product(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_fetch_values0(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_pred0(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_dig_n(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_sum1(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_concat(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_index_v(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_cycle_endless(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_slice_groups(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_join_str(Compiler *c, const BopCtx *x, Buf *b);
int emit_op_array_pred_class(Compiler *c, const BopCtx *x, Buf *b);
/* A typed array receiver of a compare (`ck` "cmp") or a blockless sum (`ck`
   "sum"), wrapped in the runtime's nil check where the array can hold nil
   (codegen_call_recv.c) */
void emit_nil_ck_recv(Compiler *c, int recv, TyKind rt, const char *ck, int float_seed, Buf *b);
/* whether an Integer or Float array receiver can hold nil, and
   whether analyze marked it so (codegen_call_recv.c) */
int elem_nil_sentinel(Compiler *c, int recv, TyKind rt);
int elem_nil_marked(Compiler *c, int recv, TyKind rt);
/* fn(recv, value, count) for Fiber#resume / #transfer, fn(value) for
   Fiber.yield (recv NULL) (codegen_call.c) */
void emit_fiber_pass_call(Compiler *c, const char *fn, const char *recv,
                          int argc, const int *argv, Buf *b);
/* Thread#raise / Fiber#raise on the receiver text rtext (codegen_call.c) */
void emit_concurrency_raise(Compiler *c, const char *rtext, int argc, const int *argv,
                            const char *ctype, char pfx, const char *fn, Buf *b);
/* Decode a CallNode's positional arguments: sets *argc and returns the argv
   array (NULL when the node has no arguments). Shared by the call emitters. */
const int *call_args(const NodeTable *nt, int id, int *argc);
int poly_shl_root_slot(Compiler *c, int recv);   /* the boxed local/ivar a << chain starts at, or -1 */
/* Emit `node` into a fresh buffer and return it (caller reads .p, frees it).
   Collapses the `Buf b; memset(&b,0,sizeof b); emit_expr(c,node,&b);` idiom. */
Buf expr_buf(Compiler *c, int node);
/* Receiver-typed method-call emitters (codegen_call_recv.c). Each returns 1 if
   it handled the call and emitted into `b`, else 0 (emit_call falls through). */
int emit_arg_type_guards(Compiler *c, int id, Buf *b);
int emit_builtin_arity_guard(Compiler *c, int id, Buf *b);
int emit_hash_new_arg_guard(Compiler *c, int id, Buf *b);
int emit_hash_new_capacity_wrap(Compiler *c, int id, Buf *b, int boxed);
void emit_hash_new_capacity_check(Compiler *c, int cap, Buf *b);
extern int g_hash_cap_inner;
int emit_blockless_enumerator(Compiler *c, int id, Buf *b);
int emit_unresolved_call(Compiler *c, int id, Buf *b);
int emit_array_call(Compiler *c, int id, Buf *b);
int emit_array_splat_mutator(Compiler *c, int id, Buf *b);
void emit_str_insert_text(Compiler *c, int arg, Buf *b);
int emit_hash_call(Compiler *c, int id, Buf *b);
int emit_scalar_call(Compiler *c, int id, Buf *b);
int emit_object_call(Compiler *c, int id, Buf *b);
int emit_native_object_protocol(Compiler *c, int id, Buf *b);
/* The C test for "temp _t<tmp> of kind t holds nil" (want_nil) or "holds a
   non-nil value" (!want_nil), spelled per kind: the int/float nil flag, a
   NULL pointer, a poly tag. Shared by the index-or/and-write forms. */
void emit_slot_nil_test(Compiler *c, TyKind t, int tmp, int want_nil, Buf *b);
int emit_native_case_eq(Compiler *c, int cond, TyKind subj_t, const char *subj_ref, Buf *b);
int exc_subclass_defines(Compiler *c, const char *name);
int exc_user_method_name(Compiler *c, const char *name, int argc);
int emit_value_recv_call(Compiler *c, int id, Buf *b);
int emit_range_call(Compiler *c, int id, Buf *b);
int emit_boxed_class_aref(Compiler *c, int id, Buf *b);
int emit_poly_call(Compiler *c, int id, Buf *b);
int diagnose_eval_call(Compiler *c, int id);
int diagnose_unsupported_call(Compiler *c, int id);
int diag_user_defines(Compiler *c, const char *name);
int recv_user_defines(Compiler *c, const char *name);
int emit_object_ivar_list(Compiler *c, int recv, int cid, Buf *b);
void emit_object_ivar_remove(Compiler *c, int id, int recv, TyKind rt, int cid, const char *sym, Buf *b);
int emit_object_ivar_call(Compiler *c, int id, const char *name, int recv, TyKind rt,
                          int cid, int argc, const int *argv, Buf *b);
const char *case_map_suffix(Compiler *c, int argc, const int *argv);
int emit_op_poly_case_options(Compiler *c, const BopCtx *x, Buf *b);
int user_defines_or_reads(Compiler *c, const char *name);
int native_class_defines(Compiler *c, const char *name);
const char *array_index_bad_class(Compiler *c, int id);
/* the poly dispatch helpers other files read; the rest of its helpers
   are in codegen_poly.h */
int  poly_block_dispatch_cands(Compiler *c, int id, int *cand, int max);
int  poly_redispatch_kind(Compiler *c, int id, const char *name, int argc);
int  face_arg_misfit(Compiler *c, unsigned kind, int arg);
int  face_args_misfit(Compiler *c, int id, unsigned kind);
int poly_name_user_claimed(Compiler *c, const char *name, int argc);
/* Does CRuby take argc arguments to cls#name, by the instance arity table
   (sp_builtin_arity_spec_tbl)? 1 for a name the table has no row for. */
int builtin_arity_admits(const char *cls, const char *name, int argc);
/* ...and the count it expects when it does not (NULL when it admits it) */
const char *builtin_arity_expected(const char *cls, const char *name, int with_block, int argc,
                                   char *exp, size_t n);
void emit_complex_coerce(Compiler *c, int node, Buf *b);
int emit_complex_real_args(Compiler *c, const int *argv, int argc, int polar, Buf *b);
void emit_brk_wrapped_call(Compiler *c, int id, Buf *b);
/* 1 if a break-carrying call at `id` can skip the serial-addressed setjmp
   scope (every break in its block is a same-function goto); 0 if it needs
   the real sp_brk_push/setjmp/sp_brk_throw wrapper, which -- like a begin/
   rescue's setjmp -- makes a local written before the throw and read after
   indeterminate unless declared volatile (see begin_volatile_names). */
int brk_wrapper_light(Compiler *c, int id);
int brk_wrapper_surely_light(Compiler *c, int id);
void emit_array_splice(Compiler *c, int id, int recv, TyKind rt, int start_node, int len_node, int range_node, int rhs_node, Buf *b);
int splice_to_ary_mi(Compiler *c, TyKind rhs_ty);
TyKind emit_splice_to_ary_src(Compiler *c, int rhs_node, TyKind rhs_ty, int mi, int ta, Buf *b, Buf *out);
void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
int scope_has_return(Compiler *c, int scope_idx);
int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr);
int emit_inline_call(Compiler *c, int id, Buf *b, int indent);
int emit_poly_recv_block_dispatch(Compiler *c, int id, Buf *b, int indent);
int emit_poly_recv_block_value(Compiler *c, int id, Buf *b);
int is_block_call(Compiler *c, int id);
int is_blockless_block_param_call(Compiler *c, int id);
const char *blockless_block_param_call_name(Compiler *c, int id);
void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr, TyKind want_ty);
typedef struct BiRen BiRen;
/* A spliced block's parameter aliases (see emit_block_binds), undone by the
   caller once the body is emitted. */
typedef struct BlockAliases { Scope *s[16]; const char *nm[16]; int n, open; } BlockAliases;
void block_alias_hold(BlockAliases *al, Scope *s, LocalVar *lv);
void block_aliases_release(BlockAliases *al);
void emit_block_kw_binds(Compiler *c, int blk, int ykw, Scope *bsc, Buf *b, int indent,
                         int as_expr, BiRen *bi, BlockAliases *al);
int block_param_wants_alias(Compiler *c, int blk, int k, int n);
int block_kw_wants_alias(Compiler *c, int blk, const char *key);
void emit_block_binds(Compiler *c, int blk, const int *yargs, int yc,
                      Buf *b, int indent, int as_expr, BiRen *bi, BlockAliases *al);
int block_binds_gathered(Compiler *c, int blk);
int emit_boxed_step_binds(Compiler *c, int blk, const char *vals, Buf *b, int indent, int as_expr);
void emit_yield_proc_call(Compiler *c, int args_node, TyKind result_ty, Buf *b, int indent, int as_expr);
int emit_inline_expr(Compiler *c, int id, Buf *b);
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig, const char *p0_ren, TyKind src_type, const char *src_expr, Buf *b, int indent);
int subtree_has_own_redo(const NodeTable *nt, int id);
void emit_loop_body(Compiler *c, int body, Buf *b, int indent);
int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
int emit_array_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
                           Buf *b, int indent, int *tr, int *torig, int *twp);
int emit_array_filter_loop_handles(Compiler *c, int recv, int block, const char *name,
                                   Buf *b, int indent, int *tr, int *torig, int *twp);
void emit_synth_line_marker(Buf *b);
/* --ext-init / --ext-entry (library emission, docs/internals/ext-design.md):
   when g_ext_init_name is set, codegen emits `void <name>(void)` in place of
   main, entries go non-static, and g_ext_header_text carries the generated
   header for main.c to write beside the C. */
extern const char *g_ext_init_name;
extern const char *g_ext_entries;
extern char *g_ext_header_text;
void emit_interp(Compiler *c, int id, Buf *b);
void emit_puts_one(Compiler *c, int arg, Buf *b, int indent);
void emit_print_one(Compiler *c, int arg, Buf *b, int indent);
void emit_p_one(Compiler *c, int arg, Buf *b, int indent);
int emit_output_call(Compiler *c, int id, Buf *b, int indent);
void system_refuse_unsupported(Compiler *c, int id, const int *argv, int argc);
int emit_system_splat(Compiler *c, const int *argv, int argc, Buf *b);
int int_value_plain(Compiler *c, int node);   /* an Integer that can never be nil */
int int_value_plain_promote(Compiler *c, int node);
int int_local_plain(Compiler *c, LocalVar *lv, const char *name);
int emit_output_spilled(Compiler *c, const char *name, int argc, const int *argv, Buf *b, int indent);
void emit_assign(Compiler *c, int id, Buf *b, int indent);
void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
int emit_array_op_assign(Compiler *c, const char *lval, TyKind t, const char *op, int v, Buf *b);
int emit_scalar_op_assign(Compiler *c, const char *lval, TyKind t, const char *op,
                          int v, int capture, int lhs_nil, int lhs_plain, Buf *b);
const char *poly_pow_fn(TyKind rt);   /* sp_poly_pow, or sp_poly_pow_recv for a receiver that may lack `**` */
int emit_poly_op_assign(Compiler *c, const char *lval, const char *op, int v,
                        int capture, Buf *b);
void emit_poly_unboxed(Compiler *c, int node, TyKind t, const char *conv, Buf *b);
void emit_range_endpoint(Compiler *c, int node, const char *none, Buf *b);
void emit_tail_recv_value(Compiler *c, int id, int rr, Buf *b);
const char *op_assign_int_conv(TyKind slot, const char *op);
void emit_cond(Compiler *c, int id, Buf *b);
int static_isa_cond(Compiler *c, int pred);
int static_respond_to_cond(Compiler *c, int pred);
int call_on_builtin_class_missing(Compiler *c, int id);
int static_block_given_cond(Compiler *c, int pred);
int static_nil_ivar_cond(Compiler *c, int pred);
void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail);
int emit_poly_class_when(Compiler *c, int cond_id, const char *tmp, Buf *b);
void emit_class_val_when(const char *cn, int t, Buf *b);
void emit_pm_eq(Compiler *c, int t, TyKind pt, int valnode, Buf *b);
int emit_pm_cond(Compiler *c, int pat, int t, TyKind pt, Buf *b);
void emit_pm_bind_pattern(Compiler *c, int pat, const char *src_poly, int indent, Buf *b, Scope *sc);
void emit_case_match(Compiler *c, int id, Buf *b, int indent, int tail, int value_cr);
void emit_case(Compiler *c, int id, Buf *b, int indent);
void emit_case_branch_value(Compiler *c, int stmts, TyKind rt, int cr, Buf *b);
void emit_case_expr(Compiler *c, int id, Buf *b);
void emit_while(Compiler *c, int id, Buf *b, int indent, int is_until);
void emit_for(Compiler *c, int id, Buf *b, int indent);
void emit_return(Compiler *c, int id, Buf *b, int indent);
int rescue_is_catchall_name(const char *n);
int subtree_has_retry(const NodeTable *nt, int id);
void emit_rescue(Compiler *c, int id, Buf *b, int indent, int fr, const char *resultvar);
void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar);
void emit_with_prelude(Compiler *c, int id, Buf *b, int indent, void (*inner)(Compiler *, int, Buf *, int));
void emit_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail(Compiler *c, int id, Buf *b, int indent);
int tail_iter_receiver(Compiler *c, int id);
int expr_is_arr_or_nil(Compiler *c, int v);
void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmts(Compiler *c, int id, Buf *b, int indent);
void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
int emit_top_stmts(Compiler *c, int id, Buf *b, int indent, size_t *cuts);
int needs_root(TyKind t);
/* Whether a variable of an inferred type takes a root, and the root itself,
   picking the rbval macro for boxed poly and the string one for a String. */
int ty_gc_rootable(Compiler *c, TyKind t);
void emit_gc_root_var(Compiler *c, TyKind t, const char *name, Buf *b);
void emit_gc_root_tmp(Compiler *c, TyKind t, int tmp, Buf *b);
int ty_gc_holds_refs(Compiler *c, TyKind t);
void emit_gc_root_tmp_refs(Compiler *c, TyKind t, int tmp, Buf *b);
void emit_srange_handle_roots(Compiler *c, const char *ref, Buf *b);
void emit_srange_marks(Compiler *c, const char *ref, Buf *b);
/* `_t<tmp>` when the node was already evaluated into that temp, else the node */
void emit_node_or_tmp(Compiler *c, int node, int tmp, Buf *b);
/* the key of a hash store, as the kind's set takes it (codegen_stmt.c) */
void emit_hash_store_key(Compiler *c, int key, TyKind rt, Buf *b);
const char *hash_box_cls(TyKind t);
const char *hash_order_key(TyKind t, int tr, int ti);
const char *hash_order_val(TyKind t, int tr, int ti);
const char *hash_order_oval(TyKind t, int tr, int ti);
int emit_hash_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
                          const char *rs, Buf *b, int indent, int *tr, int *torig, int *twp);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
TyKind yield_site_type(Compiler *c, int node);
void emit_int_expr(Compiler *c, int node, Buf *b);
/* an index ahead of a cached bounds compare: raw when it may be nil, 1 when
   the caller owes its nil test where the compare sends it (codegen.c) */
int emit_int_index_raw(Compiler *c, int node, Buf *b);
void emit_str_expr(Compiler *c, int node, Buf *b);
void emit_path_expr(Compiler *c, int node, Buf *b);
void emit_to_s_expr(Compiler *c, int node, Buf *b);
/* Converted String operands held across the call they enter (codegen.c).
   `guarded` declines the hold: the arm wrapped its body in a nil-receiver
   dispatch check, and a hoisted conversion would run before that check
   raises its NoMethodError -- CRuby never asks #to_str for a call that
   does not dispatch. */
typedef struct { Buf b; int *tmp; int n, cap; int guarded; } ConvHold;
extern ConvHold *g_conv_hold;
extern unsigned g_conv_emitted;  /* implicit conversions emitted so far; read as a delta */
Buf *conv_hold_begin(Buf *b, int *tmp);
void conv_hold_end(int tmp);
/* nil-accepting slots ("x".split(nil), StringIO#read(nil)): no strict
   nil/true/false TypeError arm -- see emit_nilbool_conv_raise in codegen.c */
void emit_int_expr_nilable(Compiler *c, int node, Buf *b);
void emit_int_expr_bound(Compiler *c, int node, const char *none, Buf *b);
void emit_str_expr_nilable(Compiler *c, int node, Buf *b);
void emit_str_expr_sep(Compiler *c, int node, Buf *b);
/* strict with CRuby's rb_convert_type wording ("of nil into Integer") */
void emit_int_expr_conv(Compiler *c, int node, Buf *b);
void emit_to_int_expr(Compiler *c, int node, Buf *b);
void emit_int_expr_offt(Compiler *c, int node, Buf *b);
int emit_unresolved_coerced(Compiler *c, int node, TyKind target, Buf *b);
int emit_unresolved_coerced_text(Compiler *c, int node, TyKind target, const char *txt, Buf *b);
int call_answers_no_value(Compiler *c, int node);
void emit_int_divisor(Compiler *c, int node, Buf *b);
void emit_float_expr(Compiler *c, int node, Buf *b);
void emit_float_coerce_expr(Compiler *c, int node, Buf *b);
/* Emit `node` as a scalar operand: like a plain emit_expr, except an
   unresolved-constant read (which lowers to a NameError raise valued as an
   sp_Class struct) is voided and replaced by `zero` ("0" / "0.0"), so the
   raise survives but the struct never reaches an int/float slot. */
void emit_scalar_operand(Compiler *c, int node, const char *zero, Buf *b);
/* Emit `node` with `emit` into `val`, and the setup statements the emission
   spills into g_pre into `pre` instead. A statement expression that binds a
   receiver to a temp and only then evaluates an argument places `pre` right
   before the argument's value: left in g_pre, the setup (an array literal's
   pushes) runs ahead of the whole expression, and so ahead of the receiver
   Ruby evaluates first. */
void emit_rooted_key_call(Compiler *c, const char *fn, const char *recv,
                          const int *argv, int argc, Buf *b);
int emit_rooted_arg_list(Compiler *c, const int *argv, int argc,
                         const char *ctype, const char *root,
                         void (*emit)(Compiler *, int, Buf *), int *tmps, Buf *b);
void emit_split_pre(Compiler *c, int node, void (*emit)(Compiler *, int, Buf *), Buf *pre, Buf *val);
/* A braced conditional arm whose hoisted setup stays inside that arm. */
void emit_cond_arm(Compiler *c, int node, Buf *b, void (*emit)(Compiler *, int, Buf *, void *), void *ctx);
void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol);
void declare_local_named(Compiler *c, Buf *b, LocalVar *lv, const char *name, int vol);
void emit_cell_shadow_store(Compiler *c, Scope *encl, const char *name, Buf *b, int indent);
int scope_has_begin(Compiler *c, int si);
/* the C type of a local declared outside emit_scope_decls, volatile when vol */
void emit_local_ctype(Compiler *c, TyKind ty, int vol, Buf *b);
void emit_scope_decls(Compiler *c, Scope *s, Buf *b);
void emit_scope_decls_ends(Compiler *c, Scope *s, Buf *b, size_t *ends);
int method_is_void(Scope *s);
void emit_method_cname(Compiler *c, Scope *s, Buf *b);
void emit_poly_iter_obj_normalize(Compiler *c, int tv, Buf *b);
void emit_poly_iter_obj_reject(Compiler *c, int tv, const char *name, Buf *b);
void emit_poly_iter_obj_reject_as(Compiler *c, int tv, const char *name, const char *shown, Buf *b);
void emit_method_signature(Compiler *c, Scope *s, Buf *b);
void emit_method(Compiler *c, Scope *s, Buf *b);
int is_nested_block(const char *ty);
void proc_collect_locals(Compiler *c, int id, NameSet *locals);
int conv_reads_shared_storage(Compiler *c, int node);
int emit_array_into_poly_slot(Compiler *c, TyKind slot, int v, Buf *b);
void proc_collect_used(Compiler *c, int id, NameSet *out);
int proc_params_node(Compiler *c, int create);
const char *proc_param_name(Compiler *c, int create, int idx);
int proc_numbered_params_node(Compiler *c, int create); /* -1 unless numbered */
int proc_body_node(Compiler *c, int create);
int proc_slot_is_direct(TyKind t);
int proc_slot_is_ptr(TyKind t);
int proc_body_has_yield(Compiler *c, int id);
int proc_body_has_return(Compiler *c, int id);
int proc_does_nonlocal_return(Compiler *c, int create);
int scope_creates_returning_proc(Compiler *c, int si);
int fiber_cap_needs_root(TyKind t);
int fiber_body_uses_self(Compiler *c, int id);
void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen, int size_node);
void emit_proc_literal(Compiler *c, int create, Buf *b);
int is_builtin_reopen(const char *name);
int is_exc_name(const char *n);
int class_is_exc_subclass(Compiler *c, int ci);
const char *class_ruby_name(Compiler *c, int ci);
const char *obj_str_cname(Compiler *c, int cid, int want_inspect);
const char *obj_str_mname(Compiler *c, int cid, int want_inspect);
int obj_str_ret_poly(Compiler *c, int cid, int want_inspect);
const char *exc_builtin_parent(Compiler *c, int ci);
void emit_class_struct(Compiler *c, ClassInfo *ci, Buf *b);
int class_needs_scan(ClassInfo *ci);
void emit_class_scan(Compiler *c, ClassInfo *ci, Buf *b);
int comp_class_is_module(Compiler *c, ClassInfo *ci);
int hv_value_class(Compiler *c, int recv);   /* analyze_infer.c (#4846) */
void emit_class_new(Compiler *c, ClassInfo *ci, Buf *b);
int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
void emit_super(Compiler *c, int id, Buf *b);
void emit_regex_section(Compiler *c, Buf *b);
/* IndexOperatorWriteNode in value position: evaluate the receiver and key
   once into prelude temps so the write and the read-back that follows share
   them (#3417). Returns 0 when nothing was hoisted. */
int emit_index_opw_hoist(Compiler *c, int id, Buf *pre, int indent);
void emit_index_opw_unhoist(void);
void emit_index_opw_value_into(const char *tmp);
int emit_index_opw_value_taken(void);
extern const char *g_iow_recv_ref;
extern const char *g_iow_key_ref;

void refuse_yield_capwrap(Compiler *c, int blk, int yc, const int *yv);

/* ---- nil out of band: sp_oint / sp_ofloat (codegen_util.c) ----
   No bit pattern of an sp_int or an sp_float means nil. A nullable Integer
   or Float is carried as an sp_oint / sp_ofloat (value + nil flag) in a
   local, a parameter, a return, a temp, a static; an instance ivar keeps its
   sp_int field and a byte in the object's iv__nilb. One predicate decides
   a node's C value (node_is_oint); emit_expr always yields the plain scalar
   and emit_oint_expr the oint form. */
/* the C spellings for the Integer (TY_INT) or Float (TY_FLOAT) kind */
const char *oint_ctype(TyKind t);   /* "sp_oint" / "sp_ofloat" */
const char *oint_nil(TyKind t);     /* "sp_oint_nil()" / "sp_ofloat_nil()" */
const char *oint_of(TyKind t);      /* "sp_oint_of" / "sp_ofloat_of" */
const char *oint_val(TyKind t);     /* "sp_oint_val" / "sp_ofloat_val" (NoMethodError for nil) */
const char *oint_arg(TyKind t);     /* "sp_oint_arg" / "sp_ofloat_arg" (TypeError for nil) */
const char *oint_box(TyKind t);     /* "sp_box_oint" / "sp_box_ofloat" */
const char *oint_unbox(TyKind t);   /* "sp_unbox_oint" / "sp_unbox_ofloat" */
/* is t an Integer or a Float kind */
int oint_kind(TyKind t);
/* a local's / parameter's slot is an sp_oint / sp_ofloat */
int slot_is_oint(const LocalVar *lv);
/* the C type of a local's slot: the oint type, or emit_ctype's */
void emit_slot_ctype(Compiler *c, const LocalVar *lv, Buf *b);
/* instance ivar iv of class cid has a nil byte in iv__nilb; its index;
   the number of bytes the class's iv__nilb has (0: none), one length for
   its whole class family */
int ivar_has_nilbit(Compiler *c, int cid, int iv);
int ivar_nilbit_index(Compiler *c, int cid, int iv);
int class_nilbyte_count(Compiler *c, int cid);
/* the C text of the nil: the `((o)->iv__nilb[k])` test, and the set
   (`= 1`) / clear (`= 0`) stores, for the receiver text `obj`
   ("self->", "_t3->", "o." ...: the prefix up to the field) */
void ivar_nilbit_test(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap);
void ivar_nilbit_set(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap);
void ivar_nilbit_clear(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap);
void ivar_nilbit_assign(Compiler *c, int cid, int iv, const char *obj, const char *nil, char *out, size_t cap);
/* the ivar slot a read / write node names: 1 the object's field (cid, iv),
   2 the class-level or top-level static civ_C_x, 0 none; whether a read's
   slot is an oint (a field with a nil bit, an oint static) */
int ivar_node_slot(Compiler *c, int node, int *cid, int *iv);
int ivar_read_slot_is_oint(Compiler *c, int node);
/* an Integer literal as a C constant (INT64_MIN spelled as an expression) */
void emit_int_lit(Buf *b, long long v);
/* a value emitted only for its effects: a nil read there is no unwrap (codegen_util.c) */
extern int g_value_discarded;
/* the caller declared emit_block_value_into's `dest` as its oint (codegen_fold.c) */
extern int g_bv_dest_oint;
/* can a `next` of this block body (not a nested block's) hand the slot a
   nil: a bare `next`, `next nil`, a value that can be nil (codegen_call.c) */
int block_next_may_be_nil(Compiler *c, int id, int depth);
/* Struct.new / Data.new of class ci with call id's arguments: the members,
   and the nil bits of members left nil (codegen_call.c) */
int emit_struct_new_call(Compiler *c, int id, int ci, int argc, const int *argv, Buf *b);
/* member i of class ci read as `expr` through `objprefix` ("_t3->"), boxed
   with its nil: an Integer / Float member by its nil bit (codegen.c) */
void emit_member_boxed_text(Compiler *c, ClassInfo *ci, int i, const char *objprefix, const char *expr, Buf *b);
/* the head of a Range walk that stops at the ends of sp_int (see codegen_util.c) */
void emit_range_walk_head(Buf *b, int indent, const char *var, int decl, const char *first,
                          const char *step, const char *last);
/* a global's / class-level ivar's / cvar's static is an sp_oint */
int gvar_is_oint(Compiler *c, const LocalVar *g);
int civ_is_oint(Compiler *c, int cid, int iv);
int cvar_is_oint(Compiler *c, int cid, int idx);
/* The node's natural C expression is an sp_oint / sp_ofloat (an Integer or
   Float node only): a nullable read no guard narrowed, a call answering an
   oint, a nil literal, a conditional with a nullable arm. */
int node_is_oint(Compiler *c, int node);
/* node_is_oint, or a leaf read of a slot that is an oint (a nil guard may
   have narrowed the read): emit_oint_expr yields the bare oint for it, and
   its box reads the nil */
int node_has_oint_form(Compiler *c, int node);
/* node as an sp_oint / sp_ofloat: the bare producer, a plain value wrapped
   in sp_oint_of, nil as sp_oint_nil(); a slot read as its slot's own oint */
void emit_oint_expr(Compiler *c, int node, TyKind t, Buf *b);
/* set by emit_oint_expr for the one node it is about to emit: emit_expr
   consumes it (leaves the oint producer bare) before any child is emitted */
extern int g_want_oint;
/* an element read whose oint is unwrapped at once emits the checked plain
   read (codegen_util.c emit_scalar_operand_op, emit_expr's sp_oint_arg) */
/* a boxed field whose writes may store nil starts with the unset mark
   (codegen_util.c); the mark is a nil's cls_id no box carries */
#define SP_IVAR_UNSET_MARK 0x5e70
int poly_ivar_unset_marked(Compiler *c, int cid, int iv);
extern const char g_ck_opnd[];
/* the leaf slot read emit_expr is rendering is wanted as its own oint
   (codegen_expr.c) */
extern int g_oint_read;
/* the yield in value position wants its oint (codegen_iter.c); the `next`
   slot of the block being spliced is an oint */
extern int g_yield_want_oint;
extern int g_ie_next_oint;
/* A slot of kind t known to hold nil, read where a value of that kind is
   wanted: the kind's nil for the kinds that have one (NULL, a boxed nil),
   and for an Integer or Float, whose plain scalar has no nil, the TypeError
   a nil raises where an Integer is wanted (sp_oint_arg(sp_oint_nil())). */
void emit_slot_nil_read(Compiler *c, TyKind t, Buf *b);
/* the method being emitted answers an oint (set beside g_ret_type); the
   result slot a tail fills does (beside g_result_ty) */
extern int g_ret_oint;
extern int g_result_oint;
/* a conditional's result slot holds its nil (node_is_oint of it): its C
   type and dead value */
int cond_res_oint(Compiler *c, int id, TyKind res);
void emit_res_ctype(Compiler *c, TyKind res, int res_o, Buf *b);
const char *res_zero(Compiler *c, TyKind res, int res_o);
/* a store's right-hand side into an ivar with a nil bit, the bit kept in
   step (codegen_util.c): from node v, or from an sp_oint text */
void emit_ivar_value_nilbit(Compiler *c, int cid, int iv, const char *obj, int v, Buf *b);
void emit_ivar_text_nilbit(Compiler *c, int cid, int iv, const char *obj, const char *otext, Buf *b);
/* the right-hand side of `@x = nil` on an Integer / Float ivar */
void emit_ivar_nil_store(Compiler *c, int id, TyKind t, Buf *b);
/* the backstop: a nil into a plain Integer / Float slot is refused at compile time */
__attribute__((noreturn)) void refuse_nil_store(Compiler *c, int node, TyKind t, const char *where);
int node_may_be_nil(Compiler *c, int node);
int call_names_only_void_methods(Compiler *c, int node);   /* codegen_stmt.c */
/* node as a plain scalar through its oint form: `sp_oint_val(<oint>, op)`
   when the node may be nil (cmp_operand_may_be_nil), else emit_expr */
void emit_oint_unwrap_ck(Compiler *c, int node, TyKind t, const char *op, Buf *b);
int poly_int_key_boxed(Compiler *c, int key);
void emit_scalar_operand_op(Compiler *c, int node, const char *op, Buf *b);
/* Ruby truthiness of an Integer / Float node as a C condition: `!o.nil`
   for one with an oint form, else always true (the node evaluated) */
void emit_oint_truthy(Compiler *c, int node, TyKind t, Buf *b);
/* the value of a typed Array store as nil_store_sfx's store takes it */
void emit_elem_store_value(Compiler *c, const char *k, int node, Buf *b);
/* an op-assign on an Integer / Float field with a nil bit: the field's
   nil test (NoMethodError for nil at the operator) and the bit's clear
   after the store (codegen_stmt.c emit_scalar_op_assign reads them) */
extern const char *g_opa_niltest, *g_opa_clear;
/* a method's Integer / Float return that can be nil is an sp_oint /
   sp_ofloat (codegen.c): the C return type, the dead value where control
   never arrives, and a C call of it boxed */
int method_ret_is_oint(const Scope *s);
void emit_method_ret_ctype(Compiler *c, const Scope *s, Buf *b);
const char *method_ret_zero(Compiler *c, const Scope *s);
void emit_boxed_ret_call(Compiler *c, const Scope *s, const char *call, Buf *b);
int emit_ptr_array_build(Compiler *c, int v, TyKind want, Buf *b);
/* Is value v's String one no other name can see a copy of (codegen_stmt.c)?
   share_value_needs_handle asks it. */
int strbuf_flow_unseen(Compiler *c, int v);

/* --share-check (share_check.c): mark the value of node v, about to be
   written into b, when it has to be the shared handle (face: 'x' as the
   emitter has it, 'B' a box, 'H' a handle for a shared slot); read the
   marks in b[from..] against the text around them, record the copies and
   strip them, where a function's text is final and before any pass reads
   it as text (and over the whole unit, for what is left); once the unit is
   written, write the reports. */
void share_check_mark(Compiler *c, int v, char face, Buf *b);
void share_check_harvest(Compiler *c, Buf *b, size_t from);
void share_check_report(Compiler *c);
/* The text without the marks that lead it, for an emitter that reads an
   operand's text as a temp or compares it with one. */
const char *share_check_unmarked(const char *t);
size_t share_check_strip(char *s, size_t len);
#endif
