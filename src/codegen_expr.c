#include "codegen_internal.h"
#include "repr.h"
#include "share.h"
#include "holder.h"
#include "call_plan.h"
#include "value_pos.h"
#include "codegen_call_arms.h"

/* defined? support: does this subtree reference a constant the compiler
   cannot resolve? Any such reference makes the whole defined? answer nil
   (CRuby checks every reference without evaluating). */
static int subtree_has_unresolved_const(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  NodeKind ty_k = nt_kind(c->nt, id);
  if (!ty) return 0;
  if (ty_k == NK_ConstantReadNode) {
    const char *cn = nt_str(c->nt, id, "name");
    if (!cn) return 0;
    if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) return 0;
    if (comp_is_wellknown_const(cn)) return 0;
    return 1;
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_unresolved_const(c, nt_ref_at(c->nt, id, i))) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (subtree_has_unresolved_const(c, ids[k])) return 1;
  }
  return 0;
}

/* Adjacent string literals joined by `\`-continuation (or `+`-folded at parse
   time) produce an InterpolatedStringNode whose own parts are themselves
   InterpolatedStringNodes. Flatten the part tree into one list of leaf parts
   (StringNode / EmbeddedStatementsNode) so the format/arg assembly below sees a
   single flat sequence and emits one sp_sprintf call. */
static void interp_flatten_own(const NodeTable *nt, int id, int **out, int **own, int *n, int *cap) {
  int pn = 0;
  const int *parts = nt_arr(nt, id, "parts", &pn);
  for (int k = 0; k < pn; k++) {
    int pid = parts[k];
    NodeKind pk = nt_kind(nt, pid);
    if (pk == NK_InterpolatedStringNode) {
      interp_flatten_own(nt, pid, out, own, n, cap);
      continue;
    }
    if (*n >= *cap) {
      int ncap = *cap ? *cap * 2 : 8;
      int *grown = realloc(*out, (size_t)ncap * sizeof(int));
      int *og = own ? realloc(*own, (size_t)ncap * sizeof(int)) : NULL;
      if (!grown || (own && !og)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      *out = grown; if (own) *own = og;
      *cap = ncap;
    }
    if (own) (*own)[*n] = id;
    (*out)[(*n)++] = pid;
  }
}
static void interp_flatten(const NodeTable *nt, int id, int **out, int *n, int *cap) {
  interp_flatten_own(nt, id, out, NULL, n, cap);
}

/* Does the InterpolatedStringNode id fold to one literal: every leaf part of
   its flattened tree a StringNode (`"a" "b"`, a squiggly heredoc whose lines
   indent differently)? That is the condition emit_interp folds on
   (ndyn_or_scalar == 0 below), asked of the node alone. */
int interp_is_literal_fold(const NodeTable *nt, int id) {
  int *flat = NULL, n = 0, cap = 0, fold = 1;
  interp_flatten(nt, id, &flat, &n, &cap);
  for (int k = 0; k < n && fold; k++)
    if (nt_kind(nt, flat[k]) != NK_StringNode) fold = 0;
  free(flat);
  return fold;
}

/* Single-buffer construction: every part is either an escaped literal
   (compile-time length), a bounded-width scalar (int <= 21 digits, bool
   <= 5), or a dynamic part pre-evaluated -- in part order, preserving
   side-effect order -- into a rooted `const char *` temp whose byte length
   feeds the capacity sum. One sp_str_alloc_raw then serves the whole
   string; sp_w_* writers append each part. (The previous lowering built
   one heap string per part and ran an sp_sprintf pass over them.)
   The plan is shared with the append form (`s << "..#{x}.."`), which
   writes the same parts into the receiver instead of a fresh string. */
enum { WK_LIT, WK_INT, WK_BOOL, WK_NIL, WK_DYN };
/* neutral: the temp of a boxed part's flag, set when its value was no
   String, or -1; such a part takes no part in the result's encoding */
typedef struct { int kind; int tmp; int lit_off; int lit_esc_len; long lit_len; int plain; int neutral; } WPart;
typedef struct {
  WPart *wp; int nwp; int ndyn_or_scalar;
  Buf lits;      /* escaped literal texts, concatenated */
  Buf decls;     /* the pre-evaluated temps, as statements */
  long fixed_cap;
  int *flat;
} InterpPlan;
static void interp_plan_free(InterpPlan *pl) {
  free(pl->lits.p); free(pl->decls.p); free(pl->wp); free(pl->flat);
}
/* --share-strings: is part k of parts[0..n), whose value is expr, a call
   publishing the handle of a String the rule shares
   (call_publishes_shared_handle) before a later part that can change a String in place
   (operand_may_change_string)? CRuby reads every part's String once all of
   them ran (`"#{x.change} #{x.change}"`); its bytes taken as the part ran,
   the result kept the String as it was then. */
static int interp_part_reads_late(Compiler *c, const int *parts, int n, int k, int expr) {
  /* (a call publishing the handle of a String the rule shares, a read of a
     slot that is the handle, a boxed receiver's call whose arms answer one:
     any value the String plan can hand over as the handle) */
  int u = nt_paren_single(c->nt, expr);
  int holds = call_publishes_shared_handle(c, expr);
  if (!holds && u >= 0 && comp_ntype(c, u) == TY_STRING && !arg_ran_first(u, 0))
    holds = share_plan_with_need(c, u, SPR_TAKE, SPN_OBJECT).form == SPF_HANDLE &&
            share_plan_producer(c, u, NULL) != SPK_NONE;
  if (!holds) return 0;
  for (int j = k + 1; j < n; j++)
    if (operand_may_change_string(c, parts[j])) return 1;
  return 0;
}

/* ...is nm, called with argc arguments on a String, a conversion (to_s,
   to_str, +@) that turns a nil receiver into a String? */
static int interp_nil_conversion(const char *nm, int argc) {
  return argc == 0 && is_string_conversion(nm);
}
/* ...is nm a String method that answers its receiver or nil (`upcase!`,
   `sub!`, `strip!`)? */
static int interp_link_nilable(const char *nm, int argc, int has_blk) {
  return (bop_answers_self(TY_STRING, nm, argc, has_blk) & BOPF_SELF_OR_NIL) != 0;
}
/* ...is n a writer of the instance variable iv, an attr_writer's or a
   `def s=` (`o.s = x`, `o.s, y = ...`), on any receiver, self among them? */
static int interp_is_ivar_writer(const NodeTable *nt, int n, const char *iv) {
  NodeKind k = nt_kind(nt, n);
  const char *nm = k == NK_CallNode || k == NK_CallTargetNode ? nt_str(nt, n, "name") : NULL;
  size_t l = nm ? strlen(nm) : 0;
  return l > 1 && nm[l - 1] == '=' && iv[0] == '@' && strlen(iv + 1) == l - 1 && !strncmp(nm, iv + 1, l - 1);
}
/* --share-strings: what the subtree at n does to the variable v reads (a
   local, an instance variable or a global of that name): bit 1, a String
   mutator called on it; bit 2, a write that rebinds it. */
static int interp_changes_var(Compiler *c, int n, NodeKind vk, const char *vn) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  int bits = 0;
  if (k == NK_CallNode && vn) {
    int r = nt_ref(nt, n, "receiver");
    const char *nm = nt_str(nt, n, "name");
    if (r >= 0 && nt_kind(nt, r) == vk && nt_str(nt, r, "name") && sp_streq(nt_str(nt, r, "name"), vn) && nm &&
        (is_string_rebind_mutator(nm) || bop_name_mutates(nm, 0)))
      bits |= 1;
  }
  if (vk == NK_InstanceVariableReadNode && vn && interp_is_ivar_writer(nt, n, vn)) bits |= 2;
  if (vn && nt_str(nt, n, "name") && sp_streq(nt_str(nt, n, "name"), vn) &&
      ((vk == NK_LocalVariableReadNode && (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOperatorWriteNode ||
                                           k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
                                           k == NK_LocalVariableTargetNode)) ||
       (vk == NK_InstanceVariableReadNode && (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOperatorWriteNode ||
                                              k == NK_InstanceVariableOrWriteNode || k == NK_InstanceVariableAndWriteNode ||
                                              k == NK_InstanceVariableTargetNode)) ||
       (vk == NK_GlobalVariableReadNode && (k == NK_GlobalVariableWriteNode || k == NK_GlobalVariableOperatorWriteNode ||
                                            k == NK_GlobalVariableOrWriteNode || k == NK_GlobalVariableAndWriteNode ||
                                            k == NK_GlobalVariableTargetNode))))
    bits |= 2;
  /* code the walk cannot see into: a method of the program's own, a dispatch,
     a send, a proc's call, a block, a yield, a super. It may change the
     String in place through a parameter it is handed (`f(s)`), and it may
     rebind a global or an instance variable (`def reset = (@s = +"z")`), or a
     local a proc captured: the part then keeps the String it named. */
  {
    const char *nm = k == NK_CallNode ? nt_str(nt, n, "name") : NULL;
    int has_blk = k == NK_CallNode && nt_ref(nt, n, "block") >= 0;
    int callish = nm && is_rebinding_reach_call(nm);
    int user = k == NK_CallNode && (cplan_user(c, n)->mi >= 0 || cplan_user(c, n)->dispatch != CP_NONE);
    int opaque = user || has_blk || callish || k == NK_YieldNode || k == NK_SuperNode || k == NK_ForwardingSuperNode ||
                 k == NK_LambdaNode;
    if (opaque && vn) {
      if (user) {
        int args = nt_ref(nt, n, "arguments"), an = 0;
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        for (int i = 0; i < an; i++)
          if (nt_kind(nt, av[i]) == vk && nt_str(nt, av[i], "name") && sp_streq(nt_str(nt, av[i], "name"), vn)) bits |= 1;
      }
      Scope *sc = comp_scope_of(c, n);
      if (vk == NK_LocalVariableReadNode) {
        if (has_blk || callish || k == NK_YieldNode || k == NK_LambdaNode) bits |= 2;
      }
      else if (vk != NK_InstanceVariableReadNode || !sc || subtree_may_write_ivar(c, n, vn, sc->class_id, 0))
        bits |= 2;
    }
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++) bits |= interp_changes_var(c, nt_ref_at(nt, n, i), vk, vn);
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) bits |= interp_changes_var(c, ids[j], vk, vn);
  }
  return bits;
}
/* ...the instance variable a call to a method of the program's own that
   answers it (`def m = @s`, an attr_reader) reads: its name, or NULL */
static const char *interp_reader_var(Compiler *c, int call, char *buf, size_t cap) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, call) != NK_CallNode) return NULL;
  const CallPlan *p = cplan_user(c, call);
  if (p->mi >= 0 && p->dispatch == CP_DIRECT) {
    int body = c->scopes[p->mi].body, n = 0;
    const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
    int last = n == 1 ? st[0] : (body >= 0 && nt_kind(nt, body) == NK_InstanceVariableReadNode ? body : -1);
    return last >= 0 && nt_kind(nt, last) == NK_InstanceVariableReadNode ? nt_str(nt, last, "name") : NULL;
  }
  /* an attr_reader of the class the call is made in (self) */
  const char *nm = nt_str(nt, call, "name");
  int r = nt_ref(nt, call, "receiver");
  Scope *sc = comp_scope_of(c, call);
  if (p->mi < 0 && nm && (r < 0 || nt_kind(nt, r) == NK_SelfNode) && nt_ref(nt, call, "arguments") < 0 &&
      nt_ref(nt, call, "block") < 0 && sc && sc->class_id >= 0 && !sc->is_cmethod &&
      comp_reader_in_chain(c, sc->class_id, nm, NULL)) {
    snprintf(buf, cap, "@%s", nm);
    return buf;
  }
  return NULL;
}
/* ...is part k, a read of a variable that holds its bytes (no handle), changed
   in place by a later part and rebound by none? CRuby reads the String after
   the last part ran, so the variable is read there (a rebinding write keeps
   the String the part named). */
static int interp_var_read_late(Compiler *c, const int *parts, int n, int k, int expr, int *base) {
  const NodeTable *nt = c->nt;
  int u = nt_paren_single(c->nt, expr);
  if (u < 0) return 0;
  int kind = 1, b = u, nil_converted = 0;
  /* a String method that answers its receiver, or nil (`s << "a"`,
     `s.upcase!`, `s.itself`, `s.to_s`, `+s`, `s&.itself`), over a variable: the
     part is the variable's String itself, the call run where it stands and the
     String read after the last part */
  while (b >= 0 && nt_kind(nt, b) == NK_CallNode) {
    const char *nm = nt_str(nt, b, "name");
    int r = nt_paren_single(c->nt, nt_ref(nt, b, "receiver")), argc = call_plain_argc(c, b);
    if (!nm || r < 0 || comp_ntype(c, r) != TY_STRING || cplan_user(c, b)->mi >= 0) break;
    int self = (bop_answers_self(TY_STRING, nm, argc, nt_ref(nt, b, "block") >= 0) & (BOPF_SELF | BOPF_SELF_OR_NIL)) ||
               (argc == 0 && is_string_self_answer(nm));
    if (!self) break;
    /* (a nil from a bang below a conversion reads as "" there, not as the variable) */
    if (nil_converted && interp_link_nilable(nm, argc, nt_ref(nt, b, "block") >= 0)) return 0;
    if (interp_nil_conversion(nm, argc)) nil_converted = 1;
    kind = 2;
    b = r;
  }
  if (kind == 2 && (nt_kind(nt, b) == NK_CallNode)) kind = 1, b = u;
  /* (a call that answers an instance variable is a read of it) */
  char rvb[96];
  const char *rv = kind == 1 ? interp_reader_var(c, b, rvb, sizeof rvb) : NULL;
  NodeKind uk = rv ? NK_InstanceVariableReadNode : nt_kind(nt, b);
  const char *un = rv ? rv : nt_str(nt, b, "name");
  if (uk != NK_LocalVariableReadNode && uk != NK_InstanceVariableReadNode && uk != NK_GlobalVariableReadNode) return 0;
  if (!un || comp_ntype(c, u) != TY_STRING || comp_ntype(c, b) != TY_STRING || arg_ran_first(u, 0)) return 0;
  int bits = 0;
  for (int j = k + 1; j < n; j++) bits |= interp_changes_var(c, parts[j], uk, un);
  if (bits != 1) return 0;
  if (base) *base = b;
  return kind;
}

static void interp_plan(Compiler *c, int id, InterpPlan *pl) {
  const NodeTable *nt = c->nt;
  int n = 0;
  int *flat = NULL, fcap = 0, *own = NULL;
  interp_flatten_own(nt, id, &flat, &own, &n, &fcap);
  const int *parts = flat;
  /* (each adjacent literal is built on its own: a part is held only until
     the end of its segment, sn[k] parts in) */
  int *sn = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  if (!sn) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = n - 1; k >= 0; k--)
    sn[k] = k + 1 < n && own[k + 1] == own[k] ? sn[k + 1] : k + 1;
  WPart *wp = malloc(sizeof(WPart) * (size_t)(n > 0 ? n : 1));
  if (!wp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int nwp = 0, ndyn_or_scalar = 0;
  Buf lits; memset(&lits, 0, sizeof lits);
  Buf decls; memset(&decls, 0, sizeof decls);
  /* a part that is a shared String slot's read is a fresh copy of its
     bytes; CRuby interpolates the String as it is when the whole string
     is built, so beside a later part that runs code (`"#{a}#{a << "x"}"`)
     the copy is read after every part, here */
  Buf late; memset(&late, 0, sizeof late);
  long fixed_cap = 0;

  for (int k = 0; k < n; k++) {
    int pid = parts[k];
    NodeKind pk = nt_kind(nt, pid);
    /* a segment's held parts are read where it ends */
    if (k > 0 && own[k] != own[k - 1] && late.p) { buf_puts(&decls, late.p); late.len = 0; late.p[0] = '\0'; }
    if (pk == NK_StringNode) {
      const char *content = nt_str(nt, pid, "content");
      int off = lits.len;
      long blen = 0;
      /* the node's own byte length, not strlen: an adjacent-literal part
         holding a NUL (`"\0" "1"`) stopped the walk and the byte was
         dropped from the fold (the opportunistic NUL policy) */
      size_t clen = content ? nt_str_len(nt, pid, "content") : 0;
      for (size_t ci = 0; ci < clen; ci++, blen++) {
        unsigned char ch = (unsigned char)content[ci];
        if (ch == '\\') buf_puts(&lits, "\\\\");
        else if (ch == '"') buf_puts(&lits, "\\\"");
        else if (ch == '\n') buf_puts(&lits, "\\n");
        else if (ch == '\t') buf_puts(&lits, "\\t");
        else if (ch == '\r') buf_puts(&lits, "\\r");
        else if (ch >= 0x20 && ch < 0x7f) buf_printf(&lits, "%c", ch);
        else buf_printf(&lits, "\\%03o", ch);
      }
      wp[nwp].kind = WK_LIT; wp[nwp].tmp = -1; wp[nwp].plain = 0; wp[nwp].neutral = -1;
      wp[nwp].lit_off = off; wp[nwp].lit_esc_len = lits.len - off;
      wp[nwp].lit_len = blen;
      nwp++;
      fixed_cap += blen;
    }
    else if (pk == NK_EmbeddedStatementsNode) {
      int st = nt_ref(nt, pid, "statements");
      int bn = 0;
      const int *body = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
      int expr = bn > 0 ? body[bn - 1] : -1;
      /* `#{ s1; s2; ...; sN }` evaluates every statement in order and uses sN's
         value. Run the leading statements for side effects; if sN is itself an
         assignment, perform it and read the assigned variable back as the value. */
      char vexpr[48]; vexpr[0] = 0;
      /* ...into this part's place in `decls`, in order with the parts around
         it: hoisted to g_pre they ran before the whole enclosing expression,
         so `"#{x}" + "#{x = "b"}"` read x after the assignment (#5574) */
      for (int si = 0; si + 1 < bn; si++) emit_stmt(c, body[si], &decls, 0);
      NodeKind ek = nt_kind(nt, expr);
      /* a part of a class the program reopened with its own to_s is already
         `<part>.to_s` (desugar_interp_reopened_to_s) */
      TyKind t = comp_ntype(c, expr);
      /* A bare implicit-self call inside an included-module method is analyzed
         generically (self type unknown -> TY_UNKNOWN), but codegen emits the
         method for a concrete class. Re-resolve the call against the class
         being emitted so the interpolation dispatches on the real return type
         (the same self-class lookup codegen uses for implicit-self calls). */
      if (t == TY_UNKNOWN && ek == NK_CallNode &&
          nt_ref(nt, expr, "receiver") < 0) {
        const char *cn = nt_str(nt, expr, "name");
        Scope *cs = comp_scope_of(c, expr);
        int dcid = g_ie_class_id >= 0 ? g_ie_class_id
                 : g_emitting_class_id >= 0 ? g_emitting_class_id
                 : cs->class_id;
        if (cn && dcid >= 0) {
          int mi = call_target(c, expr, dcid, cn, 0, NULL);
          if (mi >= 0) t = c->scopes[mi].ret;
        }
      }
      if ((ek == NK_LocalVariableWriteNode || ek == NK_LocalVariableOperatorWriteNode ||
                  ek == NK_LocalVariableOrWriteNode || ek == NK_LocalVariableAndWriteNode)) {
        emit_stmt(c, expr, &decls, 0);
        const char *vn = nt_str(nt, expr, "name");
        LocalVar *lvp = vn ? scope_local(comp_scope_of(c, expr), vn) : NULL;
        if (lvp) t = lvp->type;
        int tv = ++g_tmp;
        emit_ctype(c, t, &decls);
        buf_printf(&decls, " _t%d = ", tv); emit_local_ref(c, expr, vn, &decls);
        if (lvp && slot_is_oint(lvp)) buf_puts(&decls, ".v");   /* the written value, never nil */
        buf_puts(&decls, "; ");
        snprintf(vexpr, sizeof vexpr, "_t%d", tv);
      }
      /* --share-strings: such a part's handle is taken here, in order, and
         read as the String it holds after every part ran (late) */
      if (!vexpr[0] && bn == 1 && interp_part_reads_late(c, parts, sn[k], k, expr)) {
        Buf hb; memset(&hb, 0, sizeof hb);
        emit_strbuf_handle_of(c, expr, &hb);
        int hv = ++g_tmp, tv3 = ++g_tmp;
        buf_printf(&decls, "sp_String *_t%d = SP_STR_HDL(%s); SP_GC_ROOT(_t%d); ", hv, hb.p ? hb.p : "NULL", hv);
        buf_printf(&late, "const char *_t%d = sp_str_or_empty(sp_strbuf_read(_t%d)); SP_GC_ROOT(_t%d); "
                   "size_t _l%d = sp_str_byte_len(_t%d); ", tv3, hv, tv3, tv3, tv3);
        free(hb.p);
        wp[nwp].kind = WK_DYN; wp[nwp].tmp = tv3; wp[nwp].plain = 0; wp[nwp].neutral = -1;
        wp[nwp].lit_off = 0; wp[nwp].lit_esc_len = 0; wp[nwp].lit_len = 0;
        nwp++;
        ndyn_or_scalar++;
        continue;
      }
      /* --share-strings: a call answering its receiver, over a variable that a
         later part changes in place, is run here and its String read late */
      int late_call = t == TY_STRING && !vexpr[0] && interp_var_read_late(c, parts, sn[k], k, expr, NULL) == 2;
      /* Build this part's conversion expression. Bounded scalars keep their
         native C type (written digit-by-digit later); everything else
         converts to a marker-carrying string whose byte length is summed. */
      Buf conv; memset(&conv, 0, sizeof conv);
      int wkind = WK_DYN, neutral = -1;
      char *iv_pre = NULL;   /* the value's text, emitted once by the probe below */
      #define EMIT_IV() do { if (vexpr[0]) buf_puts(&conv, vexpr); \
                             else if (iv_pre) buf_puts(&conv, iv_pre); \
                             else emit_expr(c, expr, &conv); } while (0)
      /* A part that DIVERGES: the raise emitters close their expression with a
         dead placeholder of whatever type the node had, and the arm chosen
         from that node's type may disagree with it -- an unresolvable constant
         read renders `(sp_raise_cls(...), (sp_Class){-1})` while the node types
         poly, so sp_poly_to_s took an sp_Class (#4092). Discard it and answer
         the empty string; the raise means nothing reads the answer. */
      /* an Integer or Float part that may be nil is rendered from its oint
         (nil writes nothing) */
      int oint_part = oint_kind(t) && !vexpr[0] && node_has_oint_form(c, expr);
      { Buf ivp; memset(&ivp, 0, sizeof ivp);
        int ivp_emitted = 0;
        if (vexpr[0]) buf_puts(&ivp, vexpr);
        else { if (oint_part) emit_oint_expr(c, expr, t, &ivp); else emit_expr(c, expr, &ivp); ivp_emitted = 1; }
        const char *ivt = ivp.p ? ivp.p : "";
        /* a call to a method that always raises diverges the same way, and
           its void C form has no arm to take (it was refused) */
        if (strncmp(past_open_parens(ivt), "sp_raise_", 9) == 0 ||
            (!vexpr[0] && call_never_returns(c, expr))) {
          buf_printf(&conv, "((void)(%s), sp_str_empty)", ivt);
          free(ivp.p);
          goto iv_done;
        }
        /* The probe EMITTED the value; emitting it again for the real arm
           would run its g_pre hoists and temp numbering twice. Hand the text
           to EMIT_IV instead, the way a pre-evaluated temp already does. */
        if (ivp_emitted && ivp.p) { free(iv_pre); iv_pre = ivp.p; }
        else free(ivp.p); }
      if (t == TY_INT && oint_part) {
        /* nil-aware: a nullable Integer holding nil writes nothing */
        buf_puts(&conv, "sp_int_opt_to_s("); EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_INT) {
        wkind = WK_INT;
        EMIT_IV();
      }
      else if (t == TY_STRING) {
        /* a nullable string (NULL) interpolates as the empty string (a part
           read after the last part keeps its nil, to read nothing then) */
        if (late_call) EMIT_IV();
        /* (one-String mode: a value of either face, `"#{file || "-"}"`
           with file a String-or-nil handle) */
        else { buf_puts(&conv, "sp_str_or_empty(SP_STR_B("); EMIT_IV(); buf_puts(&conv, "))"); }
      }
      else if (t == TY_STRBUF) {
        /* (one representation: a part whose value is a handle, a write's in
           value position; read through its face) */
        buf_puts(&conv, "sp_str_or_empty(SP_STR_B("); EMIT_IV(); buf_puts(&conv, "))");
      }
      else if (t == TY_FLOAT) {
        /* and a nullable Float holding nil writes nothing too */
        buf_puts(&conv, oint_part ? "sp_float_opt_to_s(" : "sp_float_opt_to_s(sp_ofloat_of(");
        EMIT_IV(); buf_puts(&conv, oint_part ? ")" : "))");
      }
      else if (t == TY_BOOL) {
        wkind = WK_BOOL;
        EMIT_IV();
      }
      else if (t == TY_SYMBOL) {
        buf_puts(&conv, "sp_sym_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_POLY) {
        /* a boxed value that is no String (an Integer, nil, ...) reads as
           US-ASCII text, which takes no part in the encoding as a String part
           does: "#{1}#{b}" with a binary b is binary */
        neutral = ++g_tmp;
        int tvp = ++g_tmp;
        buf_printf(&decls, "int _t%d = 0; ", neutral);
        buf_printf(&conv, "({ sp_RbVal _t%d = ", tvp);
        EMIT_IV();
        buf_printf(&conv, "; _t%d = !(sp_poly_is_strbuf(_t%d) || _t%d.tag == SP_TAG_STR); sp_poly_to_s(_t%d); })",
                   neutral, tvp, tvp, tvp);
      }
      else if (t == TY_EXCEPTION) {
        /* an exception interpolates as its to_s, the same helper e.to_s
           lowers to: the message alone left a user to_s out */
        if (exc_has_user_msg_override(c)) {
          buf_puts(&conv, "sp_user_exc_to_s((sp_Exception *)(");
          EMIT_IV(); buf_puts(&conv, "))");
        } else {
          buf_puts(&conv, "sp_exc_message(");
          EMIT_IV(); buf_puts(&conv, ")");
        }
      }
      else if (t == TY_NIL) {
        wkind = WK_NIL;
        EMIT_IV();
      }
      else if (t == TY_RATIONAL) {
        buf_puts(&conv, "sp_rational_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_COMPLEX) {
        buf_puts(&conv, "sp_complex_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_TIME) {
        buf_puts(&conv, "sp_time_to_s_v(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      /* a Range interpolates as its to_s, the same renderers Range#to_s
         lowers to; with no arm every Range kind was refused (#4824) */
      else if (t == TY_RANGE) {
        buf_puts(&conv, "sp_range_str(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_FLOAT_RANGE) {
        buf_puts(&conv, "sp_frange_inspect(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_STR_RANGE) {
        buf_puts(&conv, "sp_srange_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      /* a Regexp, Array or Hash slot's nil is NULL, which interpolates as
         the empty string the way a nullable String's does */
      else if (t == TY_REGEX || t == TY_POLY_ARRAY || (ty_is_array(t) && array_kind(t)) ||
               (ty_is_hash(t) && ty_hash_cname(t))) {
        int ntv = ++g_tmp;
        buf_puts(&conv, "({ "); emit_ctype(c, t, &conv);
        buf_printf(&conv, " _t%d = ", ntv);
        EMIT_IV();
        buf_printf(&conv, "; _t%d ? ", ntv);
        if (t == TY_REGEX) buf_printf(&conv, "sp_re_to_s_str((void *)_t%d)", ntv);
        else if (t == TY_POLY_ARRAY) buf_printf(&conv, "sp_PolyArray_inspect(_t%d)", ntv);
        else if (ty_is_hash(t)) buf_printf(&conv, "sp_%sHash_inspect(_t%d)", ty_hash_cname(t), ntv);
        else buf_printf(&conv, "sp_%sArray_inspect(_t%d)", array_kind(t), ntv);
        buf_puts(&conv, " : sp_str_empty; })");
      }
      else if (ty_is_object(t) && comp_ary_is_str(c, ty_object_class(t))) {
        /* a String subclass instance is a String: its bytes, with no #to_s
           call -- CRuby's rb_obj_as_string takes any T_STRING as it is, so
           a `def to_s` of the subclass does not run here (`"#{sb}"`), while
           its answer, the instance again for a SafeBuffer's `self`, reads
           the same way (`"#{sb.to_s}"`); called through the struct's own
           #to_s, the instance's pointer landed in the const char * and the
           C did not build. A part that may be nil is nil's to_s, the empty
           String. */
        int to = ++g_tmp;
        buf_printf(&conv, "({ sp_String *_t%d = (sp_String *)(", to);
        EMIT_IV();
        buf_printf(&conv, "); _t%d ? sp_strbuf_read(_t%d) : sp_str_empty; })", to, to);
      }
      else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
        const char *cn = obj_str_cname(c, ty_object_class(t), 0);
        /* a poly-returning to_s (String unioned with e.g. nil) yields a boxed
           sp_RbVal -- render it through sp_poly_to_s so it's a const char *
           in the interpolation (#3266). */
        int ret_poly = obj_str_ret_poly(c, ty_object_class(t), 0);
        if (ret_poly) buf_puts(&conv, "sp_poly_to_s(");
        /* a part that may be nil (the nil fact, #7444) reads as nil's to_s,
           the empty String, where its class's to_s would run on no object */
        int part_nil = !ret_poly && !comp_ty_value_obj(c, t) && nil_fact_tracked(t) && nil_fact_node(c, expr);
        /* a value-type object (single-ivar) has a by-VALUE to_s signature;
           casting its receiver to a pointer is a C type error (#2357) */
        if (!part_nil && (comp_ty_value_obj(c, t) || (!vexpr[0] && !iv_pre && expr_is_held_ref(c, expr)))) {
          const char *tsf = obj_str_fn(c, ty_object_class(t), 0);
          if (comp_ty_value_obj(c, t)) buf_printf(&conv, "%s(", tsf);
          else buf_printf(&conv, "%s((sp_%s *)", tsf, cn);
          EMIT_IV(); buf_printf(&conv, "%s)", obj_str_blockarg(c, ty_object_class(t), 0));
        }
        else {
          /* rooted: the part may be a fresh object (`"#{C.new}"`) held
             nowhere else while its #to_s allocates */
          int to = ++g_tmp;
          buf_printf(&conv, "({ sp_%s *_t%d = (sp_%s *)(", cn, to, cn);
          EMIT_IV();
          buf_printf(&conv, "); SP_GC_ROOT(_t%d); ", to);
          if (part_nil) buf_printf(&conv, "_t%d ? ", to);
          buf_printf(&conv, "%s(_t%d%s)%s; })", obj_str_fn(c, ty_object_class(t), 0), to,
                     obj_str_blockarg(c, ty_object_class(t), 0), part_nil ? " : sp_str_empty" : "");
        }
        if (ret_poly) buf_puts(&conv, ")");
      }
      else if (ty_is_ptr_array(t) || ty_is_obj_array(t)) {
        Buf ivb; memset(&ivb, 0, sizeof ivb);
        if (vexpr[0]) buf_puts(&ivb, vexpr);
        else if (iv_pre) buf_puts(&ivb, iv_pre);
        else emit_expr(c, expr, &ivb);
        buf_puts(&conv, "sp_poly_inspect(");
        emit_boxed_text(c, t, ivb.p ? ivb.p : "NULL", &conv);
        buf_puts(&conv, ")");
        free(ivb.p);
      }
      else if (t == TY_CLASS) {
        buf_printf(&conv, "%s(", comp_class_display_fn(c));
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_BIGINT) {
        /* NULL is this slot's nil, which interpolates as the empty string
           the way a nullable String's does (#4800). */
        int bgv = ++g_tmp;
        buf_printf(&conv, "({ sp_Bigint *_t%d = ", bgv);
        EMIT_IV();
        buf_printf(&conv, "; _t%d ? sp_bigint_to_s(_t%d) : sp_str_empty; })", bgv, bgv);
      }
      else if (t == TY_IO || t == TY_DIR) {
        /* the IO family has Object's to_s (the protocol arm's render) */
        buf_puts(&conv, t == TY_IO ? "sp_io_to_s(" : "sp_Dir_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_nullable_builtin_id(t)) {
        /* a reference-backed builtin handle (Proc, Fiber, Thread, ...):
           renders as its #inspect form rather than refusing the interpolation.
           CRuby's #to_s for these prints the object address, which the
           runtime does not reproduce for them (the IO family above has it);
           the #inspect form is the same shape and actually informative
           (#3539). */
        buf_puts(&conv, "sp_poly_inspect(sp_box_nullable_obj((void *)(");
        EMIT_IV(); buf_printf(&conv, "), %s))", ty_nullable_builtin_id(t));
      }
      else if (ty_is_object(t)) {
        /* user object without to_s: fall back to poly_to_s of boxed value,
           boxed by value for a value-type class (sp_box_obj takes a pointer,
           and the C did not compile) */
        int cid2 = ty_object_class(t);
        if (c->classes[cid2].is_value_type) {
          buf_printf(&conv, "sp_poly_to_s(sp_box_vobj_%s(", c->classes[cid2].c_name);
          EMIT_IV(); buf_puts(&conv, "))");
        }
        else {
          buf_printf(&conv, "sp_poly_to_s(sp_box_obj(");
          EMIT_IV(); buf_printf(&conv, ", %d))", cid2);
        }
      }
      else if (t == TY_UNKNOWN && nt_kind(nt, expr) == NK_CallNode && call_names_only_void_methods(c, expr)) {
        /* a call on a method that answers nothing, a C void function (one
           whose value is a call on a constant defined nowhere, which raises
           NameError when it runs): evaluated for its effect, and read as
           nil is, the empty string */
        buf_puts(&conv, "((void)(");
        EMIT_IV(); buf_puts(&conv, "), sp_str_empty)");
      }
      else if (t == TY_UNKNOWN) {
        /* An untyped value (e.g. a method resolved only through an included
           module) is already emitted as a boxed sp_RbVal, so stringify it the
           same way as a poly value rather than rejecting the interpolation. */
        buf_puts(&conv, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else {
        free(conv.p); free(lits.p); free(decls.p); free(late.p); free(wp); free(flat);
        unsupported(c, pid, "interpolation value");
      }
      iv_done:
      free(iv_pre);
      #undef EMIT_IV
      /* Pre-evaluate into an ordered temp. A dynamic part's string is rooted
         (its bytes are copied after later parts may allocate, and the final
         allocation itself can collect); bounded scalars need none.
         Collected into `decls` and emitted as a leading sequence of a
         statement-expression below -- NOT into g_pre, since emit_interp may
         run while g_pre holds a half-written enclosing statement.
         The length is the BYTE length. It used to be strlen, on the reason
         that a string value can be a foreign C literal with no sp_str_hdr
         and no marker byte (lib raise messages, class names, "" fallbacks)
         -- but strlen stops at an embedded NUL, so `"#{bytes}"` silently
         dropped everything past the first one, and interpolation is how a
         program assembles bytes (#4632). sp_str_byte_len answers strlen for
         exactly those foreign strings: it reads the marker byte first and
         only trusts a header behind one of the five it knows. That is the
         same test every other binary-safe path here makes (the write and
         puts operands, String#==, the formatter's %s). */
      int tv2 = ++g_tmp;
      if (wkind == WK_INT) {
        buf_printf(&decls, "sp_int _t%d = %s; ", tv2, conv.p ? conv.p : "0");
        fixed_cap += 21;  /* SP_W_INT_MAX: -9223372036854775808 */
      }
      else if (wkind == WK_BOOL) {
        buf_printf(&decls, "sp_bool _t%d = %s; ", tv2, conv.p ? conv.p : "0");
        fixed_cap += 5;
      }
      else if (wkind == WK_NIL) {
        buf_printf(&decls, "(void)(%s); ", conv.p ? conv.p : "0");
        tv2 = -1;
      }
      else if (t == TY_STRING && !vexpr[0] && repr_of(c, expr).strbuf_src == RS_HANDLE &&
               (nt_kind(nt, expr) == NK_LocalVariableReadNode || nt_kind(nt, expr) == NK_InstanceVariableReadNode) &&
               /* (not a define_method loop variable the literal stands in for) */
               !(g_dm_subst_name && nt_kind(nt, expr) == NK_LocalVariableReadNode && sp_streq(nt_str(nt, expr, "name"), g_dm_subst_name))) {
        /* one representation: the part is held as its handle where it is
           evaluated and read when the parts are joined, as CRuby collects
           the parts' objects first (a later part that rebinds the variable
           leaves this part's String; one that changes it in place shows) */
        int th = ++g_tmp;
        Buf hb; memset(&hb, 0, sizeof hb);
        /* (a local through emit_local_ref: a captured or renamed one is not
           its bare lv_ name) */
        if (nt_kind(nt, expr) == NK_LocalVariableReadNode) emit_local_ref(c, expr, nt_str(nt, expr, "name"), &hb);
        else emit_strbuf_handle_of(c, expr, &hb);
        buf_printf(&decls, "sp_String *_t%d = SP_STR_HDL(%s); SP_GC_ROOT(_t%d); ", th, hb.p ? hb.p : "NULL", th);
        buf_printf(&late, "const char *_t%d = sp_str_or_empty(SP_STR_B(_t%d)); SP_GC_ROOT(_t%d); size_t _l%d = sp_str_byte_len(_t%d); ",
                   tv2, th, tv2, tv2, tv2);
        free(hb.p);
      }
      else {
        int defer = 0;
        if (bn == 1 && t == TY_STRING && !vexpr[0] && strbuf_read_copies(c, expr))
          for (int k2 = k + 1; k2 < sn[k] && !defer; k2++)
            defer = nt_kind(nt, parts[k2]) == NK_EmbeddedStatementsNode && subtree_has_side_effect(c, parts[k2]);
        /* --share-strings: a variable that holds its bytes, changed in place by a later part */
        int late_base = -1;
        if (!defer && t == TY_STRING && !vexpr[0]) defer = interp_var_read_late(c, parts, sn[k], k, expr, &late_base);
        if (defer == 2) {
          /* the call runs here, its String is read after the last part
             (nil, a bang that changed nothing, reads as nothing) */
          int tr = ++g_tmp;
          Buf rd; memset(&rd, 0, sizeof rd);
          emit_expr(c, late_base, &rd);
          buf_printf(&decls, "const char *_t%d = %s; SP_GC_ROOT(_t%d); ", tr, conv.p ? conv.p : "NULL", tr);
          buf_printf(&late, "const char *_t%d = _t%d ? sp_str_or_empty(%s) : sp_str_empty; SP_GC_ROOT(_t%d); "
                     "size_t _l%d = sp_str_byte_len(_t%d); ", tv2, tr, rd.p ? rd.p : "sp_str_empty", tv2, tv2, tv2);
          free(rd.p);
        }
        else
        buf_printf(defer ? &late : &decls, "const char *_t%d = %s; SP_GC_ROOT(_t%d); size_t _l%d = sp_str_byte_len(_t%d); ",
                   tv2, conv.p ? conv.p : "sp_str_empty", tv2, tv2, tv2);
      }
      free(conv.p);
      wp[nwp].kind = wkind; wp[nwp].tmp = tv2;
      wp[nwp].plain = wkind == WK_INT && int_value_plain(c, expr);   /* never nil (#7612) */
      wp[nwp].neutral = neutral;
      wp[nwp].lit_off = 0; wp[nwp].lit_esc_len = 0; wp[nwp].lit_len = 0;
      nwp++;
      ndyn_or_scalar++;
    }
    else {
      free(lits.p); free(decls.p); free(late.p); free(wp); free(flat);
      unsupported(c, pid, "interpolation part");
    }
  }
  if (late.p) buf_puts(&decls, late.p);
  free(late.p);
  free(own); free(sn);
  pl->wp = wp; pl->nwp = nwp; pl->ndyn_or_scalar = ndyn_or_scalar;
  pl->lits = lits; pl->decls = decls; pl->fixed_cap = fixed_cap; pl->flat = flat;
}

/* Does the subtree at n call a String mutator? */
static int interp_part_mutates(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  if (nt_kind(nt, n) == NK_CallNode && nt_str(nt, n, "name") && is_string_rebind_mutator(nt_str(nt, n, "name"))) return 1;
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (interp_part_mutates(nt, nt_ref_at(nt, n, i))) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (interp_part_mutates(nt, ids[j])) return 1;
  }
  return 0;
}

/* --share-strings: an interpolation whose part changes an attribute
   assignment's String while another part changes a String too. CRuby reads
   every part's String after all of them ran; the parts here are read as each
   runs, a copy of the String at that moment (as for a variable). */
static void interp_refuse_assign_order(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int *flat = NULL, n = 0, cap = 0;
  interp_flatten(nt, id, &flat, &n, &cap);
  int hit = 0;
  for (int i = 0; i < n && !hit; i++) {
    int body = nt_ref(nt, flat[i], "statements"), bn = 0;
    const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn != 1 || !strbuf_assign_route(c, bb[0]) || !interp_part_mutates(nt, bb[0])) continue;
    for (int j = 0; j < n && !hit; j++)
      if (j != i && interp_part_mutates(nt, flat[j])) hit = 1;
  }
  free(flat);
  if (hit)
    unsupported_feature(c, id, "an interpolation that changes an attribute assignment's String "
                        "in one part and a String in another reads each part as it runs, where CRuby reads them all "
                        "after the last (a String is not yet shared by reference through an interpolation). Make the "
                        "changes statements of their own.");
}

void emit_interp(Compiler *c, int id, Buf *b) {
  interp_refuse_assign_order(c, id);
  InterpPlan pl; memset(&pl, 0, sizeof pl);
  interp_plan(c, id, &pl);
  WPart *wp = pl.wp; int nwp = pl.nwp, ndyn_or_scalar = pl.ndyn_or_scalar;
  Buf lits = pl.lits, decls = pl.decls; long fixed_cap = pl.fixed_cap; int *flat = pl.flat;

  if (ndyn_or_scalar == 0) {
    /* adjacent literals ("a" "b") fold to one literal: frozen per the
       InterpolatedStringNode's own file pragma flag. A frozen fold is the
       same object as the literal of its bytes, with the full static header
       -- the bare "\xf1" prefix promises an sp_str_hdr that would not exist
       (#1749 family). */
    if (nt_int(c->nt, id, "fzl", 0)) {
      size_t raw3 = 0;
      for (int k = 0; k < nwp; k++) raw3 += (size_t)wp[k].lit_len;
      Buf e; memset(&e, 0, sizeof e);
      for (int k = 0; k < nwp; k++)
        buf_printf(&e, "%.*s", wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off);
      emit_frozen_literal(b, e.p, e.len, raw3);
      free(e.p);
    }
    else {
      buf_puts(b, "(&(\"\\xff\" \"");
      for (int k = 0; k < nwp; k++)
        buf_printf(b, "%.*s", wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off);
      buf_puts(b, "\")[1])");
    }
    free(lits.p); free(decls.p); free(wp); free(flat);
    return;
  }

  int rid = ++g_tmp, wpid = ++g_tmp;
  buf_printf(b, "({ %s", decls.p ? decls.p : "");
  buf_printf(b, "size_t _cap%d = %ldUL", rid, fixed_cap);
  for (int k = 0; k < nwp; k++)
    if (wp[k].kind == WK_DYN) buf_printf(b, " + _l%d", wp[k].tmp);
  buf_puts(b, "; ");
  buf_printf(b, "char *_t%d = sp_str_alloc_raw(_cap%d + 1); ", rid, rid);
  buf_printf(b, "char *_t%d = _t%d; ", wpid, rid);
  /* The result's encoding, part by part as CRuby picks it: the first part's,
     taken over by a later part's high bytes (sp_str_enc_step). Only a String
     part can be binary; without one the result is UTF-8. */
  int eid = -1;
  for (int k = 0; k < nwp && eid < 0; k++) if (wp[k].kind == WK_DYN) eid = ++g_tmp;
  /* a leading literal makes it UTF-8 from the start */
  int lead = -1;
  for (int k = 0; k < nwp && lead < 0; k++)
    if (wp[k].kind == WK_DYN || wp[k].kind == WK_LIT) {
      if (wp[k].kind == WK_LIT && wp[k].lit_len == 0) continue;
      lead = k;
    }
  int lead_lit = lead >= 0 && wp[lead].kind == WK_LIT;
  if (eid >= 0) buf_printf(b, "int _e%d = %d; ", eid, lead_lit ? 0 : -1);
  for (int k = 0; k < nwp; k++) {
    if (eid >= 0 && wp[k].kind == WK_LIT && wp[k].lit_len > 0 && !(lead_lit && k == lead))
      buf_printf(b, "_e%d = sp_str_enc_step_i(_e%d, _t%d, (size_t)(_t%d - _t%d), \"%.*s\", %ld, 0); ",
                 eid, eid, rid, wpid, rid, wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off,
                 wp[k].lit_len);
    else if (eid >= 0 && wp[k].kind == WK_DYN) {
      if (wp[k].neutral >= 0) buf_printf(b, "if (!_t%d) ", wp[k].neutral);
      buf_printf(b, "_e%d = sp_str_enc_step_i(_e%d, _t%d, (size_t)(_t%d - _t%d), _t%d, _l%d, sp_str_is_binary(_t%d)); ",
                 eid, eid, rid, wpid, rid, wp[k].tmp, wp[k].tmp, wp[k].tmp);
    }
    switch (wp[k].kind) {
      case WK_LIT:
        if (wp[k].lit_len > 0)
          buf_printf(b, "memcpy(_t%d, \"%.*s\", %ld); _t%d += %ld; ",
                     wpid, wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off,
                     wp[k].lit_len, wpid, wp[k].lit_len);
        break;
      case WK_INT:
        buf_printf(b, "_t%d = %s(_t%d, _t%d); ", wpid, wp[k].plain ? "sp_w_int_plain" : "sp_w_int", wpid, wp[k].tmp);
        break;
      case WK_BOOL:
        buf_printf(b, "_t%d = sp_w_bool(_t%d, _t%d); ", wpid, wpid, wp[k].tmp);
        break;
      case WK_NIL:
        break;
      default:
        buf_printf(b, "memcpy(_t%d, _t%d, _l%d); _t%d += _l%d; ",
                   wpid, wp[k].tmp, wp[k].tmp, wpid, wp[k].tmp);
        break;
    }
  }
  buf_printf(b, "*_t%d = 0; sp_str_set_len(_t%d, (size_t)(_t%d - _t%d)); ", wpid, rid, wpid, rid);
  if (eid >= 0) buf_printf(b, "if (_e%d == 1) sp_str_mark_binary(_t%d); ", eid, rid);
  buf_printf(b, "(const char *)_t%d; })", rid);
  free(lits.p); free(decls.p); free(wp); free(flat);
}

/* `s << "lit #{x} lit"`: the interpolation's parts appended to `s` one by
   one, with no intermediate string. The value form above builds the whole
   string in a fresh buffer and the append then copies it again into `s`; a
   template that appends a page-sized interpolation copied the page once per
   nesting level (campfire's room page moved seven times its size through
   memmove per request). The parts are pre-evaluated into rooted temps first,
   in order, exactly as the value form does, so every side effect and every
   raise happens before the first byte lands in `s`; a part that aliases `s`
   itself (`s << "#{s}"`) is appended by the length taken at evaluation, so an
   earlier append growing `s` in place cannot change what it contributes.
   `open`/`close` bracket each part's append statement: open(receiver text)
   ... part ... close; a NULL length-taking variant is spelled by the caller
   through `open_n`/`close_n` when it has one. Answers 0 when the plan has no
   dynamic part (a literal fold, left to the plain path). */
int emit_interp_append(Compiler *c, int id, const char *open, const char *open_n, Buf *b, int indent) {
  InterpPlan pl; memset(&pl, 0, sizeof pl);
  interp_plan(c, id, &pl);
  if (pl.ndyn_or_scalar == 0) { interp_plan_free(&pl); return 0; }
  emit_indent(b, indent);
  buf_printf(b, "{ %s\n", pl.decls.p ? pl.decls.p : "");
  for (int k = 0; k < pl.nwp; k++) {
    WPart *w = &pl.wp[k];
    if (w->kind == WK_NIL) continue;
    if (w->kind == WK_LIT && w->lit_len == 0) continue;
    emit_indent(b, indent + 1);
    switch (w->kind) {
      case WK_LIT:
        buf_printf(b, "%s(&(\"\\xff\" \"%.*s\")[1]), %ldUL);\n", open_n,
                   w->lit_esc_len, (pl.lits.p ? pl.lits.p : "") + w->lit_off, w->lit_len);
        break;
      case WK_INT: {
        /* the digits go through a stack buffer into the length-taking
           append: sp_int_to_s allocated a String per part only for the
           append to copy it. The byte in front of the digits is a 0, the
           marker no heap String has, so a root the append puts on them
           (SP_GC_ROOT_STR) reads them as foreign memory. A WK_INT part is a
           plain Integer (a nullable one is converted by sp_int_opt_to_s). */
        int dg = ++g_tmp;
        buf_printf(b, "{ char _d%d[24]; _d%d[0] = 0; %s_d%d + 1, (size_t)(sp_w_int(_d%d + 1, _t%d) - (_d%d + 1))); }\n",
                   dg, dg, open_n, dg, dg, w->tmp, dg);
        break;
      }
      case WK_BOOL:
        buf_printf(b, "%s_t%d ? SPL(\"true\") : SPL(\"false\"));\n", open, w->tmp);
        break;
      default:
        buf_printf(b, "%s_t%d, _l%d);\n", open_n, w->tmp, w->tmp);
        break;
    }
  }
  emit_indent(b, indent); buf_puts(b, "}\n");
  interp_plan_free(&pl);
  return 1;
}

/* ---- expression ---- */

static int fold_int_const_name(Compiler *c, const char *name, long long *out, int depth);

/* Fold a compile-time integer-constant expression to its value: integer
   literals, references to other int-literal constants, and binary integer
   arithmetic over folded operands (no `/`/`%` -- Ruby floor semantics differ
   from C for negatives). Returns 1 + *out on success. */
static int fold_int_node(Compiler *c, int id, long long *out, int depth) {
  if (id < 0 || depth > 16) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  NodeKind k = nt_kind(nt, id);
  if (!ty) return 0;
  if (k == NK_IntegerNode) {
    if (nt_str(nt, id, "bigval")) return 0;  /* out-of-int64 literal: not a foldable int */
    *out = nt_int(nt, id, "value", 0); return 1;
  }
  if (k == NK_ConstantReadNode) return fold_int_const_name(c, nt_str(nt, id, "name"), out, depth + 1);
  if (k == NK_ParenthesesNode) {
    int bd = nt_ref(nt, id, "body"); int n = 0; const int *bb = bd >= 0 ? nt_arr(nt, bd, "body", &n) : NULL;
    return (n == 1) ? fold_int_node(c, bb[0], out, depth + 1) : 0;
  }
  if (k == NK_CallNode) {
    int recv = nt_ref(nt, id, "receiver");
    const char *nm = nt_str(nt, id, "name");
    int args = nt_ref(nt, id, "arguments"); int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || an != 1 || !nm) return 0;
    long long a, bb2;
    if (!fold_int_node(c, recv, &a, depth + 1) || !fold_int_node(c, av[0], &bb2, depth + 1)) return 0;
    if (sp_streq(nm, "+")) { *out = a + bb2; return 1; }
    if (sp_streq(nm, "-")) { *out = a - bb2; return 1; }
    if (sp_streq(nm, "*")) { *out = a * bb2; return 1; }
    /* Ruby shifts by a negative count shift the other way; a C shift by a
       negative (or >= width) count is UB. Fold the direction-flipped and
       saturated cases explicitly; bail on a large `<<` (Ruby promotes to
       Bignum -- not representable as a folded int). */
    if (sp_streq(nm, "<<")) {
      if (bb2 < 0) { long long s = -bb2; *out = s >= 64 ? (a < 0 ? -1 : 0) : (a >> s); return 1; }
      if (bb2 >= 64) return 0;
      *out = a << bb2; return 1;
    }
    if (sp_streq(nm, ">>")) {
      if (bb2 < 0) { long long s = -bb2; if (s >= 64) return 0; *out = a << s; return 1; }
      *out = bb2 >= 64 ? (a < 0 ? -1 : 0) : (a >> bb2); return 1;
    }
    if (sp_streq(nm, "&")) { *out = a & bb2; return 1; }
    if (sp_streq(nm, "|")) { *out = a | bb2; return 1; }
    if (sp_streq(nm, "^")) { *out = a ^ bb2; return 1; }
    return 0;
  }
  return 0;
}

static int fold_int_const_name(Compiler *c, const char *name, long long *out, int depth) {
  if (!name || depth > 16) return 0;
  int wnode = -1;
  for (int i = 0; i < c->nt->count; i++) {
    const char *t = nt_type(c->nt, i);
    NodeKind k = nt_kind(c->nt, i);
    if (!t) continue;
    /* a compound/or/and write mutates the constant slot at runtime -> not a
       stable compile-time value, don't fold. */
    if (k == NK_ConstantOperatorWriteNode || k == NK_ConstantOrWriteNode ||
        k == NK_ConstantAndWriteNode) {
      const char *n = nt_str(c->nt, i, "name");
      if (n && sp_streq(n, name)) return 0;
      continue;
    }
    if (k != NK_ConstantWriteNode) continue;
    const char *n = nt_str(c->nt, i, "name");
    if (!n || !sp_streq(n, name)) continue;
    if (wnode >= 0) return 0;   /* reassigned -> not a stable constant */
    wnode = i;
  }
  if (wnode < 0) return 0;
  return fold_int_node(c, nt_ref(c->nt, wnode, "value"), out, depth);
}

/* Emit an integer divisor operand, substituting a compile-time-constant
   value with its literal so the C compiler can strength-reduce `/`/`%` by
   a constant. Falls back to the normal expression emit otherwise. Folding
   is restricted to divisor positions: literalizing arbitrary constants
   (e.g. loop bounds in optcarrot) regresses layout-sensitive hot loops. */
void emit_int_divisor(Compiler *c, int node, Buf *b) {
  long long v;
  if (fold_int_node(c, node, &v, 0)) { emit_int_lit(b, v); return; }
  /* The callers hand this straight to sp_imod, whose divisor is an sp_int.
     A boxed operand is an sp_RbVal struct, so emitting it raw did not
     produce a wrong number -- it produced C that does not compile, and
     `17.modulo(x)` stopped building the moment x was boxed. The strict
     conversion is the one the sibling arms use for the same position
     (Integer#remainder reaches sp_iremainder through it), so a boxed value
     that is not an integer raises CRuby's TypeError rather than being read
     as one. */
  if (repr_of(c, node).kind == RK_BOXED) {
    buf_puts(b, "sp_poly_arg_int_chk("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  emit_expr(c, node, b);
}

/* True if `root` contains a `break <value>` that binds to the enclosing loop
   (not one inside a nested loop, block, lambda, or def/class/module scope, which
   capture their own break -- but a break in a block-bearing call's receiver or
   arguments does bind to the loop, so those are still traversed). A valued break
   makes the loop's value the break value rather than nil; detect it so a loop in
   value position rejects instead of silently yielding nil. */
int loop_has_valued_break(Compiler *c, int root) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  NodeKind k = nt_kind(nt, root);
  if (ty) {
    if (k == NK_BreakNode) {
      int a = nt_ref(nt, root, "arguments");
      int an = 0;
      if (a >= 0) nt_arr(nt, a, "arguments", &an);
      return an > 0;
    }
    if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode ||
        k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode ||
        k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode)
      return 0;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (loop_has_valued_break(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (loop_has_valued_break(c, el[j])) return 1;
  }
  return 0;
}

/* A receiverless `raise`/`fail` call: it diverges, so in a value position it
   yields no value (its C form is void). A branch whose value is such a call
   must be emitted as a statement, not assigned to the result temp. */
static int node_is_raise(Compiler *c, int nd) {
  const NodeTable *nt = c->nt;
  if (nd < 0 || nt_kind(nt, nd) != NK_CallNode) return 0;
  if (nt_ref(nt, nd, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, nd, "name");
  return nm && (is_raise_alias(nm));
}

/* One arm of a value-position if/unless: box a concrete arm into a poly
   result, and give empty []/{} literals the result's container type. */
/* An arm already rendered as TEXT, carried into a BIGINT result temp. The
   poly cases beside each call box; this one wraps, because the temp is an
   sp_Bigint * and a plain Integer arm is otherwise assigned into it as a
   pointer -- the same defect #4794 fixed for the arms that go through
   emit_ternary_arm, in the paths that do not. Answers 1 when it emitted. */
static int emit_arm_text_as_bigint(TyKind res, TyKind at, const char *txt, Buf *out) {
  if (res != TY_BIGINT || at == TY_BIGINT) return 0;
  buf_printf(out, "%s(%s)", at == TY_POLY ? "sp_poly_as_bigint" : "sp_bigint_new_int",
             txt && txt[0] ? txt : "0");
  return 1;
}
static void emit_ternary_arm_o(Compiler *c, int nd, TyKind res, int res_o, Buf *b);
static void emit_ternary_arm(Compiler *c, int nd, TyKind res, Buf *b) {
  emit_ternary_arm_o(c, nd, res, 0, b);
}
/* `res_o`: the result slot is an sp_oint / sp_ofloat (cond_res_oint): the
   arm is its oint form, a nil arm its nil */
static void emit_ternary_arm_o(Compiler *c, int nd, TyKind res, int res_o, Buf *b) {
  const NodeTable *nt = c->nt;
  if (node_is_empty_container(c, nd)) nd = nt_paren_single(c->nt, nd);   /* `([])` is `[]` */
  NodeKind bk = nt_kind(nt, nd);
  if (res_o && oint_kind(res)) { emit_oint_expr(c, nd, res, b); return; }
  if (res == TY_POLY && repr_of(c, nd).kind != RK_BOXED) { emit_boxed(c, nd, b); return; }
  /* A String result beside an arm that picks up a handle (`v.is_a?(String)
     ? v.dup : String.new`, whose narrowed dup is a String): the handle's
     bytes. Put in as it stands, an sp_String * shared one C conditional
     with a const char *, and the slot read the handle as bytes. */
  if (res == TY_STRING && repr_of(c, nd).as_ty == TY_STRBUF && nt_kind(nt, nt_paren_single(c->nt, nd)) == NK_CallNode &&
      strbuf_call_picks_up(c, nt_paren_single(c->nt, nd))) {
    str_read_handle_open(b, 0);
    emit_expr(c, nd, b);
    str_read_handle_close(b);
    return;
  }
  /* An arm with no C VALUE cannot sit in a C conditional beside a typed
     sibling: a method whose body is `nil` compiles to a void function, and
     `a && a.analyze` put that call straight into one arm of `? :` whose other
     arm is an sp_Att *. Sequence the call and produce the sibling type's nil
     -- NULL for a pointer, which IS nil there -- the same shape as the raise
     coercion below. The poly arm above already handles it by boxing, which is
     why only a TYPED sibling ever saw this (#4163). */
  { TyKind at = repr_of(c, nd).as_ty;
    if ((at == TY_VOID || at == TY_NIL) && res != TY_POLY && res != TY_UNKNOWN &&
        res != TY_VOID && res != TY_NIL) {
      Buf ab; memset(&ab, 0, sizeof ab);
      emit_expr(c, nd, &ab);
      const char *nv = nil_value(res);
      if (!nv) nv = default_value_from_compiler(c, res);
      if (!nv) nv = "0";
      /* (one representation: a String arm's nil is typed bytes; an untyped
         NULL made the conditional a void *, which the face macros read as
         the handle) */
      if (res == TY_STRING) nv = "((const char *)0)";
      /* a bare `nil` arm emits nothing at all; only a call needs sequencing */
      if (ab.p && ab.p[0]) buf_printf(b, "(%s, %s)", ab.p, nv);
      else buf_puts(b, nv);
      free(ab.p);
      return;
    }
  }
  /* A BIGINT result with an arm that is not one. The arm is a value, not a
     pointer, and C put it straight into the sp_Bigint * slot the sibling arm
     types: `return c ? 9223372036854775808 : 9223372036854775807` returned
     the second literal AS a pointer, and the first read of it segfaulted.
     The C compiler does say so -- `pointer/integer type mismatch in
     conditional expression` -- but the generated TU is built with -Wall off,
     so it is a warning and the build completes.

     The implicit-tail form never showed it: there each branch returns on its
     own and takes the return path's int->bigint wrap. Only an arm sitting in
     a C conditional beside a bigint sibling reaches here. */
  if (res == TY_BIGINT && !repr_of(c, nd).big) {
    emit_bigint_operand_ext(c, nd, b);
    return;
  }
  if ((ty_is_array(res) || ty_is_hash(res)) && bk == NK_CallNode &&
      repr_of(c, nd).untyped && emit_empty_container_for_slot(c, nd, res, b))
    return;
  if (ty_is_array(res) && bk == NK_ArrayNode) {
    int bn = 0; nt_arr(nt, nd, "elements", &bn);
    if (bn == 0) {
      const char *rk = (res == TY_POLY_ARRAY) ? "Poly" : array_kind(res);
      buf_printf(b, "sp_%sArray_new()", rk ? rk : "Int");
      return;
    }
    emit_expr(c, nd, b);
    return;
  }
  if (ty_is_hash(res) && (bk == NK_HashNode || bk == NK_KeywordHashNode)) {
    int bn = 0; nt_arr(nt, nd, "elements", &bn);
    const char *hc = ty_hash_cname(res);
    if (bn == 0 && hc) { buf_printf(b, "sp_%sHash_new()", hc); return; }
    emit_expr(c, nd, b);
    return;
  }
  {
    Buf ab; memset(&ab, 0, sizeof ab);
    emit_expr(c, nd, &ab);
    int un = nt_paren_single(c->nt, nd);
    /* An arm that compiles to a NoMethodError raise (e.g. `u.details` where u is
       unresolvable) evaluates to sp_RbVal but never returns; the sibling arm has
       the concrete result type, so coerce the raise to it -- `(raise, default)`
       -- to keep the C ternary's two arms the same type (#2949). The raise may
       come parenthesized (`(u.details)`) or as a call on a raising receiver,
       `((void)(<raise>), nil)` (`u.a && u.a.b`): both diverge the same way. */
    if (ab.p && text_diverges(ab.p) &&
        res != TY_POLY && res != TY_UNKNOWN && res != TY_VOID) {
      buf_printf(b, "(%s, %s)", ab.p, default_value_from_compiler(c, res));
    }
    else if (ab.p && repr_of(c, un).as_ty == TY_UNKNOWN && res != TY_POLY && res != TY_UNKNOWN &&
             res != TY_VOID && (nt_kind(nt, un) == NK_IfNode || nt_kind(nt, un) == NK_UnlessNode))
      emit_unbox_text(c, res, ab.p, b);
    else buf_puts(b, ab.p ? ab.p : "");
    free(ab.p);
  }
}

/* Effect-free simple read whose re-evaluation is safe (and cheap): gates the
   IndexOperatorWriteNode expression form below, which re-reads receiver and
   key after the mutation. */
static int idx_opw_node_is_cheap(const NodeTable *nt, int node) {
  NodeKind k = nt_kind(nt, node);
  return (k == NK_LocalVariableReadNode ||
               k == NK_InstanceVariableReadNode ||
               k == NK_IntegerNode || k == NK_FloatNode ||
               k == NK_StringNode || k == NK_SymbolNode);
}

/* Emit a read of recv[key] for the receiver shapes emit_index_op_write
   handles, yielding the slot's static type (which the new infer rule reports
   as the op-write expression's type, so the two always agree). */
static void emit_index_get(Compiler *c, int recv, int key, Buf *b) {
  Repr rr = repr_of(c, recv);
  TyKind rt = rr.as_ty;
  if (ty_is_hash(rt) && ty_hash_cname(rt)) {
    buf_printf(b, "sp_%sHash_get(", ty_hash_cname(rt));
    if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    /* a key held boxed (hash_opw_okey) was an Integer for the store to take it */
    if (g_iow_key_ref && hash_opw_okey(c, key, rt)) buf_printf(b, "sp_poly_hkey_i(%s)", g_iow_key_ref);
    else if (g_iow_key_ref) buf_puts(b, g_iow_key_ref); else emit_hash_key(c, key, rr.key, b);
    buf_puts(b, ")");
    return;
  }
  if (ty_is_array(rt)) {
    const char *k = rr.elem == TY_POLY ? "Poly" : array_kind(rt);
    buf_printf(b, "sp_%sArray_get(", k ? k : "Poly");
    if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    if (g_iow_key_ref) buf_puts(b, g_iow_key_ref); else emit_int_expr(c, key, b);
    buf_puts(b, ")");
    return;
  }
  /* TY_POLY receiver: dispatch by key type, mirroring emit_index_op_write */
  TyKind kt = comp_ntype(c, key);
  if (kt == TY_INT && poly_int_key_boxed(c, key)) kt = TY_POLY;   /* its nil rides boxed */
  /* A receiver proved to hold only a poly array or nil needs none of the
     dispatch: analyze established that for the root elision, and the read is
     hot enough in optcarrot's per-pixel path to be worth spending it on. */
  const char *fn = kt == TY_SYMBOL ? "sp_poly_get_sym" :
                   kt == TY_STRING ? "sp_poly_get_str" :
                   kt == TY_INT    ? (expr_is_arr_or_nil(c, recv) && decide_node(c->nt, recv, "aon-get", NULL)
                                                                  ? "sp_poly_arr_get_aon"
                                                                  : "sp_poly_arr_get_hash")
                                   : "sp_poly_index_poly";
  buf_printf(b, "%s(", fn);
  if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
  buf_puts(b, ", ");
  if (g_iow_key_ref) buf_puts(b, g_iow_key_ref);
  else if (kt == TY_SYMBOL || kt == TY_STRING) emit_expr(c, key, b);
  else if (kt == TY_INT) emit_int_expr(c, key, b);
  else emit_boxed(c, key, b);
  buf_puts(b, ")");
}

/* `h[k] ||= v` stores when the slot is nil or false, `&&=` when it is not, and
   that test has to be spelled per slot type. An Integer slot's nil rides beside
   the value (sp_oint), so the test reads its nil flag. A plain `!x` both
   skipped the store on an ABSENT key (whose nil was then a nonzero value) and
   overwrote a legitimately stored 0 (which is truthy in Ruby). The
   typed-array branches spell it correctly; the hash branch did not, and it
   is the expression form that is reached when the result is used (#3421). */
void emit_slot_nil_test(Compiler *c, TyKind t, int tmp, int want_nil, Buf *b) {
  const char *n = want_nil ? "" : "!";
  switch (t) {
    /* an Integer or Float temp is its sp_oint */
    case TY_INT: case TY_FLOAT: buf_printf(b, "%s_t%d.nil", n, tmp); return;
    case TY_SYMBOL: buf_printf(b, "%s(_t%d == (sp_sym)-1)", n, tmp); return;
    case TY_POLY:   buf_printf(b, "%ssp_poly_truthy(_t%d)", want_nil ? "!" : "", tmp); return;
    default: break;
  }
  /* bool, string, and every pointer-backed slot: nil and false are both the
     zero value, which is what Ruby treats as falsy here. */
  buf_printf(b, "%s_t%d", want_nil ? "!" : "", tmp);
  (void)c;
}

/* The zero for a result slot of this type. default_value answers NULL for an
   object, which suits a pointer-backed class and not a by-value one, whose C
   representation is a bare struct -- `sp_Frame _t9 = NULL` is not valid C. */
static const char *slot_zero(Compiler *c, TyKind t) {
  if (comp_ty_value_obj(c, t)) return raise_tail_value_c(c, t);
  return default_value_from_compiler(c, t);
}

/* The fallback of `x = expr rescue fallback` ends in a jump (`next`, or
   `(n += 1; next)`): it leaves before x is written. A `next` that is the
   block's value (`map { |s| Integer(s) rescue next 0 }`, next_is_block_value)
   is not a jump here: the expression emitter answers it as its value, which
   is the modifier's value and so the block's. */
static int rescue_fallback_jumps(Compiler *c, int r) {
  const NodeTable *nt = c->nt;
  while (r >= 0 && (nt_kind(nt, r) == NK_ParenthesesNode || nt_kind(nt, r) == NK_StatementsNode)) {
    if (nt_kind(nt, r) == NK_ParenthesesNode) { r = nt_ref(nt, r, "body"); continue; }
    int n = 0; const int *b = nt_arr(nt, r, "body", &n);
    r = n > 0 ? b[n - 1] : -1;
  }
  if (r < 0) return 0;
  NodeKind k = nt_kind(nt, r);
  if (k == NK_NextNode) return !next_is_block_value(c, r);
  return k == NK_BreakNode || k == NK_RedoNode ||
         k == NK_RetryNode || k == NK_ReturnNode;
}

/* Is `id` somewhere inside the subtree at `root`? */
static int node_in_subtree(const NodeTable *nt, int root, int id) {
  if (root < 0) return 0;
  if (root == id) return 1;
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (node_in_subtree(nt, nt_ref_at(nt, root, i), id)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (node_in_subtree(nt, ids[j], id)) return 1;
  }
  return 0;
}

/* A NameError's class and those above it (rescue_chain_catches) */
static const char *const nameerror_above[] = { "NameError", "StandardError", "Exception", "Object", "BasicObject",
                                               "Kernel", NULL };

/* Is this constant reference written inside something that catches the
   NameError it raises? Referencing a missing constant on purpose, behind a
   `rescue NameError`, is how a program probes for one -- ruby/spec does exactly
   that -- and CRuby says nothing about it at any stage. Warning there reports a
   program whose behaviour is already right (#4062). */
static int const_ref_is_rescued(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  for (int b = 0; b < nt->count; b++) {
    NodeKind k = nt_kind(nt, b);
    if (k == NK_BeginNode) {
      int rc = nt_ref(nt, b, "rescue_clause");
      if (rc < 0 || !rescue_chain_catches(c, rc, nameerror_above, 1)) continue;
      if (node_in_subtree(nt, nt_ref(nt, b, "statements"), id)) return 1;
    }
    /* `X rescue fallback` catches StandardError, so it catches this too */
    else if (k == NK_RescueModifierNode &&
             node_in_subtree(nt, nt_ref(nt, b, "expression"), id)) return 1;
  }
  return 0;
}

/* Is this constant reference on the right of a `&&` whose left arm is a
   statically false defined? guard (comp_defined_guard_false)? That arm never
   runs, so the reference never raises: `defined?(::X) && ::X` is how a
   program reads a constant it may not have, as the `? :` form does, which
   folds its dead arm away before it is emitted. */
static int const_ref_is_guarded(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  for (int b = 0; b < nt->count; b++) {
    if (nt_kind(nt, b) != NK_AndNode) continue;
    if (!comp_defined_guard_false(c, nt_ref(nt, b, "left"))) continue;
    if (node_in_subtree(nt, nt_ref(nt, b, "right"), id)) return 1;
  }
  return 0;
}

/* The constant path AS WRITTEN, for a message someone has to find in their
   source. The `par_nmc` the arms below key on is deliberately the parent's
   LEAF name (the ffi and platform-constant tables are keyed that way), and
   reusing it for the message named `SSL::VERIFY_NONE` where the source said
   `OpenSSL::SSL::VERIFY_NONE`, and `C::D` for `A::B::C::D`. Neither string
   occurs in the program, which is exactly what a reader greps for. Walk the
   parent chain instead.

   A prefix that is not a written constant (`BLOCK::CODE`, where BLOCK holds a
   class) stops the walk: what is left is the written tail, which is still more
   of the path than the leaf pair was. `Object::X` keeps dropping the Object,
   as CRuby does -- Object's constants are the top-level ones (#3976). */
void const_path_written(const NodeTable *nt, int id, char *out, size_t cap) {
  const char *parts[16];
  int n = 0, cur = id;
  while (cur >= 0 && n < 16) {
    const char *t = nt_type(nt, cur);
    NodeKind k = nt_kind(nt, cur);
    const char *nm = t ? nt_str(nt, cur, "name") : NULL;
    if (!nm || !*nm) break;
    if (k == NK_ConstantPathNode) { parts[n++] = nm; cur = nt_ref(nt, cur, "parent"); }
    else if (k == NK_ConstantReadNode) { parts[n++] = nm; break; }
    else break;
  }
  if (n > 1 && sp_streq(parts[n - 1], "Object")) n--;
  size_t off = 0;
  out[0] = 0;
  for (int i = n - 1; i >= 0; i--) {
    int w = snprintf(out + off, off < cap ? cap - off : 0, "%s%s",
                     i == n - 1 ? "" : "::", parts[i]);
    if (w < 0) break;
    off += (size_t)w;
    if (off >= cap) { out[cap - 1] = 0; break; }
  }
}

/* One build-time warning per constant that the whole program never defines.
   The reference still emits its runtime NameError (CRuby's behaviour, which
   ruby/spec asserts), but the build no longer says nothing at all about a name
   that can never resolve -- a dropped require reads as a runtime engine bug
   otherwise (#3976). Deduplicated by name so a constant read in a loop body
   warns once. */
static void warn_undefined_constant(Compiler *c, int id, const char *nm) {
  static char seen[64][128];
  static int nseen = 0;
  if (!nm || !*nm) return;
  for (int i = 0; i < nseen; i++) if (sp_streq(seen[i], nm)) return;
  if (nseen < 64) { snprintf(seen[nseen], sizeof seen[0], "%s", nm); nseen++; }
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  int fid = (int)nt_int(c->nt, id, "node_file", 0);
  const char *file = nt_file_path(c->nt, fid);
  if (!file || !*file) file = c->nt->source_file;
  if (!file || !*file) file = "source.rb";
  /* `Point::LIMIT` with LIMIT written at the top level only: defined, but
     not where the path looks (const_path_misses_toplevel) */
  const char *why = const_path_misses_toplevel(c, id)
    ? "only the top level defines it, which a path through a class or module does not reach"
    : "defined nowhere in the program";
  if (ln > 0)
    fprintf(stderr, "spinel: %s:%d: warning: uninitialized constant %s: %s (raises NameError when reached)\n",
            file, ln, nm, why);
  else
    fprintf(stderr, "spinel: warning: uninitialized constant %s: %s (raises NameError when reached)\n", nm, why);
}

/* `recv.attr ||= v` / `&&=` where the reader or the writer is a real `def`.
   The direct-ivar shapes below are a fast path for a generated accessor pair,
   where the ivar IS the attribute; a hand-written reader or writer has to be
   called, and skipping it wrote the ivar and never ran the writer -- silently
   when the ivar happened to share the method's name, and as a C build error
   naming a struct member that does not exist when it did not (#4148).

   CRuby's shape, measured: the receiver is evaluated once, the reader is
   called once, and on the assigning branch the value is the ASSIGNED value --
   not the writer's return and not a re-read (`def v=(x); @v = x * 10; end`
   leaves 70 in the ivar and answers 7). The right-hand side is not evaluated
   at all when the reader's answer decides it.

   Answers 1 when it emitted; 0 leaves the caller's existing shapes alone. */
/* The guarded assignment of an or/and-write, `if (COND) LHS = RHS`, with the
   RHS's setup captured: the statements a composite RHS spills to g_pre (a
   hash literal's fills, a block-taking call's loop) would otherwise run
   ahead of the guard on every evaluation, so `@map ||= { 0 => T.new }` built
   the table each call and threw it away, and a RHS that succeeds once and
   raises afterwards raised where CRuby answered the memo (#4513). The setup
   is spliced inside the conditional. Value form yields the LHS after; the
   statement form ends its line. */
void emit_orw_guard(Compiler *c, int v, TyKind slot, const char *cond, const char *lhs,
                    int value_form, int indent, Buf *b) {
  int boxed = slot == TY_POLY;
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  Buf *saved_pre = g_pre; g_pre = &vpre;
  if (boxed) emit_boxed(c, v, &vval);
  else emit_coerce(c, v, slot, CO_HOLD, "a local variable's `||=` or `&&=`", &vval);
  g_pre = saved_pre;
  if (!value_form) emit_indent(b, indent);
  if (value_form) buf_puts(b, "({ ");
  if (cond) buf_printf(b, "if (%s) { ", cond);
  if (vpre.p) buf_puts(b, vpre.p);
  buf_printf(b, "%s = %s;", lhs, vval.p ? vval.p : (boxed ? "sp_box_nil()" : "0"));
  if (cond) buf_puts(b, " }");
  if (value_form) buf_printf(b, " %s; })", lhs);
  else buf_puts(b, "\n");
  free(vpre.p); free(vval.p);
}

/* The guarded store of `REF ||= v` / `REF &&= v` on a shared-handle String
   slot: the RHS goes in as a handle (an alias by handle, anything else
   freshly wrapped), its setup spliced inside the guard. */
void emit_strbuf_orw_guard(Compiler *c, const char *ref, int v, int is_or, Buf *b) {
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  Buf *saved_pre = g_pre; g_pre = &vpre;
  if (strbuf_slot_text(c, v, &vval)) { }
  else if (emit_strbuf_write_handle(c, v, &vval)) { }
  /* (--share-strings: in the String plan's form, a box's handle) */
  else if (c->share_plan) emit_str_form_handle(c, v, &vval);
  else { str_new_handle_open(&vval); emit_str_expr(c, v, &vval); str_new_handle_close(&vval); }
  g_pre = saved_pre;
  buf_printf(b, "if (%s%s) { ", is_or ? "!" : "", ref);
  if (vpre.p) buf_puts(b, vpre.p);
  buf_printf(b, "%s = %s; }", ref, vval.p ? vval.p : "");
  free(vpre.p); free(vval.p);
}

/* A local bound to an ivar write on a shared-handle String slot
   (`b = (@buf ||= +"")`, `b = @buf = +""`): the write, then the slot's
   HANDLE, so the local and the ivar are one object and an append through
   the local shows in the ivar. Answers 0 when `v` is no such write. */
int emit_strbuf_ivar_write_handle(Compiler *c, int v, Buf *b) {
  const NodeTable *nt = c->nt;
  int w = strbuf_ivar_alias_value(nt, v);
  if (w < 0 || nt_kind(nt, w) == NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(nt, w, "name");
  int cid = strbuf_ivar_owner(c, w);
  if (!nm || cid < 0) return 0;
  int iv = comp_ivar_index(&c->classes[cid], nm);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_STRBUF) return 0;
  char ref[300];
  Scope *cs = comp_scope_of(c, w);
  if (cs && cs->class_id < 0) snprintf(ref, sizeof ref, "civ_Toplevel_%s", iv_c(nm + 1));
  /* a class method's ivar is its class's C global, as strbuf_slot_text
     names it; self there is the class value */
  else if (cs && cs->is_cmethod) snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cid].name, iv_c(nm + 1));
  else snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
  buf_puts(b, "({ ");
  if (nt_kind(nt, w) == NK_InstanceVariableWriteNode) emit_stmt_inner(c, w, b, 0);
  else {
    int is_or = nt_kind(nt, w) == NK_InstanceVariableOrWriteNode;
    emit_strbuf_orw_guard(c, ref, nt_ref(nt, w, "value"), is_or, b);
    /* `||=` leaves the ivar assigned (ivar_write_mark), `&&=` as it was */
    char mk[300];
    const char *m = is_or ? ivar_write_mark(c, w, mk, sizeof mk) : NULL;
    if (m) buf_printf(b, " %s", m);
  }
  buf_printf(b, " %s; })", ref);
  return 1;
}

/* A write (through single-statement parentheses) whose slot holds the
   handle the --share-strings rule assigned (repr_write_share), where a
   handle is taken -- a box, a slot that holds a handle: the write under
   the handle mark, whose value is then its slot's handle (each write's
   value form reads the mark), so both hold one String. The value read out
   of the slot, wrapped, forked a copy a later change through the variable
   missed. Answers 0 when `v` is no such write. */
int emit_strbuf_write_handle(Compiler *c, int v, Buf *b) {
  int w = nt_paren_single(c->nt, v);
  if (!repr_write_share(c, w)) return 0;
  /* one a call ran first is the handle emit_arg_temp took then */
  int th = ran_first_handle(v);
  if (th < 0 && w != v) th = ran_first_handle(w);
  if (th >= 0) { buf_printf(b, "_t%d", th); return 1; }
  int sv = view_push_repr(c, w, VR_STRBUF_BOX, 1);
  emit_expr(c, w, b);
  view_pop(c, sv);
  return 1;
}

static void emit_strbuf_slot_read(Compiler *c, int id, Repr rp, const char *sref, Buf *b);

/* `REF ||= v` / `REF &&= v` in value position (write node id) on a slot
   that holds the rule's handle: the guarded store takes the RHS as a
   handle, then, for `||=`, a class variable's set flag (fcid/fnm, or
   -1/NULL), and the value is the slot's read: the handle itself under the
   handle mark, else its read face. An `&&=` stores only into a slot that
   holds a String, whose flag is already set; flagging it after the guard
   called an unset class variable defined. An ivar's assigned mark
   (ivar_write_mark, or NULL) is set the same way. */
static void emit_strbuf_orw_share_value(Compiler *c, int id, const char *ref, int v, int is_or,
                                        int fcid, const char *fnm, const char *mark, Buf *b) {
  buf_puts(b, "({ ");
  emit_strbuf_orw_guard(c, ref, v, is_or, b);
  buf_puts(b, " ");
  if (is_or) emit_cvar_set_flag(c, fcid, fnm, 0, b);
  if (is_or && mark) buf_printf(b, "%s ", mark);
  emit_strbuf_slot_read(c, id, repr_of(c, id), ref, b);
  buf_puts(b, "; })");
}

/* `REF ||= v` / `REF &&= v` on a typed slot of kind T, as a value: the
   guard tests the slot kind's own nil (NULL for a pointer-backed slot, the
   nil flag or nil byte for an Integer or Float, a sentinel for Symbol) and
   the RHS converts to the slot's kind.
   Shared by an ivar and by a generated attribute read through a typed
   receiver, which had its own partial copy and lost the write (#5428).
   `mark` (or NULL) is the ivar's assigned mark (ivar_set_mark), set after
   an `||=`'s guard: the ivar is assigned whichever way it went. */
void emit_slot_orw_value(Compiler *c, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                         const char *mark, Buf *b) {
  emit_slot_orw_value_o(c, t, elems_handle, ref, v, is_or, NULL, -1, -1, NULL, mark, b);
}
/* The same for an Integer or Float slot that holds its nil: `niltest` is
   the C test that it is nil (`ref.nil` for an oint slot, the nil bit's test
   for an object field, NULL for a plain slot that is never nil); a field
   with a bit also names its class, ivar and receiver prefix, so the store
   keeps the bit in step (emit_ivar_value_nilbit). */
void emit_slot_orw_value_o(Compiler *c, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                           const char *niltest, int cid, int iv, const char *pfx, const char *mark, Buf *b) {
  if (!is_or) mark = NULL;
  /* The RHS is rendered with its setup captured: the statements a composite
     RHS spills to g_pre (a hash literal's fills, a block-taking call's loop)
     would otherwise run unconditionally, ahead of the guard, so
     `@map ||= { 0 => T.new }` built the table on every call and discarded
     it, and a RHS that succeeds once and raises afterwards raised on the
     second call where CRuby answered the memo (#4513). The setup is spliced
     inside the conditional, where it runs only when the assignment is
     taken. The poly arm did this already; every slot kind does now. */
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  const char *cond = NULL;
  char condb[400];
  int unconditional = 0;
  /* A shared-handle string slot takes the RHS as a handle (an alias by
     handle, anything else freshly wrapped), and the expression's value is
     the slot's read face with the handle published, as a plain write's is. */
  if (t == TY_STRBUF) {
    buf_puts(b, "({ ");
    emit_strbuf_orw_guard(c, ref, v, is_or, b);
    buf_printf(b, "%s%s ", mark ? " " : "", mark ? mark : "");
    str_read_handle(b, ref, 1);
    buf_puts(b, "; })");
    return;
  }
  if (t == TY_POLY) {
    Buf *saved_pre = g_pre; g_pre = &vpre;
    emit_boxed(c, v, &vval);
    g_pre = saved_pre;
    snprintf(condb, sizeof condb, "%ssp_poly_truthy(%s)", is_or ? "!" : "", ref);
    cond = condb;
    if (!vval.p) buf_puts(&vval, "sp_box_nil()");
  }
  else {
    Buf *saved_pre = g_pre; g_pre = &vpre;
    /* an empty `[]`/`{}` takes the slot's variant; its own inferred one
       comes from reads elsewhere, not from this slot (#4111) */
    if (t == TY_BIGINT && !repr_of(c, v).big && ty_is_numeric(comp_ntype(c, v))) {
      /* an Integer into a Bignum slot is promoted, as a plain ivar write does */
      buf_puts(&vval, "sp_bigint_new_int("); emit_int_expr(c, v, &vval); buf_puts(&vval, ")");
    }
    else if (emit_empty_literal_as(c, v, t, &vval)) { }
    /* a typed array into a general Array slot is rebuilt as one, as a plain
       write does */
    /* --share-strings: a slot whose elements the rule shares, each String
       as a handle, as its plain write */
    else if (elems_handle && comp_ntype(c, v) == TY_STR_ARRAY) emit_str_array_handles(c, v, &vval);
    else if (emit_array_into_poly_slot(c, t, v, &vval)) { }
    else if (seeded_array_kind_mismatch(t, comp_ntype(c, v)))
      emit_array_store_value(c, t, v, &vval);   /* a seed-pinned kind converts */
    /* an Integer or Float slot holding its nil: a field with a bit keeps
       the bit in step, an oint slot takes the oint */
    else if (oint_kind(t) && cid >= 0) emit_ivar_value_nilbit(c, cid, iv, pfx, v, &vval);
    else if (oint_kind(t) && niltest) emit_oint_expr(c, v, t, &vval);
    else emit_coerce(c, v, t, CO_HOLD, "a variable's `||=` or `&&=`", &vval);
    g_pre = saved_pre;
    if (t == TY_BOOL || t == TY_STRING)
      snprintf(condb, sizeof condb, "%s%s", is_or ? "!" : "", ref);
    /* a plain Integer or Float is never nil: `||=` never stores, `&&=` always */
    else if (oint_kind(t) && !niltest)
      snprintf(condb, sizeof condb, "%s", is_or ? "0" : "1");
    else if (oint_kind(t))
      snprintf(condb, sizeof condb, "%s%s", is_or ? "" : "!", niltest);
    else if (t == TY_SYMBOL)   /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      snprintf(condb, sizeof condb, "%s %s= (sp_sym)-1", ref, is_or ? "=" : "!");
    else if (t == TY_CLASS)   /* a Class slot's nil is SP_CLASS_NIL (#5357) */
      snprintf(condb, sizeof condb, "%ssp_class_nil_p(%s)", is_or ? "" : "!", ref);
    /* a pointer-backed slot (object/array/hash/Bignum/fiber/proc/...) reads falsy
       when NULL, so `@x ||= v` is `if (!@x) @x = v` and `@x &&= v` is
       `if (@x) @x = v`. Falling through to a bare read dropped the init when
       this or-write was the RHS of a poly-receiver setter switch (#1447). */
    else if (ty_is_object(t) || ty_is_array(t) || ty_is_hash(t) || t == TY_BIGINT ||
             t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_PROC || t == TY_IO ||
             t == TY_MATCHDATA || t == TY_EXCEPTION || t == TY_REGEX) {
      snprintf(condb, sizeof condb, "%s%s", is_or ? "!" : "", ref);
      /* an unresolved-call RHS (`@x ||= recv.map{...}` where recv typed
         poly/unknown) is a raise-all returning sp_RbVal; into a pointer-backed
         slot, keep the raise for effect but yield the slot's typed NULL so the
         assignment compiles -- the raise aborts before the NULL is reached
         (#2457). */
      const char *ivtxt = vval.p ? vval.p : "";
      if (strncmp(ivtxt, "sp_raise_nomethod(", 18) == 0 ||
          strncmp(ivtxt, "(sp_raise_cls(", 14) == 0) {
        Buf w; memset(&w, 0, sizeof w);
        buf_printf(&w, "(%s, %s)", ivtxt, default_value_from_compiler(c, t));
        free(vval.p); vval = w;
      }
    }
    else if (!is_or) unconditional = 1;
    else { free(vpre.p); free(vval.p); buf_puts(b, ref); return; }
    if (!unconditional) cond = condb;
  }
  if (unconditional) {
    buf_puts(b, "({ ");
    if (vpre.p) buf_puts(b, vpre.p);
    buf_printf(b, "%s = %s; %s%s%s; })", ref, vval.p ? vval.p : "", mark ? mark : "", mark ? " " : "", ref);
  }
  else {
    buf_printf(b, "({ if (%s) { ", cond);
    if (vpre.p) buf_puts(b, vpre.p);
    buf_printf(b, "%s = %s; } %s%s%s; })", ref, vval.p ? vval.p : "", mark ? mark : "", mark ? " " : "", ref);
  }
  free(vpre.p); free(vval.p);
}

/* NoMethodError for a half of an `||=` / `&&=` pair, as an ordinary call
   raises it -- head is the message up to the class name: the receiver
   (recv, of type rt) rides it as NoMethodError#receiver. */
static void emit_orw_raise(Compiler *c, const char *head, int cid, TyKind rt,
                           const char *recv, Buf *b) {
  buf_puts(b, "(sp_exc_stage_recv("); emit_boxed_text(c, rt, recv, b);
  buf_puts(b, "), sp_raise_cls(\"NoMethodError\", ");
  if (anon_class_text(c, cid, head, b)) buf_puts(b, "))");
  else {
    const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
    buf_printf(b, "(&(\"\\xff\" \"%s%s\")[1])))", head, rn);
  }
}

/* ...for a non-public half */
static void emit_orw_vis_raise(Compiler *c, int vis, const char *nm, int cid, TyKind rt,
                               const char *recv, Buf *b) {
  char head[400];
  snprintf(head, sizeof head, "%s method '%s' called for an instance of ",
           vis == SP_VIS_PRIVATE ? "private" : "protected", nm);
  emit_orw_raise(c, head, cid, rt, recv, b);
}

int emit_call_or_write_via_methods(Compiler *c, int id, int is_or, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *attr = nt_str(nt, id, "name");
  int v = nt_ref(nt, id, "value");
  if (recv < 0 || !attr || v < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_object(rt)) return 0;
  int cid = ty_object_class(rt);
  int rmi = -1, wmi = -1, rdc = -1, wdc = -1;
  int rk = comp_resolve_member(c, cid, attr, 0, &rdc, &rmi);
  int wk = comp_resolve_member(c, cid, attr, 1, &wdc, &wmi);
  /* The pair calls each half directly, past the visibility check an
     ordinary call makes, so on an explicit receiver a half the caller may
     not call raises NoMethodError in its place, as CRuby does: the reader
     before anything else, the writer only when the assignment runs. Two
     attr halves go the field-store way unless one of them must raise. */
  char wn[300]; snprintf(wn, sizeof wn, "%s=", attr);
  int may_private = nt_ignore_visibility(nt, id);
  int rvis = may_private ? SP_VIS_PUBLIC : method_vis_for_caller(c, id, cid, attr);
  int wvis = may_private ? SP_VIS_PUBLIC : method_vis_for_caller(c, id, cid, wn);
  /* No reader at all -- no method, no attribute, no method_missing to
     answer it -- is the reader call's NoMethodError, raised before the
     value runs. The field-store way below has no ivar to store into. */
  if (rk == SP_MEMBER_NONE && comp_method_in_chain(c, cid, "method_missing", NULL) < 0) {
    TyKind want = repr_of(c, id).as_ty;
    if (want == TY_UNKNOWN || want == TY_VOID) want = TY_POLY;
    int trv = ++g_tmp; char rv[32]; snprintf(rv, sizeof rv, "_t%d", trv);
    char head[400]; snprintf(head, sizeof head, "undefined method '%s' for an instance of ", attr);
    buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " %s = ", rv); emit_expr(c, recv, b); buf_puts(b, "; ");
    emit_orw_raise(c, head, cid, rt, rv, b);
    buf_printf(b, "; %s; })", oint_kind(want) && node_is_oint(c, id) ? oint_nil(want)
                                                                    : default_value_from_compiler(c, want));
    return 1;
  }
  if (rk != SP_MEMBER_METHOD && wk != SP_MEMBER_METHOD &&
      rvis == SP_VIS_PUBLIC && wvis == SP_VIS_PUBLIC) return 0;
  /* a method half takes the class that defines it, an ancestor of the
     receiver's (MethodAttr#block_params= on an AnyMethod) */
  const char *rcn = c->classes[rk == SP_MEMBER_METHOD && rdc >= 0 ? rdc : cid].c_name;
  const char *wcn = c->classes[wk == SP_MEMBER_METHOD && wdc >= 0 ? wdc : cid].c_name;
  /* Both halves have to exist for the pair to run; a missing one is
     NoMethodError in CRuby, which the ordinary call path reports. */
  if (rk == SP_MEMBER_NONE || wk == SP_MEMBER_NONE) return 0;
  if ((rk == SP_MEMBER_METHOD && rmi < 0) || (wk == SP_MEMBER_METHOD && wmi < 0)) return 0;

  TyKind want = repr_of(c, id).as_ty;
  if (want == TY_UNKNOWN || want == TY_VOID) want = TY_POLY;
  if (rvis != SP_VIS_PUBLIC) {
    int trv = ++g_tmp; char rv[32]; snprintf(rv, sizeof rv, "_t%d", trv);
    buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " %s = ", rv); emit_expr(c, recv, b); buf_puts(b, "; ");
    emit_orw_vis_raise(c, rvis, attr, cid, rt, rv, b);
    buf_printf(b, "; %s; })", oint_kind(want) && node_is_oint(c, id) ? oint_nil(want)
                                                                    : default_value_from_compiler(c, want));
    return 1;
  }
  TyKind rdt = (rk == SP_MEMBER_METHOD) ? (TyKind)c->scopes[rmi].ret : TY_UNKNOWN;
  int rnb = -1;   /* a generated reader's ivar that keeps its nil in a nil bit */
  /* A generated reader is the ivar itself, read straight off the struct, so
     the temp takes the ivar's own type. Taking the expression's type put a
     boxed ivar into an sp_int when the def writer had no other call site to
     type its parameter (#4827); the arms below convert to `want`. */
  if (rk != SP_MEMBER_METHOD) {
    char rivn[300]; snprintf(rivn, sizeof rivn, "@%s", attr);
    int riv = comp_ivar_index(&c->classes[cid], rivn);
    TyKind ivt = riv >= 0 ? c->classes[cid].ivar_types[riv] : TY_UNKNOWN;
    if (ivt != TY_UNKNOWN && ivt != TY_VOID && ivt != TY_NIL) rdt = ivt;
    if (riv >= 0 && rdt == ivt && oint_kind(ivt) && ivar_has_nilbit(c, cid, riv)) rnb = riv;
  }
  if (rdt == TY_UNKNOWN || rdt == TY_VOID) rdt = want;
  TyKind vt = repr_of(c, v).as_ty;
  /* TY_NIL joins them: emit_ctype spells it `void`, so a literal nil on the
     right declared a void temp. nil is a value here (`u.g ||= nil` is nil in
     CRuby), and the slot that has to hold it is the poly one. */
  if (vt == TY_UNKNOWN || vt == TY_VOID || vt == TY_NIL) vt = want;

  int tr = ++g_tmp, tv = ++g_tmp, tw = ++g_tmp;
  /* a reader written out that answers its nil beside the value: the temp is
     that oint, the test its flag; a plain number reader is never nil */
  int ro = (rk == SP_MEMBER_METHOD && oint_kind(rdt) && method_ret_is_oint(&c->scopes[rmi])) || rnb >= 0;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
  if (!comp_ty_value_obj(c, rt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tr);
  /* the reader, once */
  if (ro) buf_puts(b, oint_ctype(rdt)); else emit_ctype(c, rdt, b);
  buf_printf(b, " _t%d = ", tv);
  if (rk == SP_MEMBER_METHOD) {
    Buf rb; memset(&rb, 0, sizeof rb);
    buf_puts(&rb, mc_method(c, &c->scopes[rmi]));
    buf_printf(&rb, "((sp_%s *)_t%d)", rcn, tr);
    TyKind got = (TyKind)c->scopes[rmi].ret;
    if (got == TY_UNKNOWN || got == TY_VOID) got = rdt;
    if (got == rdt) buf_puts(b, rb.p ? rb.p : "");
    else if (rdt == TY_POLY) emit_boxed_text(c, got, rb.p ? rb.p : "", b);
    else emit_unbox_text(c, rdt, rb.p ? rb.p : "", b);
    free(rb.p);
  }
  /* a number ivar's nil is its nil bit, not the field: read as the oint,
     so `@n = nil` makes `o.n ||= 1` assign */
  else if (rnb >= 0) {
    char pfx[160], bt[200];
    snprintf(pfx, sizeof pfx, "((sp_%s *)_t%d)->", c->classes[cid].c_name, tr);
    ivar_nilbit_test(c, cid, rnb, pfx, bt, sizeof bt);
    buf_printf(b, "((%s){ %siv_%s, %s != 0 })", oint_ctype(rdt), pfx, iv_c(attr), bt);
  }
  else { buf_printf(b, "((sp_%s *)_t%d)->iv_%s", c->classes[cid].c_name, tr, iv_c(attr)); }
  buf_puts(b, "; ");
  if (rdt == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tv);
  else if (!is_scalar_ret(rdt) && !comp_ty_value_obj(c, rdt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tv);
  /* the test, then either the reader's answer or the assignment's value.
     `||=` assigns when the reader is falsy, `&&=` when it is truthy. */
  buf_puts(b, "(");
  if (ro) emit_slot_nil_test(c, rdt, tv, is_or ? 0 : 1, b);
  /* a plain number is never nil: `||=` keeps the reader's value, `&&=` assigns */
  else if (oint_kind(rdt)) buf_puts(b, is_or ? "1" : "0");
  else emit_slot_nil_test(c, rdt, tv, is_or ? 0 : 1, b);
  buf_puts(b, ") ? ");
  /* the expression's own form: the reader's oint where the node answers one
     (`&&=` on a nil attribute is nil), its plain value otherwise */
  int wo = oint_kind(want) && node_is_oint(c, id);
  { char sv[48];
    if (ro && !wo) snprintf(sv, sizeof sv, "_t%d.v", tv);
    /* a plain reader under a node that answers the oint: lifted */
    else if (!ro && wo && rdt == want) snprintf(sv, sizeof sv, "%s(_t%d)", oint_of(want), tv);
    else snprintf(sv, sizeof sv, "_t%d", tv);
    /* a boxed answer takes the reader's oint whole: its nil boxes as nil, not 0 */
    if (ro && !wo && want == TY_POLY && rdt != TY_POLY) buf_printf(b, "%s(_t%d)", oint_box(rdt), tv);
    else if (rdt == want) buf_puts(b, sv);
    else if (want == TY_POLY) emit_boxed_text(c, rdt, sv, b);
    /* (one-String mode: the other face of the same String) */
    else if (rdt == TY_STRBUF && want == TY_STRING) buf_printf(b, "SP_STR_B(%s)", sv);
    else if (rdt == TY_STRING && want == TY_STRBUF) buf_printf(b, "SP_STR_HDL(%s)", sv);
    /* nil-preserving: the reader can answer nil, and this arm is exactly the
       one taken when it did (`&&=` on a nil attribute). Plain unboxing lands
       on the payload under the tag -- 0 for an int slot, where nil is
       the oint's flag -- so `b.v &&= 7` answered 0 where CRuby answers nil. */
    else emit_unbox_nilable_text(c, want, sv, b);
  }
  buf_puts(b, " : ({ ");
  /* An empty container literal has no type of its own: comp_ntype answers
     poly and the emitter picks the concrete container from how the attribute
     is used later, so `_t%d` was declared sp_RbVal and initialized with an
     sp_IntArray * (#4277). Render it into the slot the declaration promises.
     Every other value type answers the same both ways, so this changes only
     the shapes that did not compile. The setup a composite RHS spills to
     g_pre is captured and spliced here, in the branch that assigns, so a
     short-circuited `||=`/`&&=` runs none of it, as emit_slot_orw_value_o
     does. */
  { Buf vpre, vval; memset(&vpre, 0, sizeof vpre); memset(&vval, 0, sizeof vval);
    Buf *saved_pre = g_pre; g_pre = &vpre;
    if (vt == TY_POLY && repr_of(c, v).kind != RK_BOXED) emit_boxed(c, v, &vval);
    else emit_expr(c, v, &vval);
    g_pre = saved_pre;
    if (vpre.p) buf_puts(b, vpre.p);
    emit_ctype(c, vt, b); buf_printf(b, " _t%d = %s; ", tw, vval.p ? vval.p : "");
    free(vpre.p); free(vval.p); }
  if (vt == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tw);
  else if (!is_scalar_ret(vt) && !comp_ty_value_obj(c, vt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tw);
  buf_puts(b, "(void)(");
  if (wvis != SP_VIS_PUBLIC) {
    char rv[32]; snprintf(rv, sizeof rv, "_t%d", tr);
    emit_orw_vis_raise(c, wvis, wn, cid, rt, rv, b);
  }
  else if (wk == SP_MEMBER_METHOD) {
    buf_puts(b, mc_method(c, &c->scopes[wmi]));
    buf_printf(b, "((sp_%s *)_t%d, ", wcn, tr);
    { Scope *ws = &c->scopes[wmi];
      LocalVar *pv = (ws->nparams > 0 && ws->pnames[0]) ? scope_local(ws, ws->pnames[0]) : NULL;
      TyKind pt = pv ? pv->type : vt;
      char sw[32]; snprintf(sw, sizeof sw, "_t%d", tw);
      emit_coerce_text(c, v, vt, pt, CO_HOLD, sw, "a conditional attribute writer's argument", b);
    }
    buf_puts(b, ")");
  }
  else {
    /* the writer is the ivar's store: a boxed field takes the value boxed,
       as a plain attr write does (a reader written out over an
       `attr_accessor`'s writer, `meth.name ||= "unknown"`) */
    char wivn[300]; snprintf(wivn, sizeof wivn, "@%s", attr);
    int wiv = comp_ivar_index(&c->classes[cid], wivn);
    TyKind wivt = wiv >= 0 ? c->classes[cid].ivar_types[wiv] : TY_UNKNOWN;
    buf_printf(b, "((sp_%s *)_t%d)->iv_%s = ", c->classes[cid].c_name, tr, iv_c(attr));
    char sw[32]; snprintf(sw, sizeof sw, "_t%d", tw);
    if (wivt == TY_POLY && vt != TY_POLY)
      emit_coerce_text(c, v, vt, wivt, CO_HOLD, sw, "a conditional attribute writer's argument", b);
    else buf_puts(b, sw);
  }
  buf_puts(b, "); ");
  if (wk != SP_MEMBER_METHOD && wvis == SP_VIS_PUBLIC) {
    char ivn[300], obj[64], mk[400];
    snprintf(ivn, sizeof ivn, "@%s", attr);
    snprintf(obj, sizeof obj, "((sp_%s *)_t%d)", c->classes[cid].c_name, tr);
    const char *m = comp_ivar_index(&c->classes[cid], ivn) >= 0
                      ? ivar_set_mark(c, cid, ivn, obj, "->", mk, sizeof mk) : NULL;
    if (m) buf_printf(b, "%s ", m);
  }
  { char sw[32]; snprintf(sw, sizeof sw, "_t%d", tw);
    if (wo) buf_printf(b, "%s(", oint_of(want));
    if (vt == want) buf_puts(b, sw);
    else if (want == TY_POLY) emit_boxed_text(c, vt, sw, b);
    else emit_unbox_text(c, want, sw, b);
    if (wo) buf_puts(b, ")");
  }
  buf_puts(b, "; }); })");
  return 1;
}

/* `$g |= v` / `@@a |= v` (and `&=` `-=` `+=` `*=`) on an array slot as a
   value -- a method's last expression, a block's, an assignment's rhs: the
   statement form's array write, then the updated slot. The value arm emitted
   the raw C operator, `|` between two array pointers, which did not compile
   (#4833 fixed the statement form only). Answers 1 when it emitted. */
static int emit_array_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                      const char *op, int v, Buf *b) {
  if (!(ty_is_array(t) || t == TY_POLY_ARRAY)) return 0;
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_array_op_assign(c, ref, t, op, v, &ab);
  if (ok) buf_printf(b, "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

/* The scalar twin: the statement form, then the slot, in a statement
   expression, so a global or class variable takes the overflow-checked
   helpers in value position too. */
/* `id`: the op-write node, whose value is the slot's oint where the write
   can be nil and the slot's value where it is plain */
static int emit_scalar_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                       const char *op, int v, int lhs_nil, int id, Buf *b) {
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_scalar_op_assign(c, ref, t, op, v, 1, lhs_nil, 0, &ab);
  if (ok) buf_printf(b, lhs_nil && oint_kind(t) && !node_is_oint(c, id) ? "({ %s%s.v; })" : "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

/* The boxed twin, for a poly slot. */
static int emit_poly_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                     const char *op, int v, Buf *b) {
  if (t != TY_POLY) return 0;
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_poly_op_assign(c, ref, op, v, 1, &ab);
  if (ok) buf_printf(b, "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

static void emit_expr_node(Compiler *c, int id, Buf *b);

/* `next v` in value position -- the tail of a proc/block body, as in
   `proc { next 5 }`. Leaving the block with a value IS the body's value
   there, so the expression is just v (a bare `next` yields nil). The
   early-exit forms are handled by the statement emitter (#3026).

   A `next` an expression holds anywhere else -- `x = (c ? (next 5) : 7)`,
   `c && (next)` ahead of more statements, an argument, an element -- leaves
   the block from there: the statement emitter's lowering inside a statement
   expression, which a `continue`, a `return` or a `goto` may leave. The
   value after it is never reached and gives the expression the C type it
   had. Answers 0 for a tail `next` with several values, which the caller
   refuses as before. */
static int emit_next_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int nargs = nt_ref(nt, id, "arguments");
  int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
  if (!next_is_block_value(c, id)) {
    buf_puts(b, "({\n");
    emit_stmt(c, id, b, g_indent + 1);
    emit_indent(b, g_indent + 1);
    buf_printf(b, "%s; })", nvc == 0 ? "sp_box_nil()" : default_value_from_compiler(c, repr_of(c, id).as_ty));
    return 1;
  }
  if (nvc == 1) { emit_expr(c, nv[0], b); return 1; }
  if (nvc == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
  return 0;
}

/* How many expressions enclose the one being emitted: a call nested in an
   argument sits deeper than the call that takes the argument. */
int g_expr_depth = 0;

/* Does self at `defined?` node `id` answer bare call `cn` with a builtin
   method: one of BasicObject's public ones, which every object has, one of
   Object's, which every object has but one below BasicObject, or in a class
   body or a class method, one of Class's or Module's own? */
static int defined_bare_builtin(Compiler *c, int id, const char *cn) {
  Scope *s = comp_scope_of(c, id);
  if (basicobject_own_method(cn) && !sp_streq(cn, "initialize") && !sp_streq(cn, "method_missing"))
    return 1;
  if (builtin_object_method_known(cn))
    return !(s && !s->is_cmethod && s->class_id >= 0 && class_is_blank_slate(c, s->class_id));
  int cls = !s ? -1 : s->is_cmethod ? s->class_id : s->class_id < 0 && g_ie_class_id < 0 ? g_class_body_id : -1;
  return cls >= 0 && class_object_answers_builtin(cn, comp_class_is_module(c, &c->classes[cls]));
}

/* A class or module body's own ivar (outside an instance_eval), written by
   ivar write node `id` used as a value: its civ_ slot, the one the statement
   form writes, in `out`. 0 when the write is not one. */
static int ivar_class_body_slot(Compiler *c, int id, Scope *s, char *out, size_t cap) {
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm || g_class_body_id < 0 || civ_class_of(c, id, s) != g_class_body_id) return 0;
  snprintf(out, cap, "civ_%s_%s", c->classes[g_class_body_id].name, iv_c(nm + 1));
  return 1;
}

/* A variable's read as the slot itself, to assign to: no sp_srange_live
   around a String Range's (#8321). A read of a String Range takes no wrapper
   now either: its kept endpoints stay handles, read where they are used
   (sp_srange_first / sp_srange_last), so the slot form is the read. */
void emit_slot_expr(Compiler *c, int id, Buf *b) {
  emit_expr_node(c, id, b);
}
/* The receiver of a native class's binding called on an instance of a
   subclass of the class (#9269), and that class's C struct: the pointer is
   the native object's, cast where the receiver is emitted */
int g_nsub_cast_node = -1;
const char *g_nsub_cast_type = NULL;
static int g_rat_box_node = -1;
/* --int-overflow=promote: the conversions whose Rational is exact only as a
   big one when its terms leave sp_int, rendered boxed (lib/spinel_rt.h):
   Float#to_r and Kernel#Rational(String). 0 for any other node. */
static int emit_promote_rat_exact(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  /* (a `&.` call's emitter boxes the answer itself) */
  if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") >= 0 || nt_safe_nav(nt, id)) return 0;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), argc = 0;
  const int *argv = call_args(nt, id, &argc);
  if (!nm) return 0;
  if (sp_streq(nm, "to_r") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_FLOAT &&
      comp_method_in_chain(c, comp_core_class(c, CORE_FLOAT), "to_r", NULL) < 0) {
    /* boxed, so a Float that may be nil answers nil's (0/1) */
    buf_puts(b, "sp_poly_to_r_m("); emit_boxed(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  if (sp_streq(nm, "rationalize") && argc <= 1 && recv >= 0 && comp_ntype(c, recv) == TY_FLOAT &&
      comp_method_in_chain(c, comp_core_class(c, CORE_FLOAT), "rationalize", NULL) < 0) {
    buf_puts(b, "sp_poly_rationalize_m("); emit_boxed(c, recv, b);
    buf_printf(b, ", %d, ", argc);
    if (argc) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, ")");
    return 1;
  }
  if (sp_streq(nm, "Rational") && argc == 1 && recv < 0 && comp_method_index(c, "Rational") < 0 &&
      (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_STRBUF)) {
    buf_puts(b, "sp_str_to_r_strict_v("); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}
void emit_expr(Compiler *c, int id, Buf *b) {
  if (b == g_pre && g_pre) { emit_into_pre_line(c, emit_expr, id); return; }
  if (emit_const_class_call_wrap(c, id, b, emit_expr)) return;
  /* a block's statement an emitter inlines (gsub!'s `{ |m| m + m }`): its
     operands are not held across the block, and two copies of one live
     String, neither rooted, let the second's allocation collect the first */
  if (g_one_held && node_is_block_stmt(c, id)) {
    g_one_held = 0;
    emit_expr(c, id, b);
    g_one_held = 1;
    return;
  }
  /* --defer-refusals: an identity Set's value checked where it arrives */
  if (g_idset_marks && emit_idset_checked(c, id, b, 0)) return;
  /* an argument of a call re-emitted as its builtin sees the reopenings */
  if (g_io_skip_reopen && id != g_io_skip_node) {
    g_io_skip_reopen = 0;
    emit_expr(c, id, b);
    g_io_skip_reopen = 1;
    return;
  }
  if (id == g_nsub_cast_node) {
    g_nsub_cast_node = -1;
    buf_printf(b, "((%s *)(", g_nsub_cast_type);
    emit_expr(c, id, b);
    buf_puts(b, "))");
    g_nsub_cast_node = id;
    return;
  }
  /* --int-overflow=promote holds a Rational boxed (infer_type); a builtin
     that renders one as an sp_Rational is boxed here, a boxed one passed on.
     A node an emitter has bound to a temp of its own (view_bind) is that
     temp, whatever its C type. */
  int rat_bound = 0;
  for (int i = g_n_argov - 1; i >= 0 && !rat_bound; i--) if (g_argov_node[i] == id) rat_bound = 1;
  if (g_promote_mode && id != g_rat_box_node && an_rat_boxed(id) && !rat_bound && emit_promote_rat_exact(c, id, b))
    return;
  if (g_promote_mode && id != g_rat_box_node && an_rat_boxed(id) && !rat_bound) {
    int save = g_rat_box_node;
    g_rat_box_node = id;
    buf_puts(b, "sp_promote_rat_box(");
    emit_expr(c, id, b);
    buf_puts(b, ")");
    g_rat_box_node = save;
    return;
  }
  /* One representation: an expression's default face is bytes, and an
     element read of a typed String container answers the handle
     (one_str_elem_call), so it is read through SP_STR_B here. An emitter
     that wants the handle itself asks through strh_elem_handle_text, which
     names the node in g_one_elem_raw. A sink handed the bytes of a live
     payload finds its handle again through the payload's owner link
     (sp_strface_owner), so neither face loses the String's identity. */
  /* (a node an emitter has bound to a temp reads as that temp, in the face
     its binder chose) */
  int one_bound = 0;
  for (int i = g_n_argov - 1; i >= 0 && !one_bound; i--) if (g_argov_node[i] == id) one_bound = 1;
  if (!one_bound && id != g_one_elem_raw && one_str_elem_call(c, id, TY_UNKNOWN)) {
    int save = g_one_elem_raw;
    g_one_elem_raw = id;
    buf_puts(b, "SP_STR_B(");
    emit_expr(c, id, b);
    buf_puts(b, ")");
    g_one_elem_raw = save;
    return;
  }
  if (g_repr_check) repr_check_ask(c, id);
  int frame = g_repr_check ? repr_channel_begin(c, id) : -1;
  g_expr_depth++;
  /* nil out of band: emit_oint_expr asked for the bare sp_oint of this one
     node (g_want_oint, consumed here before any child is emitted). Without
     that, an oint producer is read as a plain scalar through sp_oint_arg
     (TypeError for nil), and a leaf read of an oint slot as its `.v`. */
  int want_o = g_want_oint; g_want_oint = 0;
  Repr opr = repr_of(c, id);
  TyKind ot = opr.as_ty;
  /* a node bound to a hoisted temp (view_bind) is that plain temp */
  int bound = 0, bound_o = 0;
  for (int i = g_n_argov - 1; i >= 0 && !bound; i--) if (g_argov_node[i] == id) { bound = 1; bound_o = g_argov_oint[i]; }
  int is_o = bound ? oint_kind(ot) && bound_o : opr.opt_producer;
  NodeKind ok = nt_kind(c->nt, id);
  int leaf_o = !bound && want_o && !is_o && (ok == NK_NilNode || opr.kind == RK_OPT);
  g_oint_read = (is_o || leaf_o) &&
                (ok == NK_LocalVariableReadNode || ok == NK_InstanceVariableReadNode ||
                 ok == NK_ClassVariableReadNode || ok == NK_GlobalVariableReadNode);
  int wrap_arg = is_o && !want_o, wrap_of = want_o && !is_o && !leaf_o;
  /* a wrapper pending: the node is rendered aside first, since a text that
     diverges (a raise token answering the plain default) takes no wrapper */
  Buf side; memset(&side, 0, sizeof side);
  /* (an oint wanted of an oint producer is rendered aside too: a diverging
     text there carries the plain default, which the oint slot cannot take) */
  int keep_o = want_o && oint_kind(ot) && is_o;
  Buf *ob = (wrap_arg || wrap_of || keep_o) ? &side : b;
  /* an Array subclass instance read where an Array is wanted -- a splat, a
     destructuring, a `for` collection, an element write that is no call --
     is typed as its Array (an_ary_viewed): the same pointer, cast to the
     Array it starts with (#7449) */
  if (an_ary_viewed(c, id) && (array_new_copies(comp_ntype(c, id)) || ty_is_hash(comp_ntype(c, id)))) {
    buf_printf(ob, "((%s)(", c_type_name(comp_ntype(c, id)));
    emit_expr_node(c, id, ob);
    buf_puts(ob, "))");
  }
  else {
    /* a plain consumer of an element read: the read may unwrap itself */
    int sv_n = g_ck_node, sv_d = g_ck_done; const char *sv_o = g_ck_op;
    if (wrap_arg) { g_ck_node = id; g_ck_op = NULL; g_ck_done = 0; }
    emit_expr_node(c, id, ob);
    if (wrap_arg && g_ck_done && ob == &side) {
      buf_puts(b, side.p ? side.p : "0"); free(side.p);
      g_ck_node = sv_n; g_ck_op = sv_o; g_ck_done = sv_d;
      g_oint_read = 0; g_expr_depth--;
      if (g_repr_check) repr_channel_end(c, frame);
      return;
    }
    if (wrap_arg) { g_ck_node = sv_n; g_ck_op = sv_o; g_ck_done = sv_d; }
  }
  if (ob == &side) {
    /* a diverging text (a raise token) carries the plain default: it is
       never unwrapped (sp_oint_arg would not take it), and wrapped as the
       plain scalar it is where the oint is wanted */
    if (keep_o) {
      if (side.p && text_diverges(side.p)) buf_printf(b, "({ (void)(%s); %s; })", side.p, oint_nil(ot));
      else buf_puts(b, side.p ? side.p : "");
      free(side.p);
      g_oint_read = 0;
      g_expr_depth--;
      if (g_repr_check) repr_channel_end(c, frame);
      return;
    }
    int skip = wrap_arg && side.p && text_diverges(side.p);
    if (wrap_arg && !skip) buf_printf(b, "%s(", oint_arg(ot));
    else if (wrap_of) buf_printf(b, "%s(", oint_of(oint_kind(ot) ? ot : TY_INT));
    buf_puts(b, side.p ? side.p : "");
    if (!skip) buf_puts(b, ")");
    free(side.p);
  }
  g_oint_read = 0;
  g_expr_depth--;
  if (g_repr_check) repr_channel_end(c, frame);
}

/* The slot read emit_expr is about to render (a local, an ivar, a cvar, a
   global) is wanted as its own sp_oint: set by emit_expr for that one leaf,
   consumed by the leaf's emitter. A plain read of an oint slot is its `.v`. */
int g_oint_read = 0;
/* The read face of local `name`'s slot at node `id`: the slot itself, or its
   `.v` when the slot is an oint read as a plain scalar. */
static void emit_local_read_face(Compiler *c, int id, const char *name, LocalVar *lv, Buf *b) {
  int want = g_oint_read; g_oint_read = 0;
  emit_local_ref(c, id, name, b);
  if (slot_is_oint(lv) && !want) buf_puts(b, ".v");
}

/* `_tN = v` inside the guard of an `a[i] ||= v` / `&&= v` value, with v's
   own prelude emitted in the guard too: hoisted above it, that prelude ran
   even when the guard skipped the write, and before the slot read. */
static void emit_guarded_slot_assign(Compiler *c, int v, int tn, Buf *b) {
  Buf rvb; memset(&rvb, 0, sizeof rvb);
  Buf *svp = g_pre; g_pre = b;
  emit_expr(c, v, &rvb);
  g_pre = svp;
  buf_printf(b, "_t%d = %s", tn, rvb.p ? rvb.p : "0");
  free(rvb.p);
}

/* `_tN = <v as an sp_oint>`, as emit_guarded_slot_assign, for an Integer or
   Float element slot read with its nil */
static void emit_guarded_oint_slot_assign(Compiler *c, int v, TyKind t, int tn, Buf *b) {
  Buf rvb; memset(&rvb, 0, sizeof rvb);
  Buf *svp = g_pre; g_pre = b;
  emit_oint_expr(c, v, t, &rvb);
  g_pre = svp;
  buf_printf(b, "_t%d = %s", tn, rvb.p ? rvb.p : oint_nil(t));
  free(rvb.p);
}

static void emit_guarded_poly_slot_assign(Compiler *c, int v, int tn, Buf *b) {
  /* The right-hand side's own prelude belongs INSIDE the guard. Hoisted
     above it, a call there runs even when the key is already present --
     which turned the memoizing `memo[n] ||= f.(n-1) + f.(n-2)` into
     unbounded recursion. */
  Buf rvb; memset(&rvb, 0, sizeof rvb); Buf *svp = g_pre; g_pre = b;
  emit_boxed(c, v, &rvb); g_pre = svp;
  buf_printf(b, "_t%d = %s", tn, rvb.p ? rvb.p : "sp_box_nil()"); free(rvb.p);
}

static void emit_if_arm_value(Compiler *c, int last, TyKind res, int res_o, int tr) {
  Repr lr = repr_of(c, last);
  TyKind lt = lr.as_ty;
  int saved_gi = g_indent; g_indent = g_indent + 2;
  Buf le; memset(&le, 0, sizeof le);
  /* a poly result boxes the arm as the ternary's arm does (emit_boxed): an
     Integer or Float that may be nil boxes its oint, nil included */
  int boxed = !(res_o && oint_kind(res)) && res == TY_POLY && lr.kind != RK_BOXED;
  if (res_o && oint_kind(res)) emit_oint_expr(c, last, res, &le);
  else if (boxed) emit_boxed(c, last, &le);
  else emit_expr(c, last, &le);
  g_indent = saved_gi;
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = ", tr);
  if (res_o && oint_kind(res)) buf_puts(g_pre, le.p ? le.p : oint_nil(res));
  else if (boxed) buf_puts(g_pre, le.p ? le.p : "sp_box_nil()");
  else if (emit_arm_text_as_bigint(res, lt, le.p ? le.p : default_value_from_compiler(c, lt), g_pre)) { }
  /* (one representation: the arm of either face into the temp's, SP_STR_TO) */
  else if (le.p && one_str_to_open(g_pre, res, "_t%d", tr)) { buf_puts(g_pre, le.p); buf_puts(g_pre, ")"); }
  else buf_puts(g_pre, le.p ? le.p : default_value_from_compiler(c, res));
  buf_puts(g_pre, ";\n");
  free(le.p);
}

/* The last statement of an arm of a multi-statement if used as a value (emit_if_expr):
   assigned to the result temp `tr`. An empty `[]` / `{}` / bare `Array.new` /
   `Hash.new` caches TY_UNKNOWN for a reason that is not "has no value": it has
   no ELEMENT type until something supplies one. Running it for effect leaves
   the slot at its default, which is how `if c then [] end` answered nil where
   CRuby answers []. emit_ternary_arm already builds it against a result type.
   A tail with no value is run for its effect alone. */
static void emit_if_arm_tail(Compiler *c, int last, TyKind res, int res_o, int tr) {
  TyKind lt = repr_of(c, last).as_ty;
  if (lt == TY_UNKNOWN && node_is_empty_container(c, last)) {
    Buf le; memset(&le, 0, sizeof le);
    emit_ternary_arm(c, last, res, &le);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "_t%d = %s;\n", tr, le.p ? le.p : default_value_from_compiler(c, res));
    free(le.p);
  }
  else if (lt == TY_NIL || lt == TY_UNKNOWN || lt == TY_VOID) emit_stmt(c, last, g_pre, g_indent + 2);
  else emit_if_arm_value(c, last, res, res_o, tr);
}

/* The engine's String constants (RUBY_VERSION, ...) are frozen in CRuby, and
   each is one object however often it is read. Each is a frozen static laid
   out like a frozen literal -- a real sp_str_hdr, then the 0xf1 marker --
   defined once ahead of the bodies at its first read, so `RUBY_VERSION << "x"`
   raises FrozenError and RUBY_VERSION.equal?(RUBY_VERSION) still holds. `lit`
   is the C source of a string literal (RUBY_PLATFORM's is the runtime's
   macros). `var` is named under sp_str_, a prefix the runtime owns, so a
   user `def ruby_version` (sp_ruby_version) cannot take the same C name.
   --share-strings: the header links to a literal slot of its own, as a
   frozen literal's does (fzl_emit_defs), so the handle a shared slot
   takes for the constant is its own one (sp_String_literal_handle), not
   a new handle each read. */
static void emit_engine_const_str(Compiler *c, const char *var, const char *lit, Buf *b) {
  char key[64];
  snprintf(key, sizeof key, " %s = {", var);
  if (!g_proc_protos.p || !strstr(g_proc_protos.p, key)) {
    buf_printf(&g_proc_protos,
                 "static sp_StringLiteral %s_slot = { NULL, NULL };\n"
                 "static struct { sp_str_hdr h; unsigned char m; char d[sizeof(%s)]; } %s = "
                 "{ { (sp_str_hdr *)((char *)&%s_slot + 1), sizeof(%s) | SP_STR_SIZE_ASCII7, sizeof(%s) - 1, 0 }, 0xf1, %s };\n",
                 var, lit, var, var, lit, lit, lit);
  }
  buf_printf(b, "((const char *)%s.d)", var);
}

/* A write's nil result uses the expression's representation, not its
   receiver's field representation. In a rebound block these can differ:
   a caller's String slot types the result while the receiver stores an
   Integer field's nil. The store above still uses the receiver's own kind.
   The ivar's assigned mark (ivar_write_mark) goes between the two. */
static void emit_ivar_write_result(Compiler *c, int id, int value, TyKind slot,
                                   const char *ref, Buf *b) {
  Repr result = repr_of(c, id);
  TyKind wt = result.as_ty;
  char mk[300];
  const char *m = ivar_write_mark(c, id, mk, sizeof mk);
  buf_puts(b, "; ");
  if (m) buf_printf(b, "%s ", m);
  /* the slot's oint where the write answers one (node_is_oint): the field
     beside its nil bit, or the oint static as it is */
  if (oint_kind(wt) && node_is_oint(c, id)) {
    int wcid, wiv, wk = ivar_node_slot(c, id, &wcid, &wiv);
    if (wk == 1) {
      char pfx[128], bt[160];
      snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref);
      ivar_nilbit_test(c, wcid, wiv, pfx, bt, sizeof bt);
      buf_printf(b, "((%s){ %s, %s != 0 }); })", oint_ctype(wt), ref, bt);
      return;
    }
  }
  if (nt_kind(c->nt, value) == NK_NilNode && ie_class_of(c, id) >= 0 && nil_value(wt)) {
    buf_printf(b, "%s; })", nil_value(wt));
    return;
  }
  /* A boxed slot can feed a concrete expression, as in a retyped
     instance_exec body whose assignment answers a Symbol. */
  if (slot == TY_POLY && result.kind != RK_BOXED && wt != TY_UNKNOWN && wt != TY_VOID && wt != TY_NIL &&
      is_scalar_ret(wt)) {
    emit_unbox_text(c, wt, ref, b);
    buf_puts(b, "; })");
    return;
  }
  buf_printf(b, "%s; })", ref);
}

/* The value of a local's `||=` / `&&=`: its slot. A slot that holds the
   --share-strings handle is an sp_String *: where the write is typed
   String and no handle mark asks for the handle, the slot's read face, as
   `=`'s value is (an sp_String * where a String is read did not build).
   The default build's mutable handle needs the same read face. */
static void emit_local_orw_result(Compiler *c, int id, LocalVar *lv, const char *ref, Buf *b) {
  Repr r = repr_of(c, id);
  if (!r.handle && r.as_ty == TY_STRING && repr_slot_kind(c, lv) == RK_STRBUF)
    emit_strbuf_slot_read(c, id, r, ref, b);
  else {
    buf_puts(b, ref);
    /* an oint slot answers its value where the expression is plain (an
       expression typed past the slot -- poly -- is boxed from the oint by
       its consumer) */
    if (lv && repr_slot_kind(c, lv) == RK_OPT && !r.opt_producer && oint_kind(r.as_ty)) buf_puts(b, ".v");
  }
}

/* Local-variable reads and writes and instance-variable writes in value position (emit_expr_node's arms, in their order) */
static int emit_local_ivar_write_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *lrn = nt_str(nt, id, "name");
    /* a splat operand the length dispatch reads (splat_operand_read): a boxed
       one is the Array the splat makes of it, a scalar wrapped in one */
    static int in_splat_view;
    if (!in_splat_view && repr_of(c, id).kind == RK_BOXED && nt_int(nt, id, "splat_view", 0)) {
      in_splat_view = 1;
      buf_puts(b, "sp_splat_to_array("); emit_expr(c, id, b); buf_puts(b, ")");
      in_splat_view = 0;
      return 1;
    }
    /* a local a shared-handle shim reads through its shadow, which the shim
       writes back into the handle: no copy of the String */
    /* compile-time define_method substitution: the loop var IS the literal */
    if (g_dm_subst_name && lrn && sp_streq(lrn, g_dm_subst_name) && g_dm_subst_node >= 0) {
      emit_expr(c, g_dm_subst_node, b); return 1;
    }
    /* a `return .. if p.nil?`-guarded param read: unbox the poly slot to the
       non-nil type the narrowing pass proved (#1661) */
    if (c->nilnarrow[id] != TY_UNKNOWN) {
      Buf rb2; memset(&rb2, 0, sizeof rb2);
      emit_local_ref(c, id, lrn, &rb2);
      /* A `rescue K => e` arm's read of a binding two arms share (#4343). The
         slot is a sp_Exception *, and every user exception subclass's struct
         opens with that header, so the arm's class is a plain pointer cast --
         not the poly unbox the guards below want. */
      {
        LocalVar *elv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
        if (elv && elv->type == TY_EXCEPTION && ty_is_object(c->nilnarrow[id])) {
          buf_puts(b, "((");
          emit_ctype(c, c->nilnarrow[id], b);
          buf_printf(b, ")%s)", rb2.p ? rb2.p : "");
          free(rb2.p);
          return 1;
        }
      }
      /* The narrowing was recorded while the slot was boxed; a slot that
         has since settled on the narrowed type itself (`v = yield x` whose
         every site answers an Array) is read as it is, not unboxed from a
         box it never was. */
      {
        LocalVar *nlv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
        if (nlv && nlv->type == c->nilnarrow[id] && nlv->type != TY_POLY) {
          free(rb2.p);
          emit_local_read_face(c, id, lrn, nlv, b);
          return 1;
        }
      }
      /* An `is_a?(Array)`-narrowed read: a boxed array can be any element-typed
         representation (Int/Float/Str/Poly array), so normalize to a PolyArray
         at runtime rather than casting the raw .v.p to one kind. */
      if (c->nilnarrow[id] == TY_POLY_ARRAY)
        buf_printf(b, "sp_poly_to_poly_array(%s)", rb2.p ? rb2.p : "");
      else
        emit_unbox_text(c, c->nilnarrow[id] == TY_STRBUF ? repr_of(c, id).as_ty : c->nilnarrow[id],
                        rb2.p ? rb2.p : "", b);
      free(rb2.p);
      return 1;
    }
    LocalVar *slv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
    Repr rp = repr_of(c, id);
    /* The node cache can lag a later narrowing of the local itself (a
       map-block hash-key param settling from poly to string): a POLY-typed
       read of a concretely-declared local boxes the declared value so the
       consumer's poly dispatch stays well-typed (#2730). */
    if (slv && slv->type != TY_POLY && slv->type != TY_UNKNOWN &&
        repr_of_slot(c, slv).kind != RK_STRBUF && rp.kind == RK_BOXED) {
      Buf rb3; memset(&rb3, 0, sizeof rb3);
      emit_local_ref(c, id, lrn, &rb3);
      /* an oint slot boxes with its nil */
      if (slot_is_oint(slv)) buf_printf(b, "%s(%s)", oint_box(slv->type), rb3.p ? rb3.p : "");
      else emit_boxed_text(c, slv->type, rb3.p ? rb3.p : "", b);
      free(rb3.p);
      return 1;
    }
    if (slv && repr_of_slot(c, slv).kind == RK_STRBUF) {
      /* A container-store / equal?-arg read of a shared-mutable string yields
         the live HANDLE, not a copy (#3227 phase 3). */
      if (rp.handle) {
        if (g_repr_check) repr_channel_note(c, id, RCH_HANDLE);
        emit_local_ref(c, id, lrn, b);
        return 1;
      }
      /* a parameter that only reads the bytes for the length of the call
         takes the live buffer: the copy below is O(len) per call (#7482) */
      if (rp.read_raw && decide_node(c->nt, id, "strbuf-raw", NULL)) {
        if (g_repr_check) repr_channel_note(c, id, RCH_BYTES);
        buf_puts(b, "(");
        emit_local_ref(c, id, lrn, b);
        buf_puts(b, " ? sp_String_cstr(");
        emit_local_ref(c, id, lrn, b);
        buf_puts(b, ") : NULL)");
        return 1;
      }
      /* A mutable-string local read yields an independent GC string copy: its
         sp_String buffer is not itself a GC object, so a bare cstr pointer
         would dangle once the wrapper is unreachable (e.g. after `return`).
         Transient append/length use the raw sp_String via dedicated paths. */
      /* publish the handle to the deep-return side channel as the copy is
         read out: a marked caller of a shared-returning method picks it up
         right after the call (#3227 P6) */
      /* Through sp_strbuf_read_pub, which sequences the channel's write
         (two reads among one call's arguments), reads a nil handle as nil
         (a parameter or a local can be one: `def run(cmd, text: nil)`,
         `q = nil; q = +"x" if c`), and keeps a frozen handle's mark on the
         copy it answers. */
      buf_puts(b, "sp_strbuf_read_pub(");
      if (g_repr_check) repr_channel_note(c, id, RCH_PUBLISH);
      emit_local_ref(c, id, lrn, b);
      buf_puts(b, ")");
      return 1;
    }
    /* A POLY variable handed to a parameter the callee appends to in place:
       a plain String it holds becomes the shared handle and is stored back
       first, so the callee appends to the variable's own String (#6179). */
    if (slv && slv->type == TY_POLY && rp.poly_lift) {
      Buf rl; memset(&rl, 0, sizeof rl);
      emit_local_ref(c, id, lrn, &rl);
      emit_poly_lift_ref(rl.p ? rl.p : "", b);
      free(rl.p);
      return 1;
    }
    emit_local_read_face(c, id, lrn, slv, b); return 1;
  }
  if (sp_streq(ty, "LocalVariableWriteNode")) {
    /* assignment used as expression: ({ lv = rhs; lv; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    /* `x = y = nil` as expression: inner writes become statements inside the
       stmt-expr; this target takes its own typed nil (not the inner slot). */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) {
        buf_puts(b, "({ ");
        emit_stmt_inner(c, v, b, 0);
        emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
        if (lv && lv->type == TY_RANGE) buf_puts(b, "(sp_Range){0}");
        else if (lv) buf_puts(b, default_value_from_compiler(c, lv->type));
        else buf_puts(b, "sp_box_nil()");
        buf_puts(b, "; "); emit_local_ref(c, id, nm, b); buf_puts(b, "; })");
        return 1;
      }
    }
    if (lv && repr_of_slot(c, lv).kind == RK_STRBUF) {
      /* a shared-handle local: the statement form owns every way a value
         becomes the handle (alias, boxed element, fresh wrap); the raw
         const char * went into the sp_String * slot here (a write inside
         Array.new's block, whose non-tail statements are emitted as
         expressions). The value is the slot's ordinary read face, as the
         ivar twin's is. */
      buf_puts(b, "({ ");
      emit_assign(c, id, b, 0);
      /* boxed where the slot holds the rule's handle, under the handle
         mark (emit_boxed_strbuf): the handle itself, as a marked read */
      if (repr_of(c, id).handle && repr_write_share(c, id)) {
        buf_puts(b, " ");
        emit_local_ref(c, id, nm, b);
        buf_puts(b, "; })");
        return 1;
      }
      /* a nil write leaves the handle NULL: the value is nil, not a read
         through it (CodeRabbit on #4990) */
      buf_puts(b, " sp_strbuf_read_pub(");
      if (g_repr_check) repr_channel_note(c, id, RCH_PUBLISH);
      emit_local_ref(c, id, nm, b);
      buf_puts(b, "); })");
      return 1;
    }
    buf_puts(b, "({ ");
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    int sk = splice_store_open(c, id, b);   /* an open splice alias of the variable */
    int ven = 0;
    int v_empty_array = nt_kind(nt, v) == NK_ArrayNode && (nt_arr(nt, v, "elements", &ven), ven == 0);
    if (lv && lv->type == TY_POLY && repr_of(c, v).kind != RK_BOXED) emit_boxed(c, v, b);
    /* an empty `[]` is built at the slot's representation, as the statement
       form and the ivar twin below build it: its own default, an IntArray,
       went into an sp_PolyArray *, sp_FloatArray * or sp_PtrArray * slot.
       Array.new's block emits its non-tail statements here, so
       `y = [] if i == 0` there stopped the C build. */
    else if (lv && v_empty_array && ty_is_ptr_array(lv->type)) buf_puts(b, ptr_array_new_c(lv->type));
    else if (lv && v_empty_array && emit_empty_container_for_slot(c, v, lv->type, b)) { }
    else if (lv && lv->type == TY_POLY_ARRAY && ty_is_array(comp_ntype(c, v)) &&
             repr_of(c, v).elem != TY_POLY) {
      /* a typed array into a poly-array slot: convert, as emit_assign does
         (the statement form; this is its expression twin, #2834) */
      Repr avr = repr_of(c, v);
      buf_printf(b, "sp_PolyArray_from_%s(",
                 avr.elem == TY_INT ? "int_array"
                 : avr.elem == TY_STRING ? "str_array" : "float_array");
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (lv && lv->type != TY_POLY && lv->type != TY_UNKNOWN &&
             repr_of(c, v).untyped)
      /* `if (x = obj.unresolved(...)) ...`: the gate's raise-all token into a
         typed slot, coerced (mirrors the statement-form emit_assign). */
      emit_unresolved_coerced(c, v, lv->type, b);
    /* an int or boxed value into a TY_BIGINT slot wraps, exactly as the
       statement form and the return path do. This is the INNER write of a
       chain (`b = a = 0` with `a` promoted to bigint by its loop), which is
       the one write that reaches this expression twin with a bigint target:
       raw, the literal was reinterpreted as an sp_Bigint* and a boxed chain
       value did not compile at all. */
    else if (lv && lv->type == TY_BIGINT && !repr_of(c, v).big) {
      if (repr_of(c, v).kind == RK_BOXED) {
        buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, v, b); buf_puts(b, ")");
      }
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
    }
    /* poly RHS into a scalar/string slot: the same unbox the statement form
       applies (emit_poly_rhs_coerced) */
    else if (lv && slot_is_oint(lv)) emit_oint_expr(c, v, lv->type, b);
    else if (lv && emit_poly_rhs_coerced(c, lv->type, v, b)) { }
    else if (lv) emit_coerce(c, v, lv->type, CO_HOLD, "a local variable write", b);
    else emit_expr(c, v, b);
    splice_store_close(c, id, sk, b);
    buf_puts(b, "; "); emit_local_ref(c, id, nm, b);
    /* the value: the slot's oint where the write answers one, its value
       where the write is plain (`(w = 7).to_s`) */
    if (lv && slot_is_oint(lv) && !node_is_oint(c, id)) buf_puts(b, ".v");
    buf_puts(b, "; })");
    return 1;
  }
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
    /* c += 3 used as expression: ({ <op-assign>; <c>; }). The value-yielding
       read goes through emit_local_ref so a celled (captured) local derefs its
       shared cell (*_cell_c) instead of a bare lv_c -- the latter is undeclared
       inside a block that captured c by cell (e.g. `mutex.synchronize { c += 1 }`
       in a thread). */
    const char *nm = nt_str(nt, id, "name");
    LocalVar *olv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    /* Without --share-strings, a pure alias pair's handle local takes `+=`
       as a statement only: the value kept under another name (`x = (s +=
       y)`) would be a copy the alias rule never joined to s's String */
    if (olv && olv->type == TY_STRBUF && repr_of_slot(c, olv).handle && !repr_of_slot(c, olv).share) {
      unsupported(c, id, "operator assignment");
      return 1;
    }
    buf_puts(b, "({ ");
    emit_op_assign(c, id, b, 0);
    /* a local holding a shared handle: the handle itself where one is
       taken (repr_write_share, under the handle mark), else its read face,
       as a read of the local gives */
    if (olv && repr_of_slot(c, olv).handle && repr_of_slot(c, olv).kind == RK_STRBUF &&
        !(repr_of(c, id).handle && repr_write_share(c, id))) {
      buf_puts(b, "sp_strbuf_read_pub(");
      if (g_repr_check) repr_channel_note(c, id, RCH_PUBLISH);
      emit_local_ref(c, id, nm, b);
      buf_puts(b, ")");
    }
    else emit_local_ref(c, id, nm, b);
    if (olv && slot_is_oint(olv) && !node_is_oint(c, id)) buf_puts(b, ".v");   /* as the plain write's value */
    buf_puts(b, "; })");
    return 1;
  }
  if (sp_streq(ty, "InstanceVariableWriteNode")) {
    /* @ivar = rhs used as expression: ({ self->iv_x = rhs; self->iv_x; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws = comp_scope_of(c, id);
    int cid2 = cws ? cws->class_id : -1;
    if (cid2 < 0 && g_class_body_id >= 0) cid2 = g_class_body_id;
    if (!nm || v < 0) { buf_puts(b, "0"); return 1; }
    /* inside an instance_eval/exec splice the block scope has no class_id, so
       the ivar belongs to the rebound receiver class (g_ie_class_id). */
    int ivcls2 = ie_class_of(c, id) >= 0 ? ie_class_of(c, id) : cid2 >= 0 ? cid2 : g_ie_class_id;
    /* a top-level ivar's slot is the Toplevel pseudo-class's, the one the
       store below writes (civ_Toplevel_x): without its kind the value went
       in as it was, and `y = (@a = [])` put an Integer array into a slot
       a later write had made a poly array */
    if (ivcls2 < 0) ivcls2 = c->toplevel_class;
    TyKind ivt2 = TY_UNKNOWN;
    int ivnull2 = 0, iveh2 = 0;
    if (ivcls2 >= 0) {
      int iv2 = comp_ivar_index(&c->classes[ivcls2], nm);
      if (iv2 >= 0) {
        ivt2 = c->classes[ivcls2].ivar_types[iv2]; ivnull2 = c->classes[ivcls2].ivar_nullable_int[iv2];
        iveh2 = repr_of_ivar(c, ivcls2, iv2).elems_handle;
      }
    }
    const char *vty2 = nt_type(nt, v);
    NodeKind vty2_k = nt_kind(nt, v);
    int ven2 = 0;
    int v_empty_array2 = vty2_k == NK_ArrayNode && (nt_arr(nt, v, "elements", &ven2), ven2 == 0);
    int v_empty_hash2 = 0;
    if (!v_empty_array2 && vty2) {
      int hen2 = 0;
      if (vty2_k == NK_HashNode || vty2_k == NK_KeywordHashNode)
        v_empty_hash2 = (nt_arr(nt, v, "elements", &hen2), hen2 == 0);
    }
    char ref2e[300];
    int fz_cid = -1;  /* instance-path writes guard the frozen bit */
    if (cws && cws->is_cmethod && cid2 >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_%s_%s", c->classes[cid2].name, iv_c(nm + 1));
    else if (cid2 < 0 && g_ie_class_id >= 0) {
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
      fz_cid = g_ie_class_id;
    }
    else if (cid2 < 0 && c->toplevel_class >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_Toplevel_%s", iv_c(nm + 1));
    else if (ivar_class_body_slot(c, id, cws, ref2e, sizeof ref2e)) {}
    else {
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
      fz_cid = cid2;
    }
    /* `@a = @b = nil` as expression: inner writes become statements inside the
       stmt-expr; this target takes its own typed nil (not the inner slot). */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) {
        buf_puts(b, "({ ");
        if (fz_cid >= 0) emit_frozen_obj_guard(c, fz_cid, g_self ? g_self : "self", b);
        emit_stmt_inner(c, v, b, 0);
        buf_printf(b, "%s = ", ref2e);
        switch (ivt2) {
        case TY_RANGE: buf_puts(b, "(sp_Range){0}"); break;
        case TY_POLY: buf_puts(b, "sp_box_nil()"); break;
        case TY_INT: case TY_FLOAT: emit_ivar_nil_store(c, id, ivt2, b); break;
        case TY_STRING: buf_puts(b, "NULL"); break;
        default: buf_puts(b, default_value_from_compiler(c, ivt2)); break;
        }
        char mk2[300];
        const char *m2 = ivar_write_mark(c, id, mk2, sizeof mk2);
        buf_printf(b, "; %s%s%s; })", m2 ? m2 : "", m2 ? " " : "", ref2e);
        return 1;
      }
    }
    buf_puts(b, "({ ");
    if (fz_cid >= 0) emit_frozen_obj_guard(c, fz_cid, g_self ? g_self : "self", b);
    buf_printf(b, "%s = ", ref2e);
    /* an open splice alias of the ivar (a plain String slot's) */
    int sk = ivt2 == TY_STRING ? splice_store_open(c, id, b) : -1;
    Repr rp = repr_of(c, v);
    int wcid2, wiv2, wk2 = ivar_node_slot(c, id, &wcid2, &wiv2);
    /* an Integer or Float field with a nil bit, or an oint static */
    if (wk2 == 1 && ivar_has_nilbit(c, wcid2, wiv2)) {
      char wpfx2[128]; snprintf(wpfx2, sizeof wpfx2, "%s%s", g_self, g_self_deref);
      emit_ivar_write_nilbit(c, id, wcid2, wiv2, wpfx2, v, b);
    }
    else if (wk2 == 2 && civ_is_oint(c, wcid2, wiv2)) emit_oint_expr(c, v, ivt2, b);
    else if (oint_kind(ivt2) && node_may_be_nil(c, v)) refuse_nil_store(c, v, ivt2, "an instance variable write");
    else if (v_empty_array2 && ty_is_ptr_array(ivt2)) buf_puts(b, ptr_array_new_c(ivt2));
    /* `def initialize = (@a = [X.new])` into a narrowed pointer-array ivar:
       the literal builds at the slot's kind, as the statement form does */
    else if (ty_is_ptr_array(ivt2) && emit_ptr_array_build(c, v, ivt2, b)) { }
    else if (v_empty_array2 && ivt2 == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array2 && array_kind(ivt2)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt2));
    else if (v_empty_hash2 && ty_is_hash(ivt2)) {
      const char *hcn = ty_hash_cname(ivt2);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt2 == TY_STRBUF && (rp.as_ty != TY_STRBUF || repr_handle_apart(c, v, rp)) && rp.kind != RK_BOXED) {
      /* a shared-handle slot takes an alias RHS by handle and wraps anything
         else in a fresh handle, exactly as the statement form does; the raw
         const char * went into the sp_String * slot here (a value-position
         write, `def w(v) = (@body = v.to_s)`, #3993 / #4567). A value typed
         as the handle whose text is its String's bytes (repr_handle_apart:
         a new String the slot's handle mark reaches, `(@log = pier.dup)`)
         is wrapped too. The value of the expression is the slot's ordinary
         read face below. */
      Buf srefW2; memset(&srefW2, 0, sizeof srefW2);
      /* nil is the slot's nil, as the statement form stores it; converted,
         it raised TypeError (`def initialize = (@a = nil)`) */
      if (v >= 0 && nt_kind(nt, v) == NK_NilNode) buf_puts(b, "NULL");
      else if (strbuf_seq_nil_tail(c, v)) emit_strbuf_ivar_store(c, 0, v, b);
      else if (!strbuf_ivar_store_routes(c, v) && strbuf_slot_text(c, v, &srefW2)) {
        buf_puts(b, srefW2.p);
        free(srefW2.p);
      }
      /* The statement form takes a conditional arm by arm too. */
      else if (strbuf_ivar_store_aliases(c, v))
        emit_strbuf_ivar_store(c, 1, v, b);
      else if (emit_strbuf_write_handle(c, v, b)) { }
      /* a route that hands on the handle (`@a = yield`, emit_strbuf_route) */
      else if (emit_strbuf_route(c, v, b)) { }
      else {
        buf_printf(b, "%s(", strbuf_new_func(c, v));
        emit_str_expr(c, v, b);
        buf_puts(b, ")");
      }
      /* under the handle mark, the handle itself, as the local's twin; and
         where the write is typed as the handle (a value a handle is asked
         of, as #3993's box asks it), whose read face did not build */
      char mk3[300];
      const char *m3 = ivar_write_mark(c, id, mk3, sizeof mk3);
      buf_printf(b, "; %s%s", m3 ? m3 : "", m3 ? " " : "");
      if ((repr_of(c, id).handle && repr_write_share(c, id)) || repr_of(c, id).ty == TY_STRBUF) {
        buf_printf(b, "%s; })", ref2e);
        if (g_repr_check) repr_channel_note(c, id, RCH_HANDLE);
      }
      else {
        buf_printf(b, "sp_strbuf_read_pub(%s); })", ref2e);
        if (g_repr_check) repr_channel_note(c, id, RCH_PUBLISH);
      }
      return 1;
    }
    else if (ivt2 == TY_POLY && rp.kind != RK_BOXED) emit_boxed(c, v, b);
    /* a typed array into the general Array slot the ivar widened to (an
       `o.a << nil` through its reader), rebuilt as the statement form does:
       an endless `def initialize(z) = (@a = [z])` assigned the typed array
       itself and the C did not build; --share-strings: one whose elements
       the rule shares wraps each String as a handle */
    else if (iveh2 && comp_ntype(c, v) == TY_STR_ARRAY) emit_str_array_handles(c, v, b);
    else if (emit_array_into_poly_slot(c, ivt2, v, b)) { }
    else if (seeded_array_kind_mismatch(ivt2, comp_ntype(c, v))) {
      /* an array of another kind into a seed-pinned array ivar: converted,
         as the statement form does */
      emit_array_store_value(c, ivt2, v, b);
    }
    else if (ivt2 != TY_POLY && ivt2 != TY_UNKNOWN && rp.kind == RK_BOXED) {
      /* poly rhs assigned to a typed ivar: unbox to the concrete type */
      Buf _rb; memset(&_rb, 0, sizeof _rb);
      emit_expr(c, v, &_rb);
      Buf _ck; memset(&_ck, 0, sizeof _ck);
      if (ivcls2 >= 0 && class_ivar_pinned(&c->classes[ivcls2], nm))
        emit_rbs_checked_text(c, ivt2, nm, _rb.p ? _rb.p : "sp_box_nil()", &_ck);
      else buf_puts(&_ck, _rb.p ? _rb.p : "sp_box_nil()");
      emit_unbox_text(c, ivt2, _ck.p ? _ck.p : "sp_box_nil()", b);
      free(_ck.p);
      free(_rb.p);
    }
    else {
      /* a subclass instance stored into an ancestor-typed ivar slot (#3418) */
      emit_obj_upcast_prefix(c, ivt2, comp_ntype(c, v), b);
      emit_coerce(c, v, ivt2, CO_HOLD, "an instance variable write", b);
    }
    splice_store_close(c, id, sk, b);
    emit_ivar_write_result(c, id, v, ivt2, ref2e, b);
    return 1;
  }
  if (sp_streq(ty, "InstanceVariableOrWriteNode") || sp_streq(ty, "InstanceVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws3 = comp_scope_of(c, id);
    int cid3 = cws3 ? cws3->class_id : -1;
    if (cid3 < 0 && g_class_body_id >= 0) cid3 = g_class_body_id;
    /* a toplevel method's ivar is the Toplevel pseudo-class global, as in
       the statement form */
    int tl3 = cid3 < 0 && g_ie_class_id < 0 && cws3 && !cws3->is_cmethod;
    if (tl3) cid3 = c->toplevel_class;
    TyKind ivt3 = TY_UNKNOWN;
    int eh3 = 0;
    /* inside an instance_eval/exec block the slot is the receiver's, as the
       plain write above reads it: the enclosing method's class has no such
       ivar, or one of another type */
    int tcid3 = ie_class_of(c, id) >= 0 ? ie_class_of(c, id) :
                cws3 && cws3->class_id < 0 && !cws3->is_cmethod && g_ie_class_id >= 0 ? g_ie_class_id : cid3;
    if (tcid3 >= 0) {
      int iv3 = comp_ivar_index(&c->classes[tcid3], nm);
      if (iv3 >= 0) { ivt3 = c->classes[tcid3].ivar_types[iv3]; eh3 = repr_of_ivar(c, tcid3, iv3).elems_handle; }
    }
    char ref3[300];
    if (cws3 && cws3->is_cmethod && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_%s_%s", c->classes[cid3].name, iv_c(nm + 1));
    else if (ivar_class_body_slot(c, id, cws3, ref3, sizeof ref3)) {}
    else if (tl3 && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_Toplevel_%s", iv_c(nm + 1));
    else
      snprintf(ref3, sizeof ref3, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    char mk3[300];
    const char *m3 = ivar_write_mark(c, id, mk3, sizeof mk3);
    /* boxed where the slot holds the rule's handle (the handle mark): the
       handle itself */
    if (repr_of(c, id).handle && repr_write_share(c, id))
      emit_strbuf_orw_share_value(c, id, ref3, v, is_or, -1, NULL, m3, b);
    else emit_ivar_orw_value(c, id, ivt3, eh3, ref3, v, is_or, m3, b);
    return 1;
  }
  if (sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    /* the local as every other write names it: a captured one lives in its
       cell (`(*_cell_x)`), an inlined one under its renamed C name */
    /* a number local given a boxed value (`u = (v &&= z)`, z boxed) answers
       a boxed value, as its type says: the local's number, or nil when its
       nil flag is set */
    static int orw_boxing_id = -1;
    if ((t == TY_INT || t == TY_FLOAT) && repr_of(c, id).kind == RK_BOXED && orw_boxing_id != id) {
      int sv = orw_boxing_id; orw_boxing_id = id;
      buf_printf(b, "%s(", slot_is_oint(lv) ? oint_box(t) : ty_box_fn(t));
      emit_expr_node(c, id, b);
      buf_puts(b, ")");
      orw_boxing_id = sv;
      return 1;
    }
    char lhs[300];
    { Buf lr; memset(&lr, 0, sizeof lr); emit_local_ref(c, id, nm, &lr);
      snprintf(lhs, sizeof lhs, "%s", lr.p ? lr.p : ""); free(lr.p); }
    char cond[400];
    if (t == TY_POLY) {
      snprintf(cond, sizeof cond, "%ssp_poly_truthy(%s)", is_or ? "!" : "", lhs);
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (t == TY_BOOL) {
      snprintf(cond, sizeof cond, "%s%s", is_or ? "!" : "", lhs);
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (t == TY_SYMBOL) {
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      snprintf(cond, sizeof cond, "%s %s= (sp_sym)-1", lhs, is_or ? "=" : "!");
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (!is_or) {
      /* `x &&= v` assigns only when x is not nil, as the statement form
         tests (a slot with no nil representation always assigns) */
      Buf nb; memset(&nb, 0, sizeof nb);
      if (local_nil_test(c, lv, lhs, &nb)) {
        /* through emit_assign, as `||=` below: a nil or boxed value takes
           the coercion the plain `x = v` does (nil, not 0) */
        Buf apre, abody;
        memset(&apre, 0, sizeof apre); memset(&abody, 0, sizeof abody);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &apre; g_indent = 0;
        emit_assign(c, id, &abody, 0);
        g_pre = sv_pre; g_indent = sv_ind;
        buf_printf(b, "({ if (!(%s)) { ", nb.p);
        if (apre.p) buf_puts(b, apre.p);
        if (abody.p) buf_puts(b, abody.p);
        buf_puts(b, " } ");
        emit_local_orw_result(c, id, lv, lhs, b);
        buf_puts(b, "; })");
        free(apre.p); free(abody.p);
      }
      else emit_orw_guard(c, v, t, NULL, lhs, 1, 0, b);
      free(nb.p);
    }
    else {
      /* `x ||= v` in value position on a slot that can hold nil (see
         local_nil_test): the old bare read assumed every non-poly local was
         already truthy and dropped the assignment entirely (#3388). */
      Buf rb; memset(&rb, 0, sizeof rb); emit_local_ref(c, id, nm, &rb);
      Buf nb; memset(&nb, 0, sizeof nb);
      if (rb.p && local_nil_test(c, lv, rb.p, &nb)) {
        Buf apre, abody;
        memset(&apre, 0, sizeof apre); memset(&abody, 0, sizeof abody);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &apre; g_indent = 0;
        emit_assign(c, id, &abody, 0);
        g_pre = sv_pre; g_indent = sv_ind;
        buf_printf(b, "({ if (%s) { ", nb.p);
        if (apre.p) buf_puts(b, apre.p);
        if (abody.p) buf_puts(b, abody.p);
        buf_puts(b, " } ");
        emit_local_orw_result(c, id, lv, rb.p, b);
        buf_puts(b, "; })");
        free(apre.p); free(abody.p);
      }
      else buf_puts(b, lhs);
      free(nb.p); free(rb.p);
    }
    return 1;
  }
  return 0;
}

/* Instance-variable reads, class- and global-variable reads and writes, and the $1 / $& references (emit_expr_node's arms, in their order) */
/* A read of a shared-mutable String slot (an ivar's or a global's, `sref`):
   a marked read yields the live HANDLE, an ordinary read a GC copy of the
   current contents (NULL stays nil) (#3227). */
static void emit_strbuf_slot_read(Compiler *c, int id, Repr rp, const char *sref, Buf *b) {
  if (rp.handle) {
    buf_printf(b, "%s", sref);
    if (g_repr_check) repr_channel_note(c, id, RCH_HANDLE);
  }
  /* a parameter that only reads the bytes for the length of the call
     takes the live buffer (#7482) */
  else if (rp.read_raw && decide_node(c->nt, id, "strbuf-raw", NULL)) {
    buf_printf(b, "(%s ? sp_String_cstr(%s) : NULL)", sref, sref);
    if (g_repr_check) repr_channel_note(c, id, RCH_BYTES);
  }
  else {
    str_read_handle(b, sref, 1);   /* (one representation: a copy when held, g_one_held) */
    if (g_repr_check) repr_channel_note(c, id, RCH_PUBLISH);
  }
}

/* The same for a slot read at node `id` that is not a variable's (a Struct
   member's `s[:a]`), as the node's own Repr asks. */
void emit_strbuf_node_read(Compiler *c, int id, const char *sref, Buf *b) {
  emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
}

static int emit_ivar_cvar_gvar_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (sp_streq(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@x" */
    Scope *cs = comp_scope_of(c, id);
    Repr rp = repr_of(c, id);
    /* A POLY ivar handed to a parameter the callee appends to in place: a
       plain String it holds becomes the shared handle and is stored back
       first, as a POLY local's marked read is (poly_strbuf_lift); an
       instance's field store takes its write barrier from gc_wb_insert. */
    if (rp.poly_lift && !g_ie_nil_ivars && rp.kind == RK_BOXED) {
      int vl = view_push_repr(c, id, VR_POLY_LIFT, 0);
      Buf rl; memset(&rl, 0, sizeof rl);
      emit_expr_node(c, id, &rl);
      view_pop(c, vl);
      emit_poly_lift_ref(rl.p ? rl.p : "", b);
      free(rl.p);
      return 1;
    }
    if (g_ie_nil_ivars) {
      TyKind it = repr_of(c, id).as_ty;
      /* the read's form: its oint where the dispatcher takes one */
      int want_ni = g_oint_read; g_oint_read = 0;
      if ((want_ni || node_is_oint(c, id)) && oint_kind(it)) buf_puts(b, oint_nil(it));
      else emit_slot_nil_read(c, it, b);
      return 1;
    }
    /* inside a shared-mutable shim over THIS slot: both the reads and the
       arm's own write-back go to the shadow (see codegen_internal.h) */
    if (g_sb_iv_name && nm && sp_streq(nm, g_sb_iv_name) &&
        strbuf_ivar_owner(c, id) == g_sb_iv_cid) {
      buf_printf(b, "%s", g_sb_iv_repl);
   /* (the shim writes the shadow back) */
      return 1;
    }
    /* a shared-mutable string slot: a marked read yields the live HANDLE, an
       ordinary read a GC copy of the current contents (NULL stays nil) (#3227) */
    { Buf srefI; memset(&srefI, 0, sizeof srefI);
      int vsm = view_push_repr(c, id, VR_STRBUF_BOX, 1);   /* let slot_text resolve regardless of mark */
      int is_sb = strbuf_slot_text(c, id, &srefI);
      view_pop(c, vsm);
      if (is_sb) {
        emit_strbuf_slot_read(c, id, rp, srefI.p, b);
        free(srefI.p);
        return 1;
      } }
    /* the slot: an object field through the receiver prefix (`self->`,
       `_t3.`), or a class-level / top-level static. A field with a nil bit
       read as its sp_oint pairs the value with its bit; an oint static read
       as a plain scalar is its `.v`. */
    int want_o = g_oint_read; g_oint_read = 0;
    char pfx[128]; pfx[0] = 0;
    int is_static = 0, is_nil = 0;
    Buf ref; memset(&ref, 0, sizeof ref);
    if (cs && cs->is_cmethod && cs->class_id >= 0) {
      buf_printf(&ref, "civ_%s_%s", c->classes[cs->class_id].name, iv_c(nm + 1));  /* module/class-level ivar */
      is_static = 1;
    }
    else if (cs && cs->class_id < 0 && g_ie_class_id >= 0) {
      /* inside instance_eval block: access ivar via receiver pointer; one
         the receiver's class never writes is nil, as on that object (the
         struct has no field for it) */
      int has = 0;
      for (int k = g_ie_class_id; k >= 0 && !has; k = c->classes[k].parent)
        has = comp_ivar_index(&c->classes[k], nm) >= 0;
      if (!has) is_nil = 1;
      else { snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref); buf_printf(&ref, "%siv_%s", pfx, iv_c(nm + 1)); }
    }
    else if (cs && cs->class_id < 0 && holder_cbody(c, id) >= 0 && ie_class_of(c, id) < 0) {
      /* A block or a proc written in a class or module body (`%w(a b).each
         do |m| ... lambda { @x } ... end`): its scope is no class's, but
         the ivar is the body class's own, as the write emitters take it
         (civ_class_of) and as a lambda written straight in the body reads
         it. Taken for the top level's, the read named `self` where no
         self exists, or an undeclared Toplevel slot. One the class never
         names is nil, as on the class object. */
      int cb = holder_cbody(c, id);
      if (comp_ivar_index(&c->classes[cb], nm) >= 0) {
        buf_printf(&ref, "civ_%s_%s", c->classes[cb].name, iv_c(nm + 1));
        is_static = 1;
      }
      else is_nil = 1;
    }
    else if (cs && cs->class_id < 0) {
      /* top-level method: ivar stored as file-scope global in Toplevel pseudo-class */
      int tl = c->toplevel_class;
      if (tl >= 0) { buf_printf(&ref, "civ_Toplevel_%s", iv_c(nm + 1)); is_static = 1; }
      else { snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref); buf_printf(&ref, "%siv_%s", pfx, iv_c(nm + 1)); }
    }
    else { snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref); buf_printf(&ref, "%siv_%s", pfx, iv_c(nm + 1)); }
    if (is_nil) {
      TyKind it = repr_of(c, id).as_ty;
      /* the read's form: its oint where the dispatcher takes one (the
         node answers oint, or the leaf was asked for it) */
      if ((want_o || node_is_oint(c, id)) && oint_kind(it)) buf_puts(b, oint_nil(it));
      else emit_slot_nil_read(c, it, b);
    }
    else {
      int icid, iiv;
      int sk = ivar_node_slot(c, id, &icid, &iiv);
      TyKind it = sk > 0 ? c->classes[icid].ivar_types[iiv] : TY_UNKNOWN;
      if (sk == 1 && ivar_has_nilbit(c, icid, iiv) && want_o) {
        char bt[160];
        ivar_nilbit_test(c, icid, iiv, pfx, bt, sizeof bt);
        buf_printf(b, "((%s){ %s, %s != 0 })", oint_ctype(it), ref.p ? ref.p : "", bt);
      }
      else if (is_static && sk == 2 && civ_is_oint(c, icid, iiv) && !want_o)
        buf_printf(b, "%s.v", ref.p ? ref.p : "");
      /* a class-level static the slot lookup did not place (an inherited
         class method reading its class's ivar): the static is the oint */
      else if (is_static && sk != 2 && oint_kind(repr_of(c, id).as_ty) && !want_o)
        buf_printf(b, "%s.v", ref.p ? ref.p : "");
      /* a field with no nil bit read where its oint is wanted (an
         instance_eval tail typed for any receiver): the value, never nil */
      else if (want_o && !is_static && sk == 1 && oint_kind(it))
        buf_printf(b, "%s(%s)", oint_of(it), ref.p ? ref.p : "");
      else buf_puts(b, ref.p ? ref.p : "");
    }
    free(ref.p);
    return 1;
  }
  if (sp_streq(ty, "ClassVariableReadNode")) {
    HolderRef h;
    if (holder_of_node_in(c, id, g_class_body_id, &h)) {
      /* --share-strings: a class variable holding the shared handle */
      Buf sref; memset(&sref, 0, sizeof sref);
      if (holder_static_handle_buf(c, id, &sref)) {
        emit_strbuf_slot_read(c, id, repr_of(c, id), sref.p, b);
        free(sref.p);
        return 1;
      }
      holder_slot_buf(c, &h, &sref);
      buf_puts(b, sref.p);
      free(sref.p);
      /* an oint static read as a plain scalar is its value */
      { int want = g_oint_read; g_oint_read = 0;
        if (cvar_is_oint(c, h.cid, h.idx) && !want) buf_puts(b, ".v"); }
      return 1;
    }
    unsupported(c, id, "class variable read (no class scope)");
  }
  if (sp_streq(ty, "ClassVariableWriteNode")) {  /* in value position: yields the assigned value */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    HolderRef h;
    if (!holder_of_node_in(c, id, g_class_body_id, &h)) { unsupported(c, id, "class variable write (no class scope)"); return 1; }
    int cid = h.cid, idx = h.idx;
    TyKind ct = idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_INT;
    Buf sref; memset(&sref, 0, sizeof sref);
    holder_slot_buf(c, &h, &sref);
    buf_printf(b, "(%s = ", sref.p);
    /* --share-strings: a class variable holding the shared handle; the
       value is its read */
    if (h.r.share) {
      LocalVar slot;
      memset(&slot, 0, sizeof slot);
      lv_set_type(&slot, TY_STRBUF);
      slot.str_shared = 1;
      if (nt_kind(nt, v) == NK_NilNode) buf_puts(b, "NULL");
      else emit_strbuf_value(c, &slot, v, b);
      emit_cvar_set_flag_after(c, cid, nm, b);
      buf_puts(b, ", ");
      emit_strbuf_slot_read(c, id, repr_of(c, id), sref.p, b);
      buf_puts(b, ")");
      free(sref.p);
      return 1;
    }
    /* an Integer or Float class variable some write leaves nil in: its oint;
       the write's value is the slot's oint where the write can be nil, its
       value otherwise */
    if (cvar_is_oint(c, cid, idx)) {
      emit_oint_expr(c, v, ct, b);
      emit_cvar_set_flag_after(c, cid, nm, b);
      buf_printf(b, node_is_oint(c, id) ? ", %s)" : ", %s.v)", sref.p);
      free(sref.p);
      return 1;
    }
    if (oint_kind(ct) && node_may_be_nil(c, v)) refuse_nil_store(c, v, ct, "a class variable write");
    if (emit_empty_container_for_slot(c, v, ct, b)) { /* emitted at the slot's type */ }
    else if (ct == TY_POLY) emit_boxed(c, v, b);
    /* --share-strings: as the statement form, each String as a handle */
    else if (h.r.elems_handle && comp_ntype(c, v) == TY_STR_ARRAY) emit_str_array_handles(c, v, b);
    else if (emit_array_into_poly_slot(c, ct, v, b)) { }
    /* the slot's nil, as the statement form writes it: an endless `def
       self.b = (@@x = nil)` stored the numeric 0 a bare NilNode renders as */
    else if (nt_kind(nt, v) == NK_NilNode && nil_value(ct)) buf_puts(b, nil_value(ct));
    /* a boxed value into a slot typed by its other writes (a writer's
       parameter reached through a `self.class.x =` dispatch, which boxes
       what it passes) is unboxed into the slot, as an ivar's is */
    else if (repr_of(c, v).kind == RK_BOXED && ct != TY_UNKNOWN) {
      Buf vb = expr_buf(c, v); emit_unbox_text(c, ct, vb.p ? vb.p : "sp_box_nil()", b); free(vb.p);
    }
    else emit_coerce(c, v, ct, CO_HOLD, "a class variable write", b);
    emit_cvar_set_flag_after(c, cid, nm, b);
    buf_puts(b, ")");
    free(sref.p);
    return 1;
  }
  if (sp_streq(ty, "GlobalVariableOperatorWriteNode")) {
    /* `$g += v` as a value: parenthesized assignment yielding the updated
       slot, the same shapes as the statement form (string + concatenates). */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable op-write (unregistered global)"); return 1; }
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    char gref[256]; snprintf(gref, sizeof gref, "gv_%s", rn);
    if (lv->type == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "(gv_%s = sp_str_concat(gv_%s, ", rn, rn);
      emit_str_expr(c, v, b); buf_puts(b, "))");
    }
    /* a global holding the shared handle: `+=` makes a new String, and the
       value is its read */
    else if (repr_of_slot(c, lv).kind == RK_STRBUF && op && is_plus_op(op)) {
      buf_printf(b, "({ gv_%s = sp_String_new_shared(sp_str_concat(sp_String_cstr(gv_%s), ", rn, rn);
      emit_str_expr(c, v, b);
      buf_printf(b, ")); sp_String_cstr(gv_%s); })", rn);
    }
    else if (emit_array_op_assign_value(c, gref, lv->type, op, v, b)) { }
    else if (emit_poly_op_assign_value(c, gref, lv->type, op, v, b)) { }
    else if (emit_scalar_op_assign_value(c, gref, lv->type, op, v, gvar_is_oint(c, lv), id, b)) { }
    else {
      buf_printf(b, "(gv_%s %s= ", rn, op ? op : "+");
      emit_coerce(c, v, lv->type, CO_HOLD, "the operand of an `op=`", b); buf_puts(b, ")");
    }
    return 1;
  }
  if (sp_streq(ty, "GlobalVariableWriteNode")) {
    /* `$g = v` as a value: ({ <the write>; gv_g; }) -- the statement form
       builds the slot's value, and the slot is the expression's value */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable write (unregistered global)"); return 1; }
    buf_puts(b, "({ ");
    emit_stmt_inner(c, id, b, 0);
    if (repr_of_slot(c, lv).kind == RK_STRBUF) {
      /* a global holding the shared handle: the handle where a handle is
         asked for, else its read face */
      char sref[256]; snprintf(sref, sizeof sref, "gv_%s", rn);
      emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
      buf_puts(b, "; })");
    }
    /* the write's value: an oint global's oint where the write can be nil,
       its value otherwise */
    else buf_printf(b, gvar_is_oint(c, lv) && !node_is_oint(c, id) ? "gv_%s.v; })" : "gv_%s; })", rn);
    return 1;
  }
  if (sp_streq(ty, "ConstantWriteNode")) {
    /* `m(A = v)` / `(A = v) + 1`: ({ <the write>; cst_A; }) -- the statement
       form sets the constant, and the constant is the expression's value */
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!comp_const_write_valued(c, cv)) {
      char msg[400];
      snprintf(msg, sizeof msg, "`%s = ...` used as a value is not supported for a constant holding %s; "
               "assign %s on a line of its own and use %s", nm ? nm : "?",
               cv && cv->type == TY_STRING ? "a String (the value would be a copy)" : "this kind of value",
               nm ? nm : "it", nm ? nm : "it");
      unsupported_feature(c, id, msg);
      return 1;
    }
    buf_puts(b, "({ ");
    emit_stmt_inner(c, id, b, 0);
    buf_printf(b, "cst_%s; })", nm);
    return 1;
  }
  if (sp_streq(ty, "GlobalVariableOrWriteNode") || sp_streq(ty, "GlobalVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable or/and-write (unregistered global)"); return 1; }
    char gref[256]; snprintf(gref, sizeof gref, "gv_%s", rn);
    int is_or = nt_kind(nt, id) == NK_GlobalVariableOrWriteNode;
    /* the handle under the mark, as an ivar's */
    if (repr_of(c, id).handle && repr_write_share(c, id))
      emit_strbuf_orw_share_value(c, id, gref, nt_ref(nt, id, "value"), is_or, -1, NULL, NULL, b);
    else if (gvar_is_oint(c, lv)) {
      char gnt[300]; snprintf(gnt, sizeof gnt, "%s.nil", gref);
      emit_slot_orw_value_o(c, lv->type, repr_of_slot(c, lv).elems_handle, gref, nt_ref(nt, id, "value"), is_or, gnt, -1, -1, NULL,
                            NULL, b);
    }
    else emit_slot_orw_value(c, lv->type, repr_of_slot(c, lv).elems_handle, gref, nt_ref(nt, id, "value"), is_or,
                             NULL, b);
    return 1;
  }
  if (sp_streq(ty, "ClassVariableOperatorWriteNode")) {
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    HolderRef h;
    if (!holder_of_node_in(c, id, g_class_body_id, &h)) { unsupported(c, id, "class variable op-write (no class scope)"); return 1; }
    int cid = h.cid, idx = h.idx;
    TyKind ct = idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_INT;
    Buf ref; memset(&ref, 0, sizeof ref);
    holder_slot_buf(c, &h, &ref);
    /* (one representation: a handle slot takes the new String in its face) */
    if (ct == TY_STRBUF && op && sp_streq(op, "+")) {
      buf_printf(b, "(%s = SP_STR_TO(%s, sp_str_plus(SP_STR_B(%s), ", ref.p, ref.p, ref.p);
      emit_str_expr(c, v, b); buf_puts(b, ")))");
    }
    else if (ct == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "(%s = sp_str_plus(%s, ", ref.p, ref.p);
      emit_str_expr(c, v, b); buf_puts(b, "))");
    }
    else if (emit_array_op_assign_value(c, ref.p, ct, op, v, b)) { }
    else if (emit_poly_op_assign_value(c, ref.p, ct, op, v, b)) { }
    else if (emit_scalar_op_assign_value(c, ref.p, ct, op, v, cvar_is_oint(c, cid, idx), id, b)) { }
    else {
      buf_printf(b, "(%s %s= ", ref.p, op ? op : "+");
      emit_coerce(c, v, ct, CO_HOLD, "the operand of an `op=`", b); buf_puts(b, ")");
    }
    free(ref.p);
    return 1;
  }
  if (sp_streq(ty, "ClassVariableOrWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    HolderRef h;
    if (!holder_of_node_in(c, id, g_class_body_id, &h)) { unsupported(c, id, "class variable or-write (no class scope)"); return 1; }
    int cid = h.cid;
    Buf ref; memset(&ref, 0, sizeof ref);
    holder_slot_buf(c, &h, &ref);
    TyKind ot = h.idx >= 0 ? c->classes[cid].cvar_types[h.idx] : TY_UNKNOWN;
    /* --share-strings: a class variable holding the shared handle, as the
       statement form (the raw String went into its sp_String * slot) */
    if (h.idx >= 0 && repr_of_cvar(c, cid, h.idx).share) {
      emit_strbuf_orw_share_value(c, id, ref.p, v, 1, cid, nm, NULL, b);
      free(ref.p);
      return 1;
    }
    int oo = oint_kind(ot) && h.idx >= 0 && cvar_is_oint(c, cid, h.idx);
    buf_puts(b, "(");
    if (oint_kind(ot) && !oo) buf_puts(b, "1");   /* a plain number is never nil: `||=` never stores */
    else emit_slot_truthy(ot, ref.p, b);
    buf_printf(b, " ? %s : (%s = ", ref.p, ref.p);
    if (ot == TY_POLY) emit_boxed(c, v, b);
    else if (oo) emit_oint_expr(c, v, ot, b);
    else if (h.r.elems_handle && comp_ntype(c, v) == TY_STR_ARRAY) emit_str_array_handles(c, v, b);
    else emit_expr(c, v, b);
    emit_cvar_set_flag_after(c, cid, nm, b);
    buf_puts(b, "))");
    free(ref.p);
    return 1;
  }
  if (sp_streq(ty, "ClassVariableAndWriteNode")) {
    int v = nt_ref(nt, id, "value");
    const char *nm = nt_str(nt, id, "name");
    HolderRef h;
    if (!holder_of_node_in(c, id, g_class_body_id, &h)) { unsupported(c, id, "class variable and-write (no class scope)"); return 1; }
    Buf ref; memset(&ref, 0, sizeof ref);
    holder_slot_buf(c, &h, &ref);
    TyKind at = h.idx >= 0 ? c->classes[h.cid].cvar_types[h.idx] : TY_UNKNOWN;
    if (h.idx >= 0 && repr_of_cvar(c, h.cid, h.idx).share) {   /* as `||=` */
      emit_strbuf_orw_share_value(c, id, ref.p, v, 0, h.cid, nm, NULL, b);
      free(ref.p);
      return 1;
    }
    int ao = oint_kind(at) && h.idx >= 0 && cvar_is_oint(c, h.cid, h.idx);
    buf_puts(b, "(");
    if (oint_kind(at) && !ao) buf_puts(b, "1");   /* a plain number is never nil: `&&=` always stores */
    else emit_slot_truthy(at, ref.p, b);
    buf_printf(b, " ? (%s = ", ref.p);
    if (at == TY_POLY) emit_boxed(c, v, b);
    else if (ao) emit_oint_expr(c, v, at, b);
    else if (h.r.elems_handle && comp_ntype(c, v) == TY_STR_ARRAY) emit_str_array_handles(c, v, b);
    else emit_expr(c, v, b);
    buf_printf(b, ") : %s)", ref.p);
    free(ref.p);
    return 1;
  }
  if (sp_streq(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ is the record separator "\n"; $! / $; /
       $, read nil (spinel doesn't honor the split/print-sep defaults) */
    if (nm && sp_streq(nm, "$stdin")) { buf_puts(b, "sp_io_stdin()"); return 1; }
    /* A program that REASSIGNS $stdout / $stderr gets a real global for it
       (gv_stdout / gv_stderr, NULL until the assignment runs). Reading the
       stream has to consult that global, or `$stderr = $stdout` writes to the
       real stderr anyway and `$stderr == $stdout` answers false (#3406).
       A program that never assigns has no such global and emits as before. */
    if (nm && (is_standard_output_global(nm))) {
      const char *base = sp_streq(nm, "$stdout") ? "sp_io_stdout()" : "sp_io_stderr()";
      const char *gv = sp_streq(nm, "$stdout") ? "gv_stdout" : "gv_stderr";
      LocalVar *sv = comp_gvar(c, nm + 1);
      if (sv && sv->type == TY_IO) buf_printf(b, "(%s ? %s : %s)", gv, gv, base);
      /* a slot that can hold a non-IO (`$stdout = StringIO.new`) is boxed;
         nil there means never assigned, which reads as the stream itself
         (assigning nil raises TypeError in Ruby) */
      else if (std_stream_boxed(c, nm))
        buf_printf(b, "(sp_poly_nil_p(%s) ? sp_box_nullable_obj((void *)(%s), SP_BUILTIN_IO) : %s)", gv, base, gv);
      else buf_puts(b, base);
      return 1;
    }
    if (nm && sp_streq(nm, "$/")) { emit_str_literal(b, "\n"); return 1; }
    if (nm && sp_streq(nm, "$?")) { buf_puts(b, "sp_last_process_status()"); return 1; }
    if (nm && sp_streq(nm, "$$")) { buf_puts(b, "((sp_int)getpid())"); return 1; }
    if (nm && (is_program_name_global(nm))) { buf_puts(b, "sp_program_name"); return 1; }
    if (is_loaded_features_global(nm)) { buf_puts(b, "sp_get_loaded_features()"); return 1; }
    if (is_load_path_global(nm)) { buf_puts(b, "sp_get_load_path()"); return 1; }
    if (nm && sp_streq(nm, "$!")) { buf_puts(b, "((sp_Exception *)sp_errinfo())"); return 1; }
    if (nm && (sp_streq(nm, "$;") || sp_streq(nm, "$,"))) { buf_puts(b, "0"); return 1; }
    /* regex match globals that Prism may emit as GlobalVariableReadNode */
    if (nm && sp_streq(nm, "$~")) { buf_puts(b, "sp_re_last_matchdata()"); return 1; }
    if (nm && sp_streq(nm, "$&"))  { buf_puts(b, "sp_re_match_str");  return 1; }
    if (nm && sp_streq(nm, "$`"))                          { buf_puts(b, "sp_re_pre_match()");  return 1; }
    if (nm && sp_streq(nm, "$'"))                          { buf_puts(b, "sp_re_post_match()"); return 1; }
    if (nm && sp_streq(nm, "$+")) {
      buf_puts(b, "({ int _bri = 9; while (_bri > 0 && !sp_re_captures[_bri-1]) _bri--; _bri > 0 ? sp_re_captures[_bri-1] : NULL; })");
      return 1;
    }
    if (nm && nm[0] == '$') {
      const char *rn = comp_resolve_gvar(c, nm + 1);
      LocalVar *gv = comp_gvar(c, rn);
      /* --share-strings: a global holding the shared handle (#6765) */
      if (gv && repr_of_slot(c, gv).kind == RK_STRBUF) {
        char sref[256]; snprintf(sref, sizeof sref, "gv_%s", rn);
        emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
        return 1;
      }
      if (gv) {
        /* an oint global read as a plain scalar is its value */
        int want = g_oint_read; g_oint_read = 0;
        buf_printf(b, gvar_is_oint(c, gv) && !want ? "gv_%s.v" : "gv_%s", rn);
        return 1;
      }
    }
    unsupported(c, id, "global variable read");
  }
  if (sp_streq(ty, "NumberedReferenceReadNode")) {
    /* $1..$9 -> the n-th capture of the last match (NULL when absent) */
    long long n = nt_int(nt, id, "number", 0);
    if (n >= 1 && n <= 9) buf_printf(b, "sp_re_captures[%lld]", n);
    else buf_puts(b, "NULL");
    return 1;
  }
  if (sp_streq(ty, "BackReferenceReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) { buf_puts(b, "NULL"); return 1; }
    if (sp_streq(nm, "$~")) buf_puts(b, "sp_re_last_matchdata()");
    else if (sp_streq(nm, "$&")) buf_puts(b, "sp_re_match_str");
    else if (sp_streq(nm, "$`"))                 buf_puts(b, "sp_re_pre_match()");
    else if (sp_streq(nm, "$'"))                 buf_puts(b, "sp_re_post_match()");
    else if (sp_streq(nm, "$+")) {
      /* last group that participated: scan captures[] backwards */
      buf_puts(b, "({ int _bri = 9; while (_bri > 0 && !sp_re_captures[_bri-1]) _bri--; _bri > 0 ? sp_re_captures[_bri-1] : NULL; })");
    }
    else buf_puts(b, "NULL");
    return 1;
  }
  return 0;
}

/* The NameError raise of a constant defined nowhere, valued as a Class
   placeholder. `checks` is the text of the parents' presence checks (it opens
   a comma expression and ends with ", "), or empty: they go into the message
   operand, which C evaluates before the raise, so an unset parent's NameError
   still comes first and the raise stays at the head of the text. */
static void emit_const_undefined_raise(Buf *b, const char *checks, const char *name) {
  buf_puts(b, "(sp_raise_cls(\"NameError\", ");
  if (checks && checks[0]) buf_printf(b, "%s\"uninitialized constant %s\")", checks, name);
  else buf_printf(b, "\"uninitialized constant %s\"", name);
  buf_puts(b, "), ((sp_Class){-1}))");
}

/* A constant read: ConstantReadNode and ConstantPathNode (A::B) (emit_expr_node's arms, in their order).
   `checks` holds the parents' presence checks of a path: the raise of an
   undefined path takes them and empties the buffer, any other form leaves
   them to the caller. */
static int emit_constant_value(Compiler *c, int id, Buf *b, Buf *checks, const NodeTable *nt, const char *ty, int slot) {
  if (sp_streq(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    /* --share-strings: a constant holding the shared handle (#6765) */
    if (cv && !slot && repr_of_slot(c, cv).kind == RK_STRBUF && !cv->init_guarded) {
      char sref[256]; snprintf(sref, sizeof sref, "cst_%s", nm);
      emit_const_read_open(c, id, cv, b);
      emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
      emit_const_read_close(c, id, cv, b);
      return 1;
    }
    if (cv && cv->type != TY_UNKNOWN) {
      if (cv->init_guarded) {
        /* a read during the const's own Class.new init raises NameError */
        emit_const_read_open(c, id, cv, b);
        buf_printf(b, "(sp_init_in_progress_%s ? (sp_raise_cls(\"NameError\","
                      " \"uninitialized constant %s\"), %scst_%s) : %scst_%s)",
                   nm, nm, slot ? "&" : "", nm, slot ? "&" : "", nm);
        emit_const_read_close(c, id, cv, b);
      }
      else emit_const_slot_read(c, id, cv, nm, slot, 1, b);
      return 1;
    }
    /* `include Math` exposes the module's bare constants (#2600) */
    if (c->has_include_math && nm && !comp_const(c, nm)) {
      if (sp_streq(nm, "PI")) { buf_puts(b, "M_PI"); return 1; }
      if (sp_streq(nm, "E"))  { buf_puts(b, "M_E"); return 1; }
    }
    if (nm && sp_streq(nm, "RUBY_DESCRIPTION")) {
      /* The `ruby -v` shape -- engine, version, release and revision,
         platform -- so a harness that records RUBY_DESCRIPTION can tell two spinel builds
         apart. The platform comes from the runtime header at C compile
         time, the same way RUBY_PLATFORM does, so a cross-build names its
         target rather than the host. */
      char lit[256];
      snprintf(lit, sizeof lit, "\"%s [\" SP_RUBY_ARCH \"-\" SP_RUBY_OS \"]\"",
               g_ruby_description ? g_ruby_description : "spinel " SP_RUBY_VERSION);
      emit_engine_const_str(c, "sp_str_ruby_description", lit, b);
      return 1;
    }
    if (nm && sp_streq(nm, "RUBY_VERSION"))     { emit_engine_const_str(c, "sp_str_ruby_version", "\"" SP_RUBY_VERSION "\"", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_ENGINE"))      { emit_engine_const_str(c, "sp_str_ruby_engine", "\"spinel\"", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_ENGINE_VERSION")) { emit_engine_const_str(c, "sp_str_ruby_engine_version", "\"" SP_RUBY_VERSION "\"", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_PLATFORM"))    { emit_engine_const_str(c, "sp_str_ruby_platform", "SP_RUBY_ARCH \"-\" SP_RUBY_OS", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_RELEASE_DATE")) { emit_engine_const_str(c, "sp_str_ruby_release_date", "\"2026-09-15\"", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_REVISION"))    { emit_engine_const_str(c, "sp_str_ruby_revision", "\"229531a6cfbf07e3caef30dbac24a2a3f3fed482\"", b); return 1; }
    if (nm && sp_streq(nm, "RUBY_PATCHLEVEL"))  { buf_puts(b, "((sp_int)0)"); return 1; }
    if (nm && sp_streq(nm, "RUBY_COPYRIGHT"))   { emit_engine_const_str(c, "sp_str_ruby_copyright", "\"ruby - Copyright (C) 1993-2026 Yukihiro Matsumoto\"", b); return 1; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return 1; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return 1; }
    if (nm && sp_streq(nm, "STDOUT")) { buf_puts(b, "sp_io_stdout()"); return 1; }
    if (nm && sp_streq(nm, "STDERR")) { buf_puts(b, "sp_io_stderr()"); return 1; }
    if (nm && sp_streq(nm, "STDIN"))  { buf_puts(b, "sp_io_stdin()"); return 1; }
    /* `OpenStruct` as a class value, matching what an OpenStruct's #class
       returns (name-keyed, cls_id -1); require "ostruct" gated (#3155). */
    if (nm && sp_streq(nm, "OpenStruct") && sp_feature_required("ostruct")) {
      buf_puts(b, "((sp_Class){(sp_int)-1, SPL(\"OpenStruct\")})");
      return 1;
    }
    if (nm) {
      int _cidx = comp_class_index(c, nm);
      emit_const_read_open(c, id, NULL, b);
      if (_cidx >= 0) {
        buf_printf(b, "((sp_Class){%d})", _cidx);  /* user class as value: TY_CLASS unboxed */
      }
      else {
        int _bcid = builtin_class_id(nm);
        if (_bcid != 0)
          buf_printf(b, "((sp_Class){%d})", _bcid);  /* builtin class as value */
        else if (is_builtin_exception_name(nm) || is_builtin_class_name(nm) ||
                 is_builtin_module_name(nm)) {
          /* a builtin class, module or exception class with no cls_id of its
             own (SystemCallError, LoadError, Process, GC, Method, Random): a
             name-backed Class value, like OpenStruct above -- sp_class_eq and
             the boxed form both compare by name, and an instance's #class
             answers the same (`Random.new.class`) */
          buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", nm);
        }
        else {
          /* A constant defined NOWHERE in the program: spinel is closed-world
             and `const_set` only stores into a constant the program already
             defines, so no definition can arrive later. Say so at build time --
             a dropped `require` otherwise reads as an engine bug, since the
             build succeeds and the first request crashes (#3976). It stays a
             warning, not an error: referencing a missing constant to test the
             NameError is legal Ruby, and ruby/spec does exactly that. */
          if (!const_ref_is_rescued(c, id) && !const_ref_is_guarded(c, id))
            warn_undefined_constant(c, id, nm);
          emit_const_undefined_raise(b, NULL, nm);
        }
      }
      emit_const_read_close(c, id, NULL, b);
    }
    else unsupported(c, id, "constant read");
    return 1;
  }
  if (sp_streq(ty, "ConstantPathNode")) {
    /* M::CONST -> the flat constant named by the final path component */
    const char *nm = nt_str(nt, id, "name");
    int par_idc = nt_ref(nt, id, "parent");
    NodeKind par_tyc_k = nt_kind(nt, par_idc);
    /* the parent's LEAF name also qualifies through a nested path
       (Outer::CSql::TEXT) -- the ffi decl registers under its module's
       unqualified name */
    const char *par_nmc = ((par_tyc_k == NK_ConstantReadNode ||
                                       par_tyc_k == NK_ConstantPathNode))
                          ? nt_str(nt, par_idc, "name") : NULL;
    /* A constant naming a CLASS is a written path (Probe::Block::CODE). A
       constant HOLDING one (BLOCK = Probe::Block; BLOCK::CODE) is a receiver
       whose class only the value knows, so it takes the run-time read below --
       treating it as a path looked the leaf up and answered a top-level
       constant of that name (#4259). */
    if (par_nmc && par_tyc_k == NK_ConstantReadNode && an_const_read_is_value(c, par_idc))
      par_nmc = NULL;
    /* An ffi_const is parent-qualified; resolve it BEFORE the leaf-keyed
       plain-constant table, or a same-leaf plain constant in another module
       silently claims the reference (and its type). */
    if (par_nmc && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (sp_streq(c->ffi_consts[fci].mod, par_nmc) &&
            sp_streq(c->ffi_consts[fci].name, nm)) {
          buf_printf(b, "((sp_int)%d)", c->ffi_consts[fci].val);
          return 1;
        }
      }
    }
    /* a ::-scoped BUILTIN class is a first-class Class value (#2840) */
    if (par_nmc && nm) {
      char qbuf[160];
      snprintf(qbuf, sizeof qbuf, "%s::%s", par_nmc, nm);
      int qid = builtin_class_id(qbuf);
      if (qid != 0) {
        buf_printf(b, "((sp_Class){(sp_int)%d, SPL(\"%s\")})", qid, qbuf);
        return 1;
      }
      /* the Errno:: family (and its id-less siblings) is a name-backed
         Class value; raised exceptions carry this same qualified name */
      if (is_builtin_exception_name(qbuf)) {
        buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", qbuf);
        return 1;
      }
    }
    /* Errno::ENOENT::Errno: the class's number, from the runtime table
       (the numbers differ by platform) (#4560) */
    if (nm && sp_streq(nm, "Errno") && par_idc >= 0) {
      char pq[160];
      const char *pqn = isa_const_qualname(nt, par_idc, pq, sizeof pq);
      if (pqn && !strncmp(pqn, "Errno::", 7) && is_builtin_exception_name(pqn)) {
        buf_printf(b, "sp_errno_num(\"%s\")", pqn);
        return 1;
      }
    }
    /* `klass::CODE` where the receiver is a VALUE rather than a written path:
       which constant it names is a run-time question, and answering it with
       the leaf-named one made two different receivers answer the same thing --
       the top-level CODE for both Probe::Block and Probe::Region (#4257). The
       constants themselves are already stored per owner (cst_Probe__Block__CODE
       and cst_Probe__Region__CODE both exist), so switch on the class the
       value carries and read the one that class owns. */
    { Repr prtr = repr_of(c, par_idc >= 0 ? par_idc : -1);
      TyKind prt = par_idc >= 0 ? prtr.as_ty : TY_UNKNOWN;
    if (nm && par_idc >= 0 && !par_nmc && (prt == TY_CLASS || prt == TY_POLY)) {
      /* A constant is stored under its OWNER's qualified spelling
         (Probe__Block__CODE) while the class's c_name is its leaf (Block), so
         the owner is found by asking each class whether a constant keyed
         "<...>__<c_name>__<nm>" or "<c_name>__<nm>" exists. */
      /* every class that owns one: a fixed 64 left the rest to the default
         arm, which raised NameError for a class that defines the constant */
      int *ccls = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
      const char **ckey = malloc(sizeof(char *) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
      if (!ccls || !ckey) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      int nc = 0;
      TyKind ct = TY_UNKNOWN; int uniform = 1;
      int *cands, ncands = comp_const_tail_cands(c, nm, &cands);
      for (int k = 0; k < c->nclasses; k++) {
        int ci2 = comp_class_owned_const(c, k, nm, cands, ncands);
        if (ci2 < 0) continue;
        const char *found = c->consts[ci2].name; TyKind ft = c->consts[ci2].type;
        if (nc == 0) ct = ft;
        else if (ft != ct) uniform = 0;
        ccls[nc] = k; ckey[nc] = found; nc++;
      }
      free(cands);
      if (nc > 0 && uniform && ct != TY_UNKNOWN) {
        int tk = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ sp_Class _t%d = ", tk);
        /* a poly slot carries the class boxed; unwrap it to the same key */
        if (prtr.kind == RK_BOXED) { buf_puts(b, "sp_unbox_class("); emit_boxed(c, par_idc, b); buf_puts(b, ")"); }
        else emit_expr(c, par_idc, b);
        buf_puts(b, "; ");
        emit_ctype(c, ct, b);
        buf_printf(b, " %s_t%d = %s; switch (_t%d.cls_id) {", slot ? "*" : "", tr,
                   slot ? "NULL" : default_value_from_compiler(c, ct), tk);
        for (int i = 0; i < nc; i++) {
          /* a constant not assigned yet is as missing as one the class lacks */
          if (const_has_flag(comp_const(c, ckey[i])))
            buf_printf(b, " case %d: if (!cst_%s__set) sp_raise_cls(\"NameError\", sp_sprintf("
                          "\"uninitialized constant %%s::%s\", %s(_t%d)));"
                          " _t%d = %scst_%s; break;", ccls[i], ckey[i], nm, comp_class_display_fn(c), tk, tr, slot ? "&" : "", ckey[i]);
          else
            buf_printf(b, " case %d: _t%d = %scst_%s; break;", ccls[i], tr, slot ? "&" : "", ckey[i]);
        }
        /* A class with no such constant is CRuby's NameError, not the
           leaf-named constant of some unrelated scope. */
        buf_printf(b, " default: sp_raise_cls(\"NameError\", sp_sprintf("
                      "\"uninitialized constant %%s::%s\", sp_class_to_s(_t%d))); break;",
                   nm, tk);
        buf_printf(b, " } _t%d; })", tr);
        free(ccls); free(ckey);
        return 1;
      }
      free(ccls); free(ckey);
    } }
    LocalVar *cpcv = nm && !const_path_in_closed_builtin(c, id) && !const_path_misses_toplevel(c, id) ? comp_const(c, nm) : NULL;
    /* the slot of a flagged constant already read through its receiver (emit_const_recv_wrap) */
    int cptr = cpcv ? (int)nt_int(nt, id, "const_ptr", 0) : 0;
    if (cptr) {
      char sref[40]; snprintf(sref, sizeof sref, "(*_p%d)", cptr);
      if (slot) buf_printf(b, "_p%d", cptr);
      else if (repr_of_slot(c, cpcv).kind == RK_STRBUF) emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
      else buf_puts(b, sref);
      return 1;
    }
    /* --share-strings: a constant holding the shared handle (#6765) */
    if (cpcv && !slot && repr_of_slot(c, cpcv).kind == RK_STRBUF) {
      char sref[256]; snprintf(sref, sizeof sref, "cst_%s", nm);
      emit_const_read_open(c, id, cpcv, b);
      emit_strbuf_slot_read(c, id, repr_of(c, id), sref, b);
      emit_const_read_close(c, id, cpcv, b);
      return 1;
    }
    if (cpcv && cpcv->type != TY_UNKNOWN) { emit_const_slot_read(c, id, cpcv, nm, slot, 1, b); return 1; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return 1; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return 1; }
    /* well-known module constants */
    if (par_nmc && sp_streq(par_nmc, "Float") && nm) {
      if (sp_streq(nm, "MAX"))      { buf_puts(b, "DBL_MAX"); return 1; }
      if (sp_streq(nm, "MIN"))      { buf_puts(b, "DBL_MIN"); return 1; }
      if (sp_streq(nm, "EPSILON"))  { buf_puts(b, "DBL_EPSILON"); return 1; }
      if (sp_streq(nm, "INFINITY")) { buf_puts(b, "(1.0/0.0)"); return 1; }
      if (sp_streq(nm, "NAN"))      { buf_puts(b, "(0.0/0.0)"); return 1; }
      /* DIG/MANT_DIG/RADIX and the exponent limits are Integer constants */
      if (sp_streq(nm, "DIG"))       { buf_printf(b, "((sp_int)DBL_DIG)"); return 1; }
      if (sp_streq(nm, "MANT_DIG"))  { buf_printf(b, "((sp_int)DBL_MANT_DIG)"); return 1; }
      if (sp_streq(nm, "RADIX"))     { buf_printf(b, "((sp_int)FLT_RADIX)"); return 1; }
      if (sp_streq(nm, "MAX_EXP"))    { buf_printf(b, "((sp_int)DBL_MAX_EXP)"); return 1; }
      if (sp_streq(nm, "MIN_EXP"))    { buf_printf(b, "((sp_int)DBL_MIN_EXP)"); return 1; }
      if (sp_streq(nm, "MAX_10_EXP")) { buf_printf(b, "((sp_int)DBL_MAX_10_EXP)"); return 1; }
      if (sp_streq(nm, "MIN_10_EXP")) { buf_printf(b, "((sp_int)DBL_MIN_10_EXP)"); return 1; }
    }
    if (par_nmc && sp_streq(par_nmc, "Math") && nm) {
      if (sp_streq(nm, "PI")) { buf_puts(b, "M_PI"); return 1; }
      if (sp_streq(nm, "E"))  { buf_puts(b, "M_E"); return 1; }
    }
    /* Regexp option constants: CRuby's public bits (IGNORECASE=1, EXTENDED=2,
       MULTILINE=4) the same integers Regexp.new's option arg accepts, and the
       encoding bits Regexp#options reports (FIXEDENCODING=16, NOENCODING=32). */
    if (par_nmc && sp_streq(par_nmc, "Regexp") && nm) {
      if (sp_streq(nm, "IGNORECASE")) { buf_puts(b, "((sp_int)1)"); return 1; }
      if (sp_streq(nm, "EXTENDED"))   { buf_puts(b, "((sp_int)2)"); return 1; }
      if (sp_streq(nm, "MULTILINE"))  { buf_puts(b, "((sp_int)4)"); return 1; }
      if (sp_streq(nm, "FIXEDENCODING")) { buf_puts(b, "((sp_int)16)"); return 1; }
      if (sp_streq(nm, "NOENCODING"))    { buf_puts(b, "((sp_int)32)"); return 1; }
    }
    /* well-known Encoding constants -> the matching boxed encoding value.
       Spinel has one internal representation (UTF-8 / ASCII-8BIT), so the
       many aliases map onto those two; enough for the pervasive
       `str.encoding == Encoding::UTF_8` comparison. */
    if (par_nmc && sp_streq(par_nmc, "Encoding") && nm) {
      if (sp_streq(nm, "UTF_8") || sp_streq(nm, "UTF8")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_utf8())"); return 1;
      }
      if (sp_streq(nm, "US_ASCII") || sp_streq(nm, "ASCII") || sp_streq(nm, "ANSI_X3_4_1968")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_us_ascii())"); return 1;
      }
      if (sp_streq(nm, "BINARY") || sp_streq(nm, "ASCII_8BIT")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_binary())"); return 1;
      }
      /* The Unicode version the runtime's tables follow (lib/regexp/unicase.h
         is generated from Unicode 17.0.0), a String as in CRuby */
      if (sp_streq(nm, "UNICODE_VERSION")) { emit_str_literal(b, "17.0.0"); return 1; }
      /* Every other Encoding constant is a named Encoding value the runtime
         does not transcode to or from (String#encode leaves the bytes alone
         for it), under the name CRuby gives the encoding the constant names.
         A name that is no encoding's is left undefined, as in CRuby. */
      const char *ename = encoding_const_name(nm);
      if (ename) {
        buf_puts(b, "sp_box_encoding((sp_Encoding){");
        emit_str_literal(b, ename);   /* marker-framed, like every literal */
        buf_puts(b, "})");
        return 1;
      }
    }
    if (par_nmc && sp_streq(par_nmc, "File") && nm) {
      /* Frozen Strings, as CRuby's are: framed as a frozen literal is
         (emit_str_literal_src), whose header frozen? reads. A bare literal
         answered false. */
      if (sp_streq(nm, "SEPARATOR"))      { emit_str_literal_src(b, "/", 1, 1); return 1; }
      if (sp_streq(nm, "PATH_SEPARATOR")) { emit_str_literal_src(b, ":", 1, 1); return 1; }
      if (sp_streq(nm, "ALT_SEPARATOR"))  { buf_puts(b, "((const char *)0)"); return 1; }  /* nil off Windows (#2781) */
      /* the null device, the name Process.spawn/File.open take to discard a
         stream. Windows is not a target, so it is /dev/null (#4284). */
      if (sp_streq(nm, "NULL"))           { emit_str_literal_src(b, "/dev/null", 9, 1); return 1; }
      /* the open(2) flag constants, via the C macros (#2788) */
      if (sp_streq(nm, "RDONLY"))   { buf_puts(b, "((sp_int)O_RDONLY)"); return 1; }
      if (sp_streq(nm, "WRONLY"))   { buf_puts(b, "((sp_int)O_WRONLY)"); return 1; }
      if (sp_streq(nm, "RDWR"))     { buf_puts(b, "((sp_int)O_RDWR)"); return 1; }
      if (sp_streq(nm, "CREAT"))    { buf_puts(b, "((sp_int)O_CREAT)"); return 1; }
      if (sp_streq(nm, "EXCL"))     { buf_puts(b, "((sp_int)O_EXCL)"); return 1; }
      if (sp_streq(nm, "TRUNC"))    { buf_puts(b, "((sp_int)O_TRUNC)"); return 1; }
      if (sp_streq(nm, "APPEND"))   { buf_puts(b, "((sp_int)O_APPEND)"); return 1; }
      if (sp_streq(nm, "NONBLOCK")) { buf_puts(b, "((sp_int)O_NONBLOCK)"); return 1; }
      if (sp_streq(nm, "NOFOLLOW")) { buf_puts(b, "((sp_int)O_NOFOLLOW)"); return 1; }
      if (sp_streq(nm, "NOCTTY"))   { buf_puts(b, "((sp_int)O_NOCTTY)"); return 1; }
      if (sp_streq(nm, "SYNC"))     { buf_puts(b, "((sp_int)O_SYNC)"); return 1; }
      if (sp_streq(nm, "DSYNC"))    { buf_puts(b, "((sp_int)O_DSYNC)"); return 1; }
      if (sp_streq(nm, "BINARY"))   { buf_puts(b, "((sp_int)0)"); return 1; }
      /* the flock(2) operation constants (#2808) */
      if (sp_streq(nm, "LOCK_SH")) { buf_puts(b, "((sp_int)LOCK_SH)"); return 1; }
      if (sp_streq(nm, "LOCK_EX")) { buf_puts(b, "((sp_int)LOCK_EX)"); return 1; }
      if (sp_streq(nm, "LOCK_UN")) { buf_puts(b, "((sp_int)LOCK_UN)"); return 1; }
      if (sp_streq(nm, "LOCK_NB")) { buf_puts(b, "((sp_int)LOCK_NB)"); return 1; }
      /* File.fnmatch's flags: CRuby's own values (SP_FNM_*, which
         sp_file_fnmatch reads), not the system <fnmatch.h> macros.
         SYSCASE and SHORTNAME are 0 off Windows, as in CRuby. */
      if (sp_streq(nm, "FNM_NOESCAPE"))  { buf_puts(b, "((sp_int)SP_FNM_NOESCAPE)"); return 1; }
      if (sp_streq(nm, "FNM_PATHNAME"))  { buf_puts(b, "((sp_int)SP_FNM_PATHNAME)"); return 1; }
      if (sp_streq(nm, "FNM_DOTMATCH"))  { buf_puts(b, "((sp_int)SP_FNM_DOTMATCH)"); return 1; }
      if (sp_streq(nm, "FNM_CASEFOLD"))  { buf_puts(b, "((sp_int)SP_FNM_CASEFOLD)"); return 1; }
      if (sp_streq(nm, "FNM_EXTGLOB"))   { buf_puts(b, "((sp_int)SP_FNM_EXTGLOB)"); return 1; }
      if (sp_streq(nm, "FNM_SYSCASE"))   { buf_puts(b, "((sp_int)0)"); return 1; }
      if (sp_streq(nm, "FNM_SHORTNAME")) { buf_puts(b, "((sp_int)0)"); return 1; }
    }
    if (par_nmc && (is_io_class_name(par_nmc)) && nm) {
      /* IO#seek whence constants (File inherits them from IO); the Ruby
         values 0/1/2 are what sp_File_seek expects. */
      if (sp_streq(nm, "SEEK_SET")) { buf_puts(b, "((sp_int)0)"); return 1; }
      if (sp_streq(nm, "SEEK_CUR")) { buf_puts(b, "((sp_int)1)"); return 1; }
      if (sp_streq(nm, "SEEK_END")) { buf_puts(b, "((sp_int)2)"); return 1; }
    }
    if (par_nmc && sp_streq(par_nmc, "Process") && nm) {
      if (sp_streq(nm, "CLOCK_MONOTONIC")) { buf_puts(b, "((sp_int)CLOCK_MONOTONIC)"); return 1; }
      if (sp_streq(nm, "CLOCK_REALTIME"))  { buf_puts(b, "((sp_int)CLOCK_REALTIME)"); return 1; }
      /* the CPU-time clocks are POSIX and present on Linux and macOS; emit the
         C macro so the value is the platform's own clock id, as CRuby's is. */
      if (sp_streq(nm, "CLOCK_PROCESS_CPUTIME_ID")) { buf_puts(b, "((sp_int)CLOCK_PROCESS_CPUTIME_ID)"); return 1; }
      if (sp_streq(nm, "CLOCK_THREAD_CPUTIME_ID"))  { buf_puts(b, "((sp_int)CLOCK_THREAD_CPUTIME_ID)"); return 1; }
      /* getpriority/setpriority `which` selectors (#3046) */
      if (sp_streq(nm, "PRIO_PROCESS")) { buf_puts(b, "((sp_int)PRIO_PROCESS)"); return 1; }
      if (sp_streq(nm, "PRIO_PGRP"))    { buf_puts(b, "((sp_int)PRIO_PGRP)"); return 1; }
      if (sp_streq(nm, "PRIO_USER"))    { buf_puts(b, "((sp_int)PRIO_USER)"); return 1; }
    }
    if (par_nmc && sp_streq(par_nmc, "Integer") && nm &&
        (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) {
      /* Integer::MAX/MIN do not exist in Ruby -- raise NameError at runtime */
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant Integer::%s\"), 0)", nm);
      return 1;
    }
    /* Socket::<CONST>: the value is platform-dependent, so the runtime (where
       the system headers are in scope) resolves it by name. */
    if (par_nmc && nm && sp_streq(par_nmc, "Socket") && sp_feature_required("socket")) {
      buf_printf(b, "({ sp_int _sc = sp_sock_const(\"%s\");"
                    " if (_sc < 0) sp_raise_cls(\"NameError\","
                    " \"uninitialized constant Socket::%s\"); _sc; })", nm, nm);
      return 1;
    }
    /* class/module constant as value: one its parent holds (a top-level
       class is no member of another class or module, #10577) */
    if (nm) {
      int _cpidx = const_node_class_index(c, id);
      if (_cpidx >= 0) {
        emit_const_read_open(c, id, NULL, b);
        buf_printf(b, "((sp_Class){%d})", _cpidx);
        emit_const_read_close(c, id, NULL, b);
        return 1;
      }
      int _bcpid = builtin_class_id(nm);
      if (_bcpid != 0) { buf_printf(b, "((sp_Class){%d})", _bcpid); return 1; }
    }
    /* A qualified constant defined nowhere: the same build-time answer as the
       bare form above (#3976). */
    {
      char fullname[512];
      /* CRuby qualifies the name by a NAMED module (`M::Missing`) but not by
         Object, whose constants are the top-level ones (#3976). The written
         path is what goes in the message; see const_path_written. */
      const_path_written(nt, id, fullname, sizeof fullname);
      if (!fullname[0]) {
        if (par_nmc && nm && !sp_streq(par_nmc, "Object"))
          snprintf(fullname, sizeof fullname, "%s::%s", par_nmc, nm);
        else if (nm) snprintf(fullname, sizeof fullname, "%s", nm);
        else snprintf(fullname, sizeof fullname, "?");
      }
      /* a builtin class with no cls_id that only a path names
         (Enumerator::Chain): the name-backed value the bare form above
         gives Random or Process */
      if (is_builtin_class_name(fullname) || is_builtin_module_name(fullname)) {
        buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", fullname);
        return 1;
      }
      if (!const_ref_is_rescued(c, id) && !const_ref_is_guarded(c, id))
        warn_undefined_constant(c, id, fullname);
      /* CRuby resolves the path left to right and names the first segment
         it does not find: `Mylib::VERSION` with no Mylib raises
         "uninitialized constant Mylib" */
      { int segs[32], ns = 0;
        for (int s = nt_ref(nt, id, "parent"); s >= 0 && ns < 32 &&
             (nt_kind_is_constant(nt_kind(nt, s)));
             s = nt_kind(nt, s) == NK_ConstantPathNode ? nt_ref(nt, s, "parent") : -1)
          segs[ns++] = s;
        for (int i = ns - 1; i >= 0; i--)
          if (!const_seg_resolves(c, segs[i])) {
            char miss[512];
            const_path_written(nt, segs[i], miss, sizeof miss);
            if (miss[0]) snprintf(fullname, sizeof fullname, "%s", miss);
            break;
          } }
      /* a class scope reaches Object's constants: a registration there runs
         its file before the NameError */
      if (an_const_read_effect(c, id).autoload) {
        buf_puts(checks, checks->len ? "" : "(");
        emit_const_read_open(c, id, NULL, checks);
        buf_puts(checks, "0");
        emit_const_read_close(c, id, NULL, checks);
        buf_puts(checks, ", ");
      }
      emit_const_undefined_raise(b, checks->p, fullname);
      checks->len = 0;
    }
    return 1;
  }
  return 0;
}

/* Select the persistent slot with the same lookup and guards as a read. */
/* See codegen_internal.h. The read is emitted aside and dropped: only
   whether it is the NameError's raise is kept (a read that resolves
   leaves no trace, its temporaries' numbers put back). */
int const_read_raises(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  if (!nt_kind_is_constant(k) || !comp_const_undefined(c, id)) return 0;
  Buf t, pre;
  memset(&t, 0, sizeof t); memset(&pre, 0, sizeof pre);
  Buf *saved_pre = g_pre; int saved_tmp = g_tmp;
  g_pre = &pre;
  emit_expr(c, id, &t);
  g_pre = saved_pre; g_tmp = saved_tmp;
  static const char raise[] = "(sp_raise_cls(\"NameError\"";
  int r = !pre.p && t.p && !strncmp(t.p, raise, sizeof raise - 1);
  free(t.p); free(pre.p);
  return r;
}

static int emit_constant_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty, int slot) {
  if (!nt_kind_is_constant(nt_kind(nt, id))) return 0;
  int par = nt_kind(nt, id) == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1;
  int dynamic = nt_kind(nt, par) == NK_ConstantReadNode && an_const_read_is_value(c, par);
  Buf pre = {0};
  int effect = !dynamic && !view_const_read_done(c, id) ? emit_const_read_prefix(c, par, &pre) : 0;
  if (!effect) return emit_constant_value(c, id, b, &pre, nt, ty, slot);
  Buf v = {0};
  int result = emit_constant_value(c, id, &v, &pre, nt, ty, slot);
  /* checks no raise took lead the value; a raise that took them is the value */
  if (pre.len) buf_printf(b, "%s%s)", pre.p, v.p ? v.p : "");
  else buf_puts(b, v.p ? v.p : "");
  free(pre.p); free(v.p);
  return result;
}

void emit_constant_slot(Compiler *c, int id, Buf *b) {
  emit_constant_expr(c, id, b, c->nt, nt_type(c->nt, id), 1);
}

/* defined?(super): 1 where the super of its method reaches a method, 0
   where none does, -1 where the answer is not known here. The program's
   methods are looked up as emit_super looks; a builtin's from CRuby's table
   of the core classes (refl_builtin_vis), a class method's from Class's or
   Module's, which answer for every class or module whose singleton class has
   no builtin method of its own above it. */
static int defined_super_reaches(Compiler *c, int sup) {
  Scope *s = comp_scope_of(c, sup);
  if (!s || !s->name) return 0;
  if (s->class_id < 0) return -1;
  const ClassInfo *ci = &c->classes[s->class_id];
  int mod = comp_class_is_module(c, (ClassInfo *)ci);
  if (mod && !s->is_cmethod) return -1;   /* its includer's chain decides */
  if (comp_super_shadow(c, s)) return 1;
  int p = comp_super_parent(c, s->class_id, s->is_cmethod);
  const char *u = comp_super_name(c, p, comp_scope_def_name(c, s), s->is_cmethod);
  if (!u) return -1;
  if (s->is_cmethod) {
    if (p >= 0 && comp_cmethod_in_chain(c, p, u, NULL) >= 0) return 1;
    if (core_method_vis(mod ? "Module" : "Class", u, 1) > 0) return 1;
    int root = s->class_id;
    while (c->classes[root].parent >= 0) root = c->classes[root].parent;
    const ClassInfo *ri = &c->classes[root];
    int plain = mod || (!ri->is_struct && !ri->is_builtin_const &&
                        (ri->def_node < 0 || nt_ref(c->nt, ri->def_node, "superclass") < 0));
    return plain ? 0 : -1;
  }
  char base[256];
  int setter = setter_base_name(u, base, sizeof base);
  /* an ancestor that undefs the name ahead of any definition: the super
     reaches nothing (`undef :test` in the superclass) */
  if (p >= 0 && comp_is_undeffed_in_chain(c, p, u)) return 0;
  if (p >= 0 && (comp_method_in_chain(c, p, u, NULL) >= 0 || comp_reader_in_chain(c, p, u, NULL) ||
                 (setter && comp_writer_in_chain(c, p, base, NULL))))
    return 1;
  int v = refl_builtin_vis(c, s->class_id, u, 2);
  return v < 0 ? -1 : v > 0;
}

/* A defined?(...) expression (emit_expr_node's arms, in their order) */
static int emit_defined_value(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty);
static int emit_defined_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  int v = nt_kind(nt, id) == NK_DefinedNode ? nt_ref(nt, id, "value") : -1;
  int par = c->nautoloads && v >= 0 && nt_kind(nt, v) == NK_ConstantPathNode ? nt_ref(nt, v, "parent") : -1;
  size_t mark = b->len;
  if (par < 0 || !emit_const_autoload_run(c, par, b)) return emit_defined_value(c, id, b, nt, ty);
  int result = emit_defined_value(c, id, b, nt, ty);
  if (result) buf_puts(b, ")");
  else buf_erase(b, mark, b->len - mark);
  return result;
}
static int emit_defined_value(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (!(sp_streq(ty, "DefinedNode"))) return 0;
  /* defined? examines its argument recursively without evaluating: any
     unresolvable constant anywhere in the subtree makes the whole answer
     nil (CRuby re-checks each reference). */
  /* compile-time defined? -> a label string, or nil (NULL) when undefined */
  int v = nt_ref(nt, id, "value");
  const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
  NodeKind vk = nt_kind(nt, v);
  ConstReadEffect effect = an_const_read_effect(c, v);
  const char *res = NULL;
  if (vt) {
    if (vk == NK_LocalVariableReadNode) res = "local-variable";
    else if (vk == NK_InstanceVariableReadNode) {
      /* Return "instance-variable" only when the ivar is known to be assigned. */
      const char *inm = nt_str(nt, v, "name");
      /* an attr_writer or attr_accessor of self's class or a superclass
         writes it too (a Struct member is no ivar) */
      Scope *ws = comp_scope_of(c, v);
      for (int k = ws && !ws->is_cmethod ? ws->class_id : -1; k >= 0 && inm && !res; k = c->classes[k].parent) {
        ClassInfo *wi = &c->classes[k];
        int mi = comp_ivar_index(wi, inm);
        if (comp_is_writer(wi, inm + 1) && !((wi->is_struct || wi->is_data) && mi >= 0 && mi < wi->nmembers))
          res = "instance-variable";
      }
      for (int kk = 0; kk < nt->count && !res; kk++) {
        NodeKind kt_k = nt_kind(nt, kk);
        if (kt_k == NK_InstanceVariableWriteNode &&
            inm && nt_str(nt, kk, "name") && sp_streq(nt_str(nt, kk, "name"), inm))
          res = "instance-variable";
        /* a reflective `instance_variable_set(:@x, v)` writes it too */
        if (kt_k == NK_CallNode && inm && nt_str(nt, kk, "name") &&
            sp_streq(nt_str(nt, kk, "name"), "instance_variable_set")) {
          int sa = nt_ref(nt, kk, "arguments"), san = 0;
          const int *sav = sa >= 0 ? nt_arr(nt, sa, "arguments", &san) : NULL;
          const char *sn = NULL;
          if (san == 2 && nt_kind(nt, sav[0]) == NK_SymbolNode) sn = nt_str(nt, sav[0], "value");
          else if (san == 2 && nt_kind(nt, sav[0]) == NK_StringNode) sn = nt_str(nt, sav[0], "content");
          if (sn && sp_streq(sn, inm)) res = "instance-variable";
        }
      }
      /* a class-level ivar (a class method's, a class body's, the top
         level's) is its civ_ slot, which starts nil and can be stored nil:
         assigned or not is its set flag, asked at run time */
      Scope *ds = comp_scope_of(c, v);
      int gcid = ds ? civ_class_of(c, v, ds) : -1;
      if (gcid >= 0 && inm && comp_ivar_index(&c->classes[gcid], inm) >= 0 && civ_defined_probed(c, inm)) {
        buf_printf(b, "(civ_%s_%s__set ? SPL(\"instance-variable\") : NULL)", c->classes[gcid].name, iv_c(inm + 1));
        return 1;
      }
      /* in an instance method, a slot nothing has set yet is not defined
         (ivar_set_kind): asked of self at run time */
      int dcid = ds && !ds->is_cmethod && ds->class_id >= 0 && g_ie_class_id < 0 ? ds->class_id : -1;
      if (res && dcid >= 0 && comp_ivar_index(&c->classes[dcid], inm) >= 0 &&
          ((ivar_set_kind(c, dcid, inm) & 1) ||
           poly_ivar_unset_marked(c, dcid, comp_ivar_index(&c->classes[dcid], inm)))) {
        char ex[200], tb[300];
        snprintf(ex, sizeof ex, "%s%siv_%s", g_self, g_self_deref, iv_c(inm + 1));
        buf_printf(b, "(%s ? SPL(\"instance-variable\") : NULL)", ivar_set_test(c, dcid, inm, ex, tb, sizeof tb));
        return 1;
      }
    }
    else if (vk == NK_ClassVariableReadNode) {
      /* set or not is a run-time question: the cvar's __set flag */
      HolderRef h;
      Buf ref; memset(&ref, 0, sizeof ref);
      if (holder_of_node_in(c, v, g_class_body_id, &h) && h.idx >= 0 && holder_slot_buf(c, &h, &ref))
        buf_printf(b, "(%s__set ? SPL(\"class variable\") : NULL)", ref.p);
      else buf_puts(b, "NULL");
      free(ref.p);
      return 1;
    }
    else if (vk == NK_SelfNode) res = "self";
    else if (vk == NK_NilNode) res = "nil";
    else if (vk == NK_TrueNode) res = "true";
    else if (vk == NK_FalseNode) res = "false";
    else if (vk == NK_IntegerNode || vk == NK_FloatNode ||
             vk == NK_StringNode || vk == NK_SymbolNode || vk == NK_ArrayNode ||
             /* a backtick literal, also as the call desugar_user_backtick made of it */
             vk == NK_XStringNode || vk == NK_InterpolatedXStringNode ||
             (vk == NK_CallNode && nt_int(nt, v, "xstr_call", 0))) res = "expression";
    else if (vk == NK_GlobalVariableReadNode) {
      const char *gn = nt_str(nt, v, "name");
      /* runtime-provided special globals exist regardless of writes */
      if (gn && gn[0] == '$' && gn[1] && (!gn[2] || sp_streq(gn + 1, "stdin") ||
          sp_streq(gn + 1, "stdout") || sp_streq(gn + 1, "stderr") ||
          sp_streq(gn + 1, "PROGRAM_NAME"))) {
        const char sg = gn[1];
        if (!gn[2] && (sg == '!' || sg == '~' || sg == '0' || sg == '$' ||
                       sg == '?' || sg == ';' || sg == ',' || sg == '/' ||
                       sg == '\\' || sg == '*' || sg == '&' ||
                       sg == '\'' || sg == '`' || sg == '+'))
          res = "global-variable";
        else if (gn[2]) res = "global-variable";
      }
      if (is_load_path_global(gn) || is_loaded_features_global(gn)) res = "global-variable";
      for (int kk = 0; kk < nt->count && !res; kk++) {
        NodeKind kt_k = nt_kind(nt, kk);
        if ((kt_k == NK_GlobalVariableWriteNode || kt_k == NK_GlobalVariableOperatorWriteNode) &&
            gn && nt_str(nt, kk, "name") && sp_streq(nt_str(nt, kk, "name"), gn))
          res = "global-variable";
      }
    }
    else if (nt_kind_is_constant(vk) &&
             (effect.autoload || effect.class_presence)) {
      int reads[16], n = comp_defined_reads(c, id, reads, 16);
      buf_puts(b, "(");
      for (int k = 0; k < n; k++) {
        ConstReadEffect ce = an_const_read_effect(c, reads[k]);
        if (k) buf_puts(b, " && ");
        buf_puts(b, "(");
        if (ce.presence) buf_printf(b, "cst_%s__set", ce.presence->name);
        else if (ce.class_presence) buf_printf(b, "sp_class_present_%d", ce.class_presence - 1);
        else buf_puts(b, "0");
        if (ce.autoload) buf_printf(b, " || sp_auto_%d_registered", ce.autoload);
        buf_puts(b, ")");
      }
      if (n < 1) buf_puts(b, "0");
      buf_puts(b, " ? SPL(\"constant\") : NULL)");
      return 1;
    }
    else if (vk == NK_ConstantReadNode) {
      const char *cn = nt_str(nt, v, "name");
      /* a constant not assigned yet is not defined: asked at run time */
      if (cn && const_has_flag(comp_const(c, cn))) {
        buf_printf(b, "(cst_%s__set ? SPL(\"constant\") : NULL)", cn);
        return 1;
      }
      if (cn) {
        if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) res = "constant";
        if (!res && comp_is_wellknown_const(cn)) res = "constant";
        /* exception classes are constants too (#2767) */
        if (!res && (is_builtin_exception_name(cn) || is_builtin_class_name(cn)))
          res = "constant";
      }
    }
    else if (vk == NK_ConstantPathNode && nt_kind(nt, nt_ref(nt, v, "parent")) == NK_ConstantReadNode &&
             sp_streq(nt_str(nt, nt_ref(nt, v, "parent"), "name"), "Socket") && sp_feature_required("socket")) {
      /* Socket's constants are the system's: the run time asks its table */
      buf_printf(b, "(sp_sock_const(\"%s\") >= 0 ? SPL(\"constant\") : NULL)", nt_str(nt, v, "name"));
      return 1;
    }
    else if (vk == NK_ConstantPathNode) {
      /* a fully-resolved qualified path answers "constant"; any unresolved
         segment leaves nil (the segment walk lives in the guard helpers). A
         segment not assigned yet is not defined: asked at run time */
      const char *early[8];
      int ne = comp_defined_early(c, id, early, 8), nf = 0;
      for (int k = 0; k < ne; k++) if (const_has_flag(comp_const(c, early[k]))) nf++;
      if (nf > 0) {
        buf_puts(b, "(");
        for (int k = 0; k < ne; k++)
          if (const_has_flag(comp_const(c, early[k]))) buf_printf(b, "cst_%s__set && ", early[k]);
        buf_puts(b, "1 ? SPL(\"constant\") : NULL)");
        return 1;
      }
      if (ne >= 0) res = "constant";
    }
    /* `sym.to_proc` was lowered to a lambda before this point (the
       "stp_arity" mark): it is still the Symbol's method, not an iterator */
    else if (vk == NK_CallNode && nt_int(nt, v, "stp_arity", 0))
      res = "method";
    /* an iterator is an expression, whatever it calls */
    else if (vk == NK_CallNode && nt_kind(nt, nt_ref(nt, v, "block")) == NK_BlockNode)
      res = "expression";
    /* see desugar_defined_method_call; a guard known nil at compile time
       leaves the receiver unevaluated. A respond_to? left untyped (an IO
       handle's, which spinel refuses to answer) keeps the answer nil. */
    else if (vk == NK_CallNode && nt_ref(nt, id, "method_cond") >= 0 &&
             comp_ntype(c, nt_ref(nt, id, "method_cond")) == TY_BOOL) {
      int ng = 0; const int *gs = nt_arr(nt, id, "method_guards", &ng);
      Buf g; memset(&g, 0, sizeof g);
      for (int gi = 0; gi < ng; gi++) {
        Buf gb; memset(&gb, 0, sizeof gb);
        emit_expr(c, gs[gi], &gb);
        if (sp_streq(gb.p, "NULL")) { free(gb.p); free(g.p); buf_puts(b, "NULL"); return 1; }
        buf_printf(&g, "(%s) && ", gb.p);
        free(gb.p);
      }
      buf_printf(b, "(%s(", g.p ? g.p : "");
      emit_cond(c, nt_ref(nt, id, "method_cond"), b);
      buf_puts(b, ") ? SPL(\"method\") : NULL)");
      free(g.p);
      return 1;
    }
    else if (vk == NK_CallNode && nt_ref(nt, v, "receiver") < 0) {
      const char *cn = nt_str(nt, v, "name");
      if (cn && call_target(c, v, -1, cn, CT_TOP, NULL) >= 0) res = "method";
      /* an implicit-self call inside a class resolves through the
         enclosing class's method chain (readers included) */
      else if (cn && comp_scope_of(c, id) && comp_scope_of(c, id)->class_id >= 0 &&
               (call_target(c, v, comp_scope_of(c, id)->class_id, cn, 0, NULL) >= 0 ||
                comp_reader_in_chain(c, comp_scope_of(c, id)->class_id, cn, NULL)))
        res = "method";
      else if (cn && defined_bare_builtin(c, id, cn)) res = "method";
      /* builtin kernel functions the compiler always resolves */
      else if (cn) {
        static const char *const kfns[] = {
          "puts", "print", "p", "pp", "require", "require_relative", "raise",
          "loop", "lambda", "proc", "rand", "srand", "gets", "sleep", "exit",
          "format", "sprintf", "printf", "at_exit", "catch", "throw", NULL };
        for (int bi = 0; kfns[bi]; bi++)
          if (sp_streq(cn, kfns[bi])) { res = "method"; break; }
      }
    }
    /* An assignment of any kind (local/ivar/gvar/cvar/constant/index/attr,
       plain or operator) answers "assignment" without evaluating. */
    else if (strstr(vt, "WriteNode") || vk == NK_MultiWriteNode)
      res = "assignment";
    /* `defined?(())`: the empty parentheses are nil's own word */
    else if (vk == NK_ParenthesesNode && nt_ref(nt, v, "body") < 0)
      res = "nil";
    /* A composite expression answers "expression" even when its OPERANDS
       are undefined -- defined? never evaluates its argument. */
    else if (vk == NK_AndNode || vk == NK_OrNode ||
             vk == NK_RangeNode ||
             vk == NK_HashNode || vk == NK_KeywordHashNode ||
             vk == NK_InterpolatedStringNode ||
             vk == NK_RegularExpressionNode ||
             vk == NK_LambdaNode || vk == NK_IfNode ||
             vk == NK_UnlessNode || vk == NK_CaseNode ||
             vk == NK_DefinedNode || vk == NK_BeginNode ||
             vk == NK_ParenthesesNode ||
             vk == NK_InterpolatedRegularExpressionNode ||
             vk == NK_SourceFileNode || vk == NK_SourceLineNode ||
             vk == NK_SourceEncodingNode || vk == NK_ForNode ||
             vk == NK_WhileNode || vk == NK_UntilNode)
      res = "expression";
  }
  /* a CONTAINER literal builds its elements, so an unresolvable constant
     anywhere inside nils the answer; short-circuiting forms (&&/||) do
     not examine their operands (CRuby). */
  if (res && v >= 0 &&
      (vk == NK_ArrayNode || vk == NK_HashNode ||
       vk == NK_KeywordHashNode) &&
      subtree_has_unresolved_const(c, v)) res = NULL;
  /* dynamic cases: the answer depends on runtime state, so emit a
     conditional instead of a compile-time label. */
  if (!res && vk == NK_BackReferenceReadNode) {
    /* $& / $~ / $` / $' / $+ are defined only after a successful match;
       the backing slot is NULL before one. */
    buf_puts(b, "(sp_re_match_str ? SPL(\"global-variable\") : NULL)");
    return 1;
  }
  if (!res && vk == NK_NumberedReferenceReadNode) {
    int refn = (int)nt_int(nt, v, "number", 0);
    if (refn >= 1 && refn <= 9) {
      /* value reads use sp_re_captures[n] directly ($N = captures[N]) */
      buf_printf(b, "(sp_re_captures[%d] ? SPL(\"global-variable\") : NULL)", refn);
      return 1;
    }
  }
  /* defined?(yield) answers "yield" only when the current method actually
     received a block, else nil -- the same runtime question as block_given?.
     An inlined yielding scope statically has a block; a lowered scope tests
     its runtime __yblk__ parameter; any other scope has no block. */
  if (!res && (vk == NK_SuperNode || vk == NK_ForwardingSuperNode)) {
    int r = defined_super_reaches(c, v);
    if (r < 0) unsupported(c, id, "defined?(super) where the method super reaches is not known at compile time");
    buf_puts(b, r > 0 ? "SPL(\"super\")" : "NULL");
    return 1;
  }
  if (!res && vk == NK_YieldNode) {
    if (g_block_id >= 0) { buf_puts(b, "SPL(\"yield\")"); return 1; }
    if (g_current_scope_is_lowered) {
      buf_puts(b, "(");
      emit_yblk_ref(b);
      buf_puts(b, " != NULL ? SPL(\"yield\") : NULL)");
      return 1;
    }
    buf_puts(b, "NULL");
    return 1;
  }
  if (res) buf_printf(b, "SPL(\"%s\")", res);
  else buf_puts(b, "NULL");
  return 1;
  return 0;
}

/* --share-strings: does element v of a literal carry the handle of a String
   the rule shares: a variable's that holds it, or a call's whose every
   target publishes it? */
static int lit_elem_carries_handle(Compiler *c, int v) {
  v = nt_paren_single(c->nt, v);
  if (v < 0) return 0;
  if (nt_kind(c->nt, v) == NK_CallNode) return repr_call_returns_handle(c, v);
  return strbuf_var_handle(c, v, NULL);
}
/* --share-strings: is String Array literal els[0..n) one whose elements
   are read only once all of them ran: an element carrying a handle
   (lit_elem_carries_handle) before one that can change that String in place
   (operand_may_change_string) before anything reads the Array? Its bytes taken
   at the element, the Array kept the String as it was then. */
static int str_lit_reads_late(Compiler *c, const int *els, int n) {
  int carried = 0;
  for (int j = 0; j < n; j++) {
    if (nt_kind(c->nt, els[j]) == NK_SplatNode || nil_store_sfx(c, "Str", els[j])[0]) return 0;
    if (carried && operand_may_change_string(c, els[j])) return 1;
    carried |= lit_elem_carries_handle(c, els[j]);
  }
  return 0;
}
/* The pushes of such a literal into String Array _t<t>: each element in
   its order into a rooted temp, a handle-carrying one as its handle, then
   each pushed, a handle read as the String it holds once all ran. */
static void emit_str_lit_late(Compiler *c, const int *els, int n, int t) {
  int *tmp = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  char *hnd = malloc((size_t)(n > 0 ? n : 1));
  if (!tmp || !hnd) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int j = 0; j < n; j++) {
    Buf el; memset(&el, 0, sizeof el);
    tmp[j] = ++g_tmp;
    hnd[j] = (char)lit_elem_carries_handle(c, els[j]);
    /* (one representation: the element's text may be bytes whatever
       carries the handle, so the handle is taken through its face) */
    if (hnd[j]) emit_expr_face(c, els[j], TY_STRBUF, &el);
    else if (hnd[j]) emit_strbuf_handle_of(c, els[j], &el);
    else emit_coerce(c, els[j], TY_STRING, CO_HOLD, "an Array literal's element", &el);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = %s; SP_GC_ROOT%s(_t%d);\n", hnd[j] ? "sp_String *" : "const char *", tmp[j],
               el.p ? el.p : "NULL", hnd[j] ? "" : "_STR", tmp[j]);
    free(el.p);
  }
  for (int j = 0; j < n; j++) {
    emit_indent(g_pre, g_indent);
    if (hnd[j]) buf_printf(g_pre, "sp_StrArray_push(_t%d, _t%d ? sp_String_cstr(_t%d) : NULL);\n", t, tmp[j], tmp[j]);
    else buf_printf(g_pre, "sp_StrArray_push(_t%d, _t%d);\n", t, tmp[j]);
  }
  free(tmp);
  free(hnd);
}

/* An Array or Hash literal ([...], {...}, a keyword hash) (emit_expr_node's arms, in their order) */
static int emit_array_hash_literal_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (sp_streq(ty, "ArrayNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    Repr atr = repr_of(c, id);
    TyKind at = atr.as_ty;
    /* an empty `[]` literal carries no element type of its own; it is
       emitted via the target's type in emit_assign. If we reach here for
       an empty literal, use g_ret_type context (e.g. tail position in a
       poly_array-returning method) before falling back to int array. */
    if (n == 0 && atr.untyped && ty_is_array(g_ret_type)) at = g_ret_type;
    /* a table of Integer rows the analysis builds in place (a literal an
       iterator walks row by row, block_elem_ty) */
    if (n > 0 && ty_is_ptr_array(at) && emit_ptr_array_build(c, id, at, b)) return 1;
    const char *k = array_kind(at);
    if (n == 0 && !k && at != TY_POLY_ARRAY) { buf_puts(b, "sp_IntArray_new()"); return 1; }
    /* poly (mixed-element) array: build an sp_PolyArray of boxed elements */
    if (at == TY_POLY_ARRAY) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", t);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
      for (int j = 0; j < n; j++) {
        NodeKind ek = nt_kind(nt, els[j]);
        if (ek == NK_SplatNode) {
          /* [*arr] or [*range] -- expand into poly */
          int inner = nt_ref(nt, els[j], "expression");
          if (is_empty_array_lit(nt, inner)) continue;
          Repr ir = repr_of(c, inner);
          TyKind it = ir.as_ty;
          Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
          const char *ep = el.p ? el.p : "NULL";
          emit_indent(g_pre, g_indent);
          if (ir.range == TY_INT || ir.range == TY_STRING) {
            /* check if it's a string range (bounds are TY_STRING) */
            int rn = nt_kind(nt, inner) == NK_RangeNode ? inner : -1;
            int rlo = rn >= 0 ? nt_ref(nt, rn, "left") : -1;
            int rhi = rn >= 0 ? nt_ref(nt, rn, "right") : -1;
            /* the runtime tests the argument for truth; it has always been given Prism's bit */
            int rexcl = nt_range_excl(nt, rn) ? NT_RANGE_EXCLUDE_END : 0;
            if (rlo >= 0 && comp_ntype(c, rlo) == TY_STRING) {
              Buf lo_b; memset(&lo_b, 0, sizeof lo_b); emit_expr(c, rlo, &lo_b);
              Buf hi_b; memset(&hi_b, 0, sizeof hi_b); emit_expr(c, rhi, &hi_b);
              buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s); }\n",
                         lo_b.p ? lo_b.p : "NULL", hi_b.p ? hi_b.p : "NULL", rexcl, t, ONE_SA_BOX);
              free(lo_b.p); free(hi_b.p);
            }
            else if (ir.range == TY_STRING)
              /* a String Range held in a variable: its bounds are in the value */
              buf_printf(g_pre, "{ sp_StrArray *_sa = sp_srange_to_a(%s); if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s); }\n", ep, t, ONE_SA_BOX);
            else {
              buf_printf(g_pre, "{ sp_Range _sr = %s; sp_int _e = _sr.last+(_sr.excl?0:1); for (sp_int _si = _sr.first; _si < _e; _si++) sp_PolyArray_push(_t%d, sp_box_int(_si)); }\n", ep, t);
            }
          }
          else if (ir.elem == TY_INT) {
            /* an element that is nil (the array's nil bit) streams as nil */
            buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_oint(sp_IntArray_oget(_sa, _si))); }\n",
                       ep, t);
          }
          else if (ir.elem == TY_STRING)
            buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s); }\n", ep, t, ONE_SA_BOX);
          else if (ir.elem == TY_FLOAT) {
            buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_ofloat(sp_FloatArray_oget(_sa, _si))); }\n",
                       ep, t);
          }
          else if (ir.elem == TY_POLY)
            buf_printf(g_pre, "{ sp_PolyArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }\n", ep, t);
          else if (ir.kind == RK_BOXED)
            /* `*poly`: whether it holds an array is only known at runtime, so
               splice one level if it is an array, drop nil, else push as-is
               (CRuby splat semantics). A Hash, a Range and an Enumerator go
               through sp_splat_to_array for their pairs, members or items, and
               so does a program object when a class defines #to_a. */
            buf_printf(g_pre, "{ sp_RbVal _sv = %s; if (_sv.tag == SP_TAG_OBJ && (sp_poly_is_hash_kind(_sv.cls_id) || _sv.cls_id == SP_BUILTIN_RANGE || _sv.cls_id == SP_BUILTIN_STR_RANGE || _sv.cls_id == SP_BUILTIN_ENUMERATOR%s || ((_sv.cls_id == SP_BUILTIN_OBJECT || _sv.cls_id == SP_BUILTIN_BASIC_OBJECT) && _sv.v.p && ((sp_Object *)_sv.v.p)->singleton_to_a))) _sv = sp_splat_to_array(_sv); if (!sp_poly_nil_p(_sv)) sp_PolyArray_flatten_into_n(_t%d, _sv, 1); }\n",
                       ep, obj_to_a_any(c) ? " || _sv.cls_id >= 0" : "", t);
          else if (it == TY_ENUMERATOR) {
            /* an Enumerator spreads the items it yields */
            Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx);
            buf_printf(g_pre, "sp_PolyArray_flatten_into_n(_t%d, sp_splat_to_array(%s), 1);\n", t, bx.p ? bx.p : "sp_box_nil()");
            free(bx.p);
          }
          else if (it == TY_NIL)
            /* a statically-nil splat contributes nothing (`[*nil]` == []) */
            buf_printf(g_pre, ";\n");
          /* a nullable Integer or Float holding nil is that nil */
          else if ((it == TY_INT || it == TY_FLOAT) && call_returns_nullable_int(c, inner)) {
            Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx);
            buf_printf(g_pre, "{ sp_RbVal _sv = %s; if (_sv.tag != SP_TAG_NIL) sp_PolyArray_push(_t%d, _sv); }\n",
                       bx.p ? bx.p : "sp_box_nil()", t);
            free(bx.p);
          }
          else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx); buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", t, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
          free(el.p);
        }
else {
          Buf el; memset(&el, 0, sizeof el);
          /* (a published String before an element that can change it is
             boxed as its handle: emit_boxed_operand) */
          emit_boxed_operand(c, els, n, j, &el);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t);
          buf_puts(g_pre, el.p ? el.p : "");
          buf_puts(g_pre, ");\n");
          free(el.p);
        }
      }
      buf_printf(b, "_t%d", t);
      return 1;
    }
    if (!k) unsupported(c, id, "array literal (element type)");
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, t, k);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    if (sp_streq(k, "Str") && str_lit_reads_late(c, els, n)) {
      emit_str_lit_late(c, els, n, t);
      buf_printf(b, "_t%d", t);
      return 1;
    }
    for (int j = 0; j < n; j++) {
      NodeKind ek = nt_kind(nt, els[j]);
      if (ek == NK_SplatNode) {
        /* [*range] or [*arr] inside a typed array literal */
        int inner = nt_ref(nt, els[j], "expression");
        if (is_empty_array_lit(nt, inner)) continue;
        Repr ir = repr_of(c, inner);
        TyKind it = ir.as_ty;
        Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
        const char *ep = el.p ? el.p : "NULL";
        emit_indent(g_pre, g_indent);
        if (ir.range == TY_INT || ir.range == TY_STRING) {
          int rn2 = nt_kind(nt, inner) == NK_RangeNode ? inner : -1;
          int rlo2 = rn2 >= 0 ? nt_ref(nt, rn2, "left") : -1;
          /* the argument is Prism's bit, as above */
          int rexcl2 = nt_range_excl(nt, rn2) ? NT_RANGE_EXCLUDE_END : 0;
          if (rlo2 >= 0 && comp_ntype(c, rlo2) == TY_STRING) {
            int rhi2 = nt_ref(nt, rn2, "right");
            Buf lo2; memset(&lo2, 0, sizeof lo2); emit_expr(c, rlo2, &lo2);
            Buf hi2; memset(&hi2, 0, sizeof hi2); emit_expr(c, rhi2, &hi2);
            buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, %s); }\n",
                       lo2.p ? lo2.p : "NULL", hi2.p ? hi2.p : "NULL", rexcl2, k, t, ONE_SA_ELEM);
            free(lo2.p); free(hi2.p);
          }
          else {
            buf_printf(g_pre, "{ sp_Range _sr = %s; sp_int _e = _sr.last+(_sr.excl?0:1); for (sp_int _si = _sr.first; _si < _e; _si++) sp_%sArray_push(_t%d, _si); }\n", ep, k, t);
          }
        }
        else if (ir.elem == TY_INT && sp_streq(k, "Int"))
          /* not the slot itself: a nil lives in the bitmap, and the slot alone reads 0 */
          buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push_o(_t%d, sp_IntArray_oget(_sa, _si)); }\n", ep, k, t);
        else if (ir.elem == TY_STRING && sp_streq(k, "Str"))
          buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, %s); }\n", ep, k, t, ONE_SA_ELEM);
        else if (ir.elem == TY_FLOAT && sp_streq(k, "Float"))
          buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_FloatArray_push_o(_t%d, sp_FloatArray_oget(_sa, _si)); }\n", ep, t);
        else if (it == TY_NIL)
          /* a statically-nil splat contributes nothing (`[*nil]` == []) */
          buf_printf(g_pre, ";\n");
        /* nor does a nullable Integer or Float holding nil */
        else if (((it == TY_INT && sp_streq(k, "Int")) || (it == TY_FLOAT && sp_streq(k, "Float"))) &&
                 call_returns_nullable_int(c, inner)) {
          Buf ov; memset(&ov, 0, sizeof ov);
          emit_oint_expr(c, inner, it, &ov);
          buf_printf(g_pre, "{ %s _sv = %s; if (!_sv.nil) sp_%sArray_push(_t%d, _sv.v); }\n",
                     oint_ctype(it), ov.p ? ov.p : "", k, t);
          free(ov.p);
        }
        else {
          /* Mismatched or unknown element type: emit_expr fallback */
          buf_printf(g_pre, "sp_%sArray_push%s(_t%d, %s);\n", k, nil_store_sfx(c, k, inner), t, ep);
        }
        free(el.p);
      }
else {
        Buf el; memset(&el, 0, sizeof el);
        /* element preludes flow to g_pre first; an untyped element (a raise
           token, a void call) is coerced to the element type */
        /* an element that can be nil stores with it (the array's own nil) */
        const char *esfx = nil_store_sfx(c, k, els[j]);
        if (esfx[0] && (sp_streq(k, "Int") || sp_streq(k, "Float"))) emit_elem_store_value(c, k, els[j], &el);
        else if (repr_of(c, els[j]).untyped) emit_unresolved_coerced(c, els[j], ty_array_elem(at), &el);
        else emit_coerce(c, els[j], ty_array_elem(at), CO_HOLD, "an Array literal's element", &el);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sArray_push%s(_t%d, ", k, esfx, t);
        buf_puts(g_pre, el.p ? el.p : "");
        buf_puts(g_pre, ");\n");
        free(el.p);
      }
    }
    buf_printf(b, "_t%d", t);
    return 1;
  }
  if (sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode")) {
    Repr hr = repr_of(c, id);
    TyKind ht = hr.as_ty;
    const char *hn = ty_hash_cname(ht);
    if (!hn) {
      /* Empty `{}` with unknown type: fall back to StrPolyHash */
      int ne2 = 0; nt_arr(nt, id, "elements", &ne2);
      if (ne2 == 0) hn = "StrPoly";
      else unsupported(c, id, "hash literal (key/value type)");
    }
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new();\n", hn, t, hn);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    int sym_poly = (repr_hash_is(hr, TY_SYMBOL, TY_POLY) || repr_hash_is(hr, TY_STRING, TY_POLY));
    int poly_poly = (repr_hash_is(hr, TY_POLY, TY_POLY));
    for (int j = 0; j < n; j++) {
      NodeKind ek = nt_kind(nt, els[j]);
      if (kwh_elem_dropped(nt, id, j)) { emit_dropped_value(c, nt_ref(nt, els[j], "value"), g_pre); continue; }
      if (ek == NK_AssocSplatNode) {
        /* `**h`: merge the spread hash into the fresh literal. In `{ **h, k: v }`
           the splat is emitted before the explicit assocs, so a following key
           overrides the merged one (Ruby's last-wins). Only a same-variant
           source merges directly; a differently-typed spread is rejected loudly
           rather than emitting a layout-mismatching update. */
        int src = nt_ref(nt, els[j], "value");
        Repr shr = repr_of(c, src);
        TyKind sh = shr.as_ty;
        const char *shn = ty_hash_cname(sh);
        int nsrc = -1;
        if (shr.untyped && src >= 0 && nt_kind(nt, src) == NK_HashNode) nt_arr(nt, src, "elements", &nsrc);
        /* `**{}` adds nothing */
        if (nsrc == 0) continue;
        if (src >= 0 && (sh == TY_NIL || kw_splat_checked_boxed(c, src) || kw_splat_raises(c, src))) {
          /* `**nil`, `**true`, `**1`, which inference takes for a spread of
             nothing: the operand runs where it stands and converts, raising
             CRuby's TypeError unless it is nil -- or it already has, ahead of
             the call's keywords (emit_ds_hash_materialize), and reads as
             nothing here */
          int ran = nt_kind(nt, src) == NK_NilNode;
          for (int i = 0; i < g_n_argov && !ran; i++) ran = g_argov_node[i] == src;
          if (!ran) {
            Buf sb; memset(&sb, 0, sizeof sb); emit_kw_splat_operand_inline(c, src, &sb);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "%s\n", sb.p ? sb.p : "");
            free(sb.p);
          }
          continue;
        }
        if (shn && sp_streq(shn, hn)) {
          /* same-variant source: a direct typed merge. */
          Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, src, &sb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sHash_update(_t%d, %s);\n", hn, t, sb.p ? sb.p : "");
          free(sb.p);
        }
        else if ((shr.kind == RK_BOXED || shn) && poly_poly) {
          /* a poly spread source, or a hash of another variant, into a
             poly-poly literal: its boxed (key,value) pairs set at run time
             (any hash variant). nil spreads nothing, and anything else
             converts through its #to_hash or raises CRuby's TypeError
             (sp_kw_merge_any): the walk alone took a user object or an
             Integer for no pairs at all. */
          int st = ++g_tmp;
          st = hold_operand_pre(c, src, TY_POLY, 1, st, 1);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_kw_merge_any(_t%d, _t%d);\n", t, st);
        }
        else {
          unsupported(c, id, "hash double-splat of an unmergeable source"); return 1;
        }
        continue;
      }
      int key = nt_ref(nt, els[j], "key");
      int val = nt_ref(nt, els[j], "value");
      Buf kb; memset(&kb, 0, sizeof kb);
      if (poly_poly) emit_boxed(c, key, &kb);
      else if (ty_is_hash(ht)) emit_coerce(c, key, ty_hash_key(ht), CO_HOLD, "a Hash literal's key", &kb);
      else emit_expr(c, key, &kb);
      Buf vb; memset(&vb, 0, sizeof vb);
      if (sym_poly || poly_poly) {
        /* (a published String before a pair that can change it is boxed as
           its handle: emit_boxed_operand over the value and the pairs after) */
        int *ops = malloc(sizeof(int) * (size_t)(n - j));
        if (!ops) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        ops[0] = val;
        for (int j2 = j + 1; j2 < n; j2++) ops[j2 - j] = els[j2];
        emit_boxed_operand(c, ops, n - j, 0, &vb);
        free(ops);
      }
      else if (ty_is_hash(ht)) emit_coerce(c, val, ty_hash_val(ht), CO_HOLD, "a Hash literal's value", &vb);
      else emit_expr(c, val, &vb);
      emit_indent(g_pre, g_indent);
      /* A pair's key and value are the set's sibling arguments, as a store's
         are: a key that can allocate goes into a rooted temp ahead of the
         value's build, as the store arm's does. */
      int tk = -1;
      if (ty_is_hash(ht) && subtree_may_allocate(nt, key)) {
        TyKind kt = ty_hash_key(ht);
        tk = ++g_tmp;
        buf_printf(g_pre, "{ %s _t%d = %s; ", c_type_name(kt), tk, kb.p ? kb.p : "");
        if (needs_root(kt)) { emit_gc_root_tmp(c, kt, tk, g_pre); buf_puts(g_pre, " "); }
      }
      /* a key that is a shared String slot's read is a fresh copy: beside
         a value that allocates, the value is built first, into a rooted
         temp, and the key read last, in the set itself */
      int tv = -1;
      if (tk < 0 && ty_is_hash(ht) && strbuf_read_copies(c, key) && operand_may_allocate(c, val)) {
        TyKind vt = (sym_poly || poly_poly) ? TY_POLY : ty_hash_val(ht);
        tv = ++g_tmp;
        buf_printf(g_pre, "{ %s _t%d = %s; ", vt == TY_POLY ? "sp_RbVal" : c_type_name(vt), tv, vb.p ? vb.p : "");
        emit_gc_root_tmp(c, vt, tv, g_pre); buf_puts(g_pre, " ");
      }
      buf_printf(g_pre, "sp_%sHash_set(_t%d, ", hn, t);
      if (tk >= 0) buf_printf(g_pre, "_t%d", tk); else buf_puts(g_pre, kb.p ? kb.p : "");
      buf_puts(g_pre, ", ");
      if (tv >= 0) buf_printf(g_pre, "_t%d", tv); else buf_puts(g_pre, vb.p ? vb.p : "");
      buf_puts(g_pre, (tk >= 0 || tv >= 0) ? "); }\n" : ");\n");
      free(kb.p); free(vb.p);
    }
    buf_printf(b, "_t%d", t);
    return 1;
  }
  return 0;
}

/* An if or unless in value position (and the ternary) (emit_expr_node's arms, in their order) */
static int emit_if_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (!(sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode"))) return 0;
  /* if/unless as a value: a ternary when both branches are single
     value-expressions. Arm emission (boxing / empty-literal typing) lives
     in emit_ternary_arm below the switch. */
  int pred = nt_ref(nt, id, "predicate");
  int then_b = nt_ref(nt, id, "statements");
  int is_unless = sp_streq(ty, "UnlessNode");
  int sub = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
  int tn = 0;
  const int *tb = then_b >= 0 ? nt_arr(nt, then_b, "body", &tn) : NULL;
  int else_stmts = -1;
  if (sub >= 0 && nt_kind(nt, sub) == NK_ElseNode)
    else_stmts = nt_ref(nt, sub, "statements");
  int en = 0;
  const int *eb = else_stmts >= 0 ? nt_arr(nt, else_stmts, "body", &en) : NULL;
  /* A statically-answered defined?, is_a? or block_given? predicate folds to
     its live arm: the dead arm may not even type-check against the
     receiver's storage type (a blockless yield has no type of its own).
     Mirrors emit_if's statement-form fold and the inference fold. */
  {
    int df = comp_defined_guard_false(c, pred);
    int dt = df ? 0 : comp_defined_guard_true(c, pred);
    int known = df ? 0 : (dt ? 1 : static_isa_cond(c, pred));
    if (known < 0 && !df && !dt) known = static_respond_to_cond(c, pred);
    /* a `block_given? ? a : b` pair: the live arm alone, rendered at the
       result type as the unfolded pair renders each arm */
    if (known < 0 && !df && !dt && tn == 1 && en == 1) {
      int bg = static_block_given_cond(c, pred);
      if (bg >= 0) {
        TyKind res = repr_of(c, id).as_ty;
        if (res == TY_VOID || res == TY_NIL) res = TY_POLY;
        emit_ternary_arm_o(c, (is_unless ? !bg : bg) ? tb[0] : eb[0], res, cond_res_oint(c, id, res), b);
        return 1;
      }
    }
    if (known >= 0) {
      int take_then = is_unless ? !known : known;
      if (!take_then && !is_unless && sub >= 0 &&
          nt_kind(nt, sub) == NK_IfNode) {
        emit_expr(c, sub, b);  /* the elsif chain continues as the value */
        return 1;
      }
      int live = take_then ? then_b : else_stmts;
      int ln = 0;
      const int *lb = live >= 0 ? nt_arr(nt, live, "body", &ln) : NULL;
      if (ln == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
      /* The live arm still has to be rendered at the WHOLE expression's
         type, the way the unfolded pair below is: the slot receiving this
         was declared from that type, and the arm's own may be narrower.
         Emitting the arm raw put a `const char *` into an sp_RbVal local
         whenever an is_a? predicate folded (#4280's tmpdir package could
         not compile at its simplest call). */
      TyKind fres = repr_of(c, id).as_ty;
      if (fres == TY_VOID || fres == TY_NIL) fres = TY_POLY;
      /* Only the WIDENING conversion, into a poly result. Forcing a narrower
         one would convert rather than carry -- and where the result type is
         narrower than the arm the analysis has already gone wrong, so the C
         type error that raises is the honest answer, not a silent value. */
      int fbox = fres == TY_POLY && repr_of(c, lb[ln - 1]).kind != RK_BOXED;
      /* ...and an empty `[]`, which has no element kind of its own, takes
         the result's array kind as the unfolded pair gives it: emitted raw
         it was the default Integer array, stored into a poly array slot */
      int fempty = 0;
      if (ln == 1 && ty_is_array(fres) && node_is_empty_container(c, lb[0]))
        fempty = an_empty_container_kind(c, lb[0]) == 1;
      /* ...and an Integer / Float slot that holds its nil (an sp_oint, as the
         analysis gives `x.is_a?(Integer) ? x : raise` while the dead arm is
         still counted) takes the arm in its oint form, as the unfolded pair
         renders each arm: emitted raw, a plain sp_int went into the sp_oint */
      int fo = cond_res_oint(c, id, fres);
      if (ln == 1) {
        if (fo) emit_ternary_arm_o(c, lb[0], fres, 1, b);
        else if (fbox || fempty) emit_ternary_arm(c, lb[0], fres, b); else emit_expr(c, lb[0], b);
        return 1;
      }
      /* The leading statements go into the PRELUDE, not inside a statement
         expression around the value. The last element is emitted with
         emit_expr, and an arm that is itself a conditional hoists its own
         branches into the prelude -- which is emitted before this whole
         expression. Holding the leading statements here instead put them
         AFTER the code that reads what they assign: `range = H[expected]`
         landed below the `if range.nil?` testing it, so the nil arm always
         won and a branch reached its tail without running its own first
         line (#4139). The prelude keeps them in source order. */
      for (int j = 0; j < ln - 1; j++) emit_stmt(c, lb[j], g_pre, g_indent);
      if (fo) emit_ternary_arm_o(c, lb[ln - 1], fres, 1, b);
      else if (fbox) emit_ternary_arm(c, lb[ln - 1], fres, b);
      else emit_expr(c, lb[ln - 1], b);
      return 1;
    }
  }
  if (tn == 1 && en == 1) {
    TyKind res = repr_of(c, id).as_ty;
    /* A void/nil-typed if (e.g. an arm that is a writer call, doom's
       `self.fullscreen = value if respond_to?(...)`), or an untyped one
       whose every arm diverges, has no C storage type -- emit_ctype would
       declare `void _tN` -- so hold the result boxed; void arms degrade
       to nil. */
    if (res == TY_VOID || res == TY_NIL || res == TY_UNKNOWN) res = TY_POLY;
    /* Emit each arm with a CAPTURED prelude: an arm whose sub-expressions
       hoist statements (a rooted call argument, a constructed receiver, ...)
       cannot ride a flat C ternary -- a shared prelude would evaluate BOTH
       arms eagerly (`File.exist?(f) ? File.read(f) : x` raised on the
       untaken read). Preludeless arms keep the flat form; otherwise the
       arms become real branches with their preludes scoped inside. */
    Buf ta; memset(&ta, 0, sizeof ta);
    Buf te; memset(&te, 0, sizeof te);
    Buf pa; memset(&pa, 0, sizeof pa);
    Buf pe; memset(&pe, 0, sizeof pe);
    /* a `raise`/`fail` arm diverges: it produces no value to assign, so it
       cannot ride the flat C ternary (which needs both arms to be values of
       the result type) -- force the branch form and emit it as a statement. */
    int then_raise = node_is_raise(c, tb[0]);
    int else_raise = node_is_raise(c, eb[0]);
    int res_o = cond_res_oint(c, id, res);
    Buf *sv_pre = g_pre;
    g_pre = &pa; emit_ternary_arm_o(c, tb[0], res, res_o, &ta);
    g_pre = &pe; emit_ternary_arm_o(c, eb[0], res, res_o, &te);
    g_pre = sv_pre;
    int hoists = (pa.p && pa.p[0]) || (pe.p && pe.p[0]) || then_raise || else_raise;
    if (!hoists) {
      buf_puts(b, "(");
      if (is_unless) buf_puts(b, "!(");
      emit_cond(c, pred, b);
      if (is_unless) buf_puts(b, ")");
      buf_puts(b, " ? ");
      int hw = res == TY_STRBUF;   /* (#10021: either face into the handle result) */
      /* ...and a handle arm into the bytes result as its bytes, which
         beside a bytes arm (nil's) made the C conditional a void * read
         through as bytes (`(s ? s : nil) || :done`) */
      int bw = res == TY_STRING && !res_o;
      int bwt = bw && repr_of(c, tb[0]).as_ty == TY_STRBUF, bwe = bw && repr_of(c, eb[0]).as_ty == TY_STRBUF;
      buf_printf(b, hw ? "SP_STR_HDL(%s)" : bwt ? "SP_STR_B(%s)" : "%s", ta.p ? ta.p : "0");
      buf_puts(b, " : ");
      buf_printf(b, hw ? "SP_STR_HDL(%s)" : bwe ? "SP_STR_B(%s)" : "%s", te.p ? te.p : "0");
      buf_puts(b, ")");
    }
    else {
      int tr = ++g_tmp;
      buf_puts(b, "({ ");
      emit_res_ctype(c, res, res_o, b);
      /* braced zero: valid for scalars, pointers, AND by-value structs
         (value-class objects, sp_Range); both branches assign over it */
      buf_printf(b, " _t%d = {0}; if (", tr);
      if (is_unless) buf_puts(b, "!(");
      emit_cond(c, pred, b);
      if (is_unless) buf_puts(b, ")");
      buf_puts(b, ") {\n");
      buf_puts(b, pa.p ? pa.p : "");
      /* a diverging (raise/fail) arm is emitted as a statement; the result
         temp keeps its default (never read, since the arm never returns) */
      /* (one representation: a String arm of either face lands in the bytes result as its view) */
      /* ...and a String arm of either face lands in a handle result as its handle (#10021) */
      const char *fo = res == TY_STRING && !res_o ? "SP_STR_B("
                     : res == TY_STRBUF ? "SP_STR_HDL(" : "", *fc = fo[0] ? ")" : "";
      if (then_raise) buf_printf(b, " %s;\n", ta.p ? ta.p : "0");
      else buf_printf(b, " _t%d = %s%s%s;\n", tr, fo, ta.p ? ta.p : "0", fc);
      buf_puts(b, "}\nelse {\n");
      buf_puts(b, pe.p ? pe.p : "");
      if (else_raise) buf_printf(b, " %s;\n", te.p ? te.p : "0");
      else buf_printf(b, " _t%d = %s%s%s;\n", tr, fo, te.p ? te.p : "0", fc);
      buf_printf(b, "} _t%d; })", tr);
    }
    free(ta.p); free(te.p); free(pa.p); free(pe.p);
    return 1;
  }
  /* Multi-stmt branches or no-else: emit as if/else block with a result temp.
     Preludes and the if structure go into g_pre; `b` receives only _t<N>. */
  {
    TyKind res = repr_of(c, id).as_ty;
    /* void/nil result: no C storage type (see the ternary form above). */
    if (res == TY_VOID || res == TY_NIL) res = TY_POLY;
    int res_o = cond_res_oint(c, id, res);
    int tr = ++g_tmp;
    /* Declare the temp and default-initialize it (an absent arm is nil). */
    emit_indent(g_pre, g_indent);
    emit_res_ctype(c, res, res_o, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tr, res_zero(c, res, res_o));
    /* The temp is set here, in the statement's prelude, and read where the
       `if` stands in the statement, after what is written ahead of it
       there has run: the receiver, in `src(i).merge(note: ("n#{i}" if
       i > 0))`. A value the branch allocated has no other holder in
       between, and a collection there freed it. Root it, as the case/in
       value's temp is rooted. */
    if (ty_gc_holds_refs(c, res)) {
      emit_indent(g_pre, g_indent);
      emit_gc_root_tmp_refs(c, res, tr, g_pre);
      buf_puts(g_pre, "\n");
    }
    /* Emit the condition into its own buffer: any prolog it hoists is a
       statement, and writing it to g_pre after "if (" had been written left
       the declaration in the middle of the expression. */
    Buf cnd; memset(&cnd, 0, sizeof cnd);
    emit_cond(c, pred, &cnd);
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "if (");
    if (is_unless) buf_puts(g_pre, "!(");
    buf_puts(g_pre, cnd.p ? cnd.p : "0");
    if (is_unless) buf_puts(g_pre, ")");
    buf_puts(g_pre, ") {\n");
    free(cnd.p);
    /* Then branch: side-effect stmts, then assign last expr to temp. */
    for (int i = 0; i < tn - 1; i++) emit_stmt(c, tb[i], g_pre, g_indent + 2);
    if (tn > 0) emit_if_arm_tail(c, tb[tn - 1], res, res_o, tr);
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "}\n");
    /* Else / elsif branch. */
    if (sub >= 0) {
      NodeKind sub_k = nt_kind(nt, sub);
      if (sub_k == NK_ElseNode) {
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "else {\n");
        for (int i = 0; i < en - 1; i++) emit_stmt(c, eb[i], g_pre, g_indent + 2);
        if (en > 0) emit_if_arm_tail(c, eb[en - 1], res, res_o, tr);
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "}\n");
      }
      else {
        /* elsif (sub is IfNode) or other subsequent: recurse via emit_expr */
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "else {\n");
        int saved_gi3 = g_indent; g_indent = g_indent + 2;
        Buf sub_e; memset(&sub_e, 0, sizeof sub_e);
        /* The nested elsif chain types on its own arms: a concrete chain
           (string/string) under a poly outer if (the empty then-arm's nil)
           boxes into the poly temp, as the arms above do; an Integer or
           Float chain boxes its oint, nil included. */
        Repr subtr = repr_of(c, sub);
        TyKind subt = subtr.as_ty;
        int boxed3 = !res_o && res == TY_POLY && subtr.kind != RK_BOXED && subt != TY_NIL &&
                     subt != TY_UNKNOWN && subt != TY_VOID;
        if (res_o) emit_oint_expr(c, sub, res, &sub_e);
        else if (boxed3) emit_boxed(c, sub, &sub_e);
        else emit_expr(c, sub, &sub_e);
        g_indent = saved_gi3;
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "_t%d = ", tr);
        if (res_o) buf_puts(g_pre, sub_e.p ? sub_e.p : oint_nil(res));
        else if (boxed3) buf_puts(g_pre, sub_e.p ? sub_e.p : "sp_box_nil()");
        /* a nested chain whose every written arm raises is the implicit
           nil, held in a boxed temp; into a concrete slot with a nil of
           its own (a nullable String, #4567) it is that nil, not the box */
        else if (subt == TY_NIL && res != TY_POLY && nil_value(res))
          buf_puts(g_pre, nil_value(res));
        else if (emit_arm_text_as_bigint(res, subt, sub_e.p ? sub_e.p : default_value_from_compiler(c, subt), g_pre)) { }
        else if (sub_e.p && one_str_to_open(g_pre, res, "_t%d", tr)) { buf_puts(g_pre, sub_e.p); buf_puts(g_pre, ")"); }
        else buf_puts(g_pre, sub_e.p ? sub_e.p : default_value_from_compiler(c, res));
        buf_puts(g_pre, ";\n");
        free(sub_e.p);
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "}\n");
      }
    }
    buf_printf(b, "_t%d", tr);
    return 1;
  }
  return 0;
}
/* Is `left` a `||`, `&&` or conditional (parentheses aside) whose String
   the rule hands on as a handle, so an enclosing chain holds it boxed by
   that handle, as emit_boxed_cond_arms boxes it? */
static int logic_left_carries(Compiler *c, int left) {
  int u = nt_paren_single(c->nt, left);
  NodeKind k = u >= 0 ? nt_kind(c->nt, u) : NK_NONE;
  if (k == NK_OrNode || k == NK_AndNode || k == NK_CaseNode) return strbuf_value_carries(c, u);
  return (k == NK_IfNode || k == NK_UnlessNode) && strbuf_cond_has_handle_leaf(c, u);
}

/* See codegen_internal.h. */
int logic_value_handle(Compiler *c, int v) {
  int u = nt_paren_single(c->nt, v);
  NodeKind k = u >= 0 ? nt_kind(c->nt, u) : NK_NONE;
  if (k != NK_OrNode && k != NK_AndNode) return 0;
  int left = nt_ref(c->nt, u, "left");
  Repr lr = repr_of(c, left);
  return ((lr.as_ty == TY_STRING && lr.strbuf_src == RS_HANDLE) || lr.as_ty == TY_STRBUF) &&
         repr_of(c, u).as_ty == TY_STRING;
}

/* Is `node` a call over a boxed receiver that the inference left untyped?
   Its emission is the poly dispatch, which holds the answer of a call typed
   as no value boxed (emit_poly_method_dispatch), or the boxed unresolved
   token: an sp_RbVal either way. */
static int untyped_boxed_call(Compiler *c, int node) {
  node = nt_paren_single(c->nt, node);
  if (node < 0 || nt_kind(c->nt, node) != NK_CallNode || comp_ntype(c, node) != TY_UNKNOWN) return 0;
  int recv = nt_ref(c->nt, node, "receiver");
  return recv >= 0 && repr_of(c, recv).kind == RK_BOXED;
}

/* And/or, a rescue modifier and a begin block in value position (emit_expr_node's arms, in their order) */
static int emit_and_or_begin_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (sp_streq(ty, "AndNode") || sp_streq(ty, "OrNode")) {
    int is_and = sp_streq(ty, "AndNode");
    int left = nt_ref(nt, id, "left"), right = nt_ref(nt, id, "right");
    TyKind lt = repr_of(c, left).as_ty, res = repr_of(c, id).as_ty;
    /* --share-strings: a bang's held answer on an arm is boxed as its handle
       (emit_boxed_self_answer_cond), not wrapped in a new String below */
    if (res == TY_POLY && emit_boxed_self_answer_cond(c, id, b)) return 1;
    /* `v = lookup or return`: the right operand diverges, so it has no value
       for an arm to assign. Run it as a statement inside the short-circuit and
       answer the left, which is the only value the chain can produce (#3777). */
    if (right >= 0 && nt_type(nt, right) &&
        (nt_kind(nt, right) == NK_ReturnNode ||
         nt_kind(nt, right) == NK_BreakNode ||
         nt_kind(nt, right) == NK_NextNode)) {
      TyKind vt = lt;
      if (vt == TY_VOID || vt == TY_UNKNOWN || vt == TY_NIL) vt = TY_POLY;
      int t2 = ++g_tmp;
      Buf lb; memset(&lb, 0, sizeof lb);
      /* an Integer / Float left that can be nil is held as its oint; the
         expression's value is the left's (its oint where the chain can be
         nil, else its value) */
      int lo2 = oint_kind(vt) && node_has_oint_form(c, left);
      int ro2 = cond_res_oint(c, id, vt);
      if (vt == TY_POLY && lt != TY_POLY) emit_boxed(c, left, &lb);
      else if (lo2) emit_oint_expr(c, left, vt, &lb);
      else emit_expr(c, left, &lb);
      Buf tc2; memset(&tc2, 0, sizeof tc2);
      if (vt == TY_POLY)       buf_printf(&tc2, "sp_poly_truthy(_t%d)", t2);
      else if (vt == TY_BOOL)  buf_printf(&tc2, "_t%d", t2);
      else if (oint_kind(vt))  buf_printf(&tc2, lo2 ? "(!_t%d.nil)" : "(1)", t2);
      else if (vt == TY_SYMBOL) buf_printf(&tc2, "(_t%d != (sp_sym)-1)", t2);
      else if (vt == TY_CLASS) buf_printf(&tc2, "(!sp_class_nil_p(_t%d))", t2);
      else if (vt == TY_STRING || ty_is_array(vt) || ty_is_hash(vt) || ty_is_object(vt) ||
               vt == TY_PROC || vt == TY_MATCHDATA || vt == TY_EXCEPTION ||
               ty_nullable_builtin_id(vt))
        buf_printf(&tc2, "(_t%d != 0)", t2);
      else buf_puts(&tc2, "1");
      buf_puts(b, "({ ");
      emit_res_ctype(c, vt, lo2, b);
      buf_printf(b, " _t%d = %s; if (%s%s) {\n", t2, lb.p ? lb.p : "0",
                 is_and ? "" : "!", tc2.p ? tc2.p : "1");
      emit_stmt(c, right, b, g_indent + 1);
      if (lo2 && !ro2) buf_printf(b, "}\n _t%d.v; })", t2);
      else if (!lo2 && ro2) buf_printf(b, "}\n %s(_t%d); })", oint_of(vt), t2);
      else buf_printf(b, "}\n _t%d; })", t2);
      free(lb.p); free(tc2.p);
      return 1;
    }
    if (lt == TY_BOOL && comp_ntype(c, right) == TY_BOOL) {
      /* Capture the right operand's prelude: an object subexpression there
         (`a && b.c == x`) hoists a GC-rooted temp that must run inside the
         short-circuit, not above the whole chain (issue #1773). No prelude ->
         the flat C `&&`/`||`; otherwise a real branch scopes it. */
      Buf rarm; memset(&rarm, 0, sizeof rarm);
      Buf rpre; memset(&rpre, 0, sizeof rpre);
      { Buf *sv_pre2 = g_pre; g_pre = &rpre; emit_expr(c, right, &rarm); g_pre = sv_pre2; }
      if (!(rpre.p && rpre.p[0])) {
        buf_puts(b, "(");
        emit_expr(c, left, b);
        buf_puts(b, is_and ? " && " : " || ");
        buf_puts(b, rarm.p ? rarm.p : "0");
        buf_puts(b, ")");
      }
      else {
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_bool _t%d; if (", tr);
        emit_expr(c, left, b);
        if (is_and) buf_printf(b, ") {\n%s_t%d = %s;\n}\nelse { _t%d = 0; } _t%d; })",
                               rpre.p, tr, rarm.p ? rarm.p : "0", tr, tr);
        else        buf_printf(b, ") { _t%d = 1; }\nelse {\n%s_t%d = %s;\n} _t%d; })",
                               tr, rpre.p, tr, rarm.p ? rarm.p : "0", tr);
      }
      free(rarm.p); free(rpre.p);
      return 1;
    }
    /* value form: a || b  ->  truthy(a) ? a : b ;  a && b -> truthy(a) ? b : a.
       Evaluate the left once into a temp; results widen to the unified type.
       The result is always a real value (a nil left boxes to sp_box_nil), never
       void -- a VOID/UNKNOWN unified type (`() && true`, an empty-parens operand
       whose cached type settled void) becomes poly so the temp is not `void`. */
    if (res == TY_VOID || res == TY_UNKNOWN || res == TY_NIL) res = TY_POLY;
    /* --share-strings: a left typed as the String handle (a value the
       container store demanded as one, strbuf_demand_value_leaves) beside a
       boxed result is held boxed, as a conditional's arm boxes it
       (emit_ternary_arm): held as an sp_String *, the left's text was the
       String's bytes, and the kept-left arm put a pointer beside the boxed
       right one. So is a String bang on a boxed receiver (`x.strip! || x`):
       its box is the receiver's own (emit_boxed_bang_self), where the bytes
       boxed again were a new String. So is a String left a store lifts
       (poly_strbuf_lift: a seeded String method's tail `s || v`), boxed by
       the handle it publishes, or by the receiver's where it is a builtin
       call answering its receiver (`s.itself || v`, share_plan_box_handle),
       not from its bytes. */
    int lt_hbox = res == TY_POLY &&
                  (lt == TY_STRBUF ||
                   (lt == TY_STRING && (face_str_bang_call(c, nt_paren_single(c->nt, left)) ||
                                        strbuf_var_handle(c, nt_paren_single(c->nt, left), NULL) ||
                                        (repr_of(c, nt_paren_single(c->nt, left)).poly_lift &&
                                         (repr_call_returns_handle(c, left) || logic_left_carries(c, left) ||
                                          share_plan_box_handle(c, nt_paren_single(c->nt, left)))))));
    if (lt_hbox) lt = TY_POLY;
    int t = ++g_tmp;
    int lt_falsy_const = (lt == TY_NIL || lt == TY_VOID);  /* a nil/void left has no C-typed value */
    /* one representation: a String left that is a handle (a slot's read, an
       element) is held as the handle, so the chain's value keeps its identity
       (`v = h[k] || "d"` answers the Hash's own String); the right arm is
       made a handle too (a literal's is its site's). So is a left typed as
       the handle, and a chain whose value is (`[(s || nil)]` of a String
       parameter s): an arm left as it was, nil's `(0, NULL)` or a literal's
       bytes, made the chain a void * that SP_STR_B read the handle through
       as bytes. */
    int lhandle = ((lt == TY_STRING && repr_of(c, left).strbuf_src == RS_HANDLE) ||
                                   (lt == TY_STRBUF && res == TY_STRING) ||
                                   ((lt == TY_STRING || lt == TY_STRBUF) && res == TY_STRBUF));
    /* An unresolved left emits a poly fallback. One that diverges -- a bare
       name nothing defines raises NameError -- makes the whole chain diverge:
       keep the raise and answer the result's own placeholder, so the slot
       around the chain type-checks, and a chain nested in another reads as
       diverging too (#7164). */
    Buf lunk; memset(&lunk, 0, sizeof lunk);
    /* the untyped left is a call over a boxed receiver: its text is boxed */
    int lboxed = 0;
    if (lt == TY_UNKNOWN) {
      emit_expr(c, left, &lunk);
      if (text_diverges(lunk.p ? lunk.p : "")) {
        buf_printf(b, "((void)(%s), %s)", lunk.p,
                   res == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, res));
        free(lunk.p);
        return 1;
      }
      /* A call over a boxed receiver the inference left untyped is the poly
         dispatch, whose answer is boxed when its type is no value (every
         candidate for the name unsettled: a method in a module nothing
         includes, whose body is a deferred refusal), as the unresolved
         token is: hold the left boxed and test its truth. Declared at the
         result's type, `request.cookies_digest || "SHA1"` put the sp_RbVal
         in a `const char *` and read it as always truthy. */
      if (untyped_boxed_call(c, left)) { lt = TY_POLY; lboxed = 1; }
    }
    const char *ltxt = lunk.p ? lunk.p : "";
    /* an Integer or Float left that can be nil is held as its oint (lo);
       the chain's value is an oint when either side can be nil (ro) */
    int lo = oint_kind(lt) && !lt_falsy_const && node_has_oint_form(c, left);
    int ro = cond_res_oint(c, id, res);
    buf_puts(b, "({ ");
    if (lo) buf_puts(b, oint_ctype(lt));
    else if (lhandle) buf_puts(b, "sp_String *");
    else emit_ctype(c, (lt == TY_UNKNOWN || lt_falsy_const) ? res : lt, b);
    buf_printf(b, " _t%d = ", t);
    if (lt_falsy_const) {
      buf_puts(b, "("); emit_expr(c, left, b); buf_puts(b, ", ");
      buf_puts(b, res == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, res == TY_UNKNOWN ? TY_INT : res));
      buf_puts(b, ")");
    }
    /* an unresolved (TY_UNKNOWN) left emits a poly fallback (sp_box_nil); coerce
       it to the unified scalar result so the temp's declared type matches. */
    else if (lt == TY_UNKNOWN && res == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", ltxt);
    else if (lt == TY_UNKNOWN && res == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", ltxt);
    /* a bool-unified chain with an unresolved left (a poly dispatch whose
       node type stayed unknown, e.g. alias-to-reader through a poly element):
       take its truthiness, mirroring the int/float coercions (#3276) */
    else if (lt == TY_UNKNOWN && res == TY_BOOL) buf_printf(b, "sp_poly_truthy(%s)", ltxt);
    /* The left may be an unresolved call emitting an sp_RbVal raise token
       (`x.details || "~"`, where details is typed String from the `||` but
       lowers to sp_raise_nomethod); coerce it to the temp's DECLARED type (res
       when the left itself is unknown) rather than assigning the raw token. A
       normal left emits unchanged. */
    else if (lt == TY_UNKNOWN) emit_unresolved_coerced_text(c, left, res, ltxt, b);
    else if (lboxed) buf_puts(b, ltxt);   /* the boxed dispatch, emitted above */
    else if (lo) emit_oint_expr(c, left, lt, b);
    else if (lt_hbox) emit_boxed(c, left, b);
    else if (lhandle) emit_strbuf_handle_of(c, left, b);
    /* (one representation: a String left of either face held as its bytes) */
    else if (lt == TY_STRING) { buf_puts(b, "SP_STR_B("); emit_unresolved_coerced(c, left, lt, b); buf_puts(b, ")"); }
    else emit_unresolved_coerced(c, left, lt, b);
    free(lunk.p);
    buf_puts(b, "; ");
    /* Truthiness of the left, built into its own buffer so the arms can be
       captured before it is committed to `b`. */
    Buf tcond; memset(&tcond, 0, sizeof tcond);
    if (lt == TY_POLY)      buf_printf(&tcond, "sp_poly_truthy(_t%d)", t);
    else if (lt == TY_BOOL) buf_printf(&tcond, "_t%d", t);
    else if (lt_falsy_const) buf_puts(&tcond, "0");
    else if (oint_kind(lt)) buf_printf(&tcond, lo ? "(!_t%d.nil)" : "(1)", t);  /* a nullable scalar reads falsy at nil; a plain one is always truthy */
    else if (lt == TY_STRING || ty_is_array(lt) || ty_is_hash(lt) || ty_is_object(lt) ||
             lt == TY_PROC || lt == TY_MATCHDATA || lt == TY_EXCEPTION ||
             lt == TY_BIGINT || ty_nullable_builtin_id(lt) || (lt == TY_STRBUF))
      buf_printf(&tcond, "(_t%d != 0)", t);  /* nullable pointer: NULL reads falsy */
    else if (lt == TY_SYMBOL) buf_printf(&tcond, "(_t%d != (sp_sym)-1)", t);  /* nilable symbol sentinel */
    else if (lt == TY_UNKNOWN && res == TY_BOOL) buf_printf(&tcond, "_t%d", t);  /* temp holds sp_poly_truthy(left) (#3276) */
    else if (lt == TY_CLASS) buf_printf(&tcond, "(!sp_class_nil_p(_t%d))", t);
    else                    buf_puts(&tcond, "1");  /* concrete value: always truthy */
    /* Capture each arm (widened to res). The RIGHT arm's prelude is captured
       separately: an object subexpression there (`a && b.c`) hoists a GC-rooted
       temp, which must be evaluated INSIDE the short-circuit -- after the left,
       and only when the left is truthy -- not lifted above the whole chain
       (issue #1773). The kept-left arm is pure temp/box and never hoists. */
    Buf larm; memset(&larm, 0, sizeof larm);
    Buf rarm; memset(&rarm, 0, sizeof rarm);
    Buf rpre; memset(&rpre, 0, sizeof rpre);
    #define EMIT_ARM(IS_RIGHT, TB) do { \
      if (IS_RIGHT) { emit_ternary_arm_o(c, right, res, ro, (TB)); } \
      else { if (res == TY_POLY && lt != TY_POLY) { /* box the temp by left type */ \
               /* In `a && b` the kept-left arm is reached only when the left is
                  FALSY, and for a scalar slot that means it holds nil:
                  box it as nil, or the result answers nil? with
                  false and survives compact. `a || b` keeps a truthy left, so
                  there the plain box is right. */ \
               if (oint_kind(lt) && lo) buf_printf((TB), "%s(_t%d)", oint_box(lt), t); \
               else if (lt==TY_INT) { if (is_and) buf_puts((TB), "sp_box_nil()"); \
                                 else buf_printf((TB), "sp_box_int(_t%d)", t); } \
               else if (lt==TY_STRING) buf_printf((TB), "SP_STR_BOX(_t%d)", t); \
               else if (lt==TY_FLOAT) { if (is_and) buf_puts((TB), "sp_box_nil()"); \
                                        else buf_printf((TB), "sp_box_float(_t%d)", t); } \
               else if (lt==TY_BOOL) buf_printf((TB), "sp_box_bool(_t%d)", t); \
               else if (lt==TY_SYMBOL) buf_printf((TB), "(_t%d != (sp_sym)-1 ? sp_box_sym(_t%d) : sp_box_nil())", t, t); \
               /* An Array subclass instance is boxed as its Array
                  (arysub_box_id), as emit_boxed / emit_boxed_text do: with
                  the class id the poly empty?/length path treated it as a
                  plain user object and raised NoMethodError (#7449). */ \
               else if (ty_is_object(lt)) { buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, ", t); \
                                            arysub_box_id(c, lt, (TB)); buf_puts((TB), ")"); } \
               else if (ty_is_hash(lt) && hash_box_cls(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, hash_box_cls(lt)); \
               else if (ty_is_array(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, \
                        lt==TY_INT_ARRAY ? "SP_BUILTIN_INT_ARRAY" : lt==TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : \
                        lt==TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY" : "SP_BUILTIN_POLY_ARRAY"); \
               else if (lt == TY_MATCHDATA && is_and) buf_puts((TB), "sp_box_nil()"); /* MatchData has no poly box; in `m && x` the kept-left arm is reached only when m is NULL, i.e. nil (#2896) */ \
               /* every remaining nullable builtin handle -- a Mutex, Queue,
                  Fiber, Thread, ConditionVariable -- boxes like the object and
                  container arms above. Without this the arm assigned a raw
                  sp_mutex * where the sibling arm is an sp_RbVal, and the C
                  compiler rejected the ternary outright (#3484). */ \
               else if (ty_nullable_builtin_id(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, ty_nullable_builtin_id(lt)); \
               else if (lt == TY_CLASS) buf_printf((TB), "sp_box_class(_t%d)", t); \
               /* any other kind with a box of its own (a Time, a Range, a \
                  Rational): boxed by it, where the raw temp met the boxed \
                  arm in one C conditional (#10078) */ \
               else if (!lt_falsy_const && lt != TY_UNKNOWN && ty_traits_of(lt) && ty_traits_of(lt)->box_text) { \
                 char kept_[32]; snprintf(kept_, sizeof kept_, "_t%d", t); emit_boxed_text(c, lt, kept_, (TB)); } \
               else buf_printf((TB), "_t%d", t); } \
             /* the kept left in an Integer / Float result: its oint where
                the result is one, its value where it is plain */ \
             else if (oint_kind(lt) && ro && !lo) buf_printf((TB), "%s(_t%d)", oint_of(lt), t); \
             else if (oint_kind(lt) && !ro && lo) buf_printf((TB), "_t%d.v", t); \
             /* an untyped or falsy-constant left was declared at the result's
                plain type: wrapped where the result is an oint */ \
             else if (lt_falsy_const && oint_kind(res) && ro) buf_puts((TB), oint_nil(res)); /* a nil left's value is nil */ \
             else if (lt == TY_UNKNOWN && oint_kind(res) && ro) buf_printf((TB), "%s(_t%d)", oint_of(res), t); \
             /* a boxed left kept in a typed result (a Hash's; the boxed
                dispatch of an untyped left beside a String right) is
                unboxed to it, as its oint where the result is one */ \
             else if (lt == TY_POLY && res != TY_POLY && ro && oint_kind(res)) { \
               char kept[32]; snprintf(kept, sizeof kept, "_t%d", t); \
               emit_unbox_nilable_text(c, res, kept, (TB)); } \
             else if (lt == TY_POLY && res != TY_POLY) { \
               char kept[32]; snprintf(kept, sizeof kept, "_t%d", t); \
               emit_unbox_text(c, res, kept, (TB)); } \
             else buf_printf((TB), "_t%d", t); } \
    } while (0)
    EMIT_ARM(0, &larm);
    { Buf *sv_pre2 = g_pre; g_pre = &rpre; EMIT_ARM(1, &rarm); g_pre = sv_pre2; }
    #undef EMIT_ARM
    if (lhandle && (res == TY_STRING || res == TY_STRBUF) && rarm.p) {
      Buf hr; memset(&hr, 0, sizeof hr);
      buf_printf(&hr, "SP_STR_HDL(%s)", rarm.p);
      free(rarm.p); rarm = hr;
    }
    /* (one representation: a right arm of either face beside a bytes left) */
    else if (!lhandle && !lo && lt == TY_STRING && res == TY_STRING && rarm.p) {
      Buf hr; memset(&hr, 0, sizeof hr);
      buf_printf(&hr, "SP_STR_B(%s)", rarm.p);
      free(rarm.p); rarm = hr;
    }
    int rhoists = rpre.p && rpre.p[0];
    if (!rhoists) {
      buf_printf(b, "%s ? %s : %s; })", tcond.p ? tcond.p : "1",
                 is_and ? (rarm.p ? rarm.p : "0") : (larm.p ? larm.p : "0"),
                 is_and ? (larm.p ? larm.p : "0") : (rarm.p ? rarm.p : "0"));
    }
    else {
      /* Right arm hoists: run it inside a real branch so its prelude is scoped
         and short-circuited. `res` may be void/nil (a writer arm) -- hold poly. */
      TyKind rres = (res == TY_VOID || res == TY_NIL) ? TY_POLY : res;
      int tr = ++g_tmp;
      /* a handle left makes the String result a handle (the right arm is
         wrapped to one above), so the temp holds the handle, not bytes */
      if (lhandle && rres == TY_STRING) buf_puts(b, "sp_String *");
      else emit_res_ctype(c, rres, ro, b);
      buf_printf(b, " _t%d = {0}; if (%s) {\n", tr, tcond.p ? tcond.p : "1");
      if (is_and) buf_printf(b, "%s_t%d = %s;\n}\nelse { _t%d = %s; } _t%d; })",
                             rpre.p ? rpre.p : "", tr, rarm.p ? rarm.p : "0", tr, larm.p ? larm.p : "0", tr);
      else        buf_printf(b, "_t%d = %s;\n}\nelse { %s_t%d = %s; } _t%d; })",
                             tr, larm.p ? larm.p : "0", rpre.p ? rpre.p : "", tr, rarm.p ? rarm.p : "0", tr);
    }
    free(tcond.p); free(larm.p); free(rarm.p); free(rpre.p);
    return 1;
  }

  if (sp_streq(ty, "RescueModifierNode")) {
    /* `expr rescue fallback` as an rvalue: evaluate expr under setjmp;
       on exception, evaluate fallback instead. */
    int e  = nt_ref(nt, id, "expression");
    int r  = nt_ref(nt, id, "rescue_expression");
    TyKind rt = repr_of(c, id).as_ty;
    /* a value-less modifier (`(raise "x" rescue (return :a if c))`) still
       answers one, nil when the fallback falls through: held boxed */
    if (rt == TY_VOID || rt == TY_UNKNOWN || rt == TY_NIL) rt = TY_POLY;
    int res_o = cond_res_oint(c, id, rt);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_res_ctype(c, rt, res_o, b);
    buf_printf(b, " _t%d = %s; sp_exc_check_depth(); sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp; sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n", t, res_o ? oint_nil(rt) : slot_zero(c, rt));
    buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    /* expression arm -- assign result to temp (skip diverging exprs like raise) */
    Repr er = repr_of(c, e);
    TyKind et = er.as_ty;
    /* An empty container's type reads UNKNOWN for want of an element type; it
       still produces a value, so it must be assigned rather than emitted for
       effect and discarded (#3495). */
    int one, e_yields = 0, e_nil = 0;
    switch (vp_one(nt, e, VPS_NONE | VP_PARENS_ONLY, &one)) {
    case VP1_NODE:
      e_yields = nt_kind(nt, one) == NK_YieldNode;
      e_nil = nt_kind(nt, one) == NK_NilNode;
      break;
    case VP1_NIL: e_nil = 1; break;
    case VP1_MANY: break;
    }
    int e_diverges = (er.untyped || et == TY_VOID || (et == TY_NIL && rt != TY_POLY && e_yields)) &&
                     !node_is_empty_container(c, e);
    /* --share-strings: under a handle demand (strbuf_route_begin) the
       result slot is the handle, and each value is taken as one */
    int hdem = rt == TY_STRBUF && repr_of(c, id).demand;
    /* A call spinel folds into an unconditional raise carries whatever C type
       that raise helper returns, which need not match the merged slot (the
       constant-folded `nil.clone(freeze: false)` yields an int against an
       sp_Class slot). Control never reaches the assignment, so treat it as
       diverging and emit it for effect alone. (#3021) */
    if (!e_diverges && e >= 0 && rt != TY_POLY && et != rt && !(hdem && et == TY_STRING) &&
        !ty_is_numeric(rt) && nt_kind(nt, e) == NK_CallNode)
      e_diverges = 1;
    buf_puts(b, "  ");
    /* the expression arm's own preludes (an inline-spliced body, a rooted
       temp) must land INSIDE the protected region -- with the enclosing
       statement's g_pre they would run before the setjmp, unprotected
       (#2723). Same swap the rescue arm below has always done. */
    /* the guard is an exception frame: a return, break or next out of the
       expression pops it, and an ensure region inside it sees a handler
       between itself and an enclosing ensure (`g_exc_frame_depth >
       outer->exc_base + 1`) */
    g_exc_frame_depth++;
    if (e >= 0 && !e_diverges) {
      Buf epre; memset(&epre, 0, sizeof epre);
      Buf *sv_pre0 = g_pre; int sv_ind0 = g_indent;
      g_pre = &epre; g_indent = 1;
      Buf ev; memset(&ev, 0, sizeof ev);
      if (rt == TY_POLY && er.kind != RK_BOXED) emit_boxed(c, e, &ev);
      else if (res_o) emit_oint_expr(c, e, rt, &ev);
      else if (rt == TY_STRING && (comp_scope_of(c, id)->ret_pub_fresh || comp_scope_of(c, id)->ret_pub_bang ||
                                   (comp_scope_of(c, id)->ret_handle && tail_pub_leaf(c, e))))
        emit_tail_value(c, e, &ev);
      else if (hdem) emit_strbuf_handle_of(c, e, &ev);
      /* A nil type alone can also be an opaque call that raises. Only
         proven nil values are converted to a slot's own nil here. */
      else if (et == TY_NIL && e_nil && nil_value(rt)) emit_ternary_arm_o(c, e, rt, res_o, &ev);
      else emit_expr_slot(c, e, rt, &ev);
      g_pre = sv_pre0; g_indent = sv_ind0;
      if (epre.p) buf_puts(b, epre.p);
      buf_printf(b, "_t%d = %s;", t, ev.p ? ev.p : "");
      free(epre.p); free(ev.p);
    }
    else if (e >= 0) {
      /* diverging expression like raise: emit as stmt (no assignment) */
      Buf epre; memset(&epre, 0, sizeof epre);
      Buf *sv_pre0 = g_pre; int sv_ind0 = g_indent;
      g_pre = &epre; g_indent = 1;
      Buf ev; memset(&ev, 0, sizeof ev);
      emit_expr(c, e, &ev);
      g_pre = sv_pre0; g_indent = sv_ind0;
      if (epre.p) buf_puts(b, epre.p);
      buf_printf(b, "%s;", ev.p ? ev.p : "");
      free(epre.p); free(ev.p);
    }
    g_exc_frame_depth--;
    /* restore the handled-exception depth too: a body that exits by raising
       out of its own rescue leaves its push behind, and `$!` would keep
       reading it long after the handler is gone (#3726) */
    buf_puts(b, " sp_exc_top--;\n}\nelse {\n  sp_exc_top--;\n  sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n  "
                "sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n  "
                "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n  "
                /* a bare rescue catches StandardError and its descendants only:
                   this arm caught everything, so a subclass of Exception was
                   swallowed (#3725) */
                "if (!sp_exc_is_standard_error((const char *)sp_last_exc_cls)) {\n    "
                "  sp_pending_exc_obj = sp_exc_obj[sp_exc_top]; sp_bt_keep = 1;\n    "
                "  sp_raise_cls((const char *)sp_last_exc_cls, sp_exc_msg[sp_exc_top]);\n  "
                "}\n  ");
    /* materialize the handled exception and push it so `$!` (and #cause
       threading for a nested raise) see it inside the fallback, exactly like
       a full rescue arm; popped when the arm value settles. */
    int tce = ++g_tmp;
    buf_printf(b, "sp_Exception *_t%d = sp_exc_obj[sp_exc_top] ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                  " : sp_exc_new_for_catch(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n  ", tce);
    /* an exception never loses a cause it already carries, as in
       emit_rescue (#3745); an explicit `cause:` was set by the raise */
    buf_printf(b, "if (!_t%d->cause) { sp_gc_wb((void *)_t%d); _t%d->cause = (sp_Exception *)sp_pending_cause; } sp_pending_cause = NULL;\n  ", tce, tce, tce);
    buf_printf(b, "sp_rescue_push((void *)_t%d);\n  ", tce);
    /* rescue arm: its preludes (e.g. a hoisted `$!` read) must land INSIDE
       this else block, after the push -- swap g_pre to a local buffer. */
    if (r >= 0) {
      Buf rpre; memset(&rpre, 0, sizeof rpre);
      Buf *sv_pre = g_pre; int sv_ind = g_indent;
      g_pre = &rpre; g_indent = 1;
      Buf rv; memset(&rv, 0, sizeof rv);
      /* a return or retry out of the fallback pops the exception pushed
         above, as one out of a rescue clause does (#8273) */
      g_rescue_save_stack[g_rescue_save_depth++] = (RescueSave){ g_exc_frame_depth };
      /* a jump out of the fallback (`stat = File.stat(f) rescue next`)
         leaves before the temp is written, and the placeholder its
         expression form ends in is no value of the temp's type */
      int r_jumps = rescue_fallback_jumps(c, r);
      if (r_jumps) emit_expr(c, r, &rv);
      else if (rt == TY_POLY && repr_of(c, r).kind != RK_BOXED) emit_boxed(c, r, &rv);
      else if (res_o) emit_oint_expr(c, r, rt, &rv);
      else if (rt == TY_STRING && (comp_scope_of(c, id)->ret_pub_fresh || comp_scope_of(c, id)->ret_pub_bang ||
                                   (comp_scope_of(c, id)->ret_handle && tail_pub_leaf(c, r))))
        emit_tail_value(c, r, &rv);
      else if (hdem) emit_strbuf_handle_of(c, r, &rv);
      /* an empty `[]` or `{}` has no element type of its own and takes
         the other arm's, as a ternary arm does; emitted bare it is an
         IntArray in a StrArray slot (`s.split(".") rescue []`) */
      else if (node_is_empty_container(c, r) && (ty_is_array(rt) || ty_is_hash(rt)))
        emit_ternary_arm_o(c, r, rt, res_o, &rv);
      else emit_expr_slot(c, r, rt, &rv);
      g_rescue_save_depth--;
      g_pre = sv_pre; g_indent = sv_ind;
      if (rpre.p) buf_puts(b, rpre.p);
      free(rpre.p);
      if (r_jumps) buf_printf(b, "(void)(%s);", rv.p ? rv.p : "0");
      else buf_printf(b, "_t%d = %s;", t, rv.p ? rv.p : "");
      free(rv.p);
    }
    buf_puts(b, "\n  sp_rescue_sp--;");
    buf_printf(b, "\n}\n_t%d; })", t);
    return 1;
  }

  if (sp_streq(ty, "BeginNode")) {
    /* begin/rescue as an rvalue: hoist the block into g_pre so the temp
       is assigned before the surrounding expression reads it. */
    TyKind rt = repr_of(c, id).as_ty;
    /* a void/nil-typed begin (its tail is a writer call or another
       value-less statement) has no C storage type; hold the result boxed
       and let the value-less tail leave it nil. */
    if (rt == TY_VOID || rt == TY_NIL || (c->n_cext_extensions && rt == TY_UNKNOWN)) rt = TY_POLY;
    int res_o = cond_res_oint(c, id, rt);
    int t = ++g_tmp;
    char rv[32]; snprintf(rv, sizeof rv, "_t%d", t);
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    TyKind srt = g_result_ty; g_result_ty = rt;
    int sro = g_result_oint; g_result_oint = res_o;
    /* the value sits in the temp while the ensure body runs, which may
       allocate; the root goes in front of the region so the landing's
       watermark restore keeps it. An empty ensure clause runs nothing between
       the write and the read, so it gets no root. */
    int ec = nt_ref(c->nt, id, "ensure_clause");
    int hold = ec >= 0 && nt_ref(c->nt, ec, "statements") >= 0 && ty_gc_rootable(c, rt);
    if (g_pre) {
      /* Hoisted, the temp holds the value past the begin as well, until the
         surrounding expression reads it, and what an operand after this one
         hoists runs in between: in `(begin; a; rescue; b; end) + (begin; c;
         ensure; d; end)` the second begin runs with the first one's value in
         a temp nothing else knows. So here the temp is rooted with or
         without an ensure body, and so are the Strings a by-value kind
         carries. Not where the begin is its statement, all that a write
         stores or a return's one value (hoisted_value_is_stmt): the temp is
         read at once, and it keeps the root it had. */
      emit_indent(g_pre, g_indent); emit_res_ctype(c, rt, res_o, g_pre);
      buf_printf(g_pre, " _t%d = %s;", t, res_o ? oint_nil(rt) : slot_zero(c, rt));
      if (hoisted_value_is_stmt(c, id)) { if (hold) { buf_puts(g_pre, " "); emit_gc_root_tmp(c, rt, t, g_pre); } }
      else if (ty_gc_holds_refs(c, rt)) { buf_puts(g_pre, " "); emit_gc_root_tmp_refs(c, rt, t, g_pre); }
      buf_puts(g_pre, "\n");
      emit_begin(c, id, g_pre, g_indent, rv);
    }
    else {
      /* No prelude available (e.g. inside another expression's prelude):
         fall back to a GCC statement expression. */
      buf_puts(b, "({ ");
      emit_res_ctype(c, rt, res_o, b); buf_printf(b, " _t%d = %s;", t, res_o ? oint_nil(rt) : slot_zero(c, rt));
      if (hold) { buf_puts(b, " "); emit_gc_root_tmp(c, rt, t, b); }
      buf_puts(b, "\n");
      emit_begin(c, id, b, 0, rv);
      buf_printf(b, "_t%d; })", t);
      g_result_poly = sp; g_result_ty = srt; g_result_oint = sro;
      return 1;
    }
    g_result_poly = sp; g_result_ty = srt; g_result_oint = sro;
    buf_printf(b, "_t%d", t);
    return 1;
  }
  return 0;
}

/* Does String Range `range` keep endpoint `end` as its handle? Every
   String endpoint but nil and a frozen literal, which never changes, so its
   identity is its bytes' own (#8321). */
static int srange_end_keeps(Compiler *c, int range, int end) {
  (void)range;
  return end >= 0 && comp_ntype(c, end) != TY_NIL && !share_frozen_literal(c, end);
}
/* ...one end into rooted temps: the handle `_t<ht>` and a NULL byte field
   `_t<st>` for an end kept so, else no handle and the end's bytes (NULL for
   a nil or an absent end). */
static void emit_srange_end_h(Compiler *c, int end, int keep, int ht, int st, Buf *b) {
  if (keep) {
    buf_printf(b, "sp_String *_t%d = ", ht);
    emit_strbuf_handle_of(c, end, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); const char *_t%d = NULL; ", ht, st);
    return;
  }
  buf_printf(b, "sp_String *_t%d = NULL; const char *_t%d = ", ht, st);
  if (end >= 0) emit_str_expr_nilable(c, end, b); else buf_puts(b, "NULL");
  buf_printf(b, "; SP_GC_ROOT_STR(_t%d); ", st);
}

/* An end of a String Range that makes nothing and runs nothing: a literal,
   or a read of a variable or a constant. */
static int srange_end_is_plain(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  return k == NK_StringNode || k == NK_ConstantReadNode || k == NK_GlobalVariableReadNode ||
         subtree_is_pure_read(c, id);
}

/* A Range whose kind only the values at run time decide: a boxed Range
   built by sp_range_new_box, or a String Range with a boxed end checked by
   sp_srange_new_box. The begin runs first, rooted while the end runs. 0 for
   any other Range. */
static int emit_range_boxed_ends(Compiler *c, int id, int left, int right, int excl, Buf *b) {
  const char *ctor;
  if (repr_of(c, id).kind == RK_BOXED) ctor = "sp_range_new_box";
  else if (repr_of(c, id).range == TY_STRING &&
           ((left >= 0 && repr_of(c, left).kind == RK_BOXED) ||
            (right >= 0 && repr_of(c, right).kind == RK_BOXED))) ctor = "sp_srange_new_box";
  else return 0;
  int t = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", t);
  if (left >= 0) emit_boxed(c, left, b); else buf_puts(b, "sp_box_nil()");
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); %s(_t%d, ", t, ctor, t);
  if (right >= 0) emit_boxed(c, right, b); else buf_puts(b, "sp_box_nil()");
  buf_printf(b, ", %d); })", excl);
  return 1;
}

/* A literal end reads the same whenever it runs: the other end's order
   against it does not show. */
static int range_end_literal(const NodeTable *nt, int n) {
  switch (nt_kind(nt, n)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_NilNode: case NK_TrueNode:
    case NK_FalseNode: case NK_SymbolNode: case NK_StringNode:
      return 1;
    default:
      return 0;
  }
}

/* fn(lo, hi, excl), the Range's builder over its two ends' C. in_order: the
   ends go to temps first, the begin before the end, as Ruby runs them. */
static void emit_range_build(Buf *b, const char *fn, Buf *lo, Buf *hi, int excl, int in_order) {
  if (in_order) {
    int tl = ++g_tmp, th = ++g_tmp;
    buf_printf(b, "({ __typeof__(%s) _t%d = %s; __typeof__(%s) _t%d = %s; %s(_t%d, _t%d, %d); })",
               lo->p, tl, lo->p, hi->p, th, hi->p, fn, tl, th, excl);
  }
  else buf_printf(b, "%s(%s, %s, %d)", fn, lo->p, hi->p, excl);
  free(lo->p); free(hi->p);
}

/* A Range literal in value position (a..b, a...b, endless and beginless) */
static int emit_range_literal(Compiler *c, int id, Buf *b, const NodeTable *nt) {
  int left = nt_ref(nt, id, "left");
  int right = nt_ref(nt, id, "right");
  int excl = nt_range_excl(nt, id);
  /* (:a..:e): a poly array of boxed symbols, walked by name succession
     (interning through the generated TU's own table) */
  if (left >= 0 && right >= 0 &&
      nt_kind(nt, left) == NK_SymbolNode &&
      nt_kind(nt, right) == NK_SymbolNode) {
    int ta = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                  " const char *_t%d = sp_sym_to_s(", ta, ta, ts);
    emit_expr(c, left, b);
    buf_printf(b, "); const char *_t%d = sp_sym_to_s(", te);
    emit_expr(c, right, b);
    buf_printf(b, "); for (;;) {"
                  " if (%d && sp_str_eq(_t%d, _t%d)) break;"
                  " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(_t%d)));"
                  " if (!%d && sp_str_eq(_t%d, _t%d)) break;"
                  " _t%d = sp_str_succ(_t%d);"
                  " if (sp_str_length(_t%d) > sp_str_length(_t%d)) break; }"
                  " _t%d; })",
               excl, ts, te,
               ta, ts,
               excl, ts, te,
               ts, ts,
               ts, te,
               ta);
    return 1;
  }
  /* ends decided at run time (`a[0]..a[1]` over a mixed Array) */
  if (emit_range_boxed_ends(c, id, left, right, excl, b)) return 1;
  /* (1.0..3.0): the distinct float range, endpoints kept as sp_float.
     A missing bound uses -/+HUGE_VAL as the beginless/endless sentinel. */
  if (repr_of(c, id).range == TY_FLOAT) {
    /* A bound written as absent and one written as Float::INFINITY are the
       same value; record which it was so #inspect can tell them apart. */
    /* A bound written as nil is the absent one, and a boxed bound is
       decided at run time: nil leaves that side open, an Integer renders
       as one (sp_poly_frange_bound and the run-time bits below). */
    int lnil = left < 0 || comp_ntype(c, left) == TY_NIL;
    int rnil = right < 0 || comp_ntype(c, right) == TY_NIL;
    int lpoly = !lnil && repr_of(c, left).kind == RK_BOXED;
    int rpoly = !rnil && repr_of(c, right).kind == RK_BOXED;
    int om = (lnil ? 1 : 0) | (rnil ? 2 : 0);
    /* Each endpoint renders as the user wrote it, so record which one was an
       Integer: a mixed literal (1.5..5) inspects as "1.5..5" (#3896). */
    if (!lnil && comp_ntype(c, left) == TY_INT) om |= 4;
    if (!rnil && comp_ntype(c, right) == TY_INT) om |= 8;
    int tl = lpoly ? ++g_tmp : 0, tr = rpoly ? ++g_tmp : 0;
    if (lpoly || rpoly) {
      buf_puts(b, "({ ");
      if (lpoly) { buf_printf(b, "sp_RbVal _t%d = ", tl); emit_expr(c, left, b); buf_puts(b, "; "); }
      if (rpoly) { buf_printf(b, "sp_RbVal _t%d = ", tr); emit_expr(c, right, b); buf_puts(b, "; "); }
    }
    buf_printf(b, "sp_frange_new%s(", (om || lpoly || rpoly) ? "_o" : "");
    if (lpoly) buf_printf(b, "sp_poly_frange_bound(_t%d, -HUGE_VAL)", tl);
    else if (left >= 0 && lnil) { buf_puts(b, "((void)("); emit_expr(c, left, b); buf_puts(b, "), -HUGE_VAL)"); }
    else if (left >= 0) emit_float_expr(c, left, b);
    else buf_puts(b, "(-HUGE_VAL)");
    buf_puts(b, ", ");
    if (rpoly) buf_printf(b, "sp_poly_frange_bound(_t%d, HUGE_VAL)", tr);
    else if (right >= 0 && rnil) { buf_puts(b, "((void)("); emit_expr(c, right, b); buf_puts(b, "), HUGE_VAL)"); }
    else if (right >= 0) emit_float_expr(c, right, b);
    else buf_puts(b, "HUGE_VAL");
    buf_printf(b, ", %d", excl);
    if (om || lpoly || rpoly) {
      buf_printf(b, ", %d", om);
      if (lpoly) buf_printf(b, " | sp_poly_frange_bits(_t%d, 1, 4)", tl);
      if (rpoly) buf_printf(b, " | sp_poly_frange_bits(_t%d, 2, 8)", tr);
    }
    buf_puts(b, ")");
    if (lpoly || rpoly) buf_puts(b, "; })");
    return 1;
  }
  /* ("a".."e"): the distinct string range, endpoints kept as strings (#3064) */
  if (repr_of(c, id).range == TY_STRING) {
    /* Two ends each made on the spot (`(i.to_s..j.to_s)`) were siblings in
       one C call: nothing held the one made first while the other was made,
       so a collection in between freed it, and gcc made the end before the
       begin. The begin goes into a rooted temp ahead of the end. An end that
       is a literal or a plain read is nothing to collect or to order, and a
       begin that already ran into a temp is held there. */
    int bind = left >= 0 && right >= 0 && !arg_ran_first(left, 0) &&
               !srange_end_is_plain(c, left) && !srange_end_is_plain(c, right);
    /* A read beside an end that runs something (`(s..(s = t; j.to_s))`) stays
       where the C compiler puts it, and goes into a rooted slot there: the
       other end may let go of the String it read, and the Range would carry
       an end already freed. So do two ends that each make a String though
       one is a read (a shared String is read as a copy), either of which a
       collection in the other could take. */
    int lt = 0, rt = 0;
    if (!bind && left >= 0 && right >= 0) {
      int lran = arg_ran_first(left, 0), rran = arg_ran_first(right, 0);
      int lrun = !lran && !srange_end_is_plain(c, left), rrun = !rran && !srange_end_is_plain(c, right);
      int lmk = !lran && operand_may_allocate(c, left), rmk = !rran && operand_may_allocate(c, right);
      /* a literal or a nil that makes nothing is nothing to keep */
      int lcon = !lmk && (nt_kind(nt, left) == NK_StringNode || nt_kind(nt, left) == NK_NilNode);
      int rcon = !rmk && (nt_kind(nt, right) == NK_StringNode || nt_kind(nt, right) == NK_NilNode);
      /* a local the other end does not name and cannot reach through a
         Proc still holds its String when the Range is made */
      const char *ln = nt_kind(nt, left) == NK_LocalVariableReadNode ? nt_str(nt, left, "name") : NULL;
      const char *rn = nt_kind(nt, right) == NK_LocalVariableReadNode ? nt_str(nt, right, "name") : NULL;
      int lkept = !lmk && ln && !subtree_reads_local(nt, right, ln) &&
                  !subtree_writes_local(c, right, ln) && !subtree_may_run_proc(c, right);
      int rkept = !rmk && rn && !subtree_reads_local(nt, left, rn) &&
                  !subtree_writes_local(c, left, rn) && !subtree_may_run_proc(c, left);
      if (!lran && !lcon && !lkept && (rrun || (lmk && rmk))) lt = ++g_tmp;
      if (!rran && !rcon && !rkept && (lrun || (lmk && rmk))) rt = ++g_tmp;
    }
    /* --share-strings: an endpoint the Range keeps as its handle holds NULL
       in its byte field, the handle its only holder: a reader takes the
       bytes off the handle where it uses them (sp_srange_first /
       sp_srange_last), so a change in place to the endpoint's String shows
       through the Range (#8321) */
    int lsh = srange_end_keeps(c, id, left);
    int rsh = srange_end_keeps(c, id, right);
    if (lsh || rsh) {
      int hl = ++g_tmp, hr = ++g_tmp, tl = ++g_tmp, tr = ++g_tmp;
      buf_puts(b, "({ ");
      emit_srange_end_h(c, left, lsh, hl, tl, b);
      emit_srange_end_h(c, right, rsh, hr, tr, b);
      buf_printf(b, "sp_srange_new_h(_t%d, _t%d, %d, _t%d, _t%d); })", tl, tr, excl, hl, hr);
      return 1;
    }
    int bt = bind ? ++g_tmp : 0;
    if (bind) {
      buf_printf(b, "({ const char *_t%d = ", bt);
      emit_str_expr_nilable(c, left, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", bt);
    }
    if (lt || rt) buf_puts(b, "({ ");
    if (lt) buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT(_t%d); ", lt, lt);
    if (rt) buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT(_t%d); ", rt, rt);
    buf_puts(b, "sp_srange_new(");
    if (bind) buf_printf(b, "_t%d", bt);
    else if (left >= 0) {
      if (lt) buf_printf(b, "(_t%d = ", lt);
      emit_str_expr_nilable(c, left, b);
      if (lt) buf_puts(b, ")");
    }
    else buf_puts(b, "NULL");
    buf_puts(b, ", ");
    if (right >= 0) {
      if (rt) buf_printf(b, "(_t%d = ", rt);
      emit_str_expr_nilable(c, right, b);
      if (rt) buf_puts(b, ")");
    }
    else buf_puts(b, "NULL");
    buf_printf(b, ", %d)", excl);
    if (bind || lt || rt) buf_puts(b, "; })");
    return 1;
  }
  /* sp_Range holds sp_int bounds, so a Range OBJECT over user objects has
     nowhere to live; emit_int_expr below would hand a pointer to an sp_int
     parameter and the C compiler would reject it. Comparable#clamp with such
     a range still works INLINE (`x.clamp(lo..hi)`, and the one-sided forms):
     there the bounds are folded straight into the comparison and no Range is
     ever built. Only materializing one is out. #2558 */
  {
    Repr lr = repr_of(c, left);
    TyKind lt = lr.as_ty;
    Repr rr2 = repr_of(c, right);
    TyKind rt2 = rr2.as_ty;
    /* sp_Range's bounds are sp_int, so a Bignum bound has nowhere to live;
       emit_int_expr below would hand a pointer to an sp_int parameter and
       the C compiler would reject it with a warning-shaped diagnostic far
       from the cause. Name the limitation instead. (#3058) */
    if (lr.big || rr2.big) {
      unsupported_feature(c, id,
        "a Range with a Bignum bound cannot be built: a Range is an unboxed "
        "value with sp_int bounds, so it has nowhere to hold one. Comparing "
        "against the bounds directly (`x >= lo && x < hi`) needs no Range and "
        "does work (see docs/limitations.md)");
      return 1;
    }
    /* A Time bound beside an open end is no Integer either: the one-sided
       builder below took `(Time.at(0)..)` for an Integer Range and the C did
       not compile (#8526). (Two Time bounds convert at run time, which
       raises CRuby's TypeError where the Range is iterated.) */
    int l_open = left < 0 || nt_kind(nt, left) == NK_NilNode;
    int r_open = right < 0 || nt_kind(nt, right) == NK_NilNode;
    if ((lt == TY_TIME && r_open) || (rt2 == TY_TIME && l_open)) {
      unsupported_feature(c, id,
        "a Range of Time objects cannot be built: a Range is an unboxed value "
        "with sp_int bounds, so it has nowhere to hold them. Comparing against "
        "the bounds directly (`t >= lo && t < hi`) needs no Range and does work "
        "(see docs/limitations.md)");
      return 1;
    }
    if (ty_is_object(lt) || ty_is_object(rt2)) {
      const char *ocn = NULL;
      int oci = ty_is_object(lt) ? ty_object_class(lt) : ty_object_class(rt2);
      if (oci >= 0 && oci < c->nclasses) ocn = class_ruby_name(c, oci);
      static char rbuf[400];
      snprintf(rbuf, sizeof rbuf,
               "a Range of %s objects cannot be built: a Range is an unboxed value "
               "with sp_int bounds, so it has nowhere to hold them. Passing the "
               "bounds directly (`x.clamp(lo..hi)`, `x.clamp(lo, hi)`, or a one-sided "
               "`lo..` / `..hi`) needs no Range and does work (see docs/limitations.md)",
               ocn ? ocn : "user");
      unsupported_feature(c, id, rbuf);
    }
  }
  /* An explicitly written `nil` bound is the same range as the omitted one
     (`nil..5` == `..5`), and an infinite Float bound cannot be converted to
     sp_int at all -- passing the emitted (1.0/0.0) through the sp_int
     parameter is UB, which is where the arbitrary integer came from. Both
     spellings take the unbounded sentinel (#3670). */
  int left_unbounded = left < 0 ||
      (nt_kind(nt, left) == NK_NilNode) ||
      lazy_endpoint_is_infinite(c, left);
  /* `-Float::INFINITY` is the negation call around the constant */
  if (!left_unbounded && left >= 0 && nt_kind(nt, left) == NK_CallNode &&
      nt_str(nt, left, "name") && sp_streq(nt_str(nt, left, "name"), "-@") &&
      lazy_endpoint_is_infinite(c, nt_ref(nt, left, "receiver")))
    left_unbounded = 1;
  int right_unbounded = right < 0 ||
      (nt_kind(nt, right) == NK_NilNode) ||
      lazy_endpoint_is_infinite(c, right);
  /* A Float end over an Integer begin, (1..2.5), or a boxed end that may be
     one: the Range walks from the Integer as CRuby does and keeps the end
     as written for the readers that answer it (sp_range_new_fend / _pend);
     the end was truncated, so (1...2.5) walked only 1 and #end read 2. */
  Repr rt_endr = repr_of(c, !right_unbounded && right >= 0 ? right : -1);
  TyKind rt_end = (!right_unbounded && right >= 0) ? rt_endr.as_ty : TY_UNKNOWN;
  int fend = rt_end == TY_FLOAT, pend = rt_endr.kind == RK_BOXED;
  /* an open side, written or arriving at run time (a boxed bound, an Integer
     that may be nil), is the Range's nobeg / noend flag: sp_range_new_o takes
     each bound as an sp_oint. A bound known present is the plain one. */
  int lo_o = left_unbounded || (left >= 0 && (repr_of(c, left).kind == RK_BOXED || (comp_ntype(c, left) == TY_INT && node_has_oint_form(c, left))));
  int hi_o = right_unbounded || (right >= 0 && !fend && !pend && comp_ntype(c, right) == TY_INT && node_has_oint_form(c, right));
  /* Ruby runs the begin before the end; C does not order a call's
     arguments (gcc ran `at(3)` before `at(1)` in `x&.slice(at(1)..at(3))`
     on a boxed x). Where an end can do something, the two go to temps in
     their order first. */
  int in_order = !left_unbounded && !right_unbounded &&
                 ((subtree_has_side_effect(c, left) && !range_end_literal(nt, right)) ||
                  (subtree_has_side_effect(c, right) && !range_end_literal(nt, left)));
  Buf lo, hi;
  memset(&lo, 0, sizeof lo); memset(&hi, 0, sizeof hi);
  const char *fn;
  if (!fend && !pend && (lo_o || hi_o)) {
    fn = "sp_range_new_o";
    if (left_unbounded) buf_puts(&lo, "sp_oint_nil()");
    else if (repr_of(c, left).kind == RK_BOXED) { buf_puts(&lo, "sp_range_lo_bound("); emit_expr(c, left, &lo); buf_puts(&lo, ")"); }
    else emit_oint_expr(c, left, TY_INT, &lo);
    if (right_unbounded) buf_puts(&hi, "sp_oint_nil()");
    else emit_oint_expr(c, right, TY_INT, &hi);
    emit_range_build(b, fn, &lo, &hi, excl, in_order);
    return 1;
  }
  fn = fend ? "sp_range_new_fend" : pend ? "sp_range_new_pend" : "sp_range_new";
  /* a nil that arrives at run time -- boxed, or an Integer slot's nil --
     is the absent bound too; read as a number, `lo..x` with x nil was the
     empty `lo..0` */
  /* A boxed begin before a Float end keeps the integer representation,
     which holds the end truncated: a beginless (x nil) such range would
     answer for `..2` where CRuby has `..2.5`. Say so at run time. */
  if (!left_unbounded && !right_unbounded && repr_of(c, left).kind == RK_BOXED &&
      comp_ntype(c, right) == TY_FLOAT) {
    buf_puts(&lo, "sp_range_lo_float_end("); emit_expr(c, left, &lo); buf_puts(&lo, ")");
  }
  /* a boxed begin holding a Float cannot be an Integer range's: it says so
     (sp_range_lo_bound), where the conversion truncated it */
  else if (!left_unbounded && repr_of(c, left).kind == RK_BOXED) {
    buf_puts(&lo, "sp_range_lo_bound("); emit_expr(c, left, &lo); buf_puts(&lo, ")");
  }
  /* (a Float or boxed end: the begin as an sp_oint, nil for an open one) */
  else if (left_unbounded) buf_puts(&lo, pend ? "sp_oint_nil()" : "SP_RANGE_NO_BEGIN");
  else if (pend) emit_oint_expr(c, left, TY_INT, &lo);
  else emit_int_expr(c, left, &lo);
  if (fend) emit_float_expr(c, right, &hi);
  else if (pend) emit_expr(c, right, &hi);
  else emit_int_expr(c, right, &hi);
  emit_range_build(b, fn, &lo, &hi, excl, in_order);
  return 1;
  return 0;
}
/* ...the arm (emit_expr_node's arms, in their order): the Range `s.upto(t)`
   walks (desugar_string_upto) tests an s that can be nil, or is boxed, once
   both ends have run (sp_srange_upto_recv) */
static int emit_range_expr(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *ty) {
  if (!(sp_streq(ty, "RangeNode"))) return 0;
  int left = nt_ref(nt, id, "left");
  Repr r = repr_of(c, id);
  if (!nt_int(nt, id, "made_upto", 0) || left < 0 || r.kind == RK_BOXED || r.range != TY_STRING ||
      (repr_of(c, left).kind != RK_BOXED && recv_never_nil(c, left)))
    return emit_range_literal(c, id, b, nt);
  buf_puts(b, "sp_srange_upto_recv(");
  emit_range_literal(c, id, b, nt);
  buf_puts(b, ")");
  return 1;
}

/* The tail of a multi-statement `(a; b)`, and the type its C value has. An
   empty `[]` / `{}` tail of a sequence the analysis typed poly is built
   boxed: its own type reads UNKNOWN, and the raw IntArray / StrPolyHash it
   emits for that was dropped by the consumer's box. */
static TyKind emit_paren_tail(Compiler *c, int paren, int tail, Buf *b) {
  TyKind tt = repr_of(c, tail).as_ty;
  if (tt == TY_UNKNOWN && repr_of(c, paren).kind == RK_BOXED && an_empty_container_kind(c, tail)) {
    emit_boxed(c, tail, b);
    return TY_POLY;
  }
  if (oint_kind(tt) && node_is_oint(c, paren)) emit_oint_expr(c, tail, tt, b);
  else emit_expr(c, tail, b);
  return tt;
}
int g_ie_rself_on;
static int g_ie_rself_raw;
static void emit_self_expr(Compiler *c, int id, Buf *b) {
  if (!g_ie_rself_raw && nt_int(c->nt, id, "ie_rself", 0)) {
    if (g_ie_rself_on) { buf_puts(b, "_sp_rself"); return; }
    Buf tb; memset(&tb, 0, sizeof tb);
    g_ie_rself_raw = 1; emit_self_expr(c, id, &tb); g_ie_rself_raw = 0;
    emit_boxed_text(c, an_self_node_type(c, id), tb.p ? tb.p : "", b);
    free(tb.p);
    return;
  }
  /* a statement of a class or module body: the class object itself */
  { int cb = self_class_body(c, id);
    /* in a `class << X` body: X's singleton class */
    if (cb >= 0 && nt_int(c->nt, id, "sclass_value", 0)) { buf_printf(b, "((sp_Class){sp_sclass_id(%d)})", cb); return; }
    if (cb >= 0) { buf_printf(b, "((sp_Class){%d})", cb); return; } }
  /* top-level self is main, which no C scope holds (#4926) */
  if (self_is_main_raw(c, id)) { buf_puts(b, "sp_main_self()"); return; }
  /* self inside a method added to Array (desugar_builtin_reopen_self_calls
     marks it): the method holds self boxed, read here as the poly array */
  if (nt_int(c->nt, id, "ary_self", 0) && g_self && sp_streq(g_self, "self") &&
      repr_of(c, id).as_ty == TY_POLY_ARRAY) {
    buf_puts(b, "sp_poly_to_poly_array(self)");
    return;
  }
  if (repr_self_shared(c, id)) {
    Repr r = repr_of(c, id);
    if (r.demand) buf_puts(b, g_self);
    else emit_strbuf_slot_read(c, id, r, g_self, b);
    return;
  }
  /* An inherited method inlined at a call site (a yielding one, say) binds
     self to the receiver temp, typed as the receiver's class, while the
     body's own typing reads self as the defining class: the standalone
     function takes `sp_Parent *self`. A value use of self (a temp, a local,
     an argument) is a Parent slot, so spell the upcast C needs (#6769). */
  if (g_emitting_class_id >= 0 && g_self_deref && sp_streq(g_self_deref, "->"))
    emit_obj_upcast_prefix(c, comp_ntype(c, id), ty_object(g_emitting_class_id), b);
  buf_puts(b, g_self); return;   /* self is the object reference (pointer) */
}

/* A multiple assignment's targets' receivers and indexes that run code, or
   that read what the right side changes, go first, as Ruby evaluates them
   before the values: all the targets', in Ruby's order (the splat's, the
   rights' and those nested in a `(a, b)` too). Each is held in a rooted temp
   that its node reads through the override table; answers how many were
   bound. */
static int masgn_hold_target_parts(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int held = 0;
  int nf = masgn_flat_targets(nt, id, NULL, 0);
  int *flat = nf > 0 ? alloca(sizeof(int) * (size_t)nf) : NULL;
  masgn_flat_targets(nt, id, flat, 0);
  for (int i = 0; i < nf; i++) {
    NodeKind tk = nt_kind(nt, flat[i]);
    if (tk != NK_IndexTargetNode && tk != NK_CallTargetNode) continue;
    int args = tk == NK_IndexTargetNode ? nt_ref(nt, flat[i], "arguments") : -1, an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    int parts[2] = { nt_ref(nt, flat[i], "receiver"), an == 1 ? av[0] : -1 };
    for (int p = 0; p < 2; p++) {
      int v = parts[p];
      if (v < 0 || g_n_argov >= MAX_ARG_OVERRIDE ||
          !(subtree_has_side_effect(c, v) || masgn_value_changes(c, id, v))) continue;
      Repr rp = repr_of(c, v);
      TyKind vt = rp.as_ty;
      if (vt == TY_UNKNOWN || vt == TY_NIL || vt == TY_VOID ||
          rp.kind == RK_STRUCT || rp.kind == RK_VOBJ) continue;
      Buf vb; memset(&vb, 0, sizeof vb);
      emit_expr(c, v, &vb);
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, vt, g_pre);
      buf_printf(g_pre, " _t%d = %s; ", t, vb.p ? vb.p : "");
      emit_gc_root_tmp(c, vt, t, g_pre);
      buf_puts(g_pre, "\n");
      free(vb.p);
      view_bind(v, "_t%d", t);
      held++;
    }
  }
  return held;
}
/* A forwarded block call has the node's representation, including its nil
   bit. The proc ABI returns a box; the optional conversion preserves nil
   until the consuming operation unwraps the value. */
static void emit_forwarded_block_value(Compiler *c, int id, Buf *b) {
  TyKind ty = repr_of(c, id).as_ty;
  int optional = oint_kind(ty) && node_is_oint(c, id);
  if (optional) buf_printf(b, "%s(", oint_unbox(ty));
  emit_yield_proc_call(c, nt_ref(c->nt, id, "arguments"),
                       optional ? TY_POLY : ty != TY_UNKNOWN ? ty : g_yield_slot_ty,
                       b, 0, 1);
  if (optional) buf_puts(b, ")");
}

static void emit_expr_node(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  NodeKind ty_k = nt_kind(nt, id);
  if (!ty) unsupported(c, id, "expression (no type)");

  /* Hoisted call argument: substitute the rooted temp (see emit_args_filled). */
  for (int i = g_n_argov - 1; i >= 0; i--)
    if (g_argov_node[i] == id) {
      /* (--share-strings: a handle bound by ran_first_bind_read is read as
         its String, and as the handle where the node is viewed as one) */
      int th = comp_ntype(c, id) == TY_STRBUF ? ran_first_read_handle(id) : -1;
      if (th >= 0) buf_printf(b, "_t%d", th);
      else buf_puts(b, g_argov_text[i]);
      /* a reader call the shim has substituted with its shadow, written back */
      return;
    }

  if (ty_k == NK_IntegerNode) {
    const char *bigval = nt_str(nt, id, "bigval");
    if (bigval) {
      /* An Integer is immutable, so one slot per distinct literal serves
         every use: the string was re-parsed and a Bignum re-allocated at
         each one, every time the line ran (#4637). */
      buf_printf(b, "sp_bigl_get(%d)", bigl_intern(bigval));
      return;
    }
    emit_int_lit(b, nt_int(nt, id, "value", 0)); return;
  }
  if (ty_k == NK_FloatNode) { const char *v = nt_content(nt, id); buf_puts(b, v ? v : "0.0"); return; }
  if (ty_k == NK_ImaginaryNode) {
    int num = nt_ref(nt, id, "numeric");
    NodeKind numk = nt_kind(nt, num);
    buf_puts(b, "((sp_Complex){0.0, (sp_float)(");
    if (num >= 0) emit_expr(c, num, b);
    else buf_puts(b, "0");
    /* 3.5i marks the imaginary component Float-classed; 4i stays Integer */
    buf_printf(b, "), %d})", numk == NK_FloatNode ? 2 : 0);
    return;
  }
  if (ty_k == NK_RationalNode) {
    const char *rn = nt_str(nt, id, "rat_num");
    const char *rd = nt_str(nt, id, "rat_den");
    int big = (int)nt_int(nt, id, "rat_big", 0);
    if (big) {
      /* A part past sp_int: the big Rational, as Rational(10**30, 3) builds
         it, each big part from its shared literal slot */
      buf_puts(b, "sp_box_brat(");
      for (int k = 0; k < 2; k++) {
        const char *part = k ? rd : rn;
        if (k) buf_puts(b, ", ");
        if (big & (1 << k)) buf_printf(b, "sp_bigl_get(%d)", bigl_intern(part));
        else buf_printf(b, "sp_bigint_new_int(%sLL)", part);
      }
      buf_puts(b, ")");
      return;
    }
    buf_printf(b, "((sp_Rational){%sLL, %sLL})", rn ? rn : "0", rd ? rd : "1");
    return;
  }
  if (ty_k == NK_StringNode) {
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_src(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0,
                         (int)nt_int(nt, id, "fzl", 0));
    return;
  }
  if (ty_k == NK_SourceFileNode) {
    /* __FILE__ is a literal of its file: frozen under that file's pragma. */
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_n(b, sc ? sc : "", sc ? strlen(sc) : 0,
                       (int)nt_int(nt, id, "fzl", 0));
    return;
  }
  if (ty_k == NK_SourceLineNode) {
    buf_printf(b, "%lld", (long long)nt_int(nt, id, "start_line", 0));
    return;
  }
  if (ty_k == NK_SourceEncodingNode) {
    buf_puts(b, "sp_box_encoding(sp_encoding_utf8())"); return;
  }
  if (ty_k == NK_RegularExpressionNode) {
    int ri = re_lit_index(c, id);
    if (ri >= 0) buf_printf(b, "sp_re_pat_%d", ri);
    else buf_puts(b, "NULL");
    return;
  }
  if (ty_k == NK_InterpolatedStringNode) { emit_interp(c, id, b); return; }
  if (ty_k == NK_XStringNode) {
    /* backtick content is a command argument, never a user-visible string */
    const char *sc = nt_str(nt, id, "content");
    buf_puts(b, "sp_backtick(");
    emit_str_literal_n(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0, 0);
    buf_puts(b, ")");
    return;
  }
  if (ty_k == NK_InterpolatedXStringNode) {
    buf_puts(b, "sp_backtick(");
    emit_interp(c, id, b);
    buf_puts(b, ")");
    return;
  }
  if (ty_k == NK_InterpolatedSymbolNode) {
    buf_puts(b, "sp_sym_intern("); emit_interp(c, id, b); buf_puts(b, ")");
    return;
  }
  if (ty_k == NK_TrueNode)  { buf_puts(b, "1"); return; }
  if (ty_k == NK_LoadNode)  { emit_load_call(c, id, b); return; }
  if (ty_k == NK_FalseNode) { buf_puts(b, "0"); return; }
  if (ty_k == NK_NilNode)   { buf_puts(b, "0"); return; }  /* default in numeric/bool context */
  if (ty_k == NK_SymbolNode) {
    /* the node's own byte length: a symbol literal may hold a NUL (#nul) */
    const char *svl = nt_str(nt, id, "value");
    int sid = comp_sym_intern_n(c, svl ? svl : "", svl ? nt_str_len(nt, id, "value") : 0);
    buf_printf(b, "((sp_sym)%d)", sid);
    return;
  }
  if (ty_k == NK_LambdaNode) { emit_proc_literal(c, id, b); return; }
  if (ty_k == NK_CaseNode) { emit_case_expr(c, id, b); return; }
  if (ty_k == NK_CaseMatchNode) {
    /* case/in as a value: declare a result temp, emit the match into g_pre
       with each arm assigning its body value to it, then yield the temp. */
    TyKind rt = repr_of(c, id).as_ty;
    if (rt == TY_UNKNOWN || rt == TY_VOID || rt == TY_NIL) rt = TY_POLY;
    int cr = ++g_tmp;
    int ro = cond_res_oint(c, id, rt);
    emit_indent(g_pre, g_indent);
    emit_res_ctype(c, rt, ro, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", cr, rt == TY_RANGE ? "(sp_Range){0}" : res_zero(c, rt, ro));
    if (ty_gc_holds_refs(c, rt)) { emit_indent(g_pre, g_indent); emit_gc_root_tmp_refs(c, rt, cr, g_pre); buf_puts(g_pre, "\n"); }
    emit_case_match(c, id, g_pre, g_indent, 0, cr);
    buf_printf(b, "_t%d", cr);
    return;
  }

  if (ty_k == NK_MatchWriteNode) {
    /* `/(?<n>..)/ =~ str`: run the match (setting the match registers), bind
       each named group to its local (NULL = nil when it did not participate),
       and yield the `=~` result (match index, or nil). */
    int call = nt_ref(nt, id, "call");
    int recv = call >= 0 ? nt_ref(nt, call, "receiver") : -1;
    int reidx = re_lit_index(c, recv);
    int args = call >= 0 ? nt_ref(nt, call, "arguments") : -1;
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (reidx < 0 || ac < 1) { unsupported(c, id, "named-capture =~ (non-literal regexp)"); return; }
    int tcount = 0; const int *tv = nt_arr(nt, id, "targets", &tcount);
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = sp_re_match_poly(sp_re_pat_%d, ", t, reidx);
    emit_str_expr_nilable(c, av[0], b);   /* nil subject: no match, as =~ */
    buf_puts(b, "); ");
    for (int ti = 0; ti < tcount; ti++) {
      const char *tnm = nt_str(nt, tv[ti], "name");
      if (!tnm) continue;
      /* (one representation: the capture, new bytes, into the variable's face) */
      Scope *nsc = comp_scope_of(c, tv[ti]);
      LocalVar *nlv = nsc ? scope_local(nsc, rename_local(tnm)) : NULL;
      int one_to = nlv && (nlv->type == TY_STRING || nlv->type == TY_STRBUF);
      if (one_to) buf_printf(b, "lv_%s = SP_STR_TO(lv_%s, sp_re_named_capture(sp_re_pat_%d, ", rename_local(tnm), rename_local(tnm), reidx);
      else buf_printf(b, "lv_%s = sp_re_named_capture(sp_re_pat_%d, ", rename_local(tnm), reidx);
      emit_str_literal(b, tnm);
      buf_puts(b, one_to ? ")); " : "); ");
    }
    buf_printf(b, "_t%d; })", t);
    return;
  }
  if (emit_range_expr(c, id, b, nt, ty)) return;
  if (emit_local_ivar_write_expr(c, id, b, nt, ty)) return;
  if (ty_k == NK_WhileNode || ty_k == NK_UntilNode) {
    /* A loop in value position evaluates to nil, unless a valued `break`
       supplies a value. When it can, declare a poly result temp defaulting to
       nil, point the break-value target at it while emitting the loop, and yield
       the temp. */
    if (loop_has_valued_break(c, nt_ref(nt, id, "statements"))) {
      int tr = ++g_tmp;
      const char *saved_bv = g_loop_break_var;
      int saved_rp = g_ie_res_poly;
      char bvbuf[24]; snprintf(bvbuf, sizeof bvbuf, "_t%d", tr);
      g_loop_break_var = bvbuf; g_ie_res_poly = 1;
      if (g_pre) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tr, tr);
        emit_while(c, id, g_pre, g_indent, ty_k == NK_UntilNode);
        buf_printf(b, "_t%d", tr);
      }
      else {
        /* No prelude available (e.g. inside another expression's prelude): fall
           back to a GCC statement expression, mirroring BeginNode. */
        buf_puts(b, "({ ");
        buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tr, tr);
        emit_while(c, id, b, 0, ty_k == NK_UntilNode);
        buf_printf(b, "_t%d; })", tr);
      }
      g_loop_break_var = saved_bv; g_ie_res_poly = saved_rp;
      return;
    }
    /* No valued break: run for side effects inside a statement-expression and
       yield the boxed nil. */
    buf_puts(b, "({ ");
    emit_while(c, id, b, 0, ty_k == NK_UntilNode);
    buf_puts(b, "sp_box_nil(); })");
    return;
  }
  if (ty_k == NK_ForNode) {
    /* A for loop in value position evaluates to its collection, or to a
       `break`'s value (nil for a bare one). The collection -- for a range
       literal, its endpoints -- is evaluated once into temps that the loop
       then reads through the argument overrides. */
    int coll = nt_paren_single(c->nt, nt_ref(nt, id, "collection"));
    int subs[2] = { -1, -1 }, ns = 0;
    if (coll >= 0 && nt_kind(nt, coll) == NK_RangeNode && repr_of(c, coll).range == TY_INT) {
      subs[ns++] = nt_ref(nt, coll, "left");
      subs[ns++] = nt_ref(nt, coll, "right");
    }
    else subs[ns++] = coll;
    if (g_n_argov + ns > MAX_ARG_OVERRIDE) unsupported(c, id, "for loop value nested too deep");
    Buf lb; memset(&lb, 0, sizeof lb);
    int ind = g_pre ? g_indent : 0;
    int sv_argov = g_n_argov;
    for (int i = 0; i < ns; i++) {
      if (subs[i] < 0) continue;
      TyKind st = repr_of(c, subs[i]).as_ty;
      int ts = ++g_tmp;
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_expr(c, subs[i], &eb);
      emit_indent(&lb, ind);
      buf_printf(&lb, "%s _t%d = %s; ", c_type_name(st), ts, eb.p ? eb.p : "0");
      emit_gc_root_tmp(c, st, ts, &lb);
      buf_puts(&lb, "\n");
      free(eb.p);
      view_bind(subs[i], "_t%d", ts);
    }
    int tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    if (coll >= 0) emit_boxed(c, coll, &rb);
    emit_indent(&lb, ind);
    buf_printf(&lb, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tr, rb.p ? rb.p : "sp_box_nil()", tr);
    free(rb.p);
    const char *saved_bv = g_loop_break_var, *saved_bs = g_brk_ser_var;
    int saved_rp = g_ie_res_poly;
    char bvbuf[24]; snprintf(bvbuf, sizeof bvbuf, "_t%d", tr);
    g_loop_break_var = bvbuf; g_ie_res_poly = 1; g_brk_ser_var = NULL;
    emit_for(c, id, &lb, ind);
    g_loop_break_var = saved_bv; g_ie_res_poly = saved_rp; g_brk_ser_var = saved_bs;
    view_unbind(sv_argov);
    if (g_pre) {
      if (lb.p) buf_puts(g_pre, lb.p);
      buf_printf(b, "_t%d", tr);
    }
    else buf_printf(b, "({ %s_t%d; })", lb.p ? lb.p : "", tr);
    free(lb.p);
    return;
  }
  if (ty_k == NK_IndexOrWriteNode || ty_k == NK_IndexAndWriteNode) {
    int is_or2 = ty_k == NK_IndexOrWriteNode;
    int ir = nt_ref(nt, id, "receiver");
    int ia = nt_ref(nt, id, "arguments");
    int iv = nt_ref(nt, id, "value");
    int iac = 0; const int *iav = ia >= 0 ? nt_arr(nt, ia, "arguments", &iac) : NULL;
    if (iac != 1 || ir < 0 || iv < 0) { unsupported(c, id, "index-or/and-write (expr)"); return; }
    Repr irr = repr_of(c, ir);
    TyKind irt = irr.as_ty;
    int ta2 = ++g_tmp, tb2 = ++g_tmp, tc2 = ++g_tmp;
    if (irr.elem == TY_POLY) {
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
      emit_guarded_poly_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_PolyArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (irr.elem == TY_INT || irr.elem == TY_FLOAT) {
      /* the element is read with its nil; the value written is its oint
         (a nil value sets the element's bit) and the expression's value is
         the element's plain scalar */
      const char *k = irr.elem == TY_INT ? "Int" : "Float";
      TyKind et = irr.elem;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; %s _t%d = sp_%sArray_oget(_t%d, _t%d);", oint_ctype(et), tc2, k, ta2, tb2);
      buf_printf(b, " if (%s_t%d.nil) { ", is_or2 ? "" : "!", tc2);
      emit_guarded_oint_slot_assign(c, iv, et, tc2, b);
      buf_printf(b, "; sp_%sArray_oset(_t%d, _t%d, _t%d); } _t%d%s; })", k, ta2, tb2, tc2, tc2, node_is_oint(c, id) ? "" : ".v");
    }
    else if (irr.elem == TY_STRING) {
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; const char *_t%d = SP_STR_B(sp_StrArray_get(_t%d, _t%d));", tc2, ta2, tb2);
      buf_printf(b, " if (%s_t%d) { ", is_or2 ? "!" : "", tc2);
      emit_guarded_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_StrArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (ty_is_hash(irt)) {
      const char *hn = ty_hash_cname(irt);
      TyKind kt = ty_hash_key(irt);
      TyKind vt = ty_hash_val(irt);
      if (!hn) { unsupported(c, id, "index-or/and-write (expr, unknown hash)"); return; }
      /* The receiver's temp is the hash's only holder once the key or the
         value drops it, and a key that can allocate has no holder at all:
         root the receiver when either can -- a call, a write -- and the key
         when it can allocate, as the statement form does. */
      int drops = subtree_has_side_effect(c, iav[0]) || subtree_has_side_effect(c, iv);
      buf_printf(b, "({ %s _t%d = ", c_type_name(irt), ta2); emit_expr(c, ir, b); buf_puts(b, "; ");
      if (subtree_may_allocate(c->nt, ir) || drops) { emit_gc_root_tmp(c, irt, ta2, b); buf_puts(b, " "); }
      /* a key that may be nil or of another class: held boxed, the read
         misses on it and the store converts it (hash_opw_okey) */
      int ok = hash_opw_okey(c, iav[0], irt);
      char rk[96], sk[64], tbn[24];
      snprintf(tbn, sizeof tbn, "_t%d", tb2);
      if (ok) {
        buf_printf(b, "sp_RbVal _t%d = ", tb2); emit_boxed(c, iav[0], b); buf_puts(b, "; ");
        if (operand_may_allocate(c, iav[0])) { emit_gc_root_tmp(c, TY_POLY, tb2, b); buf_puts(b, " "); }
        Buf okb; memset(&okb, 0, sizeof okb); emit_opw_okey_read(tbn, &okb);
        snprintf(rk, sizeof rk, "%s", okb.p ? okb.p : "sp_oint_nil()"); free(okb.p);
        snprintf(sk, sizeof sk, "sp_poly_hkey_i(_t%d)", tb2);
      }
      else {
        buf_printf(b, "%s _t%d = ", c_type_name(kt), tb2); emit_hash_key(c, iav[0], kt, b); buf_puts(b, "; ");
        if (operand_may_allocate(c, iav[0]) && needs_root(kt)) { emit_gc_root_tmp(c, kt, tb2, b); buf_puts(b, " "); }
        snprintf(rk, sizeof rk, "_t%d", tb2); snprintf(sk, sizeof sk, "_t%d", tb2);
      }
      const char *rs = ok ? "_okey" : "";
      if (vt == TY_POLY) {
        buf_printf(b, "sp_RbVal _t%d = sp_%sHash_get(_t%d, _t%d);", tc2, hn, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, _t%d, %s_t%d%s); } _t%d; })", hn, ta2, tb2, ONE_HV_O(hn), tc2, ONE_HV_C(hn), tc2);
      }
      else if (oint_kind(vt)) {
        /* an Integer or Float slot is read with its nil (a miss, a nil
           default) and tested on it; the store takes the plain value (a nil
           value widened the hash to poly), and the expression answers the
           slot: its oint where the consumer takes one, else the value */
        buf_printf(b, "%s _t%d = sp_%sHash_oget%s(_t%d, %s);", oint_ctype(vt), tc2, hn, rs, ta2, rk);
        buf_printf(b, " if (%s_t%d.nil) { _t%d = %s(", is_or2 ? "" : "!", tc2, tc2, oint_of(vt));
        { Buf rvb; memset(&rvb, 0, sizeof rvb);
          Buf *svp = g_pre; g_pre = b;
          emit_expr(c, iv, &rvb);
          g_pre = svp;
          buf_puts(b, rvb.p ? rvb.p : "0");
          free(rvb.p); }
        buf_printf(b, "); sp_%sHash_set(_t%d, %s, _t%d.v); } ", hn, ta2, sk, tc2);
        if (node_is_oint(c, id)) buf_printf(b, "_t%d; })", tc2);
        else buf_printf(b, "%s(_t%d); })", oint_arg(vt), tc2);
      }
      else {
        buf_printf(b, "%s _t%d = sp_%sHash_get%s(_t%d, %s);", c_type_name(vt), tc2, hn, rs, ta2, rk);
        buf_puts(b, " if (");
        emit_slot_nil_test(c, vt, tc2, is_or2, b);
        buf_puts(b, ") { ");
        /* (one-String mode: the String stored is the answer's own -- a new
           one gets its handle first, and the view of it is both what the
           Hash takes, its owner, and what the expression answers; the value
           is taken in either face) */
        if (ONE_HV_O(hn)[0]) {
          Buf rvb; memset(&rvb, 0, sizeof rvb);
          Buf *svp = g_pre; g_pre = b;
          emit_expr(c, iv, &rvb);
          g_pre = svp;
          buf_printf(b, "_t%d = SP_STR_B(SP_STR_HDL(%s))", tc2, rvb.p ? rvb.p : "NULL");
          free(rvb.p);
        }
        else emit_guarded_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, %s, %s_t%d%s); } _t%d; })", hn, ta2, sk, ONE_HV_O(hn), tc2, ONE_HV_C(hn), tc2);
      }
    }
    else if (irt == TY_POLY) {
      /* TY_POLY receiver: dispatch via sp_poly_get/set based on key type */
      TyKind kt2 = comp_ntype(c, iav[0]);
      if (kt2 == TY_INT && poly_int_key_boxed(c, iav[0])) kt2 = TY_POLY;   /* its nil rides boxed */
      buf_printf(b, "({ sp_RbVal _t%d = ", ta2); emit_expr(c, ir, b);
      buf_puts(b, "; ");
      if (kt2 == TY_INT) {
        buf_printf(b, "sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_poly_arr_set_hash(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else if (kt2 == TY_STRING) {
        buf_printf(b, "const char *_t%d = ", tb2); emit_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_get_str(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_poly_set_str(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else {
        /* poly key fallback: box the key, look up polymorphically, set via poly */
        buf_printf(b, "sp_RbVal _t%d = ", tb2); emit_boxed(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_index_poly(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_poly_set_poly(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
    }
    else {
      unsupported(c, id, "index-or/and-write (expr, unsupported recv type)");
    }
    return;
  }
  if (ty_k == NK_YieldNode) {
    /* Lowered self-recursive yield method: `yield` calls the synthetic __yblk__ proc. */
    if (g_current_scope_is_lowered) {
      int yargs = nt_ref(nt, id, "arguments");
      int yargc = 0; const int *yargv = yargs >= 0 ? nt_arr(nt, yargs, "arguments", &yargc) : NULL;
      /* The __yblk__ proc publishes its result through the universal boxed
         return channel (_sp_proc_poly_ret); call it for effect, then take the
         raw carrier bits from the slot (v.i aliases the pointer/int value),
         matching this lowered method's sp_int raw-carrier ABI -- the call site
         casts back to the yield's inferred type, exactly as before. */
      buf_puts(b, "((void)");
      { Buf rb; memset(&rb, 0, sizeof rb); emit_yblk_ref(&rb);
        emit_proc_yield(c, rb.p ? rb.p : "NULL", yargc, yargv, b); free(rb.p); }
      /* `.v.i` is the raw carrier the lowered method's own sp_int ABI wants,
         and the call site casts it back. A consumer whose slot is the BOXED
         value -- a Thread body's yielded_value, which is an sp_RbVal -- needs
         the whole struct instead, so take the yield's own inferred type as the
         answer to which (#3383). */
      { Repr ytr = repr_of(c, id);
        TyKind yt = ytr.as_ty;
        if (ytr.kind == RK_BOXED) buf_puts(b, ", _sp_proc_poly_ret)");
        /* a nil answer is the Integer slot's nil, where the yield can answer one (its oint form) */
        else if (yt == TY_INT) buf_puts(b, node_is_oint(c, id) ? ", sp_poly_as_int_or_nil(_sp_proc_poly_ret))" : ", sp_poly_to_i(_sp_proc_poly_ret))");
        else if (yt == TY_UNKNOWN || yt == TY_VOID || yt == TY_NIL) buf_puts(b, ", _sp_proc_poly_ret.v.i)");
        else {
          /* a typed slot other than an Integer's reads the boxed answer by
             its own kind: an array answer landed its raw carrier bits in an
             sp_PolyArray * (the flat_map of a boxed receiver whose class
             list has a Ruby `each`, so the definition was lowered) */
          buf_puts(b, ", ");
          emit_unbox_text(c, yt, "_sp_proc_poly_ret", b);
          buf_puts(b, ")");
        } }
      return;
    }
    if (g_yield_proc_ref) {
      /* Forwarded real-proc block (caller nil-checks its &block): call the proc.
         Unbox to the inline's return-slot type (g_yield_slot_ty), which the
         analyzer may have typed concretely even though sp_proc_call is poly. */
      /* In a proc form the code AROUND the yield was typed from the inlined
         view -- `yield(1) + yield(2)` compiled its operands as sp_int -- so
         unbox to this node's own type. A tail/boxed position goes through the
         boxing helper instead, which asks for the poly form. */
      { /* the node's OWN type is what this expression position wants -- the
           inline's return slot is only the right answer when the yield IS the
           tail (a yield nested in a literal wants the element type, #3688).
           A yield the analyzer typed boxed that is NOT the tail keeps the
           boxed answer: unboxing it to the return slot read the key of
           `h[yield(x)] ||= []` as the hash the method returns. */
        Repr yr = repr_of(c, id);
        TyKind yt = yr.as_ty;
        int _ytail = 1;
        if (yr.kind == RK_BOXED || yt == TY_UNKNOWN) {
          Scope *_ys = comp_scope_of(c, id);
          int _last = _ys ? scope_body_last(c, (int)(_ys - c->scopes)) : -1;
          if (_last >= 0 && _last != id) {
            /* the tail may be an `if block_given?` whose block arm ends here */
            int _tl = -1;
            if (nt_kind(nt, _last) == NK_IfNode) {
              int _ts = nt_ref(nt, _last, "statements");
              int _tn = 0; const int *_tb = _ts >= 0 ? nt_arr(nt, _ts, "body", &_tn) : NULL;
              if (_tn > 0) _tl = _tb[_tn - 1];
            }
            if (_tl != id && block_given_tail_then_last(c, _last) != id) _ytail = 0;
          }
        }
        emit_yield_proc_call(c, nt_ref(nt, id, "arguments"),
                             ((yt != TY_UNKNOWN && yr.kind != RK_BOXED) || !_ytail)
                               ? yt : g_yield_slot_ty,
                             b, 0, 1); }
      return;
    }
    if (g_block_id < 0) {
      /* An unguarded yield with no block raises LocalJumpError. A guarded yield
         (`block_given? ? yield : x`) folds its guard to a compile-time false and
         sits inside an `if (0)`, so the raise never executes there. The trailing
         sentinel keeps the comma expression well-typed for the value position. */
      refuse_yield_lost_block(c, id);
      buf_puts(b, "(sp_exc_stage_key(sp_box_str((&(\"\\xff\" \"noreason\")[1]))), "
                  "sp_raise_cls(\"LocalJumpError\", \"no block given (yield)\"), ");
      /* the sentinel in the slot's own C type: this dead arm still has to
         assign to whatever the yield's value was typed as (a String local
         took an sp_int and the C build stopped) */
      { Repr yt_repr = repr_of(c, id);
        TyKind yt = yt_repr.as_ty;
        if (yt_repr.kind == RK_BOXED || yt == TY_UNKNOWN || yt == TY_NIL || yt == TY_VOID) buf_puts(b, "sp_box_nil())");
        else if (yt == TY_INT) buf_puts(b, "(sp_int)0)");
        else { buf_puts(b, default_value_from_compiler(c, yt)); buf_puts(b, ")"); } }
      return;
    }
    g_yield_want_oint = oint_kind(repr_of(c, id).as_ty) && node_is_oint(c, id);
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1, repr_of(c, id).as_ty);
    return;
  }
  if (is_block_call(c, id)) {           /* block.call used for its value */
    g_yield_want_oint = oint_kind(repr_of(c, id).as_ty) && node_is_oint(c, id);
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1, repr_of(c, id).as_ty);
    return;
  }
  /* `blk.nil?` / `!blk` on an inlined method's own block parameter: the
     block is spliced, not a value, so the answer is whether this site has
     one (a forwarded real proc answers by its pointer). The read itself
     would name lv_<blk>, which no inline site declares (#4477). */
  {
    const char *pn = nt_str(nt, id, "name");
    int pr = nt_ref(nt, id, "receiver");
    if (pn && (sp_streq(pn, "nil?") || sp_streq(pn, "!")) && pr >= 0 &&
        nt_kind(nt, pr) == NK_LocalVariableReadNode) {
      int pan = 0; int paa = nt_ref(nt, id, "arguments");
      if (paa >= 0) nt_arr(nt, paa, "arguments", &pan);
      const char *rn = nt_str(nt, pr, "name");
      Scope *ps = rn ? comp_scope_of(c, pr) : NULL;
      if (pan == 0 && ps && ps->yields && ps->blk_param && rn && sp_streq(ps->blk_param, rn)) {
        if (g_block_id >= 0) buf_puts(b, "0");
        else if (g_yield_proc_ref) buf_printf(b, "((%s) == NULL)", g_yield_proc_ref);
        else buf_puts(b, "1");
        return;
      }
    }
  }
  if (is_blockless_block_param_call(c, id)) {
    /* A forwarded real proc (caller nil-checks its &block): <blk>.call(args)
       invokes the proc. Otherwise this site passed no block, so the parameter
       is nil and the call raises NoMethodError, as CRuby's does. It is not a
       dead path: `def m(&b) = b.call(1)` called bare reaches it, and emitting
       only the result's default value made the call silently do nothing. */
    if (g_yield_proc_ref)
      /* In a proc form the code AROUND the yield was typed from the inlined
         view -- `yield(1) + yield(2)` compiled its operands as sp_int -- so
         unbox to this node's own type. A tail/boxed position goes through the
         boxing helper instead, which asks for the poly form. */
      { /* the node's OWN type is what this expression position wants -- the
           inline's return slot is only the right answer when the yield IS the
           tail (a yield nested in a literal wants the element type, #3688) */
        /* A POLY node type is an answer, not an absence: the inline's return
           slot is only right when the call IS the tail, and a `&blk` forwarded
           into a map wants the element (boxed) form -- unboxing to the
           method's array type did not build (#3886). */
        emit_forwarded_block_value(c, id, b); }
    else if (nt_safe_nav(nt, id)) {
      /* `blk&.call`: the nil parameter answers nil instead of raising (an
         Integer / Float answer is the node's oint, as `&.` always is) */
      TyKind at = repr_of(c, id).as_ty;
      const char *nv = nil_value(at);
      if (oint_kind(at) && node_is_oint(c, id)) buf_puts(b, oint_nil(at));
      else buf_puts(b, nv ? nv : default_value_from_compiler(c, at));
    }
    else {
      Repr br = repr_of(c, id);
      TyKind bt = br.as_ty;
      buf_printf(b, "((void)sp_nil_recv_raise(\"%s\"), %s)",
                 blockless_block_param_call_name(c, id),
                 (br.kind == RK_BOXED || bt == TY_UNKNOWN || bt == TY_NIL || bt == TY_VOID)
                   ? "sp_box_nil()" : oint_kind(bt) && node_is_oint(c, id) ? oint_nil(bt) : default_value_from_compiler(c, bt));
    }
    return;
  }
  if (nt_kind(c->nt, id) == NK_SelfNode) { emit_self_expr(c, id, b); return; }
  if (emit_ivar_cvar_gvar_expr(c, id, b, nt, ty)) return;
  if (emit_constant_expr(c, id, b, nt, ty, 0)) return;
  if (emit_defined_expr(c, id, b, nt, ty)) return;
  if (ty_k == NK_ParenthesesNode) {
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (n == 0) { buf_puts(b, "sp_box_nil()"); return; }
    /* The paren's value is its tail's, and a consumer that sees this node
       typed POLY (the union of what several inline copies of the same
       parenthesized body answer) is handed the tail's own C value: a
       `const char *` went into an sp_RbVal slot unboxed and the C build
       stopped (Dir.chdir's block splice, one copy per call site). Box the
       tail where the node's own type says the consumer expects a box. */
    if (repr_of(c, id).kind == RK_BOXED) {
      Repr pttr = repr_of(c, bd[n - 1]);
      TyKind ptt = pttr.as_ty;
      if (pttr.kind != RK_BOXED && ptt != TY_UNKNOWN && ptt != TY_VOID && ptt != TY_NIL) {
        Buf inner; memset(&inner, 0, sizeof inner);
        if (n == 1) { buf_puts(&inner, "("); emit_expr(c, bd[0], &inner); buf_puts(&inner, ")"); }
        else {
          buf_puts(&inner, "({ ");
          for (int j = 0; j < n - 1; j++) emit_stmt(c, bd[j], &inner, 0);
          emit_expr(c, bd[n - 1], &inner);
          buf_puts(&inner, "; })");
        }
        emit_boxed_text(c, ptt, inner.p ? inner.p : "0", b);
        free(inner.p);
        return;
      }
    }
    if (n == 1) {
      /* the paren's value is its tail's, oint and all */
      Repr r = repr_of(c, id);
      buf_puts(b, "(");
      if (r.opt_producer) emit_oint_expr(c, bd[0], r.as_ty, b); else emit_expr(c, bd[0], b);
      buf_puts(b, ")");
      return;
    }
    /* Multi-stmt parens: `(s1; s2; expr)` -- run leading stmts, then the
       value. The tail's emission may hoist statements into g_pre (an
       instance_exec splice, a constructed receiver): the shared prelude is
       flushed BEFORE the statement containing this paren, which would run
       the tail's hoisted code ahead of the leading statements. When the tail
       hoists, hoist the leading statements ahead of it too, preserving
       intra-paren order. */
    {
      Buf cap; memset(&cap, 0, sizeof cap);
      Buf vb; memset(&vb, 0, sizeof vb);
      Buf *sv_pre = g_pre; g_pre = &cap;
      TyKind pvt = emit_paren_tail(c, id, bd[n - 1], &vb);
      g_pre = sv_pre;
      /* A prelude of nothing but held temps, declared NULL and rooted and
         assigned where the value is built, runs no code of the tail: it
         goes ahead as it is and the sequence stays in place. */
      int held_only = prelude_is_held_decls(cap.p);
      if (held_only) buf_puts(g_pre, cap.p);
      if (!(cap.p && cap.p[0]) || held_only) {
        buf_puts(b, "({ ");
        for (int j = 0; j < n - 1; j++) {
          emit_stmt(c, bd[j], b, 0);
        }
        buf_puts(b, vb.p ? vb.p : "0");
        buf_puts(b, "; })");
      }
      else {
        for (int j = 0; j < n - 1; j++) emit_stmt(c, bd[j], g_pre, g_indent);
        buf_puts(g_pre, cap.p);
        if (is_scalar_ret(pvt) && pvt != TY_VOID && pvt != TY_NIL && pvt != TY_UNKNOWN) {
          int tpv = ++g_tmp;
          emit_indent(g_pre, g_indent);
          /* the tail answered its oint (emit_paren_tail): the temp holds it */
          if (oint_kind(pvt) && node_is_oint(c, id)) buf_puts(g_pre, oint_ctype(pvt)); else emit_ctype(c, pvt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tpv, vb.p ? vb.p : "0");
          buf_printf(b, "_t%d", tpv);
        }
        else {
          /* no usable scalar value: run the tail for effect, yield nil */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "(void)(%s);\n", vb.p ? vb.p : "0");
          buf_puts(b, "sp_box_nil()");
        }
      }
      free(cap.p); free(vb.p);
    }
    return;
  }
  if (emit_array_hash_literal_expr(c, id, b, nt, ty)) return;
  if (emit_if_expr(c, id, b, nt, ty)) return;
  if (ty_k == NK_CallNode) { emit_call(c, id, b); return; }
  if (ty_k == NK_SuperNode || ty_k == NK_ForwardingSuperNode) {
    if (!emit_super_inline(c, id, b, 0, 1)) emit_super(c, id, b);
    return;
  }
  if (emit_and_or_begin_expr(c, id, b, nt, ty)) return;

  /* MultiWriteNode as expression: execute the destructuring (side effect),
     then return the RHS value (Ruby semantics: value of `a, b = arr` is arr). */
  if (ty_k == NK_MultiWriteNode) {
    int value = nt_ref(nt, id, "value");
    /* The right-hand side runs once: a tuple's elements one by one, or any
       other right-hand side whole, are held in temps that the statement and
       the result both read through the override table. Literals are read
       again in place. */
    int en = 0;
    const int *els = nt_kind(nt, value) == NK_ArrayNode ? nt_arr(nt, value, "elements", &en) : NULL;
    for (int i = 0; i < en; i++)
      if (nt_kind(nt, els[i]) == NK_SplatNode) { els = NULL; en = 0; break; }
    int one = els ? -1 : value;
    int held = 0;
    held += masgn_hold_target_parts(c, id);
    for (int i = els ? 0 : -1; i < en; i++) {
      int v = i < 0 ? one : els[i];
      if (v < 0 || g_n_argov >= MAX_ARG_OVERRIDE) continue;
      NodeKind vk = nt_kind(nt, v);
      int vn = 0;
      if (vk == NK_NilNode || vk == NK_IntegerNode || vk == NK_FloatNode || vk == NK_SymbolNode ||
          vk == NK_TrueNode || vk == NK_FalseNode || vk == NK_StringNode ||
          ((vk == NK_ArrayNode || vk == NK_HashNode) && (nt_arr(nt, v, "elements", &vn), vn == 0)))
        continue;
      TyKind vt = vk == NK_SplatNode ? TY_POLY_ARRAY : repr_of(c, v).as_ty;
      if (vt == TY_UNKNOWN) continue;
      /* --share-strings: an element the String plan gives as the handle is
         rendered through the handle face into a rooted handle temp, which the
         write stores and whose String the value reads (ran_first_bind_read) */
      if (i >= 0 && masgn_face_elem(c, id, v)) {
        Buf hw; memset(&hw, 0, sizeof hw);
        emit_str_form_handle(c, v, &hw);
        int th = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_String *_t%d = SP_STR_HDL(%s); SP_GC_ROOT(_t%d);\n", th, hw.p ? hw.p : "NULL", th);
        free(hw.p);
        ran_first_bind_read(v, th);
        held++;
        continue;
      }
      Buf vb; memset(&vb, 0, sizeof vb);
      emit_expr_face(c, v, vt, &vb);   /* (one representation: in the temp's face) */
      emit_indent(g_pre, g_indent);
      if (vt == TY_NIL || vt == TY_VOID) {
        buf_printf(g_pre, "(void)(%s);\n", vb.p ? vb.p : "0");
        view_bind(v, "sp_box_nil()");
      }
      else {
        int t = ++g_tmp;
        emit_ctype(c, vt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", t, vb.p ? vb.p : "");
        emit_gc_root_tmp(c, vt, t, g_pre);
        buf_puts(g_pre, "\n");
        /* --share-strings: a handle held here is the one the value's Array
           boxes (ran_first_handle), not its variable's, which the stores
           may have rebound */
        if (vt == TY_STRBUF) ran_first_bind(v, t, t);
        else view_bind(v, "_t%d", t);
      }
      free(vb.p);
      held++;
    }
    emit_stmt(c, id, g_pre, g_indent);
    emit_expr(c, value, b);
    view_unbind(g_n_argov - (held));
    return;
  }

  /* ivar OP= as expression: emit the mutation then read back the ivar. */
  if (ty_k == NK_InstanceVariableOperatorWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    /* an instance_eval body's slot is its receiver's (see the statement form) */
    int sc = g_ie_class_id >= 0 ? g_ie_class_id : comp_scope_of(c, id)->class_id;
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    /* the slot is read back as the read of the ivar finds it: a top-level
       ivar is the Toplevel pseudo-class's file-scope global, where `self`
       names nothing (`x = (@i += 1)` at the top level, or a min/max block) */
    int tl = cs && cs->class_id < 0 && g_ie_class_id < 0 ? c->toplevel_class : -1;
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, iv_c(nm + 1));
    else if (ivar_class_body_slot(c, id, cs, ref, sizeof ref)) sc = g_class_body_id;
    else if (tl >= 0) {
      snprintf(ref, sizeof ref, "civ_Toplevel_%s", iv_c(nm + 1));
      sc = tl;
    }
    else
      snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    TyKind svt = TY_UNKNOWN;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) svt = c->classes[sc].ivar_types[iv]; }
    const char *sop = nt_str(nt, id, "binary_operator");
    /* a `+=` on an ivar the rule holds as the shared handle answers the
       String it built, read from the handle as the ivar's own read is */
    if (svt == TY_STRBUF && sop && is_plus_op(sop)) {
      if (g_pre) {
        emit_stmt(c, id, g_pre, g_indent);
        buf_printf(b, "sp_strbuf_read_pub(%s)", ref);
      }
      else {
        char mk[300];
        const char *m = ivar_write_mark(c, id, mk, sizeof mk);
        buf_printf(b, "({ %s = ", ref);
        emit_strbuf_ivar_plus(c, nt_ref(nt, id, "value"), ref, b);
        buf_printf(b, "; %s%ssp_strbuf_read_pub(%s); })", m ? m : "", m ? " " : "", ref);
      }
      return;
    }
    if (g_pre) {
      emit_stmt(c, id, g_pre, g_indent);
      buf_puts(b, ref);
      /* an oint static answers its value: an `op=` that returned is never nil */
      { int ocid, oiv; if (ivar_node_slot(c, id, &ocid, &oiv) == 2 && civ_is_oint(c, ocid, oiv)) buf_puts(b, ".v"); }
    }
    else {
      TyKind vt = TY_UNKNOWN;
      if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
      int t = ++g_tmp;
      buf_printf(b, "({ ");
      emit_ctype(c, vt, b);
      buf_printf(b, " _t%d; ", t);
      /* inline the write */
      const char *op = nt_str(nt, id, "binary_operator");
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      /* a Float slot's operator converts its operand as Float's does, as
         the statement form (emit_scalar_op_assign) takes it */
      emit_coerce(c, nt_ref(nt, id, "value"), vt, vt == TY_FLOAT ? CO_CONVERT : CO_HOLD,
                  "the operand of an `op=`", b);
      char mk[300];
      const char *m = ivar_write_mark(c, id, mk, sizeof mk);
      buf_printf(b, "; %s%s_t%d = %s; _t%d; })", m ? m : "", m ? " " : "", t, ref, t);
    }
    return;
  }

  if (ty_k == NK_InterpolatedRegularExpressionNode) {
    /* a dynamic regexp in general value position: the shared pattern emitter
       builds the interpolated source and re_compiles into a prelude temp. */
    if (emit_regex_pat_to_buf(c, id, b)) return;
  }
  if (ty_k == NK_DefNode) {
    /* a def in value position: the definition emits separately; the
       expression's value is the method-name symbol. A `def obj.m` is also
       where obj's singleton method starts (emit_sg_activate). */
    const char *dn = nt_str(nt, id, "name");
    int sgci = sg_activates_ci(c, id), dr = nt_ref(nt, id, "receiver");
    sg_refused_def(c, id);
    if (sgci >= 0 && dr >= 0) {
      buf_puts(b, "((void)"); emit_sg_frozen_check(c, sgci, id, dr, b);
      buf_puts(b, ", ((void)(("); emit_sg_recv(c, id, dr, b);
      buf_printf(b, ")->cls_id = %d)), (sp_sym)%d)", sgci, comp_sym_intern(c, dn ? dn : ""));
      return;
    }
    buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, dn ? dn : ""));
    return;
  }
  if (ty_k == NK_CallOrWriteNode || ty_k == NK_CallAndWriteNode) {
    /* value position `x = (a.v ||= 5)`: run the conditional write, then read
       the attribute back as the expression's value (the assigned-or-existing
       one, matching CRuby). The guard is the ivar's own (emit_slot_orw_value). */
    int is_or = ty_k == NK_CallOrWriteNode;
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (emit_call_or_write_via_methods(c, id, is_or, b)) return;
    TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    if (recv >= 0 && attr && ty_is_object(rt)) {
      int class_id = ty_object_class(rt);
      char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", attr);
      int iidx = comp_ivar_index(&c->classes[class_id], ivn);
      TyKind ivt = iidx >= 0 ? c->classes[class_id].ivar_types[iidx] : TY_UNKNOWN;
      int tr = ++g_tmp;
      buf_puts(b, "({ ");
      emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
      char lhs[300]; snprintf(lhs, sizeof lhs, "_t%d->iv_%s", tr, iv_c(attr));
      char apfx[32]; snprintf(apfx, sizeof apfx, "_t%d->", tr);
      char tn[32], mk[300]; snprintf(tn, sizeof tn, "_t%d", tr);
      const char *m = iidx >= 0 ? ivar_set_mark(c, class_id, ivn, tn, "->", mk, sizeof mk) : NULL;
      /* a field with a nil bit read as the node's oint (node_is_oint):
         after the write, nil exactly when the bit says so (`&&=` over nil) */
      int ao = oint_kind(ivt) && iidx >= 0 && ivar_has_nilbit(c, class_id, iidx) && node_is_oint(c, id);
      if (ao) buf_puts(b, "(void)");
      emit_attr_orw_value(c, class_id, iidx, apfx, ivt, iidx >= 0 && repr_of_ivar(c, class_id, iidx).elems_handle, lhs, v, is_or, m, b);
      if (ao) {
        char ant[300]; ivar_nilbit_test(c, class_id, iidx, apfx, ant, sizeof ant);
        buf_printf(b, "; %s ? %s : %s(%s)", ant, oint_nil(ivt), oint_of(ivt), lhs);
      }
      buf_puts(b, "; })");
      return;
    }
  }
  if (ty_k == NK_SplatNode) {
    /* `*x` in unboxed value position (a next/break value flowing into a
       typed PolyArray slot): splat-to-array, then unwrap the pointer. */
    int sp_inner = nt_ref(nt, id, "expression");
    /* splat semantics first (nil -> [], scalar -> [v], array kept), then
       normalize the KIND: the slot is sp_PolyArray*, and a kept IntArray
       must be rebuilt, not pointer-cast (different layout). */
    buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array(");
    if (sp_inner >= 0) emit_boxed(c, sp_inner, b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, "))");
    return;
  }

  /* a[i] op= v as an expression (e.g. the tail of a block whose proc result
     is consumed): emit the mutation via the statement form, then read the
     slot back as the value -- Ruby yields the stored (post-op) value. The
     read-back re-evaluates receiver and key, so this path is gated on both
     being effect-free simple reads; anything else keeps the clean reject. */
  if (ty_k == NK_IndexOperatorWriteNode) {
    int ir = nt_ref(nt, id, "receiver");
    int ia = nt_ref(nt, id, "arguments");
    int iac = 0; const int *iav = ia >= 0 ? nt_arr(nt, ia, "arguments", &iac) : NULL;
    if (ir >= 0 && iac == 1) {
      /* A variable or a literal can simply be re-emitted for the read-back; a
         method-call receiver or key cannot, because CRuby evaluates each once
         and re-emitting would run it twice. Hoist those into temps that both
         the write and the read then share. Without this the value form
         declined outright, which is what `v[s.tag] += 1` as a block's last
         expression hit (#3417). */
      int cheap = idx_opw_node_is_cheap(nt, ir) && idx_opw_node_is_cheap(nt, iav[0]);
      /* a boxed receiver's store answers the stored value into a temp of its
         own (emit_index_opw_value_into): a String's slot read back after the
         splice is one character of it */
      int tv = repr_of(c, ir).kind == RK_BOXED && repr_of(c, id).kind == RK_BOXED ? ++g_tmp : 0;
      char tvn[24]; snprintf(tvn, sizeof tvn, "_t%d", tv);
      if (g_pre) {
        int hoisted = cheap ? 0 : emit_index_opw_hoist(c, id, g_pre, g_indent);
        if (cheap || hoisted) {
          /* The mutation cannot be emitted straight into g_pre: an RHS that
             wants a prelude of its own (a user method call hoists its receiver
             and spills each argument into a temp) drains into g_pre too, and
             appending to the buffer mid-statement dropped the declaration
             INSIDE the sp_..._set(...) argument list -- structurally invalid C,
             an unbalanced paren and a _tN used before its declaration. Collect
             the RHS's prelude and the statement separately, then splice them in
             prelude-first, as the no-g_pre branch below already does. */
          Buf *sv_pre = g_pre;
          Buf pre; memset(&pre, 0, sizeof pre);
          Buf stmt; memset(&stmt, 0, sizeof stmt);
          if (tv) {
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal %s = sp_box_nil(); SP_GC_ROOT_RBVAL(%s);\n", tvn, tvn);
            emit_index_opw_value_into(tvn);
          }
          g_pre = &pre;
          emit_index_op_write(c, id, &stmt, g_indent);
          g_pre = sv_pre;
          if (pre.p) buf_puts(sv_pre, pre.p);
          if (stmt.p) buf_puts(sv_pre, stmt.p);
          free(pre.p); free(stmt.p);
          /* the read-back runs after the mutation, so its own prelude (if any)
             belongs at the end of g_pre -- where it now lands naturally. */
          if (tv && emit_index_opw_value_taken()) buf_puts(b, tvn);
          else emit_index_get(c, ir, iav[0], b);
          if (hoisted) emit_index_opw_unhoist();
          return;
        }
      }
      else {
        /* No prelude buffer: statement-expression fallback. The RHS inside
           the mutation may itself need a prelude, so redirect g_pre into a
           local buffer and splice it in front, mirroring emit_with_prelude
           (writing into a NULL g_pre would crash the compiler). */
        Buf pre; memset(&pre, 0, sizeof pre);
        Buf stmt; memset(&stmt, 0, sizeof stmt);
        int sv_ind = g_indent;
        g_pre = &pre; g_indent = 0;
        int hoisted = cheap ? 0 : emit_index_opw_hoist(c, id, &pre, 0);
        if (cheap || hoisted) {
          if (tv) {
            buf_printf(&stmt, "sp_RbVal %s = sp_box_nil(); SP_GC_ROOT_RBVAL(%s);\n", tvn, tvn);
            emit_index_opw_value_into(tvn);
          }
          emit_index_op_write(c, id, &stmt, 0);
          g_pre = NULL; g_indent = sv_ind;
          buf_puts(b, "({ ");
          if (pre.p) buf_puts(b, pre.p);
          if (stmt.p) buf_puts(b, stmt.p);
          if (tv && emit_index_opw_value_taken()) buf_puts(b, tvn);
          else emit_index_get(c, ir, iav[0], b);
          buf_puts(b, "; })");
          free(pre.p); free(stmt.p);
          if (hoisted) emit_index_opw_unhoist();
          return;
        }
        g_pre = NULL; g_indent = sv_ind;
        free(pre.p); free(stmt.p);
      }
    }
  }

  /* retry in value position (e.g. the tail of a parenthesized group): the
     jump back to its begin makes the value dead -- emit the goto as a
     prelude statement, exactly like the statement form. */
  if (ty_k == NK_RetryNode) {
    if (g_retry_label) {
      /* inline, not a g_pre prelude: statements sequenced before the retry in
         the same group (e.g. `(c = false; retry)`) must run first. GCC/clang
         permit a goto out of a statement expression; the dead 0 satisfies the
         value slot. */
      buf_puts(b, "({ ");
      emit_retry_unwind(b);
      buf_printf(b, "goto %s; 0; })", g_retry_label);
    }
    else unsupported(c, id, "retry (outside rescue)");
    return;
  }
  if (ty_k == NK_NextNode && emit_next_expr(c, id, b)) return;

  unsupported(c, id, "expression");
}

/* ---- output statements (puts/print/p) ---- */
