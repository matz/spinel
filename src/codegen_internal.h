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
const char *strbuf_local_name(Compiler *c, int recv);
int strbuf_ivar_owner(Compiler *c, int node);
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
/* The same shim over a READER call that hands out the handle
   (`obj.name[0] = "X"`): no name to rename and no ivar node, so the call node
   itself reads as the shadow through the argument-override table. */
typedef struct { unsigned char box, demand; TyKind ty; } SbReaderSave;
int sb_reader_shim_open(Compiler *c, int recv, char *sref, size_t cap, SbReaderSave *sv);
void sb_reader_shim_close(Compiler *c, int recv, const SbReaderSave *sv);
int sb_shadowed_reader(int node);
int str_mut_var_recv(Compiler *c, int recv);
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
extern int  g_block_id;
int builtin_method_known(const char *cls, const char *m);
int builtin_arity_violation(Compiler *c, int id);
int builtin_object_method_known(const char *m);
int name_is_enumerable_module_method(const char *m);
int scope_reads_callee(Compiler *c, int si);
int sp_yield_site_type(const Compiler *c, int id, TyKind *out);
TyKind block_next_value_ntype(const Compiler *c, int node);
/* Argument-hoist overrides (see emit_args_filled): node id -> rooted temp
   name substituted by emit_expr. */
#define MAX_ARG_OVERRIDE 64
extern int  g_argov_node[MAX_ARG_OVERRIDE];
extern char g_argov_text[MAX_ARG_OVERRIDE][16];
extern int  g_n_argov;
/* The setter call (`obj.x = v`) emit_stmt is lowering: nothing reads its value,
   so emit_object_call leaves the value temp out (see setter_value_open). */
extern int  g_setter_stmt_id;
extern int  g_sn_skip;   /* safe-nav re-entry marker (see codegen_util.c) */
extern int  g_pd_skip;
extern int  g_cls_tag_skip;   /* poly-dispatch builtin-arm re-entry marker */
int subtree_may_allocate(const NodeTable *nt, int id);
int subtree_has_side_effect(Compiler *c, int id);
/* When a yielding method is inlined, g_yield_block_fallback holds the block
   that was active in the CALLER's context so nested `yield`s inside the
   passed block can chain back to the outermost caller's block. */
extern int  g_yield_block_fallback;
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
extern int g_inline_recv_class;
/* When emitting class/module body statements, the class index (-1 outside). */
extern int g_class_body_id;
/* Class id of the scope currently being emitted (-1 if none). Used to resolve
   implicit self calls in included-module methods to the including class. */
extern int g_emitting_class_id;
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
int ctor_needs_self_defaults(Compiler *c, int initm, int argc);
void emit_ctor_alloc_init(Compiler *c, int cid, int initm, int argsNode, int call_id, Buf *b);
extern const char *g_ctor_self;
extern const char *g_ctor_self_deref;
extern const char *g_arm_self;
extern const Scope *g_arm_scope;
extern int g_arm_depth;
extern int g_expr_depth;
extern int g_proc_toplevel_return;
extern int g_exc_frame_depth;      /* live begin/rescue setjmp frames (see codegen_util.c) */
extern int g_method_pr_exc_depth;
extern int g_loop_exc_base;
extern int g_loop_ensure_base;  /* g_ensure_depth at the innermost C-loop entry:
   a `next` crossing ensure regions opened INSIDE the loop defers through them
   (runs their bodies) before the C continue */
extern int g_brk_exc_base;
extern int g_block_brk_exc_base;
/* Return type of the method currently being emitted, so a tail/return value
   can be boxed when the method returns poly but the value is concrete. */
extern TyKind g_ret_type;
extern int g_c_ret_void;   /* the C function returns void (a fiber body) */
extern int g_c_ret_void;   /* the C function returns void (a fiber body) */
extern const char *g_fn_pr_label;   /* real function's return funnel (see codegen_util.c) */
extern const char *g_fn_pr_var;
extern TyKind g_fn_ret_type;
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
typedef struct { int lid; int has_retval; int exc_base; TyKind retv_ty; } EnsureCtx;
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
   g_uses_symbols -> sp_tu_init sets sp_sym_name_fn; g_uses_regex -> sp_tu_init
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
extern int g_gen_obj_hash;
extern int g_gen_obj_to_json;  /* a package wants obj reflection + >=1 user #to_json */  /* a package wants obj reflection + >=1 struct: emit+install sp_obj_to_hash */
extern int g_gen_obj_to_h;  /* >=1 instantiated Struct/Data: emit+install sym-keyed sp_obj_to_h (poly #to_h) */
extern int g_gen_obj_with;  /* >=1 instantiated Data: emit+install sp_obj_with (poly Data#with) */
extern int g_uses_regex;
extern int g_uses_argv;
extern int g_uses_threads;
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
void emit_expr_slot(Compiler *c, int node, TyKind slot, Buf *b);
void emit_typed_sink_text(Compiler *c, int node, TyKind slot, const char *text, Buf *b);

/* ---- forward decls ---- */

int is_builtin_reopen(const char *name);
int is_exc_name(const char *n);
int class_is_exc_subclass(Compiler *c, int ci);
int class_has_subclass(Compiler *c, int ocid);
int exc_has_user_msg_override(Compiler *c);
int exc_has_nonstring_msg_override(Compiler *c);
int fi_fiber_stack_risk(Compiler *c);
const char *class_ruby_name(Compiler *c, int ci);
int scope_def_line(Compiler *c, Scope *s);
const char *scope_def_file(Compiler *c, Scope *s);
const char *obj_str_cname(Compiler *c, int cid, int want_inspect);
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
                            const char *objp, const char *src, TyKind at, Buf *b);
int  method_is_void(Scope *s);
void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
void emit_boxed(Compiler *c, int node, Buf *b);
void emit_recv_rooted(Compiler *c, int recv, int t, const char *rootm, Buf *b);
int  push_recv_in_slot(Compiler *c, int recv, int argc, const int *argv, TyKind art);
void emit_rat_coerce(Compiler *c, int node, Buf *b);
void emit_super(Compiler *c, int id, Buf *b);
int  emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);
/* A splat operand whose static type is nil or a scalar: Ruby spreads nil to
   nothing and any of the others to itself. */
int splat_operand_is_scalar(TyKind t);
/* A keyword key the callee has no parameter for: emits the ArgumentError and
   returns 1. Shared by emit_args_filled and the INLINE parameter binding, which
   walks parameters looking for keys and so could not see an unclaimed one
   (#4419). */
void emit_call_arity_check(Compiler *c, Scope *m, int argc, const int *argv, int judge_rest);
/* Collect a call's keywords no declared keyword parameter takes into a fresh
   sp_SymPolyHash temp for a `**kwrest` parameter; returns the temp id. */
int emit_kwrest_collect(Compiler *c, Scope *m, int kwh, int ds_hash_tmp, TyKind ds_hash_type, int argsNode);
int emit_unknown_kwarg_raise(Compiler *c, Scope *m, int kwh);
/* The argument of a String append (`<<` / `concat`), rendered for the append.
   An Integer -- typed OR boxed -- is a CODEPOINT, not its decimal digits. Shared
   because the rule was written twice and the second copy only had the typed half
   (#4425). */
void emit_str_append_arg(Compiler *c, int arg, Buf *b);
int rest_shortfall_required(Compiler *c, Scope *m);
/* Emit a hash key, unboxing a poly value to the typed-hash's key type. */
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
int hash_key_misses(Compiler *c, int key, TyKind kt);
const char *conv_wrong_cls_name(TyKind t);
const char *conv_cls_name_of(Compiler *c, TyKind t);
TyKind obj_container_conv(Compiler *c, TyKind t, const char *conv, int *def);
void emit_str_pattern_expr(Compiler *c, int node, Buf *b);
void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b);
int hold_recv_open(Compiler *c, int recv, int boxed, const char *ctype, const char *rootm,
                   Buf *b, Buf *rb);
int emit_iter_bind_rest(Compiler *c, int block, int np, TyKind elem_t, const char *elem_src, Buf *b, int indent);
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b);
/* For a reference-backed builtin type (a genuinely nilable C pointer that can
   be NULL), return the name of its SP_BUILTIN_* class-id constant; else NULL.
   Such a value must box via sp_box_nullable_obj so a NULL becomes SP_TAG_NIL. */
const char *ty_nullable_builtin_id(TyKind t);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
/* emit_unbox_text, but a nil-tagged poly lands on the slot's own nil (an int?
   or float? sentinel) instead of the zero payload under the tag (#3412). */
void emit_unbox_nilable_text(Compiler *c, TyKind t, const char *expr, Buf *b);
/* `recv.attr ||= v` / `&&=` where the reader or the writer is a real `def`:
   emits the reader/writer pair as an expression, or answers 0 to leave the
   caller's direct-ivar shapes alone. See codegen_expr.c. */
void emit_orw_guard(Compiler *c, int v, int boxed, const char *cond, const char *lhs, int value_form, int indent, Buf *b);
int emit_call_or_write_via_methods(Compiler *c, int id, int is_or, Buf *b);
/* Wrap a boxed expression in the --rbs seed assertion (a no-op macro without
   -DSP_RBS_CHECK) before it narrows into a seeded slot. */
int seeded_array_kind_mismatch(TyKind slot, TyKind vt);
void emit_array_store_value(Compiler *c, TyKind slot, int v, Buf *b);
void emit_rbs_checked_text(Compiler *c, TyKind slot, const char *slotname,
                           const char *expr, Buf *b);
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
TyKind comp_recv_type(Compiler *c, int recv);
int proc_slot_is_ptr(TyKind t);
int proc_slot_via_poly(Compiler *c, TyKind t);
int cell_is_typed_ptr(Compiler *c, LocalVar *lv);
int call_returns_nullable_int(Compiler *c, int node);
int recv_may_be_sentinel(Compiler *c, int node);
int nil_answers_name(const char *n);
void emit_sg_activate(Compiler *c, int node, int recv, Buf *b, int indent);
int sg_activates_ci(Compiler *c, int node);
int subtree_has_param_named_pub(const NodeTable *nt, int id, const char *nm);
const char *past_open_parens(const char *s);
void emit_inlined_local_decl(Compiler *c, LocalVar *lv, const char *rn, Buf *b, int din);
/* The assignment target for an inlined method's parameter, spelled by the same
   rule that declared it (a cell-promoted one is `(*_cell_x)`). See codegen.c. */
void emit_inlined_param_target(Compiler *c, Scope *m, const char *pname,
                               const char *rn, Buf *b);
const char *cell_scan_fn(TyKind t);
const char *cell_value_struct(TyKind t);
const char *cell_value_struct_empty(TyKind t);
void emit_cell_elem_type(Compiler *c, LocalVar *lv, Buf *b);
void emit_proc_call_args(Compiler *c, int argc, const int *argv, Buf *b, int force_poly);
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
int re_lit_flags(Compiler *c, int nid);
void emit_interp(Compiler *c, int id, Buf *b);
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b);
int nameset_has(NameSet *s, const char *nm);
void nameset_add(NameSet *s, const char *nm);
/* Emit the C lvalue for local `name` in the current emission context: a
   captured var inside a proc body -> the cell in _cap; a cell local in its
   enclosing scope -> `(*_cell_x)`; otherwise the plain `lv_x`. Reads and
   writes share this (a cell deref is a valid lvalue). */
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b);
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
/* One refusal: where and what. Recorded in order for --emit-types. */
typedef struct { const char *file; int line; const char *msg; } SpDiag;
extern SpDiag *g_diags;
extern int g_ndiags;
extern jmp_buf g_unsup_recover;    /* per-unit recovery point, armed by the driver */
extern int g_unsup_armed;          /* nonzero while a recovery point is live */
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
void emit_str_cmp_conv(Compiler *c, int node, int tmp, Buf *b);
void emit_str_cmp_prologue(Compiler *c, const char *rtxt, int operand,
                           int *tr, int *to, int *ts, Buf *b);
/* 1 iff any class defines a usable #to_int / #to_str -- see codegen.c. */
int prog_has_conv_method(Compiler *c, const char *conv, TyKind want);

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what);
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
int is_builtin_exception_name(const char *n);
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
const char *ffi_c_type(const char *spec);
/* Map an FFI type spec string to the C type used in extern prototypes.
   Uses standard C types to avoid conflicting with system headers. */
const char *ffi_cb_arg_ctype(const char *spec);
void ffi_extern_name(Compiler *c, int fi, Buf *out);
int ty_is_struct_valued(TyKind t);   /* see codegen_util.c: struct passed by value */
const char *native_c_type(const char *spec);
const char *default_value(TyKind t);
const char *raise_tail_value(TyKind t);
const char *raise_tail_value_c(Compiler *c, TyKind t);
void emit_bigint_operand_ext(Compiler *c, int node, Buf *b);
const char *nil_value(TyKind t);
const char *local_init_value(Compiler *c, LocalVar *lv);
int local_nil_test(Compiler *c, LocalVar *lv, const char *ref, Buf *out);
/* Append the C type name for `t` to `b` (objects need the class name). */
const char *class_ctype(Compiler *c, int cid);
void native_arg_check(Compiler *c, int id, const char *what, NativeMethod *m,
                      int argc, const int *argv);
void emit_ctype(Compiler *c, TyKind t, Buf *b);
/* Emit the boxing prefix/suffix to convert a typed value to sp_RbVal.
   Call as: emit_box_open(t, b); emit_expr(c, node, b); emit_box_close(t, b). */
void emit_box_open(Compiler *c, TyKind t, Buf *b);
const char *ptr_array_stamp(Compiler *c, TyKind t);   /* "SP_PTR_ELEM_x, cls" for sp_box_ptr_array_k (#4486) */
void emit_box_close(Compiler *c, TyKind t, Buf *b);
/* "Int" / "Str" / "Float" for the sp_<K>Array_* runtime family. */
const char *array_kind(TyKind t);
/* "Poly" / "Ptr" / array_kind, for a loop that walks the container. "Ptr" is
   a nested numeric table (sp_PtrArray of row pointers). */
const char *array_iter_kind(TyKind t);
const char *array_to_poly_fn(TyKind t);
/* comp_ntype for a fold seed, with an empty `[]` / `{}` literal resolved to
   its container kind rather than left TY_UNKNOWN (see types.c). */
TyKind fold_seed_ntype(Compiler *c, int node);
/* `sum(seed)` through sp_poly_sum_seed, with both operands boxed into rooted
   temporaries in receiver-then-seed order (see codegen_util.c). */
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
int emit_frozen_literal_open(Buf *b, size_t raw_len);
int emit_frozen_literal_open_a(Buf *b, size_t raw_len, int ascii7);
int bytes_are_ascii7(const char *s, size_t n);
void emit_frozen_literal_close(Buf *b, int id);
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
/* Collect a String `<<` chain's args outermost-first (max 64); *base gets
   the node the chain bottoms out at. Returns the link count. */
int str_append_chain(Compiler *c, int recv, int *chain, int *base);
int kwh_only_spreads(const NodeTable *nt, int kwh);
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
/* A method scope is shadowed (and must not be emitted) when a later
   scope redefines the same (class, name, is_cmethod) -- a reopened class
   where the last definition wins, matching comp_method_in_class. A top-level
   `def` is shadowed by a later top-level `def` of the same name. */
int scope_is_shadowed(Compiler *c, int s);
#define SP_MAX_PROC_FORM 4096
extern int g_pf_emitting;   /* inside a proc-form body (#3399) */
void scope_mark_proc_form(Compiler *c, int s);
void scope_veto_proc_form(Compiler *c, int s);
int  scope_needs_proc_form(Compiler *c, int s);
int  scope_proc_form_of(Compiler *c, int s);
int  ctor_init_proc_form(Compiler *c, int cid);
void scope_proc_form_begin(Compiler *c, int s);
void scope_proc_form_end(Compiler *c, int s);
int scope_has_callable_symbol(Compiler *c, int s);
int scope_toplevel_included(Compiler *c, int s);
int emit_forwarded_proc_arg(Compiler *c, int blk_node, Buf *b);
int struct_kwarg_value(Compiler *c, int kwh, const char *name);
/* Value-equality family: operands in the same nonzero family compare by value;
   different nonzero families are never == (Ruby does no cross-type coercion,
   except int/float which share family 1). 0 = not a simple comparable type. */
int eq_family(TyKind t);
/* Compile-time `is_a?` for a concrete builtin receiver type: 1 yes, 0 no,
   -1 not determinable here. `exact` is instance_of? (no ancestor match). */
int ty_matches_class(TyKind t, const char *cn, int exact);
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
int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty, int *ids_out, TyKind *ty_out, int cap);
int patch_lv_read_ntype(Compiler *c, int scope_idx, const char *name, TyKind new_ty, int min_id, int **saved_ids, TyKind **saved_tys);
void restore_lv_read_ntype(Compiler *c, int *saved_ids, TyKind *saved_tys, int n);
int emit_iter_autosplat(Compiler *c, int block, TyKind rt, const char *elem_src, int indent);
int emit_iter_value_expr(Compiler *c, int id, Buf *b);
int iter_value_answers_recv(Compiler *c, int id);
int sn_guard_pending(Compiler *c, int id);
int emit_takewhile_with_index(Compiler *c, int id, Buf *b);
int emit_transform_hash_expr(Compiler *c, int id, Buf *b);
int emit_bsearch_expr(Compiler *c, int id, Buf *b);
int emit_poly_uniq_block(Compiler *c, int id, Buf *b);
int emit_gsub_block_expr(Compiler *c, int id, Buf *b);
int emit_sum_block_expr(Compiler *c, int id, Buf *b);
int emit_sum_block_poly_expr(Compiler *c, int id, Buf *b);
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b);
int emit_product_inspect_expr(Compiler *c, int id, Buf *b);
int emit_step_array_expr(Compiler *c, int id, Buf *b);
int emit_inject_expr(Compiler *c, int id, Buf *b);
int emit_reduce_block_expr(Compiler *c, int id, Buf *b);
int emit_sortby_expr(Compiler *c, int id, Buf *b);
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b);
void emit_block_param_assign(Compiler *c, int scope_id, const char *nm, int tidx, TyKind et, Buf *b);
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b);
int emit_lazy_class_expr(Compiler *c, int id, Buf *b);
int emit_lazy_pipeline_expr(Compiler *c, int id, Buf *b);
int lazy_alias_write_suppressible(Compiler *c, int write);  /* lazy-alias write whose uses all force it */
int emit_lazy_size_expr(Compiler *c, int id, Buf *b);
int emit_native_ctor(Compiler *c, int id, int ci, int argc, const int *argv, Buf *b);
void emit_block_value_into(Compiler *c, int block, const char *dest,
                           int want_poly, int indent);
int emit_block_cond_next(Compiler *c, int block, int indent, Buf *out);
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
int poly_block_call_needs_dispatch(Compiler *c, int id);
void emit_obj_alloc_expr(Compiler *c, int cid, Buf *b);
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out);
int arg_wants_root(Compiler *c, TyKind pt, int provided);
void emit_rooted_operand(Compiler *c, TyKind pt, int provided, const char *expr, Buf *out);
int arg_slot_for_param(Compiler *c, Scope *m, int idx, int argc);
/* 1 when a parameter default reads an earlier parameter: it must be evaluated
   with that parameter bound (see emit_args_filled). */
int default_refs_earlier_param(Compiler *c, Scope *m);
int opt_before_required(Compiler *c, Scope *m);
/* `(sp_Parent *)` when an object value flows into an ancestor-typed slot; the
   layouts match by construction, but C needs the cast spelled (#3418). */
void emit_obj_upcast_prefix(Compiler *c, TyKind slot, TyKind val, Buf *b);
/* Value node for keyword `name` inside a KeywordHashNode, or -1. */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname);
int callee_has_kwarg(Compiler *c, Scope *m, const char *name);
int callee_param_is_declared_kwarg(Compiler *c, Scope *m, const char *name);
int callee_declares_kwargs(Compiler *c, Scope *m);
int emit_ds_hash_materialize(Compiler *c, int kwh, TyKind *out_type);
void emit_ds_param_extract(Compiler *c, Scope *m, int i, int ds_hash_tmp,
                           TyKind ds_hash_type, Buf *out);
/* analyze-side helpers also called from codegen (defined in analyze_util.c /
   analyze_scope.c; canonical declarations live in analyze_internal.h) */
int is_arith_op(const char *op);
TyKind an_builtin_answer(Compiler *c, int id);
int node_is_empty_container(const NodeTable *nt, int node);
TyKind ffi_spec_to_ty(const char *spec);
int local_sole_range_node(Compiler *c, int recv);
int range_float_begin(Compiler *c, int recv);
void emit_block_param_from_boxed(Compiler *c, const char *pname, TyKind pt, const char *src, Buf *b);
void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b);
void emit_rest_pack_kwh(Compiler *c, int from, int pos_argc, const int *argv, int kwh, Buf *b);
int rest_kwh_tail(Compiler *c, Scope *m, int kwh, int pos_argc);
int kwh_positional_slot(Compiler *c, Scope *m, int kwh, int pos_argc);
int kwh_arg_param(Compiler *c, Scope *m, int pos_argc);
/* An anonymous `*` forwarding the enclosing method's rest: its C expression. */
int emit_anon_rest_ref(Compiler *c, int splat, Buf *buf);
void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b);
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx, Compiler *c, int argv_from, int pos_argc, const int *argv, Buf *b);
int is_descendant(Compiler *c, int k, int anc);
int class_builtin_superclass(Compiler *c, int i);   /* codegen.c */
int class_builtin_parent(Compiler *c, int cid);      /* codegen.c */
int class_includes_module_named(Compiler *c, int cid, const char *mod_name);
int class_isa_user(Compiler *c, int k, int cid, const char *cn);  /* codegen_call.c */
int dispatch_impl_count(Compiler *c, int cid, const char *name);
void emit_dispatch(Compiler *c, int cid, const char *name, const char *selfptr, int argsNode, int blk_node, Buf *b);
int emit_tap_then_expr(Compiler *c, int id, Buf *b);
int recv_is_const(const NodeTable *nt, int recv, const char *name);
int sp_is_fiber_storage_recv(const NodeTable *nt, int recv);
int emit_ctor_yield_inline(Compiler *c, int id, int ci, Buf *b);
void emit_call(Compiler *c, int id, Buf *b);
/* Decode a CallNode's positional arguments: sets *argc and returns the argv
   array (NULL when the node has no arguments). Shared by the call emitters. */
const int *call_args(const NodeTable *nt, int id, int *argc);
/* Emit `node` into a fresh buffer and return it (caller reads .p, frees it).
   Collapses the `Buf b; memset(&b,0,sizeof b); emit_expr(c,node,&b);` idiom. */
Buf expr_buf(Compiler *c, int node);
/* Receiver-typed method-call emitters (codegen_call_recv.c). Each returns 1 if
   it handled the call and emitted into `b`, else 0 (emit_call falls through). */
int emit_arg_type_guards(Compiler *c, int id, Buf *b);
int emit_builtin_arity_guard(Compiler *c, int id, Buf *b);
int emit_blockless_enumerator(Compiler *c, int id, Buf *b);
int emit_unresolved_call(Compiler *c, int id, Buf *b);
int emit_array_call(Compiler *c, int id, Buf *b);
int emit_hash_call(Compiler *c, int id, Buf *b);
int emit_scalar_call(Compiler *c, int id, Buf *b);
int emit_object_call(Compiler *c, int id, Buf *b);
int emit_native_object_protocol(Compiler *c, int id, Buf *b);
/* The C test for "temp _t<tmp> of kind t holds nil" (want_nil) or "holds a
   non-nil value" (!want_nil), spelled per kind: the int/float sentinels, a
   NULL pointer, a poly tag. Shared by the index-or/and-write forms. */
void emit_slot_nil_test(Compiler *c, TyKind t, int tmp, int want_nil, Buf *b);
int emit_native_case_eq(Compiler *c, int cond, TyKind subj_t, const char *subj_ref, Buf *b);
int exc_subclass_defines(Compiler *c, const char *name);
int emit_value_recv_call(Compiler *c, int id, Buf *b);
int emit_range_call(Compiler *c, int id, Buf *b);
int emit_poly_call(Compiler *c, int id, Buf *b);
int diagnose_eval_call(Compiler *c, int id);
int diagnose_unsupported_call(Compiler *c, int id);
int diag_user_defines(Compiler *c, const char *name);
int user_defines_or_reads(Compiler *c, const char *name);
const char *array_index_bad_class(Compiler *c, int id);
extern int g_poly_builtin_arm;  /* emitting a poly dispatch's builtin arm */
void emit_complex_coerce(Compiler *c, int node, Buf *b);
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
int is_block_call(Compiler *c, int id);
int is_blockless_block_param_call(Compiler *c, int id);
const char *blockless_block_param_call_name(Compiler *c, int id);
void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr, TyKind want_ty);
void emit_yield_proc_call(Compiler *c, int args_node, TyKind result_ty, Buf *b, int indent, int as_expr);
int emit_inline_expr(Compiler *c, int id, Buf *b);
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig, const char *p0_ren, TyKind src_type, const char *src_expr, Buf *b, int indent);
int subtree_has_own_redo(const NodeTable *nt, int id);
void emit_loop_body(Compiler *c, int body, Buf *b, int indent);
int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
int emit_array_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
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
int emit_output_spilled(Compiler *c, const char *name, int argc, const int *argv, Buf *b, int indent);
void emit_assign(Compiler *c, int id, Buf *b, int indent);
void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
int emit_array_op_assign(Compiler *c, const char *lval, TyKind t, const char *op, int v, Buf *b);
int emit_scalar_op_assign(Compiler *c, const char *lval, TyKind t, const char *op,
                          int v, int capture, Buf *b);
void emit_cond(Compiler *c, int id, Buf *b);
int static_isa_cond(Compiler *c, int pred);
int static_nil_ivar_cond(Compiler *c, int pred);
void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail);
int emit_poly_class_when(Compiler *c, int cond_id, const char *tmp, Buf *b);
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
int needs_root(TyKind t);
/* Whether a variable of an inferred type takes a root, and the root itself,
   picking the rbval macro for boxed poly and the string one for a String. */
int ty_gc_rootable(Compiler *c, TyKind t);
void emit_gc_root_var(Compiler *c, TyKind t, const char *name, Buf *b);
void emit_gc_root_tmp(Compiler *c, TyKind t, int tmp, Buf *b);
/* `_t<tmp>` when the node was already evaluated into that temp, else the node */
void emit_node_or_tmp(Compiler *c, int node, int tmp, Buf *b);
/* the key of a hash store, as the kind's set takes it (codegen_stmt.c) */
void emit_hash_store_key(Compiler *c, int key, TyKind rt, Buf *b);
const char *hash_box_cls(TyKind t);
const char *hash_order_key(TyKind t, int tr, int ti);
const char *hash_order_val(TyKind t, int tr, int ti);
int emit_hash_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
                          const char *rs, Buf *b, int indent, int *tr, int *torig, int *twp);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
TyKind yield_site_type(Compiler *c, int node);
void emit_int_expr(Compiler *c, int node, Buf *b);
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
/* strict with CRuby's rb_convert_type wording ("of nil into Integer") */
void emit_int_expr_conv(Compiler *c, int node, Buf *b);
int emit_unresolved_coerced(Compiler *c, int node, TyKind target, Buf *b);
void emit_int_divisor(Compiler *c, int node, Buf *b);
void emit_float_expr(Compiler *c, int node, Buf *b);
void emit_float_coerce_expr(Compiler *c, int node, Buf *b);
/* Emit `node` as a scalar operand: like a plain emit_expr, except an
   unresolved-constant read (which lowers to a NameError raise valued as an
   sp_Class struct) is voided and replaced by `zero` ("0" / "0.0"), so the
   raise survives but the struct never reaches an int/float slot. */
void emit_scalar_operand(Compiler *c, int node, const char *zero, Buf *b);
void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol);
void declare_local_named(Compiler *c, Buf *b, LocalVar *lv, const char *name, int vol);
void emit_cell_shadow_store(Compiler *c, Scope *encl, const char *name, Buf *b, int indent);
int scope_has_begin(Compiler *c, int si);
void emit_scope_decls(Compiler *c, Scope *s, Buf *b);
int method_is_void(Scope *s);
void emit_method_cname(Compiler *c, Scope *s, Buf *b);
void emit_poly_iter_obj_normalize(Compiler *c, int tv, Buf *b);
void emit_method_signature(Compiler *c, Scope *s, Buf *b);
void emit_method(Compiler *c, Scope *s, Buf *b);
int is_nested_block(const char *ty);
void proc_collect_locals(Compiler *c, int id, NameSet *locals);
int conv_reads_shared_storage(Compiler *c, int node);
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
extern const char *g_iow_recv_ref;
extern const char *g_iow_key_ref;

#endif
