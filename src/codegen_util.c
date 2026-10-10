#include "codegen_internal.h"
#include "share.h"
#include "call_plan.h"
#include "repr.h"
#include "holder.h"
#include "builtin_ops.h"
#include "codegen_call_arms.h"

Buf expr_buf(Compiler *c, int node) {
  Buf b; memset(&b, 0, sizeof b);
  emit_expr(c, node, &b);
  return b;
}

Buf *g_pre = NULL;

/* SP_COLLECT_ERRORS recovery: in collect mode a codegen gap longjmps back to a
   per-unit recovery point armed by the output driver (codegen.c), so one run
   surfaces every unsupported construct instead of aborting on the first. */
jmp_buf g_unsup_recover;
int g_unsup_armed = 0;
int g_unsup_probe = 0;   /* silent emittability probe: longjmp without printing/exiting */
int g_unsup_quiet = 0;   /* a refusal is recorded but not printed: the driver decides (orphan methods) */
/* Collection is how every compile runs: a refusal stopped the run at the
   first one, and a program brought over from CRuby learned its gaps one
   compile at a time. Now each unit's refusals are reported and the next
   unit is emitted; the run still fails at the end (codegen_program), and
   nothing is written. SP_COLLECT_ERRORS keeps its old meaning on top: the
   gaps are dropped and the rest is emitted anyway, which is what the
   collect-errors gate and a reduction want. */
int collect_mode(void) { return 1; }
/* --defer-refusals: a refused method compiles to one that raises
   NotImplementedError (naming the refusal) when it is called, and a refused
   top-level or class-body statement is skipped; the build goes on. */
int defer_refusals(void) {
  static int v = -1;
  if (v < 0) { const char *e = getenv("SPINEL_DEFER_REFUSALS"); v = e && *e ? 1 : 0; }
  return v;
}
int collect_emit_anyway(void) {
  static int v = -1;
  if (v < 0) v = getenv("SP_COLLECT_ERRORS") ? 1 : 0;
  return v;
}
/* What codegen decided at a node, for --emit-types (#4522): how a call was
   dispatched, whether a block was inlined. Kept only under --emit-types;
   a stamp anywhere else is a no-op, so the emitters say what they did
   without paying for it. */
unsigned char *g_ndecide = NULL;
int g_ndecide_cap = 0;
int g_nd_call_id = -1;
void nd_stamp(int id, int kind) {
  static int on = -1;
  if (on < 0) { const char *et = getenv("SPINEL_EMIT_TYPES"); on = (et && *et) ? 1 : 0; }
  if (!on || id < 0) return;
  if (id >= g_ndecide_cap) {
    int ncap = g_ndecide_cap ? g_ndecide_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ndecide = realloc(g_ndecide, (size_t)ncap);
    memset(g_ndecide + g_ndecide_cap, 0, (size_t)(ncap - g_ndecide_cap));
    g_ndecide_cap = ncap;
  }
  g_ndecide[id] = (unsigned char)kind;
}
/* The def a call was bound to, per node: "Owner#name" / "Owner.name" for a
   method in a class, the bare name for a top-level def; a switch's arms
   accumulate in arm order, comma-separated (a class name carries no comma).
   A consumer resolving go-to-definition took the first DefNode of that name,
   wrong whenever two classes define it (#4557). Same gate as nd_stamp. */
char **g_ndtarget = NULL;
int g_ndtarget_cap = 0;

/* --plan-check for user-method calls (#7100): the method codegen bound a
   call to, compared with the one inference bound it to (c->ucall_inf), per
   node. A node's observations accumulate (a switch stamps one per arm, a
   body emitted more than once stamps again) and are classified once, at
   the end (ucall_report):
     conflict       codegen bound another method where nothing re-reads the
                    node: view depth 0, no instance_exec scope move or
                    class, no inline splice, the emitting class the node's
                    own scope's class, not a lowered or proc-form body
     respecialized  another method, but only where the node is re-read
                    (any of the above)
     virtual        a dispatch switch, one of whose arms is inference's
     unrecorded     codegen bound a method inference bound none for
     unobserved     inference bound a method, codegen emitted the call and
                    stamped nothing
     unemitted      inference bound a method, codegen never emitted the call
                    (dead code, or a call folded to its value)
     refused        inference bound a method, codegen emitted the
                    NoMethodError its visibility gives the call instead
     dynamic        inference typed a boxed receiver's call over its
                    candidates, codegen emitted it through the run-time
                    dispatch, which binds nothing statically
   The resolver's plan (call_plan.c) is the third reading, compared with
   both: a per-program ucall-resolver line counts agreement with codegen's
   binding (agree, differ, respecialized as above, none: the resolver found
   no method) and with inference's record (agree, differ, none), and each
   disagreement outside a re-read context is listed. */
typedef struct { int mi; short owner; unsigned char seen, flags; int rmi; unsigned char rflags; } UcallObs;
enum { UO_MATCH = 1, UO_ARM = 2, UO_MISS_PLAIN = 4, UO_MISS_RESPEC = 8 };
/* the resolver's plan (cplan_user) against codegen's binding */
enum { UR_AGREE = 1, UR_DIFF_PLAIN = 2, UR_DIFF_RESPEC = 4, UR_NONE = 8,
       UR_ARM_HIT = 16, UR_ARM_MISS = 32 };   /* a switch: some arm is the plan's, some is not */
static UcallObs *g_ucobs = NULL;
static int g_ucobs_cap = 0;
/* the call nodes codegen emitted at all (emit_call, emit_super, a splice):
   an inference binding at a node never emitted -- a method no code reaches,
   a call folded away -- has nothing to compare with */
static unsigned char *g_ucemit = NULL;
static int g_ucemit_cap = 0;
/* ... and outside a probe: a call a probe tried and dropped was not
   emitted, and its refusal was never reported (the refusal shadow) */
static unsigned char *g_ucemit_np = NULL;

void ucall_emitted(int id) {
  if (id < 0) return;
  if (id >= g_ucemit_cap) {
    int ncap = g_ucemit_cap ? g_ucemit_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ucemit = realloc(g_ucemit, (size_t)ncap);
    g_ucemit_np = realloc(g_ucemit_np, (size_t)ncap);
    memset(g_ucemit + g_ucemit_cap, 0, (size_t)(ncap - g_ucemit_cap));
    memset(g_ucemit_np + g_ucemit_cap, 0, (size_t)(ncap - g_ucemit_cap));
    g_ucemit_cap = ncap;
  }
  if (!g_ucemit[id]) g_ucemit[id] = 1;
  if (!g_unsup_probe) g_ucemit_np[id] = 1;
}
void ucall_refused(int id) {
  ucall_emitted(id);
  g_ucemit[id] = 2;
}

static int ucall_respec_ctx(Compiler *c, int id) {
  if (view_depth() > 0 || comp_scope_move_depth() > 0 || g_ie_class_id >= 0 ||
      inline_splice_depth() > 0 || g_current_scope_is_lowered)
    return 1;
  Scope *s = comp_scope_of(c, id);
  int scls = s ? s->class_id : -1;
  if (g_emitting_class_id >= 0 && g_emitting_class_id != scls) return 1;
  if (s && scope_proc_form_of(c, (int)(s - c->scopes)) >= 0) return 1;
  return 0;
}

void ucall_observe(Compiler *c, int id, int mi, int owner_ci, int add) {
  if (g_repr_check) repr_channel_call(c, id, mi);
  if (id < 0 || id >= c->node_cap || mi < 0 || mi >= c->nscopes) return;
  if (id >= g_ucobs_cap) {
    int ncap = g_ucobs_cap ? g_ucobs_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ucobs = realloc(g_ucobs, (size_t)ncap * sizeof *g_ucobs);
    memset(g_ucobs + g_ucobs_cap, 0, (size_t)(ncap - g_ucobs_cap) * sizeof *g_ucobs);
    g_ucobs_cap = ncap;
  }
  UcallObs *o = &g_ucobs[id];
  if (!o->seen) { o->seen = 1; o->mi = mi; o->owner = (short)owner_ci; }
  if (add) o->flags |= UO_ARM;
  /* the resolver: its method, an arm of the switch it plans, or the
     proc-form clone of its method agrees */
  { const CallPlan *pl = cplan_user(c, id);
    int member = pl->mi >= 0 &&
                 (cplan_virtual_member(c, id, pl, mi) || (add && pl->dispatch >= CP_SWITCH) ||
                  (c->scopes[mi].is_proc_form && scope_proc_form_of(c, pl->mi) == mi));
    if (pl->mi < 0) o->rflags |= UR_NONE;
    /* a switch's arms: agreement is the plan's method among them, decided
       once every arm is in (the report) */
    else if (add) {
      o->rflags |= member ? UR_ARM_HIT : UR_ARM_MISS;
      if (!member && !o->rmi) o->rmi = mi + 1;
    }
    else if (member) o->rflags |= UR_AGREE;
    else {
      o->rflags |= ucall_respec_ctx(c, id) ? UR_DIFF_RESPEC : UR_DIFF_PLAIN;
      if (!o->rmi || !(o->rflags & UR_DIFF_PLAIN)) o->rmi = mi + 1;
    } }
  const UCallInf *inf = &c->ucall_inf[id];
  if (inf->via == UC_NONE) return;
  /* a boxed receiver's union: any of its candidates is one of its arms */
  if (inf->via == UC_POLY) {
    const char *pnm = nt_str(c->nt, id, "name");
    int npc = 0, ncc = 0;
    const PolyCand *pcs = pnm ? comp_poly_candidates(c, pnm, &npc) : NULL;
    for (int k = 0; k < npc; k++)
      if (pcs[k].mi == mi ||
          (pcs[k].mi < 0 && comp_method_in_chain(c, pcs[k].cls, pnm, NULL) == mi)) {
        o->flags |= UO_MATCH | UO_ARM; return;
      }
    const PolyCand *ccs = pnm ? comp_cmethod_candidates(c, pnm, &ncc) : NULL;
    for (int k = 0; k < ncc; k++)
      if (ccs[k].mi == mi) { o->flags |= UO_MATCH | UO_ARM; return; }
  }
  /* a yielding method's proc-form clone is that method, taken as a function
     with its block as a proc */
  if (inf->mi == mi || (c->scopes[mi].is_proc_form && scope_proc_form_of(c, inf->mi) == mi)) {
    o->flags |= UO_MATCH; return;
  }
  int miss = ucall_respec_ctx(c, id) ? UO_MISS_RESPEC : UO_MISS_PLAIN;
  if (!(o->flags & (UO_MISS_PLAIN | UO_MISS_RESPEC)) || miss == UO_MISS_PLAIN) {
    o->mi = mi; o->owner = (short)owner_ci;
  }
  o->flags |= miss;
}

static void ucall_scope_name(Compiler *c, int mi, char *out, size_t n) {
  if (mi < 0 || mi >= c->nscopes) { snprintf(out, n, "?"); return; }
  Scope *m = &c->scopes[mi];
  int ci = m->class_id;
  if (ci >= 0 && ci < c->nclasses)
    snprintf(out, n, "%s%s%s", c->classes[ci].name, m->is_cmethod ? "." : "#", m->name ? m->name : "?");
  else snprintf(out, n, "%s", m->name ? m->name : "?");
}

static void ucall_resolver_report(Compiler *c) {
  int ac = 0, dc = 0, rc = 0, nc = 0, ai = 0, di = 0, ni = 0;
  for (int id = 0; id < c->node_cap; id++) {
    const UCallInf *inf = &c->ucall_inf[id];
    const UcallObs *o = id < g_ucobs_cap ? &g_ucobs[id] : NULL;
    int seen = o && o->seen;
    if (inf->via == UC_NONE && !seen) continue;
    const CallPlan *pl = cplan_user(c, id);
    const char *nm = nt_str(c->nt, id, "name");
    char rs[256], xs[256];
    ucall_scope_name(c, pl->mi, rs, sizeof rs);
    if (seen) {
      /* a switch with the plan's method among its arms agrees; one without
         it differs like a plain binding would */
      int arm_diff = (o->rflags & UR_ARM_MISS) && !(o->rflags & UR_ARM_HIT);
      if ((o->rflags & UR_DIFF_PLAIN) || arm_diff) {
        dc++;
        ucall_scope_name(c, o->rmi - 1, xs, sizeof xs);
        fprintf(stderr, "plan-check: ucall-resolver-differ: node %d %s: resolver %s, codegen %s\n",
                id, nm ? nm : "?", rs, xs);
      }
      else if (o->rflags & UR_DIFF_RESPEC) rc++;
      else if (o->rflags & (UR_AGREE | UR_ARM_HIT)) ac++;
      else {
        nc++;
        ucall_scope_name(c, o->mi, xs, sizeof xs);
        fprintf(stderr, "plan-check: ucall-resolver-none: node %d %s: codegen %s\n", id, nm ? nm : "?", xs);
      }
    }
    if (inf->via != UC_NONE) {
      if (pl->mi < 0) {
        ni++;
        ucall_scope_name(c, inf->mi, xs, sizeof xs);
        fprintf(stderr, "plan-check: ucall-resolver-none-inference: node %d %s: inference %s (%s)\n", id,
                nm ? nm : "?", xs, inf->via < 10 ? (const char *[]){ "none", "top", "inst", "cmeth", "super",
                "send_blind", "ie", "included", "reopen", "poly" }[inf->via] : "?");
      }
      else if (cplan_virtual_member(c, id, pl, inf->mi) ||
               (inf->via == UC_POLY && pl->via == UC_POLY)) ai++;
      else {
        di++;
        ucall_scope_name(c, inf->mi, xs, sizeof xs);
        fprintf(stderr, "plan-check: ucall-resolver-differ-inference: node %d %s: resolver %s, inference %s\n",
                id, nm ? nm : "?", rs, xs);
      }
    }
  }
  fprintf(stderr, "plan-check: ucall-resolver: codegen %d agree %d differ %d respecialized %d none;"
                  " inference %d agree %d differ %d none\n", ac, dc, rc, nc, ai, di, ni);
}

/* --plan-check for refusals (#7100, CP_REFUSE): each refusal codegen
   reports outside a probe (unsup_leave), at the node it names, held against
   the call plan's (cplan_refuse). Classified once, at the end:
     ok         both refuse the node, in the same words
     wrong      the plan refuses the node and codegen says something else,
                or emits the call
     codegen    codegen refuses a node the plan decides nothing for: its own
                state decided it. Counted by what is left: call (the final
                fall-through's dump: which emitter declined), shape (any other
                internal dump: a statement or value shape, a poly splat),
                nomethod (CRuby's NoMethodError/NameError words, a rewording
                of a refusal codegen decided), string-copy (a String handed
                by value to something that appends to it: the refuse_*
                family, anchored at the last call codegen emitted), feature
                (any other documented limit)
     unreached  the plan refuses a call codegen never emitted outside a
                probe (dead code, the rest of a unit a refusal abandoned, an
                arm a probe dropped)
   A refusal raised while analysis runs is not codegen's and is not held.
   The report is printed by ucall_report, or at exit for a refused run. */
typedef struct { int id; char *msg; } RefuseObs;
static RefuseObs *g_rfobs = NULL;
static int g_nrfobs = 0, g_rfobs_cap = 0;
static Compiler *g_rfc = NULL;   /* the compiler, once codegen refuses */
static int g_rf_node = -1;       /* the node the refusal being reported names */
static int g_rf_done = 0;

static void refuse_report(Compiler *c) {
  if (g_rf_done || !c) return;
  g_rf_done = 1;
  int ok = 0, wrong = 0, call = 0, shape = 0, nom = 0, strc = 0, feat = 0, unr = 0;
  int n = c->nt->count;
  unsigned char *seen = calloc((size_t)n + 1, 1);
  for (int i = 0; i < g_nrfobs; i++) {
    int id = g_rfobs[i].id;
    const char *m = g_rfobs[i].msg;
    if (id >= 0 && id < n) seen[id] = 1;
    const CallPlan *p = cplan_refuse(c, id);
    if (p->dispatch == CP_REFUSE) {
      if (sp_streq(p->msg, m)) ok++;
      else {
        wrong++;
        fprintf(stderr, "plan-check: refuse-wrong: node %d: plan \"%s\", codegen \"%s\"\n", id, p->msg, m);
      }
    }
    else if (!strncmp(m, "unsupported call: node ", 23)) call++;
    else if (!strncmp(m, "unsupported ", 12) && strstr(m, ": node ")) shape++;
    else if (strstr(m, "(NoMethodError)") || strstr(m, "(NameError)")) nom++;
    else if (strstr(m, "not yet shared by reference")) strc++;
    else feat++;
  }
  for (int id = 0; id < n; id++) {
    if (seen[id] || nt_kind(c->nt, id) != NK_CallNode) continue;
    const CallPlan *p = cplan_refuse(c, id);
    if (p->dispatch != CP_REFUSE) continue;
    if (id < g_ucemit_cap && g_ucemit_np[id]) {
      wrong++;
      fprintf(stderr, "plan-check: refuse-wrong: node %d: plan \"%s\", codegen emitted the call\n", id, p->msg);
    }
    else unr++;
  }
  free(seen);
  fprintf(stderr, "plan-check: refuse: %d ok, %d wrong, %d codegen (%d call, %d shape, %d nomethod, %d string-copy,"
                  " %d feature), %d unreached\n",
          ok, wrong, call + shape + nom + strc + feat, call, shape, nom, strc, feat, unr);
}
/* a refused run: the plan readers that served it too (ucall_report's) */
static void refuse_report_at_exit(void) {
  if (g_rf_done) return;
  refuse_report(g_rfc);
  cplan_served_report();
}

/* unsupported / unsupported_feature: the node a codegen refusal names */
static void refuse_at(Compiler *c, int id) {
  if (!g_plan_check || !g_scopes_settled) return;
  if (!g_rfc) { g_rfc = c; atexit(refuse_report_at_exit); }
  g_rf_node = id;
}
static void refuse_observe(const char *msg) {
  if (!g_rfc || g_rf_node < 0) return;
  if (g_nrfobs == g_rfobs_cap) {
    g_rfobs_cap = g_rfobs_cap ? g_rfobs_cap * 2 : 16;
    g_rfobs = realloc(g_rfobs, (size_t)g_rfobs_cap * sizeof *g_rfobs);
  }
  g_rfobs[g_nrfobs].id = g_rf_node;
  g_rfobs[g_nrfobs++].msg = strdup(msg);
  g_rf_node = -1;
}

void ucall_report(Compiler *c) {
  refuse_report(c);
  ucall_resolver_report(c);
  cplan_served_report();
  iter_rows_check();
  pa_report();
  static const char *const via_name[] = { "none", "top", "inst", "cmeth", "super",
                                          "send_blind", "ie", "included", "reopen", "poly" };
  for (int id = 0; id < c->node_cap; id++) {
    const UCallInf *inf = &c->ucall_inf[id];
    const UcallObs *o = id < g_ucobs_cap ? &g_ucobs[id] : NULL;
    int seen = o && o->seen;
    if (inf->via == UC_NONE && !seen) continue;
    const char *nm = nt_str(c->nt, id, "name");
    char cg[256], in[256];
    ucall_scope_name(c, seen ? o->mi : -1, cg, sizeof cg);
    ucall_scope_name(c, inf->via != UC_NONE ? inf->mi : -1, in, sizeof in);
    const char *vn = inf->via < sizeof via_name / sizeof via_name[0] ? via_name[inf->via] : "?";
    if (inf->via == UC_NONE)
      fprintf(stderr, "plan-check: ucall-unrecorded: node %d %s: codegen %s%s\n", id, nm ? nm : "?", cg,
              (o->flags & UO_ARM) ? " (switch)" : "");
    else if (!seen && inf->via == UC_POLY && id < g_ucemit_cap && g_ucemit[id] == 1)
      fprintf(stderr, "plan-check: ucall-dynamic: node %d %s: inference %s (%s)\n", id, nm ? nm : "?", in, vn);
    else if (!seen && id < g_ucemit_cap && g_ucemit[id] == 2)
      fprintf(stderr, "plan-check: ucall-refused: node %d %s: inference %s (%s)\n", id, nm ? nm : "?", in, vn);
    else if (!seen && !(id < g_ucemit_cap && g_ucemit[id]))
      fprintf(stderr, "plan-check: ucall-unemitted: node %d %s: inference %s (%s)\n", id, nm ? nm : "?", in, vn);
    else if (!seen)
      fprintf(stderr, "plan-check: ucall-unobserved: node %d %s: inference %s (%s)\n", id, nm ? nm : "?", in, vn);
    else if (!(o->flags & (UO_MISS_PLAIN | UO_MISS_RESPEC))) continue;
    else if ((o->flags & UO_ARM) && (o->flags & UO_MATCH))
      fprintf(stderr, "plan-check: ucall-virtual: node %d %s: codegen switch, inference %s (%s)\n", id, nm ? nm : "?", in, vn);
    else if (o->flags & UO_MISS_PLAIN)
      fprintf(stderr, "plan-check: ucall-conflict: node %d %s: codegen %s, inference %s (%s)\n", id, nm ? nm : "?", cg, in, vn);
    else
      fprintf(stderr, "plan-check: ucall-respecialized: node %d %s: codegen %s, inference %s (%s)\n", id, nm ? nm : "?", cg, in, vn);
  }
}

void nd_callee(Compiler *c, int id, int mi, int owner_ci, int add) {
  if (g_plan_check) ucall_observe(c, id, mi, owner_ci, add);
  static int on = -1;
  if (on < 0) { const char *et = getenv("SPINEL_EMIT_TYPES"); on = (et && *et) ? 1 : 0; }
  if (!on || id < 0 || mi < 0 || mi >= c->nscopes) return;
  Scope *m = &c->scopes[mi];
  if (!m->name || !*m->name) return;
  char ent[256];
  if (owner_ci >= 0 && owner_ci < c->nclasses)
    snprintf(ent, sizeof ent, "%s%s%s", c->classes[owner_ci].name, m->is_cmethod ? "." : "#", m->name);
  else snprintf(ent, sizeof ent, "%s", m->name);
  if (id >= g_ndtarget_cap) {
    int ncap = g_ndtarget_cap ? g_ndtarget_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ndtarget = realloc(g_ndtarget, (size_t)ncap * sizeof *g_ndtarget);
    memset(g_ndtarget + g_ndtarget_cap, 0, (size_t)(ncap - g_ndtarget_cap) * sizeof *g_ndtarget);
    g_ndtarget_cap = ncap;
  }
  char *cur = g_ndtarget[id];
  if (add && cur) {
    /* a body emitted once per specialization stamps its calls again: one
       entry per arm */
    size_t el = strlen(ent);
    for (const char *p = cur; p; ) {
      const char *q = strchr(p, ',');
      size_t l = q ? (size_t)(q - p) : strlen(p);
      if (l == el && !strncmp(p, ent, el)) return;
      p = q ? q + 1 : NULL;
    }
    size_t cl = strlen(cur);
    cur = realloc(cur, cl + 1 + el + 1);
    cur[cl] = ','; memcpy(cur + cl + 1, ent, el + 1);
    g_ndtarget[id] = cur;
    return;
  }
  free(cur);
  g_ndtarget[id] = strdup(ent);
}
/* Every refusal of this run, for --emit-types and the count at the end. */
SpDiag *g_diags = NULL;
int g_ndiags = 0;
static int g_diags_cap = 0;
static void diag_record(const char *file, int line, const char *msg) {
  if (g_ndiags == g_diags_cap) {
    g_diags_cap = g_diags_cap ? g_diags_cap * 2 : 16;
    g_diags = realloc(g_diags, (size_t)g_diags_cap * sizeof *g_diags);
  }
  g_diags[g_ndiags].file = file ? strdup(file) : NULL;
  g_diags[g_ndiags].line = line;
  g_diags[g_ndiags].msg = strdup(msg);
  g_ndiags++;
}
/* Report one refusal and leave: back to the driver's recovery point when one
   is armed (the unit is abandoned), else out of the process. */
static __attribute__((noreturn)) void unsup_leave(const char *file, int line, const char *msg) {
  diag_record(file, line, msg);
  if (g_unsup_quiet && collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  refuse_observe(msg);
  if (line > 0) fprintf(stderr, "spinel: %s:%d: %s\n", file, line, msg);
  else fprintf(stderr, "spinel: %s\n", msg);
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}
/* The .rb position the parser stamped on `id`, or line 0. */
static const char *unsup_pos(Compiler *c, int id, int *line) {
  *line = (int)nt_int(c->nt, id, "node_line", 0);
  int fid = (int)nt_int(c->nt, id, "node_file", 0);
  const char *file = nt_file_path(c->nt, fid);
  if (!file || !*file) file = c->nt->source_file;
  if (!file || !*file) file = "source.rb";
  return file;
}

/* A receiver an arm passes straight into a C call beside an argument is held
   by nothing while that argument runs, nor while the call itself allocates.
   This opens a statement expression that binds the receiver to a rooted temp
   and names that temp in rb; the caller emits the call reading rb and closes
   with "; })" when this returns 1. When this receiver node is in the
   override table (g_argov_node), the operand-order rewrite or an arm that
   re-dispatches through a temp has already declared and rooted the temp in
   front of the call, so rb receives the receiver as rendered and nothing is
   opened. boxed renders through emit_boxed. The caller frees rb. */
int hold_recv_open(Compiler *c, int recv, int boxed, const char *ctype, const char *rootm,
                   Buf *b, Buf *rb) {
  memset(rb, 0, sizeof *rb);
  int bound = 0;
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == recv) bound = 1;
  if (bound) {
    if (boxed) emit_boxed(c, recv, rb); else emit_expr(c, recv, rb);
    if (!rb->p) buf_putn(rb, "", 0);
    return 0;
  }
  Buf rx; memset(&rx, 0, sizeof rx);
  if (boxed) emit_boxed(c, recv, &rx); else emit_expr(c, recv, &rx);
  int t = ++g_tmp;
  buf_printf(b, "({ %s _t%d = %s; %s(_t%d); ", ctype, t, rx.p ? rx.p : "", rootm, t);
  buf_printf(rb, "_t%d", t);
  free(rx.p);
  return 1;
}

void emit_yielder_yield(Compiler *c, int id, const char *cn, Buf *b) {
  const NodeTable *nt = c->nt;
  int ar = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
  buf_puts(b, "sp_Fiber_yield(");
  /* y.yield(*xs) yields as many values as xs holds */
  if (ac == 1 && nt_kind(nt, av[0]) == NK_SplatNode) {
    buf_puts(b, "sp_yield_splat_pack(");
    emit_boxed(c, nt_ref(nt, av[0], "expression"), b);
    buf_puts(b, ")");
  }
  else if (ac == 1) { buf_puts(b, "sp_yield_one("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }
  else if (ac > 1) {
    /* y.yield(a, b, ...) yields an array of the values */
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new_pack(); SP_GC_ROOT(_t%d);\n", t, t);
    for (int k = 0; k < ac; k++) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, av[k], g_pre); buf_puts(g_pre, ");\n");
    }
    buf_printf(b, "sp_box_poly_array(_t%d)", t);
  }
  else buf_puts(b, sp_streq(cn, "yield") ? "sp_box_empty_step()" : "sp_box_nil()");
  buf_puts(b, ")");
}

void buf_putn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
/* Remove `n` bytes at `off`. Used to take back a prologue line the emitter had
   to write before it could know whether the body would need it. */
void buf_erase(Buf *b, size_t off, size_t n) {
  if (off > b->len || n == 0) return;
  if (off + n > b->len) n = b->len - off;
  memmove(b->p + off, b->p + off + n, b->len - off - n);
  b->len -= n;
  b->p[b->len] = '\0';
}
void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp)) { buf_putn(b, tmp, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_putn(b, big, (size_t)n); free(big);
}
int  g_indent = 0;
int  g_setter_stmt_id = -1;
/* Node id whose safe-nav (&.) guard is already emitted; the re-entrant
   emit_call skips the guard block for exactly this node. */
int  g_sn_skip = -1;
/* poly-dispatch builtin-arm re-entry marker: the call node whose dispatch is
   currently emitting its builtin-container arm, so the re-entered emission
   does not build the same dispatch again (#3459). */
/* Node whose Class-tag dispatch is emitting its non-Class arm, so the
   re-entered emission takes the ordinary path instead of rebuilding it. */
int  g_cls_tag_skip = -1;
/* True if evaluating the subtree at `id` may allocate (and so may trigger
   a GC): any call, container literal, lambda, string, symbol or regexp
   interpolation, or a read of the last match qualifies -- `$~` builds its
   MatchData, $` and $' their String; $& and $+ are reads, counted with
   them. */
int subtree_allocates(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") || sp_streq(ty, "ArrayNode") ||
      sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode") ||
      sp_streq(ty, "InterpolatedStringNode") || sp_streq(ty, "InterpolatedSymbolNode") ||
      sp_streq(ty, "InterpolatedRegularExpressionNode") || sp_streq(ty, "BackReferenceReadNode") ||
      sp_streq(ty, "LambdaNode") || sp_streq(ty, "SuperNode") ||
      sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "YieldNode"))
    return 1;
  /* Prism reads the match globals as plain globals as often as not. */
  if (sp_streq(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    return nm && (sp_streq(nm, "$~") || sp_streq(nm, "$`") || sp_streq(nm, "$'"));
  }
  /* A NUL-containing (binary) string literal does not lower to an immortal
     rodata pointer: it allocates a heap string via sp_str_from_bytes (every
     evaluation when unfrozen, or once to fill a call-site cache when frozen).
     Either path can trigger a GC, so it must count as an allocating sibling
     so the operand-rooting logic protects a fresh operand next to it. A plain
     (NUL-free) literal is rodata and never allocates. */
  if (sp_streq(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    if (sc && nt_str_len(nt, id, "content") > strlen(sc)) return 1;
    return 0;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_allocates(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_allocates(nt, ids[j])) return 1;
  }
  return 0;
}
/* The same question as the callers ask it. "Cannot allocate" is what lets a
   caller leave a sibling temp unrooted, so that answer is a decision
   (src/decide.c), keyed at the operand: a refused one is treated as
   allocating, which every caller answers with the root or the ordered temp
   it gives an operand that does allocate. */
int subtree_may_allocate(const NodeTable *nt, int id) {
  if (subtree_allocates(nt, id)) return 1;
  return id >= 0 && !decide_node(nt, id, "no-alloc", NULL);
}
/* subtree_may_allocate, plus the one allocation the node table cannot show:
   an ordinary read of a shared-mutable String slot (a TY_STRBUF local or
   ivar, or a global, a constant or a class variable the --share-strings
   rule made the handle) renders as a fresh copy of the live buffer, not as
   the slot itself. A read marked to hand out the handle (strbuf_box)
   copies nothing. The checks are strbuf_slot_ref's, without the emission
   it does, on the read inside any parentheses (`(a) == b`). */
int operand_may_allocate(Compiler *c, int id) {
  if (subtree_may_allocate(c->nt, id)) return 1;
  id = unwrap_parens(c, id);
  if (id < 0 || repr_of(c, id).handle) return 0;
  if (strbuf_local_name(c, id)) return 1;
  if (repr_self_shared(c, id)) return 1;
  if (repr_static_read_kind(nt_kind(c->nt, id)) && repr_of(c, id).share) return 1;
  if (nt_kind(c->nt, id) != NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  int cid = nm ? strbuf_ivar_owner(c, id) : -1;
  int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
  return iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF;
}
/* True if evaluating the subtree at `id` can be observed by, or can observe,
   a sibling argument's evaluation: any call (a user method, a mutating builtin,
   or an index read of a container someone else may write) or an assignment.
   Ruby fixes argument evaluation to left-to-right; C leaves a call's operand
   order unspecified, so such arguments have to be sequenced into temps. */
/* A builtin operator over scalars (`x + i`, `n < 3`) computes a value and
   touches nothing reachable, so no sibling argument can observe when it ran.
   It is a CallNode all the same, and counting it as an effect sequenced
   `cell_get(cells, x + ix, y + iy)` into temps for an ordering nobody can
   see -- 12% on the life benchmark. Gated on both the result and the
   receiver being scalar, so a user class's own `+` and `Array#<<` are
   effects as before. */
int call_is_scalar_op(Compiler *c, int id) {
  static const char *const OPS[] = {
    "+","-","*","/","%","**","<",">","<=",">=","==","!=","<=>","&","|","^","<<",">>", NULL };
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm) return 0;
  int hit = 0;
  for (int i = 0; OPS[i] && !hit; i++) if (sp_streq(nm, OPS[i])) hit = 1;
  if (!hit) return 0;
  if (nt_ref(c->nt, id, "block") >= 0) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv), vt = comp_ntype(c, id);
  int scalar_r = (rt == TY_INT || rt == TY_FLOAT || rt == TY_BOOL);
  int scalar_v = (vt == TY_INT || vt == TY_FLOAT || vt == TY_BOOL);
  return scalar_r && scalar_v;
}

int subtree_has_side_effect(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode") ||
      sp_streq(ty, "YieldNode") || strstr(ty, "WriteNode"))
    return 1;
  if (sp_streq(ty, "CallNode") && !call_is_scalar_op(c, id)) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_side_effect(c, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_side_effect(c, ids[j])) return 1;
  }
  return 0;
}
/* See codegen_internal.h. A write names the variable; a call on self
   follows the one method the classes from `cls` down define for it, up to
   a few calls deep; anything else that runs code of the program's -- a
   call on another object, a yield, a super, a block -- may write it. */
int subtree_may_write_ivar(Compiler *c, int id, const char *iv, int cls, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (!strncmp(ty, "InstanceVariable", 16) && (strstr(ty, "Write") || strstr(ty, "Target"))) {
    const char *wn = nt_str(nt, id, "name");
    if (!wn || sp_streq(wn, iv)) return 1;
  }
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "YieldNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "BlockArgumentNode"))
    return 1;
  if (sp_streq(ty, "CallNode") && !call_is_scalar_op(c, id)) {
    int recv = nt_ref(nt, id, "receiver");
    const char *nm = nt_str(nt, id, "name");
    if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) return 1;
    int mi = cls >= 0 && nm && depth < 3 ? comp_method_in_chain(c, cls, nm, NULL) : -1;
    if (mi < 0 || c->scopes[mi].def_node < 0 || dispatch_impl_count(c, cls, nm) != 1) return 1;
    if (subtree_may_write_ivar(c, nt_ref(nt, c->scopes[mi].def_node, "body"), iv, cls, depth + 1)) return 1;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_may_write_ivar(c, nt_ref_at(nt, id, i), iv, cls, depth)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_may_write_ivar(c, ids[j], iv, cls, depth)) return 1;
  }
  return 0;
}
int  g_tmp = 0;
char g_ren_from[MAX_RENAME][96];
char g_ren_to[MAX_RENAME][112];
int  g_nren = 0;
int  g_block_id = -1;

RenPark ren_park(int from) {
  RenPark p = { g_nren, from, g_nren - from, NULL, NULL, -1 };
  /* from 0: a C function body of its own, which cannot name the open
     splices' slots either (view_splice_next) */
  if (from == 0) p.fence = view_push_fn();
  if (p.n > 0) {
    p.f = (char (*)[96])malloc(sizeof(char[96]) * (size_t)p.n);
    p.t = (char (*)[112])malloc(sizeof(char[112]) * (size_t)p.n);
    if (p.f && p.t) {
      memcpy(p.f, g_ren_from + from, sizeof(char[96]) * (size_t)p.n);
      memcpy(p.t, g_ren_to + from, sizeof(char[112]) * (size_t)p.n);
    }
    else { free(p.f); free(p.t); p.f = NULL; p.t = NULL; }
  }
  g_nren = from;
  return p;
}

void ren_unpark(RenPark *p) {
  if (p->fence >= 0) view_pop(NULL, p->fence);
  g_nren = p->sv;
  if (p->f && p->t) {
    memcpy(g_ren_from + p->from, p->f, sizeof(char[96]) * (size_t)p->n);
    memcpy(g_ren_to + p->from, p->t, sizeof(char[112]) * (size_t)p->n);
  }
  free(p->f); free(p->t);
}
/* comp_ntype's yield hook: while a literal block is spliced, a YieldNode's
   value is THAT block's tail, not the union the node cache holds over every
   call site -- a second site whose block answers another class was compiled
   as the first one's (#3784). Installed by codegen_main; NULL elsewhere. */
int (*sp_yield_site_type_hook)(const Compiler *c, int id, TyKind *out) = NULL;
/* `blk.call(...)` on the enclosing method's own block parameter, which the
   inliner splices exactly as it splices a `yield`. Its cached type is the one
   the proc form has -- a Proc call answers poly -- but the spliced form
   computes whatever the caller's literal block computes, so the two emissions
   of the one node want different types (#3916). */
static int blk_param_call(const Compiler *c, int id) {
  if (!g_block_param_name || !g_block_param_name[0]) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm || !sp_streq(nm, "call")) return 0;
  int r = nt_ref(c->nt, id, "receiver");
  if (r < 0 || nt_kind(c->nt, r) != NK_LocalVariableReadNode) return 0;
  const char *rn = nt_str(c->nt, r, "name");
  return rn && sp_streq(rn, g_block_param_name);
}
/* An empty `[]` literal: splatting it contributes no elements. */
int is_empty_array_lit(const NodeTable *nt, int id) {
  if (id < 0 || nt_kind(nt, id) != NK_ArrayNode) return 0;
  int n = 0; nt_arr(nt, id, "elements", &n);
  return n == 0;
}
/* The cached value type of every `next` leaving the block body `node`: a
   nested loop, block, lambda or def binds its own. TY_UNKNOWN when none.
   The block's value is its tail joined with these, exactly as analyze's
   yield_value_type joins them (block_next_value_ty). */
TyKind block_next_value_ntype(const Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NextNode) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0) return TY_NIL;
    if (an > 1) return TY_POLY_ARRAY;
    const char *aty = nt_type(nt, av[0]);
    if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
    return c->ntype[av[0]];
  }
  if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode)
    return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind t = block_next_value_ntype(c, nt_ref_at(nt, node, i));
    if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) {
      TyKind t = block_next_value_ntype(c, ids[j]);
      if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
    }
  }
  return r;
}
/* A builtin arithmetic operator whose receiver is a yield: its value is per
   call site too. The emitter lowers `yield + yield` from the operands' own
   per-site types (sp_str_plus at a site whose block answers a String, a C
   `+` at a Float one), so the node's cached type, the union over every site,
   does not describe what it emitted. A poly slot then took that concrete
   value unboxed and the C did not compile. Answer the type the lowering
   produces, for the scalar pairs it lowers directly; anything else keeps
   the cached type. On a String the lowering of `+`, `*` and `%` is a String
   expression whatever the argument: it converts the argument (`* 2.0` takes
   an Integer count), or raises CRuby's TypeError where it cannot (`+ 1`), and
   String#% formats anything. Under promote, Integer `+ - *` take the boxed promotion path and may answer
   a Bignum, so they are left alone; Integer `/` and `%` still emit a raw
   Integer and are answered as one. */
static int yield_operator_site_type(const Compiler *c, int id, TyKind *out) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *op = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!op || recv < 0 || nt_kind(nt, recv) != NK_YieldNode) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  int an = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  if (ac != 1 || !av) return 0;
  TyKind rt, at;
  if (!sp_yield_site_type(c, recv, &rt)) return 0;
  at = comp_ntype(c, av[0]);
  int arith = sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") ||
              sp_streq(op, "/") || sp_streq(op, "%");
  if (!arith) return 0;
  if (rt == TY_STRING) {
    if (sp_streq(op, "+") || sp_streq(op, "*") || sp_streq(op, "%")) {
      *out = TY_STRING; return 1;
    }
    return 0;
  }
  if (rt == TY_FLOAT && (at == TY_FLOAT || at == TY_INT)) { *out = TY_FLOAT; return 1; }
  if (rt == TY_INT && at == TY_FLOAT) { *out = TY_FLOAT; return 1; }
  if (rt == TY_INT && at == TY_INT) {
    int promotes = is_add_sub_mul(op);
    if (g_promote_mode && promotes) return 0;
    *out = TY_INT; return 1;
  }
  return 0;
}

/* A builtin whose result follows its receiver's kind, called on a yield:
   yield.abs on an Integer block is an Integer, on a Float block a Float;
   yield.first on a String Array block is a String. The call is lowered from
   the current site's block type, so answer the type that lowering produces,
   from the table analyze_infer.c's YU_RECEIVER arm also reads
   (ty_recv_builtin_result): the analyzer made the yield poly exactly where
   every site's kind is in it, so each site's value is boxed into the slot.
   The receiver may itself be such a call (`yield.reverse.first`), typed per
   site the same way one link down. Only where the analyzer took that path,
   which it marks by typing the call poly: where some site's kind is not in
   the table the call keeps one site's type, and answering the other site's
   own kind here sent its value to the raise-tail arm, which dropped it for
   nil (`-"ab"` at one site, `-5` at another, printed nil for the second). */
static int yield_builtin_method_site_type(const Compiler *c, int id, TyKind *out) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *op = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!op || recv < 0) return 0;
  if (nt_kind(nt, recv) != NK_YieldNode && nt_kind(nt, recv) != NK_CallNode) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  if (c->ntype[id] != TY_POLY) return 0;
  int an = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  TyKind rt;
  if (!sp_yield_site_type(c, recv, &rt)) return 0;
  /* a site whose class reopens the name: the reopen's return type, for an
     Array or Hash as for a scalar (yield_recv_chain_kind says why) */
  { int rmi = nt_int(nt, id, "builtin_only", 0) ? -1 : comp_builtin_kind_reopen_mi((Compiler *)c, rt, op);
    if (rmi >= 0) {
      TyKind rr = c->scopes[rmi].ret;
      if (rr == TY_UNKNOWN || rr == TY_VOID) return 0;
      *out = rr;
      return 1;
    } }
  if (ty_recv_builtin_result(op, ac, ac == 1 && av ? comp_ntype(c, av[0]) : TY_UNKNOWN, rt, out)) return 1;
  /* a site the table does not answer, beside a reopen on the chain: the
     answer the analysis recorded for this site's kind */
  return an_yield_site_builtin_answer((Compiler *)c, id, rt, out);
}

int sp_yield_site_type(const Compiler *c, int id, TyKind *out) {
  if (g_block_id < 0 || id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") && yield_operator_site_type(c, id, out)) return 1;
  if (sp_streq(ty, "CallNode") && yield_builtin_method_site_type(c, id, out)) return 1;
  if (!sp_streq(ty, "YieldNode") &&
      !(sp_streq(ty, "CallNode") && blk_param_call(c, id))) return 0;
  int bbody = nt_ref(c->nt, g_block_id, "body");
  int bn = 0;
  const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
  if (bn <= 0 || !bb) return 0;
  int tail = bb[bn - 1];
  /* A block whose tail is itself a yield (`wrap { yield }`, or the
     `{ |__fwd| yield __fwd }` a named &block forwards as) answers what the
     block one level out answers, the one the splice will run: the same
     rule emit_boxed applies (#4495), so the two agree on the type of one
     node, a level further out for each tail that is a yield again. That
     tail's own cache is the union over every expansion. */
  for (int k = 1; nt_type(c->nt, tail) && sp_streq(nt_type(c->nt, tail), "YieldNode") &&
                  yield_block_out(k) >= 0; k++) {
    int fbody = nt_ref(c->nt, yield_block_out(k), "body");
    int fn = 0; const int *fb = fbody >= 0 ? nt_arr(c->nt, fbody, "body", &fn) : NULL;
    if (fn <= 0 || !fb) break;
    tail = fb[fn - 1];
  }
  TyKind bt = c->ntype[tail];
  /* only a CONCRETE per-site answer overrides the cache; an unresolved tail
     leaves the node's own (unified) type in place */
  if (bt == TY_UNKNOWN || bt == TY_VOID) return 0;
  /* a `next v` leaves the block with v: the value is the tail OR any next
     (`{ |i| next 7 if i == 1; nil }` is 7 or nil, not nil) */
  {
    TyKind nx = block_next_value_ntype(c, bbody);
    if (nx != TY_UNKNOWN && nx != TY_VOID) bt = ty_unify(bt, nx);
  }
  *out = bt;
  return 1;
}
int  g_yield_block_fallback = -1;
/* rename-table depth at g_block_id's DEFINITION site: the spliced block body
   and its param names resolve against the entries below this mark only; the
   enclosing inline's renames above it must not capture same-named block
   locals (#3281). Paired with g_block_id / g_yield_block_fallback. */
int  g_block_nren = 0;
int  g_yield_block_fallback_nren = 0;
/* see codegen_internal.h */
const char *g_block_owner_param_name = NULL;
const char *g_yield_block_fallback_param_name = NULL;
/* The (g_self, g_self_deref) that were active when the current g_block_id
   was captured -- i.e. the caller context of the innermost yield-method
   inline. A block spliced at a `yield` is caller code: emit_block_invoke
   emits its body under these instead of the inlined method's rebound self
   (`@map.vertices[...]` inside a block passed to an inlined method must
   read the CALLER's @map). Maintained by the inliners exactly like
   g_yield_block_fallback. */
const char *g_yield_self_fallback = NULL;
/* The same three for the block one level further out (g_yield_block_fallback):
   when a spliced block's own `yield` chains to that block, its body runs
   under THIS self, not the spliced block's. Recorded at every inline from
   the level-one values it replaces, and moved down a level as the splice
   moves g_block_id out (emit_block_invoke). Without it a block passed
   through a forwarding method into an inlined callee resolved its bare
   calls against the intermediate receiver's class: `r.cnt { |i| big?(i) }`
   with `def cnt(&) = @items.count(&)` folded `big?` to NoMethodError. */
const char *g_yield_self_fallback2 = NULL;
const char *g_yield_self_deref_fallback2 = NULL;
int g_yield_emitting_class_fallback2 = -1;
const char *g_yield_self_deref_fallback = NULL;
/* Companion to g_yield_self_fallback: the CALLER's emitting-class, so a
   block body spliced into an inlined callee resolves its implicit-self
   calls against the caller's class (the block is caller code). */
int g_yield_emitting_class_fallback = -1;
const char *g_block_param_name = NULL;
/* Inside an Enumerator.new { |y| ... } generator body, the name of the yielder
   block param. A `y << v` / `y.yield(v)` on it lowers to a Fiber.yield. */
const char *g_yielder_name = NULL;
const char *g_self = "self";
/* Member-access operator for `self`: "->" when self is a pointer (the usual
   heap object), "." when emitting a value-type method body (self is a value). */
const char *g_self_deref = "->";
/* When set, emit_inline_call_x binds self from this pre-hoisted expression
   instead of re-emitting the receiver node -- used by the poly-receiver block
   dispatch to bind self to a per-arm cast of the boxed receiver (#2448). */
const char *g_inline_recv_expr = NULL;
int g_inline_recv_class = -1;
int g_class_body_id = -1;
int g_emitting_class_id = -1;
const char *g_dm_subst_name = NULL;
int g_dm_subst_node = -1;
int g_ie_class_id = -1;
int g_ie_discard_value = 0;
int g_ie_nil_ivars = 0;
const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
const char *g_retry_label = NULL;
int g_redo_stack[64];
int g_redo_depth = 0;
int g_redo_pending = 0;
int g_redo_owner[64];
/* What stands open where each label is placed: frames, ensure regions, rescue clauses. */
static int g_redo_exc_base[64], g_redo_ensure_base[64], g_redo_rescue_base[64];
/* Opens a redo label for the body `owner` and answers it; 0 when the stack is full. */
int redo_label_push(int owner) {
  if (g_redo_depth >= (int)(sizeof g_redo_stack / sizeof g_redo_stack[0])) return 0;
  g_redo_owner[g_redo_depth] = owner;
  g_redo_exc_base[g_redo_depth] = g_exc_frame_depth;
  g_redo_ensure_base[g_redo_depth] = g_ensure_depth;
  g_redo_rescue_base[g_redo_depth] = g_rescue_save_depth;
  return g_redo_stack[g_redo_depth++] = ++g_tmp;
}
/* A redo goes back to the innermost label: it pops the frames of the begins it
   is written in, as a `next` does for its loop, and the exceptions of the
   rescue clauses entered since the label. With an ensure between, it is the
   bare goto it was; so it is where the regions are no longer counted, since
   an ensure there could not be seen. */
void emit_redo_unwind(Buf *b) {
  int top = g_redo_depth - 1;
  if (g_ensure_depth != g_redo_ensure_base[top] || g_ensure_depth >= MAX_ENSURE_DEPTH) return;
  int pops = g_exc_frame_depth - g_redo_exc_base[top], k = g_rescue_save_depth - g_redo_rescue_base[top];
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
const char *g_loop_break_var = NULL;
/* When a direct instance_exec/eval splice is wrapped in a do{}while(0), this
   holds the C result temp so a top-level `next <v>` captures its value before
   continuing out of the splice (mirrors g_loop_break_var for `break`). */
const char *g_ie_next_var = NULL;
/* C-loop nesting depth within the current function body: `next` emits a plain
   `continue` only when inside a C loop; at depth 0 inside a proc function it
   is the proc's own return (Ruby block semantics), not a loop control. */
int g_c_loop_depth = 0;
int g_in_proc_body = 0;
/* Set while emitting an instance_exec/eval splice whose result temp is poly:
   a `break <v>` / `next <v>` carrying a scalar value must box it to match. */
int g_ie_res_poly = 0;
/* Set while emitting a block body wrapped in a valued-break setjmp scope:
   the C lvalue holding the enclosing scope's SERIAL, so a top-level
   `break <v>` lowers to sp_brk_throw(<serial>, v) rather than a C `break`.
   NULLed when entering a nested C loop (while/for/until/loop) or another
   emission context (method/proc/fiber body, instance_exec splice) whose own
   `break` must not target this scope. Non-lambda procs capture its value at
   creation as their break home. */
const char *g_brk_ser_var = NULL;
/* g_ensure_depth at the enclosing wrapper: a break at the SAME depth has no
   intervening ensure bodies and delivers by same-function `goto` (register-
   safe: a longjmp would roll back register-allocated locals mutated since
   the setjmp -- the known hazard of the catch/throw machinery); a deeper
   break longjmps via sp_brk_throw so the ensures run. */
int g_brk_ensure_base = 0;
/* The break-scope serial var bound to the CURRENT block (g_block_id): saved
   at the call site when a yielding method is inlined, re-installed while the
   block body is spliced at a yield site -- so a `break` in the block targets
   the call that received it even when the yield sits inside the method's own
   loops or nested iterators. Paired with g_yield_block_fallback the same way
   g_block_id is. */
const char *g_block_brk_var = NULL;
const char *g_yield_blk_brk_fallback = NULL;
int g_block_brk_ebase = 0;
int g_yield_blk_brk_efallback = 0;
/* Proc-literal body context: 1 = lambda (break returns from the lambda),
   2 = non-lambda proc with a break (targets its captured home scope). */
int g_proc_body_kind = 0;
/* C expression for the non-lambda proc's captured break-home serial. */
const char *g_proc_brk_home = NULL;
/* The CallNode id currently being emitted as the inner (unwrapped) call of its
   own break wrapper, so the wrapper is not re-entered recursively. */
int g_brk_skip_id = -1;
const char *g_result_var = NULL;
int g_result_poly = 0;
/* The TyKind of the slot g_result_var names, so a tail value can be checked
   against it (a diverging call may carry an unrelated C type). */
TyKind g_result_ty = TY_UNKNOWN;
/* Non-lambda proc `return` support. While emitting a method that owns a
   proc-return frame, g_method_pr_label / g_method_pr_var name the single-exit
   goto label and the value var, so an explicit `return` funnels there (popping
   the frame once) instead of returning directly. While emitting a returning
   proc's body, g_proc_return_home is the C expression for the home frame index
   (read from the proc capture), so `return` longjmps to the home method. */
const char *g_method_pr_label = NULL;
const char *g_method_pr_var = NULL;
const char *g_proc_return_home = NULL;
/* While a constructor's omitted defaults are emitted: the C text of the object
   being built, so a default that reads self (`def initialize(n = config_val)`)
   resolves against the new instance rather than the caller's self. */
const char *g_ctor_self = NULL;
const char *g_ctor_self_deref = NULL;
/* While a dispatch arm binds its method's arguments: the receiver, cast to the
   arm's class, that method's scope, and the expression depth of the arm's call.
   A default the arm itself omits reads self (`def m(x, y = @extra)`) against
   the receiver, not the caller's self; a call nested in an argument sits
   deeper and binds its own. */
const char *g_arm_self = NULL;
const Scope *g_arm_scope = NULL;
int g_arm_depth = -1;
/* While an inliner binds one parameter of the method it splices: that method's
   scope, the rename depth at which its own locals (the parameters bound so
   far) resolve, and the receiver its body runs on (NULL keeps the caller's).
   A default is callee code, so it reads the earlier parameters under their
   inlined names and self as the receiver. A call nested in an argument sits
   deeper than the binding and is not this one. */
const Scope *g_inl_dflt_scope = NULL;
int g_inl_dflt_nren = 0;
const char *g_inl_dflt_self = NULL;
const char *g_inl_dflt_deref = NULL;
int g_inl_dflt_class = -1;
int g_inl_dflt_depth = -1;
InlDflt inl_dflt_enter(const Scope *m, int nren, const char *self, const char *deref, int cls) {
  InlDflt sv = { g_inl_dflt_scope, g_inl_dflt_nren, g_inl_dflt_class, g_inl_dflt_depth,
                 g_inl_dflt_self, g_inl_dflt_deref };
  g_inl_dflt_scope = m; g_inl_dflt_nren = nren; g_inl_dflt_class = cls;
  g_inl_dflt_depth = g_expr_depth; g_inl_dflt_self = self; g_inl_dflt_deref = deref;
  return sv;
}
void inl_dflt_leave(InlDflt sv) {
  g_inl_dflt_scope = sv.scope; g_inl_dflt_nren = sv.nren; g_inl_dflt_class = sv.cls;
  g_inl_dflt_depth = sv.depth; g_inl_dflt_self = sv.self; g_inl_dflt_deref = sv.deref;
}
/* Emitting the body of a non-lambda proc created at top level: a `return`
   there is a TOP-LEVEL return, which ends the script (#3663). */
int g_proc_toplevel_return = 0;
/* Number of live setjmp exception frames (begin/rescue) enclosing the
   current emission point. A `return` from inside a try body must pop them
   (sp_exc_top -= N) before leaving -- a stale frame's jmp_buf points into
   a dead C stack frame, and the next raise longjmps into it (doom's
   SoundManager#[] early returns corrupted the stack this way).
   g_method_pr_exc_depth snapshots the depth at the return-funnel target so
   funnel gotos pop only the frames they actually exit. */
int g_exc_frame_depth = 0;
int g_loop_exc_base = 0;        /* frame depth at the innermost C-loop entry */
int g_loop_rescue_base = 0;     /* rescue bodies open at that entry (g_rescue_save_depth) */
int g_loop_ensure_base = 0;     /* ensure depth at the innermost C-loop entry */
int g_brk_exc_base = 0;         /* frame depth at the valued-break wrapper */
int g_block_brk_exc_base = 0;   /* ... for yield-block re-entry (mirrors g_block_brk_ebase) */
int g_brk_rescue_base = 0;      /* rescue bodies open at the valued-break wrapper */
int g_block_brk_rescue_base = 0;
int g_method_pr_exc_depth = 0;
/* ... and the ENSURE depth at that target. A `return` inside a yielding
   method's body inlined under a caller's begin..ensure was routed through
   the caller's frame (the frame route fired on any open ensure), which made
   the CALLER return: a Logger#add early-out spliced under Dir.mktmpdir's
   ensure ended the whole program silently. The funnel wins unless an ensure
   opened INSIDE the spliced body since the label was installed. */
int g_method_pr_ensure_depth = 0;
/* Loop-invariant string-length hoisting: while a loop whose receiver string is
   not mutated in its body is being emitted, g_hoist_len_recv holds that
   receiver's AST local name and g_hoist_len_var the C temp caching its length;
   a matching `s.length`/`s.size` then emits the temp instead of strlen. */
const char *g_hoist_len_var = NULL;
const char *g_hoist_len_recv = NULL;
TyKind g_ret_type = TY_UNKNOWN;
/* The C function being emitted returns void, whatever g_ret_type says the
   Ruby body's value type is. A fiber/thread body is that case: it is
   `static void _fiber_body_N(sp_Fiber *)` and publishes its value through
   _fb->yielded_value, while g_ret_type is TY_POLY so the body's own
   expressions type. `return <value>` emitted into it is a C constraint
   violation, and GCC 14 rejects it. */
int g_c_ret_void = 0;
/* The body of the Fiber.new / Thread.new / Enumerator.new block whose C
   function is being emitted, or -1. A `next` this body owns leaves that
   function (emit_next_leaving_body). */
int g_fiber_body = -1;
/* Mirror of the REAL enclosing function's return funnel: yield-method inlining
   overrides g_method_pr_label/-_var/g_ret_type with a per-inline funnel, and a
   spliced block body (which lexically belongs to the real function, so its
   `return` exits that method) restores from these. Set wherever a fresh
   function context installs (or clears) its funnel. */
const char *g_fn_pr_label = NULL;
const char *g_fn_pr_var = NULL;
TyKind g_fn_ret_type = TY_UNKNOWN;
/* The slot the proc body being emitted answers through, or NULL when the C
   function being emitted is not a value-carrying proc's (a method, a fiber
   body, a proc whose value is nil). An exit that leaves the proc writes
   here: a `next`, a lambda's `return`, either one deferred through an
   `ensure`. g_result_var does not name it for them: a `begin`, an `if` or a
   `case` in value position inside the body puts its own temp there while
   its arms are emitted. */
const char *proc_ret_slot(void) {
  return g_in_proc_body && !g_c_ret_void && g_fn_ret_type == TY_POLY ? "_sp_proc_poly_ret" : NULL;
}
int g_current_scope_is_lowered = 0;
/* the scope being emitted has an --rbs-seeded return type (#3412) */
int g_ret_seeded = 0;
/* The block-param name of the lowered method currently being emitted (its
   declared &block name, or "__yblk__" when the lowering synthesized one);
   NULL outside a lowered-method emission. Read by emit_yblk_ref. */
const char *g_lowered_blk_name = NULL;
/* Set while emitting a lifted Thread/Fiber body that reaches the lowered
   method's block: the block lives in a cell the frame carries. */
int g_yblk_celled = 0;
/* The enclosing lowered context parked across an inline: an inlined callee's
   own yields splice the call-site block, so emit_inline_call_x clears the
   lowered pair for the callee body and parks it here; emit_block_invoke
   restores it around spliced CALLER code (whose yields do belong to the
   lowered method) -- the same discipline as g_yield_self_fallback. */
int g_yield_lowered_fallback = 0;
const char *g_yield_proc_ref_fallback = NULL;
TyKind g_yield_slot_ty_fallback = TY_UNKNOWN;
const char *g_yield_lowered_blk_fallback = NULL;
/* When a yielding method is inlined and its block is a forwarded REAL proc
   (the caller nil-checks its &block, so the block can't be an inlined literal),
   this holds the C expression for that proc; the inlined `yield` calls it via
   sp_proc_call instead of splicing a block body. NULL otherwise. */
const char *g_yield_proc_ref = NULL;
/* The block argument's value behind g_yield_proc_ref, when the inline that
   set it could name one (`run(s, &method(:m))`, `run(s, &pr)`), and the ref
   it was named for: a yield checks the pair still matches before it trusts
   the node, since the ref is swapped in more places than this is. -1 when
   unknown. Read by refuse_yield_string_copies (#6179). */
int g_yield_proc_expr = -1;
/* The method whose call sites pass the block, when the yield is in a proc
   form and the proc is its block parameter; else -1. */
int g_yield_proc_method = -1;
const char *g_yield_proc_expr_ref = NULL;
/* The inlined call's return-slot type while g_yield_proc_ref is set: sp_proc_call
   yields poly, but the slot may be concrete (the analyzer typed this forwarding
   context), so a value-position yield unboxes its result to this. */
TyKind g_yield_slot_ty = TY_UNKNOWN;
/* One level further out still, paired with g_yield_self_fallback2: see
   codegen_internal.h. */
const char *g_yield_proc_ref_fallback2 = NULL;
TyKind g_yield_slot_ty_fallback2 = TY_UNKNOWN;
EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
int       g_ensure_depth = 0;
RescueSave g_rescue_save_stack[MAX_ENSURE_DEPTH];
int        g_rescue_save_depth = 0;

/* rescue bodies crossed by an exit to frame-depth pop_base: those entered at or
   deeper than pop_base (their exc_base >= pop_base). */
int rescues_crossed(int pop_base) {
  int k = 0;
  for (int i = 0; i < g_rescue_save_depth; i++)
    if (g_rescue_save_stack[i].exc_base >= pop_base) k++;
  return k;
}
/* Pop the k crossed rescue-body handlers (no frame pop). Used at sites whose
   frame-pop text is special (the begin..ensure deferred-return). */
void emit_cur_exc_restore(Buf *b, int pop_base) {
  int k = rescues_crossed(pop_base);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
/* Pop the handlers of the rescue bodies entered since `rescue_base` (the
   g_rescue_save_depth an ensure or a loop recorded on entry). The frame
   depth alone cannot tell them from one entered before at the same depth:
   a `next` in a block inside a rescue cleared that rescue's $! (#8208). */
void emit_rescue_pops_to(Buf *b, int rescue_base) {
  int k = g_rescue_save_depth - rescue_base;
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
/* A break or next leaving the innermost C loop: the frames and the rescue
   handlers opened inside its body. */
void emit_loop_unwind(Buf *b) {
  int pops = g_exc_frame_depth - g_loop_exc_base;
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  emit_rescue_pops_to(b, g_loop_rescue_base);
}
int emit_frame_unwind(Buf *b, int pop_base, const char *guard) {
  int pops = g_exc_frame_depth - pop_base;
  int k = rescues_crossed(pop_base);
  if (pops <= 0 && k == 0) return 0;
  if (guard) buf_printf(b, "if (%s) { ", guard);
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
  if (guard) buf_puts(b, "}");
  return 1;
}
Buf g_procs;
Buf g_proc_protos;
Buf g_pd_protos;
Buf g_pd_defs;
int g_proc_counter = 0;
int g_needs_proc_poly_argslot = 0; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
int g_fiber_counter = 0;
char **g_re_src; int *g_re_flg; int g_re_count, g_re_cap;
int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
/* The RegularExpressionNode behind `nid`, or -1 when the pattern is only
   knowable at run time. A bare literal, a constant bound to one
   (`PAT = /re/[.freeze]`, possibly namespaced) and a regex-typed local bound to
   one all resolve.

   One resolver for every caller. re_lit_index, re_lit_src and re_lit_flags each
   carried their own copy and they disagreed about names: re_lit_src did not
   follow a local, so codegen picked the capturing `scan` emit for a pattern
   named by a constant while typing the same call from the non-capturing shape,
   and dropped the capture groups for one named by a local (#3391). */
/* ---- whole-program indexes for codegen (#4966) ----
   Several emit-time questions were answered by walking every node, once per
   method, per scope or per call site: quadratic in program size. The answers
   depend only on the node table, which codegen extends in two places (each
   setting the new node's scope right away) and otherwise only renames call
   names in place, so the indexes are keyed on the table and its counts. */
static const NodeTable *sn_nt; static int sn_count = -1, sn_nscopes = -1;
static int *sn_off, *sn_ids;
/* A caller may be walking a list when a query from inside its loop meets a
   grown table and rebuilds: the previous arrays stay alive one more build. */
static int *sn_off_prev, *sn_ids_prev;
static void sn_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int ns = c->nscopes, n = nt->count;
  free(sn_off_prev); free(sn_ids_prev);
  sn_off_prev = sn_off; sn_ids_prev = sn_ids;
  sn_off = calloc((size_t)ns + 2, sizeof(int));
  sn_ids = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  for (int id = 0; id < n; id++) {
    int si = c->nscope[id];
    if (si >= 0 && si < ns) sn_off[si + 2]++;
  }
  for (int si = 0; si < ns; si++) sn_off[si + 2] += sn_off[si + 1];
  for (int id = 0; id < n; id++) {
    int si = c->nscope[id];
    if (si >= 0 && si < ns) sn_ids[sn_off[si + 1]++] = id;
  }
  sn_nt = nt; sn_count = n; sn_nscopes = ns;
}
const int *cg_scope_nodes(Compiler *c, int si, int *n) {
  if (sn_nt != c->nt || sn_count != c->nt->count || sn_nscopes != c->nscopes) sn_build(c);
  if (si < 0 || si >= sn_nscopes) { *n = 0; return sn_ids; }
  *n = sn_off[si + 1] - sn_off[si];
  return sn_ids + sn_off[si];
}

static const NodeTable *bo_nt; static int bo_count = -1;
static int *bo_owner;
int cg_block_owner(Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  /* nodes codegen appends only extend the map: each is above every node
     already in it, so the lowest owner of an older block cannot change */
  if (bo_nt == nt && bo_count >= 0 && nt->count > bo_count) {
    int *nb = realloc(bo_owner, sizeof(int) * (size_t)nt->count);
    if (nb) {
      bo_owner = nb;
      for (int i = bo_count; i < nt->count; i++) bo_owner[i] = -1;
      for (int o = bo_count; o < nt->count; o++) {
        int b = nt_ref(nt, o, "block");
        if (b >= 0 && b < nt->count && bo_owner[b] < 0) bo_owner[b] = o;
      }
      bo_count = nt->count;
    }
  }
  if (bo_nt != nt || bo_count != nt->count) {
    free(bo_owner);
    bo_owner = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    for (int i = 0; i < nt->count; i++) bo_owner[i] = -1;
    for (int o = nt->count - 1; o >= 0; o--) {   /* descending: the lowest owner wins */
      int b = nt_ref(nt, o, "block");
      if (b >= 0 && b < nt->count) bo_owner[b] = o;
    }
    bo_nt = nt; bo_count = nt->count;
  }
  return (blk >= 0 && blk < bo_count) ? bo_owner[blk] : -1;
}

struct CgMemoEnt { char *key; int tag; int val; CgMemoEnt *next; };
#define CG_MEMO_BUCKETS 1024
static unsigned cg_memo_hash(const char *s, int tag) {
  unsigned h = 2166136261u ^ (unsigned)tag;
  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}
int cg_memo_get(Compiler *c, CgMemo *m, const char *key, int tag, int *val) {
  /* appended nodes the answers do not depend on (codegen's own to_s calls)
     keep the memo */
  if (m->tab && m->touches && m->nt == c->nt && m->nscopes == c->nscopes && c->nt->count > m->count) {
    int hit = 0;
    for (int id = m->count; id < c->nt->count && !hit; id++) hit = m->touches(c, id);
    if (!hit) m->count = c->nt->count;
  }
  if (m->tab && (m->nt != c->nt || m->count != c->nt->count || m->nscopes != c->nscopes)) {
    for (int b = 0; b < CG_MEMO_BUCKETS; b++)
      for (CgMemoEnt *e = m->tab[b]; e; ) { CgMemoEnt *nx = e->next; free(e->key); free(e); e = nx; }
    free(m->tab); m->tab = NULL;
  }
  if (!m->tab) {
    m->tab = calloc(CG_MEMO_BUCKETS, sizeof *m->tab);
    m->nt = c->nt; m->count = c->nt->count; m->nscopes = c->nscopes;
  }
  for (CgMemoEnt *e = m->tab[cg_memo_hash(key, tag) % CG_MEMO_BUCKETS]; e; e = e->next)
    if (e->tag == tag && sp_streq(e->key, key)) { *val = e->val; return 1; }
  return 0;
}
void cg_memo_put(CgMemo *m, const char *key, int tag, int val) {
  if (!m->tab) return;
  CgMemoEnt *e = malloc(sizeof *e);
  e->key = strdup(key); e->tag = tag; e->val = val;
  unsigned b = cg_memo_hash(key, tag) % CG_MEMO_BUCKETS;
  e->next = m->tab[b]; m->tab[b] = e;
}

static int re_lit_write_node(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  return k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode || k == NK_LocalVariableWriteNode ||
         k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
         k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableTargetNode;
}
int re_lit_node(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "RegularExpressionNode")) return nid;
  int want_const = sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode");
  int want_local = sp_streq(ty, "LocalVariableReadNode") && comp_ntype(c, nid) == TY_REGEX;
  if (!want_const && !want_local) return -1;
  const char *nm = nt_str(nt, nid, "name");
  if (!nm) return -1;
  /* a constant's answer is fixed by its name, a local's by its scope and
     name: one scan per key, not per use */
  static CgMemo memo = { .touches = re_lit_write_node };
  Scope *sc = want_local ? comp_scope_of(c, nid) : NULL;
  int tag = want_const ? 1 : sc ? 2 + (int)(sc - c->scopes) : 0;
  int got;
  if (cg_memo_get(c, &memo, nm, tag, &got)) return got;
  int found = -1;
  if (want_local) found = an_regex_local_lit(c, nid);
  for (int k = 0; want_const && k < nt->count && found < 0; k++) {
    const char *kt = nt_type(nt, k);
    if (!kt) continue;
    if (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode")) continue;
    const char *kn = nt_str(nt, k, "name");
    if (!kn || !sp_streq(kn, nm)) continue;
    int v = nt_ref(nt, k, "value");
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze"))
      v = nt_ref(nt, v, "receiver");
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "RegularExpressionNode")) found = v;
  }
  cg_memo_put(&memo, nm, tag, found);
  return found;
}
int re_lit_index(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  if (nid < 0) return -1;
  const char *src = nt_str(c->nt, nid, "unescaped");
  if (!src) return -1;
  int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
  for (int i = 0; i < g_re_count; i++)
    if (g_re_flg[i] == flg && sp_streq(g_re_src[i], src)) return i;
  /* A literal is two constants, the pattern and its flags, so whether the
     engine can read it is known here rather than at the program's startup,
     where it used to surface as a RegexpError from a build that had reported
     nothing. CRuby reports it from the parse, as a SyntaxError. Checked once
     per distinct literal, after the table lookup above. */
  {
    const char *err = sp_re_literal_error(src, (int)strlen(src), flg);
    if (err) unsupported_feature(c, nid, err);
  }
  if (g_re_count >= g_re_cap) {
    g_re_cap = g_re_cap ? g_re_cap * 2 : 8;
    g_re_src = realloc(g_re_src, sizeof(char *) * (size_t)g_re_cap);
    g_re_flg = realloc(g_re_flg, sizeof(int) * (size_t)g_re_cap);
  }
  g_re_src[g_re_count] = (char *)src;
  g_re_flg[g_re_count] = flg;
  return g_re_count++;
}
const char *re_lit_src(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  return nid < 0 ? NULL : nt_str(c->nt, nid, "unescaped");
}
void emit_interp(Compiler *c, int id, Buf *b);  /* forward */
int emit_interp_append(Compiler *c, int id, const char *open, const char *open_n, Buf *b, int indent);

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && sp_streq(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    /* Emit the interpolated pattern into a local buffer: an embedded call that
       roots its own args pushes those decls to g_pre, which must land as whole
       statements before this temp's decl, not inside its initializer (#1498). */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_interp(c, nid, &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    /* the pattern text may hold a NUL (Regexp.new("a\0b")), and strlen would
       compile only the part before it */
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)sp_str_byte_len(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}
int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->v[i], nm)) return 1;
  return 0;
}
void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
const char *g_cap_struct = NULL;
NameSet *g_cap_names = NULL;
int g_needs_at_exit = 0;
int g_needs_class_machinery = 0;
int g_has_user_global_marks = 0;
/* The distinct out-of-int64 integer LITERALS of this TU. Each was emitted as
   its own sp_bigint_new_str(...) at every use, so one `0xFFFFFFFF00000000`
   written in forty places parsed a decimal string and allocated a Bignum
   forty times over, every time the line ran. An Integer is immutable, so the
   value can be built once per distinct literal and shared: these hold the
   strings, the emitter names slot i, and the slot is filled on first use and
   marked with the other TU globals. */
char **g_bigl_val = NULL;
int g_bigl_n = 0, g_bigl_cap = 0;
int bigl_intern(const char *v) {
  if (!v) return -1;
  for (int i = 0; i < g_bigl_n; i++) if (sp_streq(g_bigl_val[i], v)) return i;
  if (g_bigl_n >= g_bigl_cap) {
    g_bigl_cap = g_bigl_cap ? g_bigl_cap * 2 : 16;
    g_bigl_val = (char **)realloc(g_bigl_val, sizeof(char *) * (size_t)g_bigl_cap);
  }
  g_bigl_val[g_bigl_n] = strdup(v);
  return g_bigl_n++;
}
int g_uses_symbols = 0;
int g_uses_marshal = 0;
int g_emit_sym_rt = 0;
int g_emit_class_names = 0;
int g_emit_obj_dispatch = 0;
int g_uses_program_name = 0;
int g_reads_match_regs = 0;
int g_sub_bang_id = -1, g_sub_bang_tm = 0;
int g_gen_obj_hash = 0;
int g_gen_obj_to_json = 0;
int g_gen_obj_to_h = 0;
int g_gen_obj_struct_values = 0;
int g_gen_cls_answers = 0;
int g_gen_obj_with = 0;
int g_uses_regex = 0;
int g_uses_argv = 0;
int g_uses_threads = 0;
int g_uses_finalizers = 0;   /* ObjectSpace.define_finalizer: SP_FIN_POLL at safe points */
int g_uses_ensure = 0;       /* an ensure clause: the landings that give sp_inflight_cause back */
int g_has_user_cmp = 0;
int g_has_user_binop = 0;
int g_has_user_aset = 0;
TyKind g_ie_next_ty = TY_UNKNOWN;
int g_has_user_coerce = 0;
int g_has_user_to_io = 0;
int g_gen_obj_hashkey = 0;
int g_gen_obj_valeq = 0;
int g_re_init_needed = 0;
/* A value written into a TYPED array's element slot. A value decided at
   run time (poly) goes through the element check, which stores its own kind
   and refuses a foreign one with TypeError rather than coercing it (#4481);
   a statically typed value is emitted as it is (the emitter's own
   int/float/string forms already convert between the numeric kinds). An
   element slot takes nil, so a nilable Integer passes its nil, as `<<`
   and `[]=` store it, rather than the strict slot's TypeError: analyze marks
   the array it lands in (unshift, insert, fill) as able to hold one. */
void emit_typed_elem_value(Compiler *c, int node, TyKind et, Buf *b) {
  Repr vr = repr_of(c, node);
  TyKind vt = vr.as_ty;
  if (vr.kind == RK_BOXED && (et == TY_INT || et == TY_FLOAT || et == TY_STRING)) {
    buf_printf(b, "sp_poly_elem_%s(", et == TY_INT ? "i" : et == TY_FLOAT ? "f" : "s");
    emit_expr(c, node, b);
    buf_puts(b, ")");
    return;
  }
  /* An untyped value that never arrives -- an unresolved call's raise token,
     a call to a method that answers no value -- is coerced for its raise, as
     in the other typed slots. The numeric forms discard the token already. */
  if (vt == TY_UNKNOWN && (et == TY_STRING || call_answers_no_value(c, node))) {
    emit_unresolved_coerced(c, node, et, b);
    return;
  }
  if (et == TY_INT) emit_int_expr_nilable(c, node, b);
  else if (et == TY_FLOAT) emit_float_expr(c, node, b);
  else emit_coerce(c, node, et, CO_HOLD, "an Array element", b);
}
/* A POLY variable's slot `ref` lifted into the shared handle as it is read
   (sp_poly_strbuf_lift, #6179): the lifted value is taken into a temp first
   and stored after, so the store into a captured local's cell follows its
   write barrier with nothing allocating in between. Stored in one
   expression, the barrier could run before the lift allocated, and a
   collection there could age the cell past it. */
void emit_poly_lift_ref(const char *ref, Buf *b) {
  int t = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_lift(%s); %s = _t%d; _t%d; })", t, ref, ref, t, t);
}
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  emit_scope_local_ref(c, scope_node >= 0 ? comp_scope_of(c, scope_node) : NULL, name, b);
}
/* Is `rn`, what rename_local answers for local `name`, the shadow a
   shared-handle shim reads and writes that local through (`_sbN`, the
   shim's `lv__sbN`)? Only as a rename: a local the program itself names
   `_sb1` is its own. */
int sb_shim_shadow(const char *name, const char *rn) {
  return rn != name && !strncmp(rn, "_sb", 3) && isdigit((unsigned char)rn[3]);
}
/* A shim types its receiver local String for the arm it re-runs, though the
   slot is the handle (sb_shim_enter); it answers the local's shim state as
   it was, which sb_shim_leave puts back whole. A shim inside a proc made in
   another shim's arm enters a local the outer one lifted: putting back only
   the type cleared the outer shim's shim_ty, so its drop skipped the local,
   which kept the handle type for the rest of the outer arm. */
SbShimSave sb_shim_enter(LocalVar *lv) {
  SbShimSave sv = { lv->type, lv->shim_ty, lv->shim_lift };
  lv->shim_ty = lv->type;
  lv->type = TY_STRING;
  lv->shim_lift = 0;
  return sv;
}
void sb_shim_leave(LocalVar *lv, SbShimSave sv) {
  lv->type = sv.type;
  lv->shim_ty = sv.shim_ty;
  lv->shim_lift = sv.shim_lift;
}
/* A proc or a fiber made inside the arm shares the real slot, and types its
   capture struct and its body from the local: put the handle type back on
   the locals a live shim lowered, for it (sb_shim_drop sets them back). Only
   the shim's own local carries shim_ty, so a same-named local of an inlined
   method is left alone. */
static void sb_shim_lifted(Compiler *c, int node, int delta) {
  Scope *sc = node >= 0 ? comp_scope_of(c, node) : NULL;
  if (!sc) return;
  for (int i = 0; i < g_nren; i++) {
    if (!sb_shim_shadow(g_ren_from[i], g_ren_to[i])) continue;
    LocalVar *lv = scope_local(sc, g_ren_from[i]);
    if (!lv || lv->shim_ty == TY_UNKNOWN) continue;
    if (delta > 0) { if (lv->shim_lift++ == 0) lv->type = lv->shim_ty; }
    else if (--lv->shim_lift == 0) lv->type = TY_STRING;
  }
}
void sb_shim_lift(Compiler *c, int node) { sb_shim_lifted(c, node, 1); }
void sb_shim_drop(Compiler *c, int node) { sb_shim_lifted(c, node, -1); }
void emit_scope_local_ref(Compiler *c, Scope *s, const char *name, Buf *b) {
  /* Inside a shared-handle shim the local reads as the shim's shadow, a
     plain C local the shim declares, even when the local itself lives in a
     cell or a capture field: that slot holds the handle, which the shim
     reads before and writes back after. The cell form spelled the shadow
     `(*_cell__sbN)`, and the capture form handed the arm the handle slot,
     neither of which compiles. */
  const char *rn = rename_local(name);
  if (sb_shim_shadow(name, rn)) {
    buf_printf(b, "lv_%s", rn);
    return;
  }
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    /* A TY_PROC capture is stored as (sp_int)(uintptr_t)sp_Proc* in the cell.
       Cast it back to sp_Proc* so call sites work. A heap-object cell is a real
       typed pointer, so its deref is already the right lvalue (no cast). */
    LocalVar *clv = s ? scope_local(s, name) : NULL;
    if (clv && clv->type == TY_PROC)
      buf_printf(b, "(sp_Proc *)(uintptr_t)(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    else
      buf_printf(b, "(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = s ? scope_local(s, name) : NULL;
  if (lv && lv->is_cell) {
    /* Through the rename map, exactly as the plain form below: a method
       INLINED at its call site renames its locals, and the cell form did not
       follow -- the prologue declared `lv__y1_n` while the body read
       `(*_cell_n)`, which nothing declared (#4088). Outside an inline the map
       is empty and this is the name itself. */
    if (lv->type == TY_PROC) buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", rn);
    else buf_printf(b, "(*_cell_%s)", rn);
    return;
  }
  buf_printf(b, "lv_%s", rn);
}
void emit_yblk_ref(Buf *b) {
  /* The lowered method's block param: the declared &block name when the def
     has one, else the synthetic __yblk__. */
  const char *nm =
      (g_lowered_blk_name && g_lowered_blk_name[0]) ? g_lowered_blk_name : "__yblk__";
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm)) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*(((%s *)_cap)->c_%s))", g_cap_struct, nm);
  }
  /* A yield inside a lifted Thread/Fiber body reaches the forwarded block
     through the shared cell the body's frame carries, not a local (#3355). */
  else if (g_yblk_celled) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", nm);
  }
  else {
    buf_printf(b, "lv_%s", nm);
  }
}
void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}
/* The C representation of Ruby `nil` for a concretely-typed slot (vs
   default_value's zero-value): a fresh block-local starts nil, and several
   types have a nil of their own (NULL string, the oint's flag for a number,
   (sp_sym)-1). Types with none fall back to the zero value. */
const char *nil_value(TyKind t) {
  /* a builtin kind's nil is its ty_traits row's (types.c): a String, an
     Integer, a Float and a boxed value have one; any other kind, none */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->nil : NULL;
}

/* Does the program ask whether class variable `nm` ("@@x") is set yet --
   `defined?(@@x)` or `class_variable_defined?(:@@x)`? Only such a cvar
   carries a cvar_<C>_<x>__set flag, so every other program's writes stay
   plain stores. */
int cvar_defined_probed(Compiler *c, const char *nm) {
  const NodeTable *nt = c->nt;
  if (!nm) return 0;
  for (int k = 0; k < nt->count; k++) {
    const char *kt = nt_type(nt, k);
    if (!kt) continue;
    if (sp_streq(kt, "DefinedNode")) {
      int v = nt_ref(nt, k, "value");
      const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
      if (vt && sp_streq(vt, "ClassVariableReadNode") && nt_str(nt, v, "name") &&
          sp_streq(nt_str(nt, v, "name"), nm)) return 1;
    }
    else if (sp_streq(kt, "CallNode") && nt_str(nt, k, "name") &&
             sp_streq(nt_str(nt, k, "name"), "class_variable_defined?")) {
      int an = 0; int aa = nt_ref(nt, k, "arguments");
      const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
      if (an < 1) continue;
      const char *at = nt_type(nt, av[0]);
      const char *s = !at ? NULL : sp_streq(at, "SymbolNode") ? nt_str(nt, av[0], "value")
                    : sp_streq(at, "StringNode") ? nt_str(nt, av[0], "content") : NULL;
      if (s && sp_streq(s, nm)) return 1;
    }
  }
  return 0;
}

/* Mark class `cid`'s cvar `nm` as assigned ahead of a store to it: a
   statement (`cvar_C_x__set = 1; `) or, with as_expr, the head of a comma
   expression (`cvar_C_x__set = 1, `). Nothing when nothing probes it. */
void emit_cvar_set_flag(Compiler *c, int cid, const char *nm, int as_expr, Buf *b) {
  if (cid < 0 || !nm || !cvar_defined_probed(c, nm)) return;
  buf_printf(b, "cvar_%s_%s__set = 1%s", c->classes[cid].name, nm + 2, as_expr ? ", " : "; ");
}

/* The flag set after the store, in value position: `(cvar = rhs` + this +
   `)` keeps the cvar's value as the expression's, and the right-hand side
   still sees the cvar unset. */
void emit_cvar_set_flag_after(Compiler *c, int cid, const char *nm, Buf *b) {
  if (cid < 0 || !nm || !cvar_defined_probed(c, nm)) return;
  buf_printf(b, ", cvar_%s_%s__set = 1, cvar_%s_%s", c->classes[cid].name, nm + 2,
             c->classes[cid].name, nm + 2);
}

static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm);
int subtree_has_param_named_pub(const NodeTable *nt, int id, const char *nm) {
  return subtree_has_param_named(nt, id, nm);
}
static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);

  /* numbered params (_1.._9): the NumberedParametersNode carries no child
     parameter nodes, yet the block's locals list contains the names. */
  /* `_1` as the parser wrote it, and `_1__bNN` where scope_numbered_block_params
     gave a colliding scope's blocks their own -- the same two spellings the
     shadow rename leaves for an ordinary parameter, matched the same way. */
  if (ty && sp_streq(ty, "NumberedParametersNode") &&
      nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' &&
      (!nm[2] || !strncmp(nm + 2, "__b", 3))) return 1;
  if (ty && sp_streq(ty, "ItParametersNode") && sp_streq(nm, "it")) return 1;
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    /* shadow-renaming rewrites a param's node-table name to NAME__bpNN;
       the parser's locals list keeps the raw NAME -- match both. */
    if (pn) {
      size_t nl = strlen(nm);
      if (sp_streq(pn, nm)) return 1;
      if (block_param_written_len(pn) == nl && !strncmp(pn, nm, nl)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_param_named(nt, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_param_named(nt, ids[j], nm)) return 1;
  }
  return 0;
}

/* A FRESH cell for a captured block variable, at the top of an invocation:
   the capture struct copies the current cell pointer at proc creation, so
   per-iteration closures each keep their own binding. `cellv` names the
   cell variable -- the frame's `_cell_x`, or the capture slot when the body
   is inlined inside a proc function (#4127). */
static void emit_fresh_cell(Compiler *c, LocalVar *lv, const char *cellv, Buf *b, int indent) {
  emit_indent(b, indent);
  /* an Integer or Float slot holding its nil beside the value: a cell of the oint */
  if (oint_kind(lv->type) && slot_is_oint(lv)) {
    const char *ot = oint_ctype(lv->type);
    buf_printf(b, "%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, NULL); *%s = %s;\n", cellv, ot, ot, cellv, oint_nil(lv->type));
  }
  else if (lv->type == TY_FLOAT) {
    buf_printf(b, "%s = (sp_float *)sp_gc_alloc(sizeof(sp_float), NULL, NULL); *%s = 0.0;\n", cellv, cellv);
  }
  else if (lv->type == TY_POLY) {
    buf_printf(b, "%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval); *%s = sp_box_nil();\n", cellv, cellv);
  }
  else if (cell_value_struct(lv->type)) {
    /* a by-value struct rides a cell of its own type, as the two cell
       prologues already say; without this arm the reset fell through to
       the sp_int else and assigned an sp_int * to an sp_Class * */
    const char *vs = cell_value_struct(lv->type);
    buf_printf(b, "%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, %s); *%s = %s;\n",
               cellv, vs, vs, cell_value_struct_scan(lv->type), cellv,
               cell_value_struct_empty(lv->type));
  }
  else if (lv->type != TY_PROC && lv->type != TY_INT && lv->type != TY_BOOL &&
           lv->type != TY_SYMBOL && lv->type != TY_UNKNOWN && cell_is_typed_ptr(c, lv)) {
    const char *cell_scan = cell_scan_fn(lv->type);
    buf_printf(b, "%s = (", cellv); emit_ctype(c, lv->type, b);
    buf_puts(b, " *)sp_gc_alloc(sizeof(");
    emit_ctype(c, lv->type, b);
    buf_printf(b, "), NULL, %s); *%s = NULL;\n", cell_scan, cellv);
  }
  else if (lv->type == TY_PROC) {
    /* an int cell holding a collectable Proc still needs a scan (#4077) */
    buf_printf(b, "%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, sp_cell_scan_procint); *%s = 0;\n", cellv, cellv);
  }
  else {
    buf_printf(b, "%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, NULL); *%s = 0;\n", cellv, cellv);
  }
  /* The fresh cell is young and the capture struct it was just stored
     into can be old (the enclosing proc outlived a promotion), so the
     store is recorded -- after it, since the allocation above can
     collect and clear a record made before it. The `_cell_x` form is
     a frame local and needs nothing. */
  if (cellv[0] == '(') {
    emit_indent(b, indent);
    buf_puts(b, "sp_gc_wb((void *)_cap);\n");
  }
}
/* Which C expression names a scope's cell variable from here. */
static const char *cell_var_name(const char *rn, char *buf, size_t cap) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, rn))
    snprintf(buf, cap, "((%s *)_cap)->c_%s", g_cap_struct, rn);
  else
    snprintf(buf, cap, "_cell_%s", rn);
  return buf;
}
void emit_block_locals_reset(Compiler *c, int blk, Buf *b, int indent) {
  if (blk < 0) return;
  const char *locs = nt_str(c->nt, blk, "locals");
  if (!locs || !*locs) return;
  char tmpn_buf[128];
  const char *p = locs;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l) {
      /* names longer than the stack buffer are legal Ruby; heap-fall back
         rather than silently skipping the reset */
      char *tmpn = l < sizeof tmpn_buf ? tmpn_buf : malloc(l + 1);
      if (!tmpn) break;
      memcpy(tmpn, p, l); tmpn[l] = 0;
      /* Skip every name bound by the block's parameter list, including
         destructured, optional, rest, and shadow (`; x`) declarations --
         collected straight from the parameters subtree, since
         block_param_name only covers plain leading params. */
      int is_param = subtree_has_param_named(c->nt, nt_ref(c->nt, blk, "parameters"), tmpn);

      if (is_param) {
        /* A cell_shadow param is bound by the loop emitters WRITING THE PLAIN C
           SLOT, while every read in the body already goes through the cell. The
           publish used to sit at the capture fill -- the point the lifted proc
           is built -- so every read before that read a cell still holding the
           previous iteration's value, or nil on the first pass. Two nested
           blocks and a `next` were enough to arrange it: doom's build_composite
           saw `pref` as nil on the first patch of every texture and drew
           nothing. The slot is current from the moment the loop writes it, so
           publish here, at the top of the body, which is where the LocalVar
           doc has always said the copy belongs. */
        Scope *psc = comp_scope_of(c, blk);
        LocalVar *plv = psc ? scope_local(psc, tmpn) : NULL;
        if (plv && plv->is_cell && plv->cell_shadow) {
          const char *prn = rename_local(tmpn);
          /* Inlined inside a real proc function the cell is reachable only
             through the capture struct, and the slot is not this frame's at
             all -- there the capture fill is still the only publish. */
          if (!(g_cap_struct && g_cap_names && nameset_has(g_cap_names, prn))) {
            /* A fresh cell first, as a captured block-local gets below: the
               parameter is bound anew by every iteration too, and one shared
               cell had every proc a `&blk` callee stored answer the last
               value the parameter took (#4462). */
            char pcell_buf[160];
            emit_fresh_cell(c, plv, cell_var_name(prn, pcell_buf, sizeof pcell_buf), b, indent);
            emit_indent(b, indent);
            if (plv->type == TY_PROC)
              buf_printf(b, "*_cell_%s = (sp_int)(uintptr_t)lv_%s;\n", prn, prn);
            else
              buf_printf(b, "*_cell_%s = lv_%s;\n", prn, prn);
          }
        }
      }
      else {
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, tmpn) : NULL;
        /* A captured (cell-backed) block-local gets a FRESH cell each
           invocation: the capture struct copies the current cell pointer at
           proc creation, so per-iteration closures each keep their own
           binding -- one shared cell made every closure observe the final
           iteration's value (#3230). The prologue's SP_GC_ROOT registered
           the cell VARIABLE's address, so the reassignment stays rooted;
           earlier cells stay live through their captures' scans. */
        if (lv && lv->is_cell) {
          const char *rn2 = rename_local(tmpn);
          /* The cell VARIABLE is declared in the frame that owns the scope. A
             block body is not always emitted into that frame: when an
             enclosing block became a real proc function, this one is inlined
             INSIDE it, and there the cell is reachable only through the
             capture struct -- `_cell_terms` named nothing the function
             declared and the C build stopped (#4127). Refreshing the capture
             slot is also the right per-iteration semantics: a proc created in
             this iteration copies the slot at creation, so it keeps this
             iteration's binding. */
          char cellv_buf[160];
          const char *cellv = cell_var_name(rn2, cellv_buf, sizeof cellv_buf);
          emit_fresh_cell(c, lv, cellv, b, indent);
        }
        else if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) {
          emit_indent(b, indent);
          /* A value-type object is stored inline (sp_X, not sp_X*), so its
             empty/nil reset is a zeroed struct -- default_value_from_compiler(c, )'s blanket
             "NULL" would assign a pointer to a struct lvalue (#3267). */
          if (ty_is_object(lv->type) && c->classes[ty_object_class(lv->type)].is_value_type) {
            buf_printf(b, "lv_%s = (sp_%s){0};\n", rename_local(tmpn),
                       c->classes[ty_object_class(lv->type)].c_name);
          }
          else {
            /* a block-local starts every iteration nil: an oint slot's nil,
               a plain scalar's zero */
            const char *nv = slot_is_oint(lv) ? oint_nil(lv->type) : nil_value(lv->type);
            if (!nv) nv = lv->type == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, lv->type);
            buf_printf(b, "lv_%s = %s;\n", rename_local(tmpn), nv);
          }
        }
      }
      if (tmpn != tmpn_buf) free(tmpn);
    }
    if (!e) break;
    p = e + 1;
  }
}

/* The receiver's local name when it is a shared-mutable (TY_STRBUF) string
   local read, else NULL (#3227 shim gate). */
const char *strbuf_local_name(Compiler *c, int recv) {
  if (recv < 0) return NULL;
  const char *rty = nt_type(c->nt, recv);
  if (!rty || !sp_streq(rty, "LocalVariableReadNode")) return NULL;
  const char *rn = nt_str(c->nt, recv, "name");
  Scope *rs = rn ? comp_scope_of(c, recv) : NULL;
  LocalVar *rl = rs ? scope_local(rs, rn) : NULL;
  return repr_of_slot(c, rl).kind == RK_STRBUF ? rn : NULL;
}
/* The owning class slot of an ivar READ node, mirroring the read emitter's
   storage resolution: instance method -> its class; top-level method ->
   the Toplevel pseudo-class; a class method -> its class, whose civ_ slot
   holds it (strbuf_slot_ref); an instance_eval context returns -1 (its
   storage is not a per-instance field). */
int strbuf_ivar_owner(Compiler *c, int node) {
  Scope *cs = comp_scope_of(c, node);
  if (!cs) return -1;
  if (cs->is_cmethod) return cs->class_id;
  if (cs->class_id >= 0) return cs->class_id;
  if (g_ie_class_id >= 0) return -1;
  return comp_class_index(c, "Toplevel");
}
/* ---- Is an object's ivar set? ----
   An object lays out every ivar its class can hold, and a slot nothing has
   written yet reads nil -- which instance_variables, instance_variable_
   defined?, defined?(@x) and the default inspect answered as set. CRuby
   lists an ivar only once it is assigned. ivar_set_kind answers, for ivar
   `ivn` of an object of class `cid`:
     0 -- always set: the initialize the class runs writes it at its top
          level, unconditionally (a `super` there runs the parent's);
     1 -- set exactly when it is not nil: every write the program makes to
          it stores a value that is never nil, so a nil slot is unset;
     2 -- neither can be told; reflection lists it as before;
     3 -- the slot has an explicit presence flag: a reflection-only slot,
          or one a read of presence reaches (ivs_tracked). */
static int ivs_writes_toplevel(Compiler *c, int body, const char *ivn, int cid, int depth, int strict);
static int ivs_may_observe(Compiler *c, int n, int cid, int depth, int lvl);
/* Strict, the write must also come before anything that may see the
   object (ivs_may_observe): an observation inside initialize ahead of it
   finds the ivar unset. */
static int ivs_method_sets(Compiler *c, int mi, const char *ivn, int depth, int strict) {
  if (mi < 0 || depth > 16) return 0;
  Scope *m = &c->scopes[mi];
  if (m->def_node < 0 || nt_kind(c->nt, m->def_node) != NK_DefNode) return 0;
  /* a default of an optional parameter, or of an optional keyword, runs
     before the body: one that may see the object finds the ivar unset */
  int ps = strict ? nt_ref(c->nt, m->def_node, "parameters") : -1;
  for (int a = 0; a < 2 && ps >= 0; a++) {
    int dn = 0; const int *ds = nt_arr(c->nt, ps, a ? "keywords" : "optionals", &dn);
    for (int i = 0; i < dn; i++)
      if (ivs_may_observe(c, nt_ref(c->nt, ds[i], "value"), m->class_id, 0, 0)) return 0;
  }
  return ivs_writes_toplevel(c, nt_ref(c->nt, m->def_node, "body"), ivn, m->class_id, depth, strict);
}
static int ivs_init_sets(Compiler *c, int cid, const char *ivn, int depth, int strict) {
  if (cid < 0) return 0;
  return ivs_method_sets(c, comp_method_in_chain(c, cid, "initialize", NULL), ivn, depth, strict);
}
/* The method the `super` at node runs, from a method of class cid, as the
   emitters resolve it: the shadowed copy in the method's own class (an
   included module's, a prepend's), else the parent's chain under name.
   -1 when there is none, -2 when it cannot be told. */
static int ivs_super_method(Compiler *c, int node, int cid, const char *name) {
  Scope *s = comp_scope_of(c, node);
  if (!s || s->is_cmethod) return -2;
  const char *shadow = comp_super_shadow(c, s);
  if (shadow) { int mi = comp_method_in_class(c, s->class_id, shadow); return mi < 0 ? -2 : mi; }
  int pc = comp_super_parent(c, cid, 0);
  return pc < 0 ? -1 : comp_method_in_chain(c, pc, name, NULL);
}
/* May code under `n`, run in a method of class cid, see the object's ivars:
   self handed on (kept in an ivar of its own, it is not), a reflective read
   or a print of a variable, in it or in a method of the program's it calls
   on self (the override of a class below cid too) or the parent's
   initialize its `super` runs? lvl counts the calls followed; past the
   levels it follows, or on a call back into a method being walked, it
   answers that it may. */
static int ivs_never_nil(Compiler *c, int v);
static int ivs_may_observe(Compiler *c, int n, int cid, int depth, int lvl);
/* ... in the body of method scope mi. The answer depends on the method
   alone, not on the ivar asked about, so it is kept per scope
   (Compiler.pres_obs). */
enum { IVO_BUSY = 1, IVO_NO, IVO_MAY };
static int ivs_body_may_observe(Compiler *c, int mi, int lvl) {
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (m->def_node < 0 || nt_kind(c->nt, m->def_node) != NK_DefNode) return 1;
  if (c->pres_obs_n != c->nscopes) {
    free(c->pres_obs);
    c->pres_obs = (unsigned char *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
    if (!c->pres_obs) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->pres_obs_n = c->nscopes;
  }
  if (c->pres_obs[mi]) return c->pres_obs[mi] != IVO_NO;
  if (lvl >= 32) return 1;
  c->pres_obs[mi] = IVO_BUSY;
  int r = ivs_may_observe(c, nt_ref(c->nt, m->def_node, "body"), m->class_id, 0, lvl + 1);
  c->pres_obs[mi] = r ? IVO_MAY : IVO_NO;
  return r;
}
/* ... a call named nm on self in a method of class cid: the method cid
   resolves, or an override in a class below it, which a parent's
   initialize calling a hook runs for a subclass's instance */
static int ivs_self_call_may_observe(Compiler *c, int cid, const char *nm, int lvl) {
  int mi = comp_method_in_chain(c, cid, nm, NULL);
  if (ivs_body_may_observe(c, mi, lvl)) return 1;
  for (int d = 0; d < c->nclasses; d++) {
    if (d == cid || !is_descendant(c, d, cid)) continue;
    int md = comp_method_in_chain(c, d, nm, NULL);
    if (md >= 0 && md != mi && ivs_body_may_observe(c, md, lvl)) return 1;
  }
  return 0;
}
static int ivs_may_observe(Compiler *c, int n, int cid, int depth, int lvl) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  if (depth > 200) return 1;
  switch (nt_kind(nt, n)) {
    case NK_SelfNode: return 1;
    case NK_SuperNode: case NK_ForwardingSuperNode: {
      Scope *s = comp_scope_of(c, n);
      if (!s || !s->name) return 1;
      int sm = ivs_super_method(c, n, cid, s->name);
      if (sm == -2 || ivs_body_may_observe(c, sm, lvl)) return 1;
      break;
    }
    case NK_DefinedNode:
      if (nt_kind(nt, nt_ref(nt, n, "value")) == NK_InstanceVariableReadNode) return 1;
      break;
    case NK_InstanceVariableWriteNode:
      if (nt_kind(nt, nt_ref(nt, n, "value")) == NK_SelfNode) return 0;
      break;
    case NK_CallNode: {
      const char *nm = nt_str(nt, n, "name");
      int r = nt_ref(nt, n, "receiver");
      if (!nm) return 1;
      if ((r < 0 || nt_kind(nt, r) == NK_SelfNode) && ivs_self_call_may_observe(c, cid, nm, lvl)) return 1;
      if (is_ivar_presence_read(nm) && !is_inspect_print(nm)) return 1;
      if (is_inspect_print(nm) && r < 0) {
        int a = nt_ref(nt, n, "arguments"), ac = 0;
        const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
        for (int i = 0; i < ac; i++) if (!ivs_never_nil(c, av[i]) || nt_kind(nt, av[i]) == NK_ArrayNode ||
                                         nt_kind(nt, av[i]) == NK_HashNode) return 1;
      }
      break;
    }
    default: break;
  }
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) if (ivs_may_observe(c, nt_ref_at(nt, n, i), cid, depth + 1, lvl)) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (ivs_may_observe(c, ids[j], cid, depth + 1, lvl)) return 1;
  }
  return 0;
}
static int ivs_writes_toplevel(Compiler *c, int body, const char *ivn, int cid, int depth, int strict) {
  const NodeTable *nt = c->nt;
  if (body < 0) return 0;
  NodeKind k = nt_kind(nt, body);
  if (k == NK_StatementsNode) {
    int n = 0; const int *st = nt_arr(nt, body, "body", &n);
    for (int i = 0; i < n; i++) {
      if (ivs_writes_toplevel(c, st[i], ivn, cid, depth, strict)) return 1;
      if (strict && ivs_may_observe(c, st[i], cid, 0, 0)) return 0;
    }
    return 0;
  }
  if (k == NK_ParenthesesNode || k == NK_BeginNode) {
    /* a begin with a rescue may stop before the write */
    if (k == NK_BeginNode && nt_ref(nt, body, "rescue_clause") >= 0) return 0;
    return ivs_writes_toplevel(c, nt_ref(nt, body, k == NK_BeginNode ? "statements" : "body"), ivn, cid, depth, strict);
  }
  if (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode)
    return sp_streq(nt_str(nt, body, "name"), ivn) && !(strict && ivs_may_observe(c, body, cid, 0, 0));
  if (k == NK_MultiWriteNode) {
    int ln = 0; const int *l = nt_arr(nt, body, "lefts", &ln);
    if (strict && ivs_may_observe(c, nt_ref(nt, body, "value"), cid, 0, 0)) return 0;
    for (int i = 0; i < ln; i++)
      if (nt_kind(nt, l[i]) == NK_InstanceVariableTargetNode && sp_streq(nt_str(nt, l[i], "name"), ivn)) return 1;
    return 0;
  }
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    if (strict && ivs_may_observe(c, nt_ref(nt, body, "arguments"), cid, 0, 0)) return 0;
    return ivs_method_sets(c, ivs_super_method(c, body, cid, "initialize"), ivn, depth + 1, strict);
  }
  return 0;
}
/* The order initialize first assigns the ivars in, for the listings: the
   top-level writes of the initialize class cid runs, a `super` there
   standing for the parent's own, as ivs_writes_toplevel reads them. Each
   ivar of class leaf is entered once, by its index there. It is exact only
   while nothing else could assign one first, so the walk gives up (answers
   0) at a statement before the last first write that is no plain ivar
   write, multiple assignment or super, and at a value, a super's argument
   or a target that could run code of the object's own or write an ivar. */
static int ivs_order_walk(Compiler *c, int body, int cid, int leaf, int *ord, int *n, int depth);
static int ivs_order_runs_self(Compiler *c, int v, int depth);
static int ivs_order_method(Compiler *c, int mi, int leaf, int *ord, int *n, int depth) {
  if (mi == -1) return 1;
  if (mi < 0 || depth > 16) return 0;
  Scope *m = &c->scopes[mi];
  if (m->def_node < 0 || nt_kind(c->nt, m->def_node) != NK_DefNode) return 0;
  /* the defaults of optional parameters and keywords run before the body */
  int ps = nt_ref(c->nt, m->def_node, "parameters");
  for (int a = 0; a < 2 && ps >= 0; a++) {
    int dn = 0; const int *ds = nt_arr(c->nt, ps, a ? "keywords" : "optionals", &dn);
    for (int i = 0; i < dn; i++)
      if (ivs_order_runs_self(c, nt_ref(c->nt, ds[i], "value"), 0)) return 0;
  }
  return ivs_order_walk(c, nt_ref(c->nt, m->def_node, "body"), m->class_id, leaf, ord, n, depth);
}
static int ivs_order_init(Compiler *c, int cid, int leaf, int *ord, int *n, int depth) {
  if (cid < 0) return 1;
  return ivs_order_method(c, comp_method_in_chain(c, cid, "initialize", NULL), leaf, ord, n, depth);
}
static void ivs_order_add(Compiler *c, int leaf, const char *ivn, int *ord, int *n) {
  int ix = comp_ivar_index(&c->classes[leaf], ivn);
  for (int i = 0; ix >= 0 && i < *n; i++) if (ord[i] == ix) return;
  if (ix >= 0) ord[(*n)++] = ix;
}
/* Can the code under v run the object's own code or assign an ivar: self
   or a call on it (or none), a block, a yield, a super, an ivar write? */
static int ivs_order_runs_self(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (depth > 200) return 1;
  switch (nt_kind(nt, v)) {
    case NK_SelfNode: case NK_BlockNode: case NK_LambdaNode: case NK_YieldNode:
    case NK_SuperNode: case NK_ForwardingSuperNode:
    case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode:
    case NK_InstanceVariableOperatorWriteNode: case NK_InstanceVariableTargetNode:
      return 1;
    case NK_CallNode: if (nt_ref(nt, v, "receiver") < 0) return 1; break;
    default: break;
  }
  int nr = nt_num_refs(nt, v);
  for (int i = 0; i < nr; i++) if (ivs_order_runs_self(c, nt_ref_at(nt, v, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, v);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, v, i, &m);
    for (int j = 0; j < m; j++) if (ivs_order_runs_self(c, ids[j], depth + 1)) return 1;
  }
  return 0;
}
static int ivs_order_walk(Compiler *c, int body, int cid, int leaf, int *ord, int *n, int depth) {
  const NodeTable *nt = c->nt;
  if (body < 0) return 1;
  NodeKind k = nt_kind(nt, body);
  if (k == NK_StatementsNode) {
    int cnt = 0; const int *st = nt_arr(nt, body, "body", &cnt);
    /* once every ivar has been entered, the rest cannot change the order */
    for (int i = 0; i < cnt && *n < c->classes[leaf].nivars; i++)
      if (!ivs_order_walk(c, st[i], cid, leaf, ord, n, depth)) return 0;
    return 1;
  }
  if (k == NK_ParenthesesNode ||
      (k == NK_BeginNode && nt_ref(nt, body, "rescue_clause") < 0 && nt_ref(nt, body, "ensure_clause") < 0))
    return ivs_order_walk(c, nt_ref(nt, body, k == NK_BeginNode ? "statements" : "body"), cid, leaf, ord, n, depth);
  if (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode) {
    if (ivs_order_runs_self(c, nt_ref(nt, body, "value"), 0)) return 0;
    ivs_order_add(c, leaf, nt_str(nt, body, "name"), ord, n);
    return 1;
  }
  if (k == NK_MultiWriteNode) {
    int ln = 0; const int *l = nt_arr(nt, body, "lefts", &ln);
    if (nt_ref(nt, body, "rest") >= 0 || ivs_order_runs_self(c, nt_ref(nt, body, "value"), 0)) return 0;
    for (int i = 0; i < ln; i++) {
      if (nt_kind(nt, l[i]) != NK_InstanceVariableTargetNode && nt_kind(nt, l[i]) != NK_LocalVariableTargetNode) return 0;
    }
    int rn = 0; nt_arr(nt, body, "rights", &rn);
    if (rn > 0) return 0;
    for (int i = 0; i < ln; i++)
      if (nt_kind(nt, l[i]) == NK_InstanceVariableTargetNode) ivs_order_add(c, leaf, nt_str(nt, l[i], "name"), ord, n);
    return 1;
  }
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode)
    return nt_ref(nt, body, "block") < 0 && !ivs_order_runs_self(c, nt_ref(nt, body, "arguments"), 0) &&
           ivs_order_method(c, ivs_super_method(c, body, cid, "initialize"), leaf, ord, n, depth + 1);
  return 0;
}
/* A value that is never nil: a literal, a constructor of a class with no
   `new` of its own, an interpolation, an operator on a builtin number or
   String (which answers one or raises). */
static int ivs_never_nil(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  /* nil out of band: an Integer or Float value the analysis does not mark
     nullable is never nil (a plain slot cannot hold one) */
  if (v >= 0 && nt_kind(nt, v) != NK_NilNode && oint_kind(comp_ntype(c, v)) && !nullable_int_value(c, v) &&
      !node_has_oint_form(c, v))
    return 1;
  /* under --int-overflow=promote an Integer slot is widened to the box
     after the analysis (comp_ntype answers poly, infer_type the Integer it
     settled on): a value the analysis proved non-nil is still never nil */
  if (v >= 0 && nt_kind(nt, v) != NK_NilNode && comp_ntype(c, v) == TY_POLY) {
    an_pure_read_begin();
    TyKind it = infer_type(c, v);
    int nn = oint_kind(it) && !nullable_int_value(c, v);
    an_pure_read_end();
    if (nn) return 1;
  }
  switch (nt_kind(nt, v)) {
    case NK_StringNode: case NK_InterpolatedStringNode: case NK_XStringNode: case NK_IntegerNode:
    case NK_FloatNode: case NK_RationalNode: case NK_ImaginaryNode: case NK_SymbolNode:
    case NK_InterpolatedSymbolNode: case NK_ArrayNode: case NK_HashNode: case NK_RangeNode:
    case NK_RegularExpressionNode: case NK_InterpolatedRegularExpressionNode: case NK_TrueNode:
    case NK_FalseNode: case NK_LambdaNode:
      return 1;
    case NK_ParenthesesNode: {
      int b = nt_ref(nt, v, "body"), n = 0;
      const int *st = b >= 0 && nt_kind(nt, b) == NK_StatementsNode ? nt_arr(nt, b, "body", &n) : NULL;
      return n > 0 && ivs_never_nil(c, st[n - 1]);
    }
    case NK_CallNode: {
      const char *nm = nt_str(nt, v, "name");
      int r = nt_ref(nt, v, "receiver");
      if (!nm || r < 0) return 0;
      if (sp_streq(nm, "new") && nt_kind(nt, r) == NK_ConstantReadNode) {
        int ci = comp_class_index(c, nt_str(nt, r, "name"));
        return ci < 0 ? 1 : comp_cmethod_in_chain(c, ci, "new", NULL) < 0;
      }
      if (is_unary_sign(nm)) return nt_kind(nt, r) == NK_StringNode;
      TyKind rt = comp_ntype(c, r);
      static const char *const OPS[] = { "+", "-", "*", "/", "%", "**", "<<", "to_s", "to_i", "to_f",
                                         "to_a", "to_sym", "dup", NULL };
      if (rt != TY_INT && rt != TY_FLOAT && rt != TY_STRING) return 0;
      for (int i = 0; OPS[i]; i++) if (sp_streq(nm, OPS[i])) return 1;
      return 0;
    }
    default:
      return 0;
  }
}
/* May a value stored where a String handle is wanted be nil? Not one the list
   above proves never is. nil.dup is nil, so a dup is proven only with its
   receiver. */
int stored_value_may_be_nil(Compiler *c, int v) {
  v = unwrap_parens(c, v);
  if (!node_may_be_null_nil(c, v)) return 0;
  if (nt_kind(c->nt, v) == NK_ParenthesesNode || !ivs_never_nil(c, v)) return 1;
  if (nt_kind(c->nt, v) == NK_CallNode && sp_streq(nt_str(c->nt, v, "name"), "dup"))
    return stored_value_may_be_nil(c, nt_ref(c->nt, v, "receiver"));
  return 0;
}
/* Does the subtree under `n` write ivar `ivn`? Past the depth it follows,
   it answers that it may. */
static int ivs_subtree_writes(const NodeTable *nt, int n, const char *ivn, int depth) {
  if (n < 0) return 0;
  if (depth > 200) return 1;
  const char *t = nt_type(nt, n);
  if (t && strncmp(t, "InstanceVariable", 16) == 0 && !strstr(t, "Read") && sp_streq(nt_str(nt, n, "name"), ivn))
    return 1;
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) if (ivs_subtree_writes(nt, nt_ref_at(nt, n, i), ivn, depth + 1)) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (ivs_subtree_writes(nt, ids[j], ivn, depth + 1)) return 1;
  }
  return 0;
}
/* Is class k one whose objects' slot `cid` describes, or whose methods can
   write it: cid itself, an ancestor or a descendant? */
static int ivs_related(Compiler *c, int k, int cid) {
  return k == cid || is_descendant(c, k, cid) || is_descendant(c, cid, k);
}
/* A slot introduced only by reflection has no ordinary assignment emitter.
   Its setter can keep presence separately even when the assigned value is nil.
   Use the name across classes so inherited layouts agree on the extra field.
   A lowered Object setter can add a slot beside unrelated ordinary writers;
   those classes do not share its layout or assignment emitters. */
static int ivs_reflect_only(Compiler *c, int cid, const char *ivn) {
  const NodeTable *nt = c->nt;
  int found = 0, scoped = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, u) {
    if (!is_ivar_set_name(nt_str(nt, u, "name"))) continue;
    int a = nt_ref(nt, u, "arguments"), ac = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    if (ac != 2) continue;
    const char *sn = nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value") :
                     nt_kind(nt, av[0]) == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
    if (sn && sp_streq(sn, ivn)) {
      found = 1;
      Scope *s = comp_scope_of(c, u);
      int r = nt_ref(nt, u, "receiver");
      if (s && !s->is_cmethod && s->class_id >= 0 && is_object_root(c->classes[s->class_id].name) &&
          (r < 0 || nt_kind(nt, r) == NK_SelfNode)) {
        int n = 0; const int *ks = poly_recv_classes(c, u, &n);
        if (!ks) scoped = 1;
        for (int k = 0; ks && k < n && !scoped; k++) scoped = ivs_related(c, ks[k], cid);
      }
    }
  }
  if (!found) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (scoped && !ivs_related(c, k, cid)) continue;
    ClassInfo *ci = &c->classes[k];
    for (int j = 0; j < ci->nwriters; j++)
      if (sp_streq(ci->writers[j], ivn + 1)) return 0;
    if (ci->is_struct) {
      int iv = comp_ivar_index(ci, ivn);
      if (iv >= 0 && iv < ci->nmembers) return 0;
    }
  }
  for (int n = 0; n < nt->count; n++) {
    const char *t = nt_type(nt, n);
    if (t && strncmp(t, "InstanceVariable", 16) == 0 && !strstr(t, "Read") &&
        sp_streq(nt_str(nt, n, "name"), ivn)) {
      int owner = comp_ivar_owner(c, n);
      if (!scoped || owner < 0 || ivs_related(c, owner, cid)) return 0;
    }
  }
  return found;
}
/* The kinds above before ivs_tracked: 0, 1, 2, or 3 for a reflection-only
   slot. */
static int ivs_base_kind(Compiler *c, int cid, const char *ivn) {
  const NodeTable *nt = c->nt;
  ClassInfo *ci = &c->classes[cid];
  int iv = comp_ivar_index(ci, ivn);
  int kind;
  TyKind t = ci->ivar_types[iv];
  if (ivs_init_sets(c, cid, ivn, 0, 0)) kind = 0;
  /* a type with no nil of its own cannot tell an unset slot */
  else if (!(t == TY_INT || t == TY_FLOAT || t == TY_POLY || t == TY_STRING || t == TY_STRBUF ||
             ty_is_array(t) || ty_is_hash(t) || ty_is_object(t))) kind = 2;
  else {
    kind = 1;
    static const NodeKind WK[] = { NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
                                   NK_InstanceVariableAndWriteNode, NK_InstanceVariableOperatorWriteNode,
                                   NK_InstanceVariableTargetNode };
    for (int q = 0; q < 5 && kind == 1; q++)
      NT_FOREACH_KIND(nt, WK[q], w) {
        if (!sp_streq(nt_str(nt, w, "name"), ivn)) continue;
        Scope *ws = comp_scope_of(c, w);
        if (!ws || ws->is_cmethod) continue;
        /* a write whose owner this cannot name (a top-level method, an
           instance_eval body) may be this object's */
        if (ws->class_id < 0 || !ivs_related(c, ws->class_id, cid)) {
          if (ws->class_id < 0) { kind = 2; break; }
          continue;
        }
        if (WK[q] == NK_InstanceVariableTargetNode) { kind = 2; break; }
        if (WK[q] == NK_InstanceVariableOperatorWriteNode) continue;   /* answers a value or raises */
        if (!ivs_never_nil(c, nt_ref(nt, w, "value"))) { kind = 2; break; }
      }
    /* a write from outside: instance_variable_set, a writer, instance_eval */
    char wr[128]; snprintf(wr, sizeof wr, "%s=", ivn + 1);
    NT_FOREACH_KIND(nt, NK_CallNode, u) {
      if (kind != 1) break;
      const char *un = nt_str(nt, u, "name");
      if (!un) continue;
      int a = nt_ref(nt, u, "arguments"), ac = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
      if (sp_streq(un, "instance_variable_set")) {
        if (nt_int(nt, u, "builtin_only", 0) && !poly_ivar_set_reaches(c, u, cid)) continue;
        int nmok = ac == 2 && (nt_kind(nt, av[0]) == NK_SymbolNode || nt_kind(nt, av[0]) == NK_StringNode);
        const char *sn = !nmok ? NULL : nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value")
                                                                            : nt_str(nt, av[0], "content");
        if (!nmok || (sn && sp_streq(sn, ivn) && !ivs_never_nil(c, av[1]))) kind = 2;
      }
      else if (sp_streq(un, wr) && (ac != 1 || !ivs_never_nil(c, av[0]))) kind = 2;
      else if ((sp_streq(un, "instance_eval") || sp_streq(un, "instance_exec") || sp_streq(un, "class_eval") ||
                sp_streq(un, "class_exec")) && nt_ref(nt, u, "block") >= 0) {
        /* its block writes the receiver's ivars */
        if (ivs_subtree_writes(nt, nt_ref(nt, u, "block"), ivn, 0)) kind = 2;
      }
    }
    /* `o.x ||= v` and its kin write through the writer too */
    for (int u = 0; kind == 1 && u < nt->count; u++) {
      const char *ut = nt_type(nt, u);
      if (!ut || (!sp_streq(ut, "CallOrWriteNode") && !sp_streq(ut, "CallAndWriteNode") &&
                  !sp_streq(ut, "CallOperatorWriteNode"))) continue;
      const char *rn = nt_str(nt, u, "name"), *wn = nt_str(nt, u, "write_name");
      if ((rn && (sp_streq(rn, ivn + 1) || sp_streq(rn, wr))) || (wn && sp_streq(wn, wr))) kind = 2;
    }
  }
  if (kind == 2 && ivs_reflect_only(c, cid, ivn)) kind = 3;
  return kind;
}
/* ---- An assigned flag where presence is read ----
   A class a read of presence reaches (presence_observed) keeps a flag
   beside each ivar the kinds above cannot answer for: one a write may set
   to nil or that has no nil of its own (kind 2), or one its initialize
   sets only after something that may see the object, or that
   remove_instance_variable can unset (kind 0). A store sets the flag after
   its value is computed and a removal clears it. The flag is a field of the
   layout, which the classes above and below share and a module's methods
   write through each including class's, so one answer covers them all:
   tracked when one of them is observed and one needs the flag, unless a
   write no emitter can mark reaches the ivar. */
/* Can a write reach ivar ivn of class k that sets no flag: one the
   analysis noted (Compiler.pres_names), or one a class whose ivars the
   compiler writes itself makes (a Struct's members, which are no ivars,
   among them)? */
static int ivs_unmarked_write(Compiler *c, int k, const char *ivn) {
  ClassInfo *ki = &c->classes[k];
  int ix = comp_ivar_index(ki, ivn);
  return c->pres_any || (comp_pres_flags(c, ivn) & PRES_UNMARKED) || ki->ncs > 0 || ki->is_native_class ||
         ki->is_data || (ki->is_struct && ix >= 0 && ix < ki->nmembers);
}
/* Does ivar ivn of class cid keep an assigned flag? One answer for its
   family (presence_family): memo (the final kinds) and bmemo (the base
   kinds) take every member's. The flag is needed for an answer of kind 2,
   or of kind 0 that an observation in initialize or a removal can
   contradict. */
static int ivs_tracked(Compiler *c, int cid, const char *ivn, int *memo, int *bmemo) {
  int fam = c->classes[cid].presence_family;
  int seen = 0, needs = 0, set0 = 0, unmarked = 0;
  /* the base kinds of the members, and whether one is observed; only then
     do the costlier checks of initialize's order and of the writes run */
  for (int k = 0; k < c->nclasses && fam > 0; k++) {
    int iv = c->classes[k].presence_family == fam ? comp_ivar_index(&c->classes[k], ivn) : -1;
    if (iv < 0) continue;
    int slot = iv < 64 ? k * 64 + iv : -1;
    int base = slot >= 0 && bmemo[slot] >= 0 ? bmemo[slot] : ivs_base_kind(c, k, ivn);
    if (slot >= 0) bmemo[slot] = base;
    seen |= c->classes[k].presence_observed != 0;
    needs |= base == 2 || base == 3;
    set0 |= base == 0;
  }
  for (int k = 0; k < c->nclasses && seen; k++) {
    int iv = c->classes[k].presence_family == fam ? comp_ivar_index(&c->classes[k], ivn) : -1;
    if (iv < 0) continue;
    int base = iv < 64 ? bmemo[k * 64 + iv] : ivs_base_kind(c, k, ivn);
    if (base == 0 && !needs && !ivs_init_sets(c, k, ivn, 0, 1)) needs = 1;
    unmarked |= ivs_unmarked_write(c, k, ivn);
  }
  if (seen && !needs && set0) needs = (comp_pres_flags(c, ivn) & PRES_REMOVED) != 0;
  int tracked = seen && needs && !unmarked;
  for (int k = 0; k < c->nclasses && fam > 0; k++) {
    int iv = c->classes[k].presence_family == fam ? comp_ivar_index(&c->classes[k], ivn) : -1;
    if (iv >= 0 && iv < 64 && bmemo[k * 64 + iv] >= 0) memo[k * 64 + iv] = tracked ? 3 : bmemo[k * 64 + iv];
  }
  return tracked;
}
static int ivs_kind(Compiler *c, int cid, const char *ivn) {
  if (cid < 0 || cid >= c->nclasses || !ivn) return 2;
  ClassInfo *ci = &c->classes[cid];
  int iv = comp_ivar_index(ci, ivn);
  if (iv < 0) return 2;
  int n = c->nclasses * 64;
  if (c->pres_memo_n != n * 2) {
    free(c->pres_memo); c->pres_memo_n = n * 2;
    c->pres_memo = (int *)malloc(sizeof(int) * (size_t)(c->pres_memo_n > 0 ? c->pres_memo_n : 1));
    if (!c->pres_memo) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < c->pres_memo_n; i++) c->pres_memo[i] = -1;
  }
  int *memo = c->pres_memo;
  int slot = iv < 64 ? cid * 64 + iv : -1;
  if (slot >= 0 && memo[slot] >= 0) return memo[slot];
  int kind = slot >= 0 && memo[n + slot] >= 0 ? memo[n + slot] : ivs_base_kind(c, cid, ivn);
  if (slot >= 0) memo[n + slot] = kind;
  if (c->pres_seen && ivs_tracked(c, cid, ivn, memo, memo + n)) kind = 3;
  if (slot >= 0) memo[slot] = kind;
  return kind;
}
/* ---- The order ivars were first assigned in ----
   CRuby lists an object's ivars in the order they were first assigned,
   which differs between objects of a class that assigns some of them on
   some paths only. A family a read of presence reaches (presence_observed)
   whose ivars are not all assigned by initialize unconditionally therefore
   keeps, for every one of its ivars, the rank of its first assignment in
   place of the flag (a store sets it through ivar_set_mark, a removal
   clears it), and the reflective listings sort by it. Every ivar of such a
   family has the rank, so they order all of them; a family one of whose
   ivars a write no emitter marks reaches, or that holds an exception class
   (its layout is the runtime's), a value type or a Struct, lists in layout
   order as before. Answers one family at a time (Compiler.pres_rank). */
int ivar_ranked(Compiler *c, int cid) {
  /* a class with no ivars has no rank to keep, nor a counter */
  if (!c->pres_seen || cid < 0 || cid >= c->nclasses || c->classes[cid].presence_family <= 0 ||
      c->classes[cid].nivars == 0) return 0;
  int fam = c->classes[cid].presence_family;
  if (c->pres_rank_n != c->nclasses) {
    free(c->pres_rank);
    c->pres_rank = (unsigned char *)calloc((size_t)(c->nclasses + 1), 1);
    if (!c->pres_rank) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->pres_rank_n = c->nclasses;
  }
  unsigned char *m = &c->pres_rank[fam - 1];
  if (*m) return *m == 2;
  *m = 1;
  int seen = 0, varies = 0, ok = 1;
  for (int k = 0; k < c->nclasses && ok; k++) {
    ClassInfo *ki = &c->classes[k];
    if (ki->presence_family != fam) continue;
    seen |= ki->presence_observed != 0;
    ok = !ki->is_value_type && !class_is_exc_subclass(c, k);
    int late = 0;
    for (int j = 0; j < ki->nivars && ok; j++) {
      ok = !ivs_unmarked_write(c, k, ki->ivars[j]);
      late |= ivs_kind(c, k, ki->ivars[j]) != 0;
    }
    /* an object whose ivars all initialize assigns, unconditionally and
       first, has its class's order; one with a single ivar has no order */
    varies |= late && ki->nivars > 1;
  }
  *m = seen && varies && ok ? 2 : 1;
  return *m == 2;
}
int ivar_set_kind(Compiler *c, int cid, const char *ivn) {
  int kind = ivs_kind(c, cid, ivn);
  if (kind == 3 || cid < 0 || cid >= c->nclasses || comp_ivar_index(&c->classes[cid], ivn) < 0) return kind;
  return ivar_ranked(c, cid) ? 3 : kind;
}
/* A listing of a ranked class's ivars (ivar_ranked) walks those assigned in
   the order they were first: `case <index>:` per ivar between these, with
   _ixk the position in the walk (0 for the first). obj is the object. */
void emit_ivar_order_open(Compiler *c, int cid, const char *obj, Buf *b) {
  ClassInfo *ci = &c->classes[cid];
  buf_printf(b, "{ int _ixv[%d]; int _ixn = sp_%s_ivar_order(%s, _ixv); "
                "for (int _ixk = 0; _ixk < _ixn; _ixk++) switch (_ixv[_ixk]) { ",
             ci->nivars > 0 ? ci->nivars : 1, ci->c_name, obj);
}
void emit_ivar_order_close(Buf *b) {
  buf_puts(b, "} }");
}
/* The order the listings of class cid walk its ivars in, ord[0..nivars):
   the order initialize assigns them in where it assigns every one
   unconditionally (kind 0 all), else the layout's. A family that keeps the
   rank of each first assignment (ivar_ranked) sorts at run time instead. */
/* ... cached per class (Compiler.pres_ord), the walk being initialize's size.
   Objects made without initialize (allocate, Marshal.load) have no such
   order: the program that makes any lists in the layout's. */
/* ... the order class cid lists its ivars in, from initialize's, when it has one */
static int ivs_static_order(Compiler *c, int cid, int *ord) {
  ClassInfo *ci = &c->classes[cid];
  int n = 0, ok = ci->nivars > 1 && !ci->is_struct && !ci->is_data && !c->pres_noinit && !ivar_ranked(c, cid);
  for (int j = 0; j < ci->nivars && ok; j++) ok = ivar_set_kind(c, cid, ci->ivars[j]) == 0;
  return ok && ivs_order_init(c, cid, cid, ord, &n, 0) && n == ci->nivars;
}
int *ivar_listing_order_new(Compiler *c, int cid) {
  ClassInfo *ci = &c->classes[cid];
  size_t sz = sizeof(int) * (size_t)(ci->nivars > 0 ? ci->nivars : 1);
  if (c->pres_ord_n != c->nclasses) {
    for (int i = 0; i < c->pres_ord_n; i++) free(c->pres_ord[i]);
    free(c->pres_ord);
    c->pres_ord = (int **)calloc((size_t)(c->nclasses > 0 ? c->nclasses : 1), sizeof(int *));
    if (!c->pres_ord) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->pres_ord_n = c->nclasses;
  }
  if (!c->pres_ord[cid]) {
    int *ord = (int *)malloc(sz);
    if (!ord) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    if (!ivs_static_order(c, cid, ord)) for (int j = 0; j < ci->nivars; j++) ord[j] = j;
    c->pres_ord[cid] = ord;
  }
  int *copy = (int *)malloc(sz);
  if (!copy) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(copy, c->pres_ord[cid], sz);
  return copy;
}
/* The C test that ivar `ivn` (of class `cid`, read as `expr`) is set, for
   an ivar of kind 1 or 3; NULL when it is always reported as set. */
/* A boxed (poly) instance field whose writes may store nil (ivar_set_kind
   2): set or unset cannot be read off the value, so the constructor seeds
   it with a nil carrying SP_IVAR_UNSET_MARK in its cls_id. */
int poly_ivar_unset_marked(Compiler *c, int cid, int iv) {
  if (cid < 0 || cid >= c->nclasses || iv < 0 || iv >= c->classes[cid].nivars) return 0;
  ClassInfo *ci = &c->classes[cid];
  if (ci->ivar_types[iv] != TY_POLY || (ci->is_struct && iv < ci->nmembers)) return 0;
  return ivar_set_kind(c, cid, ci->ivars[iv]) == 2;
}
const char *ivar_set_test(Compiler *c, int cid, const char *ivn, const char *expr, char *buf, size_t cap) {
  int kind = ivar_set_kind(c, cid, ivn);
  if (kind == 3) {
    size_t n = strlen(expr) - strlen(iv_c(ivn + 1)) - 3;
    snprintf(buf, cap, "(%.*s_sp_set_%s)", (int)n, expr, iv_c(ivn + 1));
    return buf;
  }
  /* a boxed slot a nil can be written into: the object starts it with
     the unset mark (a nil whose cls_id says never written), which any
     write replaces (poly_ivar_unset_marked) */
  if (kind == 2 && poly_ivar_unset_marked(c, cid, comp_ivar_index(&c->classes[cid], ivn))) {
    snprintf(buf, cap, "((%s).cls_id != 0x%x)", expr, SP_IVAR_UNSET_MARK);
    return buf;
  }
  if (kind != 1) return NULL;
  int iv = comp_ivar_index(&c->classes[cid], ivn);
  TyKind t = c->classes[cid].ivar_types[iv];
  if (oint_kind(t)) {
    /* set when its nil bit is clear; a field with no bit is always set */
    if (!ivar_has_nilbit(c, cid, iv)) return NULL;
    size_t n = strlen(expr) - strlen(iv_c(ivn + 1)) - 3;
    char obj[256]; snprintf(obj, sizeof obj, "%.*s", (int)n, expr);
    char bt[320]; ivar_nilbit_test(c, cid, iv, obj, bt, sizeof bt);
    snprintf(buf, cap, "(!%s)", bt);
  }
  else if (t == TY_POLY) snprintf(buf, cap, "((%s).tag != SP_TAG_NIL)", expr);
  else snprintf(buf, cap, "(%s != NULL)", expr);
  return buf;
}
/* The C statement that marks ivar `ivn` (of class `cid`, whose object reads
   as `obj` through `acc`) assigned, for the end of a store into an ivar of
   kind 3; NULL for any other. */
const char *ivar_set_mark(Compiler *c, int cid, const char *ivn, const char *obj, const char *acc,
                          char *buf, size_t cap) {
  if (ivar_set_kind(c, cid, ivn) != 3) return NULL;
  /* The ranked form names the object four times, and a boxed receiver's
     is a cast spelling the class: a long class name outgrew the caller's
     buffer, and the cut text went into the C (#8406). A mark that does not
     fit is made on the heap, whole. */
  const char *iv = iv_c(ivn + 1);
  int rk = ivar_ranked(c, cid);
  int need = rk ? snprintf(NULL, 0, "sp_ivar_rank(&%s%s_sp_set_%s, &%s%s_sp_ord, sp_ivar_renumber, %s%scls_id, %s);",
                           obj, acc, iv, obj, acc, obj, acc, obj)
                : snprintf(NULL, 0, "%s%s_sp_set_%s = TRUE;", obj, acc, iv);
  if (need < 0) return NULL;
  if ((size_t)need >= cap) {
    cap = (size_t)need + 1;
    buf = malloc(cap);
    if (!buf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  if (rk)
    snprintf(buf, cap, "sp_ivar_rank(&%s%s_sp_set_%s, &%s%s_sp_ord, sp_ivar_renumber, %s%scls_id, %s);",
             obj, acc, iv, obj, acc, obj, acc, obj);
  else snprintf(buf, cap, "%s%s_sp_set_%s = TRUE;", obj, acc, iv);
  return buf;
}
/* The same for ivar write node `w`, which stores into self: an instance
   method's, or an instance_eval body's, whose self is the rebound receiver
   (the class the write emitters take, ie_class_of first); NULL for a
   class-level ivar's write. */
const char *ivar_write_mark(Compiler *c, int w, char *buf, size_t cap) {
  Scope *s = comp_scope_of(c, w);
  const char *nm = nt_str(c->nt, w, "name");
  int iec = ie_class_of(c, w);
  int cid = iec >= 0 ? iec : s && !s->is_cmethod && s->class_id >= 0 ? s->class_id : g_ie_class_id;
  if (!s || cid < 0 || !nm || !g_self) return NULL;
  return ivar_set_mark(c, cid, nm, g_self, g_self_deref, buf, cap);
}
/* The C global ivar read `node` lives in, by the read emitter's storage
   rule: a class method's ivar is the class's civ_ slot, a top-level one
   (outside an instance_eval) the Toplevel's. Fills `out` and answers 1;
   0 for an instance's field. */
int ivar_global_slot(Compiler *c, int node, char *out, size_t cap) {
  const char *nm = nt_str(c->nt, node, "name");
  Scope *cs = comp_scope_of(c, node);
  if (!nm || !cs || g_ie_nil_ivars) return 0;
  if (g_sb_iv_name && sp_streq(nm, g_sb_iv_name)) return 0;
  if (cs->is_cmethod && cs->class_id >= 0)
    snprintf(out, cap, "civ_%s_%s", c->classes[cs->class_id].name, iv_c(nm + 1));
  else if (cs->class_id < 0 && g_ie_class_id < 0 && comp_class_index(c, "Toplevel") >= 0)
    snprintf(out, cap, "civ_Toplevel_%s", iv_c(nm + 1));
  else return 0;
  return 1;
}
/* The C global global-variable read `node` reads, when it is the plain
   gv_ slot rather than a special global's runtime accessor (`$0`, `$~`,
   `$stdout`, ...): the read emitter decides, so its own text is asked. */
int gvar_global_slot(Compiler *c, int node, char *out, size_t cap) {
  const char *gn = nt_str(c->nt, node, "name");
  const char *grn = gn && gn[0] == '$' ? comp_resolve_gvar(c, gn + 1) : NULL;
  if (!grn || !comp_gvar(c, grn)) return 0;
  Buf gb; memset(&gb, 0, sizeof gb);
  emit_expr(c, node, &gb);
  int plain = gb.p && !strncmp(gb.p, "gv_", 3) && sp_streq(gb.p + 3, grn) &&
              strlen(gb.p) < cap;
  if (plain) snprintf(out, cap, "%s", gb.p);
  free(gb.p);
  return plain;
}
/* The C global a class variable read or write `node` names
   (cvar_<owner>_<name>), by the read emitter's rule: the method's class, or
   the class body the node is in, or the Toplevel, then the ancestor that
   owns the class variable. Fills `out` and answers 1, or 0. */
int cvar_global_slot(Compiler *c, int node, char *out, size_t cap) {
  const char *nm = nt_str(c->nt, node, "name");
  HolderRef h;
  if (!nm || nm[0] != '@' || nm[1] != '@' || !holder_of_node(c, node, &h) || h.kind != HK_CVAR) return 0;
  return holder_slot_text(c, &h, out, cap);
}
/* The innermost block or lambda `node` is written in, within its method;
   -1 at the method's own level. */
int *an_parent_map(const NodeTable *nt);
static int *g_lent_parent;
static int g_lent_parent_n = -1;
static unsigned g_lent_parent_ver;
static int lent_enclosing_closure(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (!g_lent_parent || g_lent_parent_n != nt->count || g_lent_parent_ver != nt->version) {
    free(g_lent_parent);
    g_lent_parent = an_parent_map(nt);
    if (!g_lent_parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    g_lent_parent_n = nt->count; g_lent_parent_ver = nt->version;
  }
  for (int p = node >= 0 && node < nt->count ? g_lent_parent[node] : -1; p >= 0; p = g_lent_parent[p]) {
    NodeKind k = nt_kind(nt, p);
    if (k == NK_BlockNode || k == NK_LambdaNode) return p;
    if (k == NK_DefNode) return -1;
  }
  return -1;
}
/* A write of the C global `slot` (ivar_global_slot, gvar_global_slot,
   cvar_global_slot) that can run while a call lent `slot` at `arg` is
   running: one in another
   method, or in a block or lambda other than the one the call is written
   in. The callee holds the slot's address, so after such a write its
   appends land in the new String, where CRuby appends to the one the call
   was handed. Answers the write, or -1 when every write is in the call's
   own method and block or in the program's top level outside a block (it
   runs before or after the call). A write in the call's own method still
   runs during the call when the callee calls that method back; that
   recursion is not followed. */
int lent_global_slot_rebound(Compiler *c, int arg, const char *slot) {
  static const NodeKind gk[] = { NK_GlobalVariableWriteNode, NK_GlobalVariableOrWriteNode,
                                 NK_GlobalVariableAndWriteNode, NK_GlobalVariableOperatorWriteNode,
                                 NK_GlobalVariableTargetNode };
  static const NodeKind ik[] = { NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
                                 NK_InstanceVariableAndWriteNode, NK_InstanceVariableOperatorWriteNode,
                                 NK_InstanceVariableTargetNode };
  static const NodeKind ck[] = { NK_ClassVariableWriteNode, NK_ClassVariableOrWriteNode,
                                 NK_ClassVariableAndWriteNode, NK_ClassVariableOperatorWriteNode,
                                 NK_ClassVariableTargetNode };
  const NodeTable *nt = c->nt;
  int is_g = nt_kind(nt, arg) == NK_GlobalVariableReadNode;
  int is_c = nt_kind(nt, arg) == NK_ClassVariableReadNode;
  const NodeKind *ks = is_g ? gk : is_c ? ck : ik;
  Scope *as = comp_scope_of(c, arg);
  int ab = lent_enclosing_closure(c, arg);
  for (int k = 0; k < 5; k++)
    for (int w = comp_kind_first(c, ks[k]); w >= 0; w = comp_kind_next(c, w)) {
      if (nt_kind(nt, w) != ks[k]) continue;
      char ws[256];
      if (is_g) {
        const char *gn = nt_str(nt, w, "name");
        const char *grn = gn && gn[0] == '$' ? comp_resolve_gvar(c, gn + 1) : NULL;
        if (!grn) continue;
        snprintf(ws, sizeof ws, "gv_%s", grn);
      }
      else if (is_c ? !cvar_global_slot(c, w, ws, sizeof ws) : !ivar_global_slot(c, w, ws, sizeof ws)) continue;
      if (!sp_streq(ws, slot)) continue;
      /* the program's top level, outside any block or lambda, runs once and
         is never reentered, so it is never running during a call */
      Scope *ws2 = comp_scope_of(c, w);
      int wb = lent_enclosing_closure(c, w);
      if (wb < 0 && ws2 && !ws2->name && ws2->def_node < 0) continue;
      if (ws2 != as || wb != ab) return w;
    }
  return -1;
}
/* Refuse lending `slot` at `arg` when lent_global_slot_rebound finds a
   write that can run during the call. */
void refuse_lent_global_rebound(Compiler *c, int arg, const char *slot, const char *target, const char *pname) {
  int w = lent_global_slot_rebound(c, arg, slot);
  if (w < 0) return;
  const char *vn = nt_str(c->nt, arg, "name");
  char msg[768];
  snprintf(msg, sizeof msg,
           "`%s` is passed to %s's parameter `%s`, which appends to it, and `%s` is assigned at line %d, "
           "where the assignment can run during the call: the append would then reach the newly "
           "assigned String instead of the one passed (a String held by a global, a class variable, or "
           "a top-level or class-level instance variable is not yet shared by reference). Pass a local "
           "and assign it back after the call.",
           vn ? vn : "?", target ? target : "a method", pname ? pname : "?", vn ? vn : "?",
           (int)nt_int(c->nt, w, "node_line", 0));
  unsupported_feature(c, arg, msg);
}
/* Emit-side lvalue for a shared-mutable string receiver: lv_<x> for a
   strbuf local, <self>-><iv_x> (or civ_Toplevel_x) for a strbuf ivar.
   Returns 1 and fills `out`, or 0 when the receiver is neither (#3227). */
/* An empty `[]` / `{}` assigned into a typed slot has to be built at the SLOT's
   representation, not at the literal's own default: the literal carries no
   element or key type, so emit_expr answers an IntArray / StrPolyHash and the
   store lands on a slot of a different C type. The global and constant writes
   spell this out inline; class variables share the rule (#4054). Returns 1
   when it emitted. */
int emit_empty_container_for_slot(Compiler *c, int v, TyKind slot, Buf *b) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  const char *vty = nt_type(nt, v);
  if (!vty) return 0;
  int n = 0;
  if (sp_streq(vty, "ArrayNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    if (slot == TY_POLY_ARRAY) { buf_puts(b, "sp_PolyArray_new()"); return 1; }
    if (array_kind(slot)) { buf_printf(b, "sp_%sArray_new()", array_kind(slot)); return 1; }
    return 0;
  }
  if (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    const char *hcn = ty_is_hash(slot) ? ty_hash_cname(slot) : NULL;
    if (!hcn) return 0;
    buf_printf(b, "sp_%sHash_new()", hcn);
    return 1;
  }
  /* `Hash.new` (bare, with a default or a capacity) and a bare `Array.new` are the same
     empty producers, and the typed slot needs the same fresh container rather
     than the boxed value the untyped call would emit (the global write says
     this too). */
  if (sp_streq(vty, "CallNode") && nt_ref(nt, v, "block") < 0) {
    const char *cn = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver");
    const char *rn = (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode) ? nt_str(nt, r, "name") : NULL;
    if (!cn || !rn || !sp_streq(cn, "new")) return 0;
    int a = nt_ref(nt, v, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (sp_streq(rn, "Array") && an == 0) {
      if (slot == TY_POLY_ARRAY) { buf_puts(b, "sp_PolyArray_new()"); return 1; }
      if (ty_is_ptr_array(slot)) { buf_puts(b, "sp_PtrArray_new()"); return 1; }
      if (array_kind(slot)) { buf_printf(b, "sp_%sArray_new()", array_kind(slot)); return 1; }
      return 0;
    }
    const char *hcn = ty_is_hash(slot) ? ty_hash_cname(slot) : NULL;
    if (!hcn || !sp_streq(rn, "Hash") || an > 1) return 0;
    /* a `capacity:` value with something to run runs after the Hash is
       built (emit_hash_new_capacity_wrap) */
    int cap = nt_ref(nt, v, "hash_capacity");
    if (cap >= 0 && g_hash_cap_inner != v) {
      int save = g_hash_cap_inner;
      g_hash_cap_inner = v;
      Buf in; memset(&in, 0, sizeof in);
      int ok = emit_empty_container_for_slot(c, v, slot, &in);
      g_hash_cap_inner = save;
      if (!ok) { free(in.p); return 0; }
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sHash *_t%d = %s; SP_GC_ROOT(_t%d); ", hcn, t, in.p ? in.p : "NULL", t);
      free(in.p);
      emit_hash_new_capacity_check(c, cap, b);
      buf_printf(b, "_t%d; })", t);
      return 1;
    }
    if (an == 1 && nt_kind(nt, av[0]) == NK_KeywordHashNode) {
      /* `capacity:` sizes nothing here, but its value is still evaluated */
      int en = 0; const int *el = nt_arr(nt, av[0], "elements", &en);
      buf_puts(b, "(");
      for (int i = 0; i < en; i++) {
        int ev = nt_kind(nt, el[i]) == NK_AssocNode ? nt_ref(nt, el[i], "value") : el[i];
        buf_puts(b, "(void)("); emit_expr(c, ev, b); buf_puts(b, "), ");
      }
      buf_printf(b, "sp_%sHash_new())", hcn);
      return 1;
    }
    if (an == 1 && nt_kind(nt, av[0]) == NK_NilNode) an = 0;
    if (an == 0) { buf_printf(b, "sp_%sHash_new()", hcn); return 1; }
    TyKind hv = ty_hash_val(slot), dt = comp_ntype(c, av[0]);
    if (hv != TY_POLY && dt != TY_UNKNOWN && dt != hv && !(hv == TY_FLOAT && dt == TY_INT))
      unsupported(c, v, "a Hash.new default of another type than the typed hash slot's values");
    buf_printf(b, "sp_%sHash_new_with_default(", hcn);
    if (ty_hash_val(slot) == TY_POLY) emit_boxed(c, av[0], b);
    else emit_expr(c, av[0], b);
    buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* emit_poly_rhs_coerced's conversion of a boxed value into a scalar or a
   String slot (a String slot takes sp_poly_arg_str instead where the
   program defines a #to_str); NULL for any other slot */
const char *poly_rhs_unbox_fn(TyKind slot) {
  const TyTraits *tr = ty_traits_of(slot);   /* the unbox_rhs column (types.c) */
  return tr ? tr->unbox_rhs : NULL;
}
/* emit_typed_sink_text's conversion of a boxed value into a typed element */
const char *poly_sink_unbox_fn(TyKind slot) {
  const TyTraits *tr = ty_traits_of(slot);   /* the unbox_sink column (types.c) */
  return tr ? tr->unbox_sink : NULL;
}

int emit_poly_rhs_coerced(Compiler *c, TyKind slot, int v, Buf *b) {
  /* yield_site_type, not comp_ntype: a `yield` carries the union over every
     call site, and the block spliced HERE may already hand back the scalar
     this slot wants. Coercing that would unbox a value that is not boxed. */
  if (v < 0 || yield_site_type(c, v) != TY_POLY) return 0;
  /* int and string carry the implicit conversion protocol: this narrowing is
     the moment a boxed user object enters a typed slot, and reading it as 0 or
     as its #to_s rendering is the silent wrong answer. bool keeps the plain
     form -- an object in a bool slot is truthy, not a number. */
  /* The int slot needs nothing here: sp_poly_to_i carries the conversion
     protocol in its cold half, so a narrowing is correct for free. The string
     slot cannot -- sp_poly_to_s renders an object through #to_s, which is
     right for interpolation -- so it takes the protocol form, and only where
     the program defines a #to_str to reach: a narrowing lands wherever the
     analysis put it, including a hot loop, and the test is not free there.
     bool keeps the plain form: an object in a bool slot is truthy. */
  /* A nil narrowed into an Integer or Float slot is the slot's own nil (a
     slot that can hold one is an oint), never the 0 under the tag (#4288).
     TY_BOOL keeps the plain form: nil in a bool slot is false. */
  /* A class-typed slot (a parameter an RBS declaration pinned to its class)
     reassigned from a boxed value (`comment = subtree.shift` over a poly
     array) took the raw sp_RbVal and the C did not compile (#4640). The
     checked unbox: the tag and class are verified, nil stays NULL. */
  if (ty_is_object(slot)) {
    Buf e; memset(&e, 0, sizeof e);
    emit_expr(c, v, &e);
    emit_unbox_text(c, slot, e.p ? e.p : "sp_box_nil()", b);
    free(e.p);
    return 1;
  }
  const char *fn = slot == TY_STRING && prog_has_conv_method(c, "to_str", TY_STRING)
                   ? "sp_poly_arg_str" : poly_rhs_unbox_fn(slot);
  if (!fn) return 0;
  buf_printf(b, "%s(", fn); emit_expr(c, v, b); buf_puts(b, ")");
  return 1;
}

static int strbuf_box_ref_as(Compiler *c, int recv, const char *fmt, Buf *b) {
  char sref[1024];
  int svm = view_push_repr(c, recv, VR_STRBUF_BOX, 1);
  int is_sb = strbuf_slot_ref(c, recv, sref, sizeof sref);
  view_pop(c, svm);
  if (!is_sb) return 0;
  buf_printf(b, fmt, sref);
  return 1;
}

/* Emit a shared-mutable string receiver for an operation that only READS its
   bytes: the live buffer, not the whole-buffer copy an ordinary value read
   makes (#3227). Answers 0 when the receiver is not such a slot, so the caller
   falls back to emit_expr. A narrowed box is not a handle slot: its ordinary
   expression reads the bytes without allocating a handle first. */
int emit_strbuf_read_ref(Compiler *c, int recv, Buf *b) { return strbuf_box_ref_as(c, recv, "sp_String_cstr(%s)", b); }
/* The object_id of a String held as a shared sp_String: the handle's address,
   which is what a box of it carries. 0 when `recv` is not one.
   A frozen literal's handle keeps the identity of its static bytes. */
int strbuf_object_ref(Compiler *c, int recv, Buf *b) {
  /* An exception's message is asked its own identity, whether or not the
     program overrides the text (exc_message_identity_operand). */
  if (exc_message_identity_operand(c, recv)) {
    buf_puts(b, "((sp_int)(uintptr_t)sp_poly_identity_ptr(");
    emit_exc_message_idbox(c, recv, b);
    buf_puts(b, "))");
    return 1;
  }
  /* A receiver-returning route has the same identity as its slot. */
  char ref[1024];
  int up = 0;
  int route = strbuf_self_route_slot(c, recv, &up, ref, sizeof ref);
  if (route && !up) {
    if (repr_of(c, route - 1).may_nil) {
      int t = ++g_tmp;
      buf_printf(b, repr_share_rule(c) ? "({ sp_String *_t%d = %s; _t%d ? (sp_int)(uintptr_t)sp_String_identity(_t%d) : %s; })"
                                     : "({ sp_String *_t%d = %s; _t%d ? (sp_int)(uintptr_t)_t%d : %s; })",
                 t, ref, t, t, repr_of(c, recv).may_nil ? "SP_NIL_OBJECT_ID" : "(sp_int)(uintptr_t)sp_str_frozen_empty");
    }
    else buf_printf(b, (repr_share_rule(c) ? "((sp_int)(uintptr_t)sp_String_identity(%s))" : "((sp_int)(uintptr_t)(%s))"), ref);
    return 1;
  }
  if (repr_of(c, recv).may_nil) {
    Buf rb; memset(&rb, 0, sizeof rb);
    if (!strbuf_box_ref_as(c, recv, "%s", &rb)) return 0;
    int t = ++g_tmp;
    buf_printf(b, repr_share_rule(c) ? "({ sp_String *_t%d = %s; _t%d ? (sp_int)(uintptr_t)sp_String_identity(_t%d) : SP_NIL_OBJECT_ID; })"
                                   : "({ sp_String *_t%d = %s; _t%d ? (sp_int)(uintptr_t)_t%d : SP_NIL_OBJECT_ID; })", t, rb.p, t, t);
    free(rb.p);
    return 1;
  }
  /* The box's payload is already its identity, with or without a handle.
     A frozen literal's handle resolves to the same identity as its bytes. */
  if (repr_share_rule(c) && recv >= 0 && repr_of(c, recv).strbuf_src == RS_SLOT_POLY) {
    buf_puts(b, "((sp_int)(uintptr_t)sp_poly_identity_ptr(");
    emit_local_ref(c, recv, nt_str(c->nt, recv, "name"), b);
    buf_puts(b, "))");
    return 1;
  }
  return strbuf_box_ref_as(c, recv, (repr_share_rule(c) ? "((sp_int)(uintptr_t)sp_String_identity(%s))" : "((sp_int)(uintptr_t)(%s))"), b);
}
/* `cont[k]` where the container hands its elements out BOXED (a poly array, a
   hash): the read is an sp_RbVal, so a shared-handle destination has to unbox
   it rather than wrap it (#3941). */
int strbuf_boxed_elem_read(Compiler *c, int v) {
  if (!container_elem_read_p(c->nt, v)) return 0;
  int r = nt_ref(c->nt, v, "receiver");
  if (r < 0) return 0;
  Repr rr = repr_of(c, r);
  /* a Hash hands its values out boxed only when it holds them boxed: a
     String-to-String Hash's read is the plain String */
  return rr.kind == RK_BOXED || rr.elem == TY_POLY ||
         (rr.key != TY_UNKNOWN && (rr.val == TY_POLY || rr.val == TY_UNKNOWN));
}
/* A String is a const char * value, so a String mutator (`<<`, the bang
   methods, replace/insert/...) is lowered to a reassignment of its receiver:
   `s = sp_str_append_grow(s, x)`. That needs a receiver whose C form is an
   lvalue holding the string: a local or ivar, and equally a global, a class
   variable or a constant (gv_X, cvar_C_X, cst_X). A constant qualifies when
   its read is the plain slot -- the Class.new-guarded read is a conditional
   expression, not an lvalue, and a `klass::NAME` path whose owner is a run-time
   value reads through a switch. Any other receiver has nowhere to put the new
   string. */
int str_mut_var_recv(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  switch (nt_kind(nt, recv)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
  case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode:
    return 1;
  case NK_ConstantReadNode: {
    LocalVar *cv = comp_const(c, nt_str(nt, recv, "name"));
    return cv && cv->type != TY_UNKNOWN && !cv->init_guarded;
  }
  case NK_ConstantPathNode: {
    /* a call on the slot of a constant read through a receiver that runs: the
       slot's pointer is held (emit_const_recv_wrap), and the node reads it */
    if (nt_int(nt, recv, "const_ptr", 0)) return 1;
    /* the path's read resolves through several tables; ask it for its text */
    if (comp_ntype(c, recv) != TY_STRING) return 0;
    int save = g_tmp;
    Buf rb = expr_buf(c, recv);
    g_tmp = save;
    const char *t = rb.p ? rb.p : "";
    int ok = strncmp(t, "cst_", 4) == 0 && t[4];
    for (const char *q = t + 4; ok && *q; q++)
      if (!(isalnum((unsigned char)*q) || *q == '_')) ok = 0;
    /* a constant with a presence flag reads as `*(test, &cst_NAME)`: still the slot */
    if (!ok && strncmp(t, "(*((cst_", 8) == 0) ok = 1;
    free(rb.p);
    return ok;
  }
  default:
    return 0;
  }
}
/* A reader call the shared-mutable shim has substituted with its shadow copy
   (through the argument-override table): the value arms may rebind it as they
   would a local. */
int sb_shadowed_reader(int node) {
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == node && strncmp(g_argov_text[i], "lv__sb", 6) == 0) return 1;
  return 0;
}
/* The arguments of the String mutator `id` that a shim re-runs on its
   shadow copy, run ahead of the copy into rooted temps the arm reads
   (emit_args_run): CRuby evaluates them before the method reads the String,
   and one that changes the String through another name (an alias, a
   captured cell, a method) changed the handle after the shim had copied it,
   so the shadow's write-back undid the change. The temps go to `pre`, for
   the caller to emit between its read of the handle and the copy; answers
   the override mark to unbind at once the arm is emitted. */
int sb_shim_args_first(Compiler *c, int id, Buf *pre, int indent) {
  int mark = g_n_argov;
  int a = nt_ref(c->nt, id, "arguments"), argc = 0;
  const int *argv = a >= 0 ? nt_arr(c->nt, a, "arguments", &argc) : NULL;
  if (argc == 0) return mark;
  Buf *sv_pre = g_pre; int sv_ind = g_indent;
  g_pre = pre; g_indent = indent;
  emit_args_run(c, argv, argc);
  g_pre = sv_pre; g_indent = sv_ind;
  return mark;
}
/* Open the shim over a reader call `recv` that hands out the shared handle:
   the handle's text goes to sref, and until sb_reader_shim_close the call node
   reads as the shadow `lv__sbT`, with the handle marks lifted. Answers T, or 0
   when `recv` is no such call. */
int sb_reader_shim_open(Compiler *c, int recv, char *sref, size_t cap, SbReaderSave *sv) {
  /* a global holding the handle (--share-strings) is such a slot too:
     strbuf_slot_ref answers for it only then */
  int gv = repr_share_rule(c) && recv >= 0 && repr_static_read_kind(nt_kind(c->nt, recv)) && !sb_shadowed_reader(recv);
  if (recv < 0 || (nt_kind(c->nt, recv) != NK_CallNode && !gv)) return 0;
  Repr rp = repr_of(c, recv);
  if (!gv && !rp.handle && !rp.demand) return 0;
  if (g_n_argov >= MAX_ARG_OVERRIDE) return 0;
  if (!strbuf_slot_ref(c, recv, sref, cap)) return 0;
  int tH = ++g_tmp;
  /* the marks lifted and the handle type dropped for the shim's lifetime:
     views, so a refusal inside it puts them back (view_unwind) */
  sv->box = rp.handle; sv->demand = rp.demand; sv->ty = rp.ty;
  sv->tok = view_push_repr(c, recv, VR_STRBUF_BOX, 0);
  view_push_repr(c, recv, VR_HANDLE_DEMAND, 0);
  sv->ntok = 2;
  if (sv->ty == TY_STRBUF) { view_push(c, recv, TY_STRING); sv->ntok = 3; }
  view_bind(recv, "lv__sb%d", tH);
  return tH;
}
void sb_reader_shim_close(Compiler *c, int recv, const SbReaderSave *sv) {
  (void)recv;
  view_unbind(g_n_argov - 1);
  for (int k = sv->ntok - 1; k >= 0; k--) view_pop(c, sv->tok + k);
}
const char *g_sb_iv_name = NULL;
int         g_sb_iv_cid  = -1;
char        g_sb_iv_repl[64];
/* Does demand-marked call `v` render as a handle itself? A reader call, a
   container's element read and a call that answers its receiver (`h << x
   << y`, `h.freeze`) do. A call on a String that makes a new one -- `+"lit"`,
   `s.dup`, `s + t` -- renders as that String, which the demand marked to be
   wrapped as a fresh handle where it is stored. */
int strbuf_marked_yields_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, v) != NK_CallNode) return 0;
  int r = nt_ref(nt, v, "receiver");
  TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
  /* A fresh object-method result is bytes to wrap, not a reader's handle. */
  if (repr_share_rule(c) && ty_is_object(rt) && share_value_fresh(c, v, 0)) return 0;
  /* A yielding method is inlined into the demanded handle slot. Other
     method calls keep their byte ABI and need a proved return pickup;
     a field reader has its own handle arm. */
  if (repr_share_rule(c) && ty_is_object(rt)) {
    int mi = cplan_user(c, v)->mi;
    if (mi >= 0) return c->scopes[mi].yields || an_memo_reader_ivar(c, mi) || repr_call_returns_handle(c, v);
  }
  /* Without the flag, a native String return uses its declared byte ABI;
     there is no native shared-answer route to yield a handle. */
  if (!repr_share_rule(c) && ty_is_object(rt)) {
    /* A user method's byte return takes the default copy wrapper. A
       boxed return already has its own representation and keeps it. */
    int mi = cplan_user(c, v)->mi;
    if (mi >= 0) return c->scopes[mi].yields || repr_of_ret(c, &c->scopes[mi]).as_ty == TY_STRING;
    int nm = comp_native_method_find(c, ty_object_class(rt), nt_str(nt, v, "name"), call_plain_argc(c, v), 0);
    if (nm >= 0 && native_spec_to_ty(c->native_methods[nm].ret) == TY_STRING) return 0;
  }
  if (rt != TY_STRING && rt != TY_STRBUF) return 1;
  const char *nm = nt_str(nt, v, "name");
  return nm && (is_append_concat(nm) || str_self_call(nt, v));
}
/* A String method answering its receiver or nil (bop_share_self_answer:
   a bang method, an iterator given a block) called on a local that holds
   the shared handle (--share-strings: or an ivar, a global, a class
   variable or a constant that holds the handle the rule assigned, or a
   reader call read as the handle on a variable or self): its value is that
   String, or nil. */
int strbuf_bang_self_local(const Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return 0;
  int self_ans = bop_share_self_answer(nt_str(nt, v, "name"), nt_ref(nt, v, "block") >= 0);
  if (!self_ans && !repr_share_rule(c)) return 0;
  int r = nt_ref(nt, v, "receiver");
  if (r < 0) return 0;
  if (!self_ans && !(nt_str(nt, v, "name") && (comp_ntype((Compiler *)c, r) == TY_STRING || comp_ntype((Compiler *)c, r) == TY_STRBUF) &&
                     bop_share_named(TY_STRING, nt_str(nt, v, "name")) == BSH_RECV))
    return 0;
  /* The builtin's receiver answer does not describe a user override. */
  int targets[CPT_MAX];
  if (cplan_targets((Compiler *)c, v, targets, CPT_MAX) != 0) return 0;
  if (self_ans && nt_kind(nt, r) == NK_LocalVariableReadNode && repr_of(c, r).kind == RK_STRBUF) return 1;
  /* (--share-strings: a String method that answers its receiver always,
     `insert`, too) */
  if (!repr_share_rule(c)) return 0;
  char ref[1024];
  NodeKind rk = nt_kind(nt, r);
  if (!self_ans) {
    int argc;
    call_args(nt, v, &argc);
    /* A sharing row can also describe a conditional copy (`+@` on a
       frozen String). Only an exact receiver answer keeps this handle. */
    if (bop_answers_self(TY_STRING, nt_str(nt, v, "name"), argc, nt_ref(nt, v, "block") >= 0) != BOPF_SELF) return 0;
  }
  /* The receiver can itself be a route over a handle, including an
     append chain followed by a prepend with several arguments. */
  if (strbuf_value_carries((Compiler *)c, r)) return 1;
  if (rk == NK_InstanceVariableReadNode || repr_static_read_kind(rk))
    return strbuf_var_handle((Compiler *)c, r, ref, sizeof ref);
  /* a reader call read as the handle on a variable or self (`o.s.strip!`),
     which emit_bang_self_handle reads once */
  int rr = rk == NK_CallNode ? nt_ref(nt, r, "receiver") : -1;
  NodeKind rrk = rr >= 0 ? nt_kind(nt, rr) : NK_NONE;
  return rk == NK_CallNode && nt_ref(nt, r, "arguments") < 0 && nt_ref(nt, r, "block") < 0 &&
         (rr < 0 || rrk == NK_SelfNode || rrk == NK_LocalVariableReadNode || rrk == NK_InstanceVariableReadNode) &&
         strbuf_call_reads_handle((Compiler *)c, r);
}
/* Is recv a demand-marked reader call typed as the handle, whose emitted
   read is the sp_String * itself (strbuf_slot_ref's call arm)? */
int strbuf_call_reads_handle(Compiler *c, int recv) {
  return recv >= 0 && nt_kind(c->nt, recv) == NK_CallNode &&
         ((repr_of(c, recv).handle && repr_of(c, recv).as_ty == TY_STRBUF && strbuf_marked_yields_handle(c, recv)) ||
          repr_of(c, recv).demand);
}
/* --share-strings: is call n a native method answering the String its
   object keeps (`native_share ... "answers"`), on an object whose String
   the rule shares? Its binding, or NULL. (The seal asks it too:
   strbuf_flow_carries.) */
const NativeMethod *strbuf_native_answer(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (!repr_share_rule(c) || n < 0 || nt_kind(nt, n) != NK_CallNode || nt_ref(nt, n, "block") >= 0) return NULL;
  int r = nt_ref(nt, n, "receiver");
  TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
  if (!ty_is_object(rt) || !c->classes[ty_object_class(rt)].is_native_class) return NULL;
  int a = nt_ref(nt, n, "arguments"), argc = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &argc);
  int nm = comp_native_method_find(c, ty_object_class(rt), nt_str(nt, n, "name"), argc, 0);
  const NativeMethod *m = nm >= 0 ? &c->native_methods[nm] : NULL;
  if (!m || !(m->share & NSH_ANSWERS) || !m->share_csym || argc != 0 || !share_node_elems_share(c, r)) return NULL;
  return m;
}
/* the handle text of such a call: its handle form's call */
static int native_share_answer_ref(Compiler *c, int n, char *out, size_t cap) {
  const NativeMethod *m = strbuf_native_answer(c, n);
  if (!m) return 0;
  int r = nt_ref(c->nt, n, "receiver");
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, r, &rb);
  int fit = rb.p && strlen(rb.p) + strlen(m->share_csym) + 3 <= cap;
  if (fit) snprintf(out, cap, "%s(%s)", m->share_csym, rb.p);
  free(rb.p);
  return fit;
}
int strbuf_slot_ref(Compiler *c, int recv, char *out, size_t cap) {
  HolderRef h;
  if (repr_self_shared(c, recv)) {
    if (!g_self || strlen(g_self) >= cap) return 0;
    snprintf(out, cap, "%s", g_self);
    return 1;
  }
  if (native_share_answer_ref(c, recv, out, cap)) return 1;
  /* via emit_local_ref: a celled/captured local derefs its cell */
  if (strbuf_local_name(c, recv) && holder_of_node(c, recv, &h)) return holder_slot_text(c, &h, out, cap);
  /* a demand-marked reader call typed as the handle (external reader
     mutation, e.g. `subs[0].topic << x`): the emitted read IS the sp_String*
     expression. Declined when it does not fit the caller's buffer (the
     branches then fall through to the value-form arms). */
  /* strbuf_handle_demand is the same demand carried without the type: a mark
     made after the node-type cache is finalized cannot move the type without
     moving the call off the surface that dispatches it (see compiler.h). */
  if (strbuf_call_reads_handle(c, recv)) {
    Buf rb2; memset(&rb2, 0, sizeof rb2);
    emit_expr(c, recv, &rb2);
    /* A container ELEMENT read comes back BOXED (a poly array element, a hash
       value), so the handle has to come out of the box; a reader call already
       emits the sp_String * itself (#3941). */
    int erecv = nt_ref(c->nt, recv, "receiver");
    const char *cnm = nt_str(c->nt, recv, "name");
    Repr er = repr_of(c, erecv);
    int boxed = cnm && sp_streq(cnm, "[]") &&
                (er.kind == RK_BOXED || er.elem == TY_POLY || er.key != TY_UNKNOWN);
    int fit = rb2.p && strlen(rb2.p) + 24 <= cap;
    if (fit) snprintf(out, cap, boxed ? "sp_poly_as_strbuf(%s)" : "(%s)", rb2.p);
    free(rb2.p);
    return fit;
  }
  /* a global or a constant holding the handle (--share-strings, #6765) */
  if (repr_share_rule(c) && recv >= 0 && repr_static_read_kind(nt_kind(c->nt, recv))) {
    /* inside the shim over it, the read is its plain shadow */
    if (sb_shadowed_reader(recv)) return 0;
    return holder_static_handle_text(c, recv, out, cap);
  }
  if (recv < 0 || nt_kind(c->nt, recv) != NK_InstanceVariableReadNode || !holder_of_node(c, recv, &h) ||
      h.idx < 0 || c->classes[h.cid].ivar_types[h.idx] != TY_STRBUF) return 0;
  /* The ivar shim's value arm writes its shadow, as the read emitter does. */
  if (g_sb_iv_name && sp_streq(h.name, g_sb_iv_name) && h.cid == g_sb_iv_cid) return 0;
  return holder_slot_text(c, &h, out, cap);
}
/* rename_local for a name whose CELL is wanted: the shim's shadow rename is
   skipped, since the shadow is a plain C local and the cell the local lives
   in keeps the name an enclosing inline gave it (or the source name). */
const char *rename_local_cell(const char *nm) {
  for (int i = g_nren - 1; i >= 0; i--)
    if (sp_streq(g_ren_from[i], nm) && !sb_shim_shadow(nm, g_ren_to[i])) return g_ren_to[i];
  return nm;
}
/* Hide the renames from `base` up while a caller's expression is emitted
   (a provided argument reads none of the callee's parameters). A call in
   that expression pushes renames of its own into the same slots, so the
   hidden ones are kept aside and put back: truncating g_nren alone left an
   inner call's `r` where the outer default read its own (#8319). */
void ren_hide(RenHide *h, int base) {
  h->base = base; h->top = g_nren; h->from = NULL; h->to = NULL;
  int n = h->top - base;
  if (n > 0) {
    h->from = malloc(sizeof *h->from * (size_t)n);
    h->to = malloc(sizeof *h->to * (size_t)n);
    if (!h->from || !h->to) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(h->from, g_ren_from[base], sizeof *h->from * (size_t)n);
    memcpy(h->to, g_ren_to[base], sizeof *h->to * (size_t)n);
  }
  g_nren = base;
}
void ren_unhide(RenHide *h) {
  int n = h->top - h->base;
  if (n > 0) {
    memcpy(g_ren_from[h->base], h->from, sizeof *h->from * (size_t)n);
    memcpy(g_ren_to[h->base], h->to, sizeof *h->to * (size_t)n);
  }
  free(h->from); free(h->to);
  g_nren = h->top;
}
const char *rename_local(const char *nm) {
  /* Innermost first. A nested inline pushes its own locals above the caller's,
     and a same-named local belongs to the inner one -- scanning forward gave
     the parent's `yield a` the child's `a` in a super chain. The park
     mechanism bounds visibility by truncating g_nren, so a backward scan sees
     exactly the same entries. */
  for (int i = g_nren - 1; i >= 0; i--)
    if (sp_streq(g_ren_from[i], nm)) return g_ren_to[i];
  return nm;
}
/* Report a feature spinel deliberately does not support (docs/limitations.md).
   Unlike `unsupported`, which describes a codegen gap and dumps the node so the
   compiler can be debugged, this names the feature and stops: the internals are
   noise when the answer is "this is a documented limit". #2652 / #2667 / #2668 */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg) {
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  refuse_at(c, id);
  int ln; const char *file = unsup_pos(c, id, &ln);
  unsup_leave(file, ln, msg);
}

/* A NoMethodError text that names a Class.new class `ci`: "<head><class>" as a C
   expression, appended to b. The class prints at run time (its constant may be
   assigned only after the program starts), so the text is built where it is
   raised and kept per raise site as a persistent string, because the raise
   stages it before anything roots it; it is rebuilt only when the class's
   display changes. In a program that uses Threads the site's cache is per
   worker, so no two workers read or write it at once. 0 (nothing appended)
   for any other class. */
int anon_class_text(Compiler *c, int ci, const char *head, Buf *b) {
  if (!comp_class_anonymous(c, ci)) return 0;
  int t = ++g_tmp;
  /* in a program that uses Threads each worker keeps its own pair (SP_TLS), so
     two workers running this site never share it */
  buf_printf(b, "({ static %sconst char *_nd%d, *_nm%d; const char *_d%d = sp_class_display((sp_Class){%d}); "
                "if (_d%d != _nd%d || !_nm%d) { const char *_m%d = sp_anon_text(\"%s\", _d%d); _nm%d = _m%d; _nd%d = _d%d; } _nm%d; })",
             g_uses_threads ? "SP_TLS " : "", t, t, t, ci, t, t, t, t, head, t, t, t, t, t, t);
  return 1;
}

/* The words `unsupported` refuses node id in, into msg; answers what the
   refusal says the call is (CplanRefuse). self_ci is the class whose body
   is being emitted (a bare name's NameError names it). Pure: the call plan
   asks it too (cplan_refuse). */
int unsup_message(Compiler *c, int id, const char *what, int self_ci, char *msg, size_t cap) {
  const char *ty = nt_type(c->nt, id);
  const char *mname = ty && sp_streq(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname) {
    int recv = nt_ref(c->nt, id, "receiver");
    int args = nt_ref(c->nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
    /* A bare unresolved identifier (Prism variable-call: no receiver, no
       parens, no args) is CRuby's NameError -- an undefined local that fell
       through to method lookup. Name the enclosing class the way CRuby does. */
    if (recv < 0 && ac == 0 && nt_ref(c->nt, id, "block") < 0 &&
        nt_int(c->nt, id, "vcall", 0)) {
      const char *cn = self_ci >= 0 ? class_ruby_name(c, self_ci) : NULL;
      snprintf(msg, cap, "undefined local variable or method '%s' for %s%s (NameError)",
               mname, cn ? "an instance of " : "main", cn ? cn : "");
      return CR_NAMEERROR;
    }
    /* A call on a typed user object whose class chain has no such method is
       not a compiler gap: it is the program's NoMethodError, caught ahead of
       time. Report it in CRuby's words instead of the internal node dump. */
    if (recv >= 0) {
      TyKind rvt = comp_ntype(c, recv);
      /* Same for a typed BUILTIN receiver: when CRuby's own surface for that
         class does not carry the name either, the call is the program's
         NoMethodError, proved at compile time rather than left for run time --
         so say so in CRuby's words. A name CRuby *does* have is a spinel gap
         and keeps the internal report (#3715). */
      {
        const char *bcn = rvt == TY_STRING || rvt == TY_STRBUF ? "String"
                        : rvt == TY_INT ? "Integer" : rvt == TY_FLOAT ? "Float"
                        : rvt == TY_SYMBOL ? "Symbol"
                        : rvt == TY_RANGE || rvt == TY_FLOAT_RANGE || rvt == TY_STR_RANGE ? "Range"
                        : rvt == TY_TIME ? "Time"
                        : ty_is_array(rvt) ? "Array" : ty_is_hash(rvt) ? "Hash" : NULL;
        if (bcn && !builtin_method_known(bcn, mname) &&
            !builtin_object_method_known(mname) &&
            !name_is_enumerable_module_method(mname) &&
            !an_user_defines_method(c, mname)) {
          snprintf(msg, cap, "undefined method '%s' for an instance of %s (NoMethodError)", mname, bcn);
          return CR_NOMETHOD;
        }
      }
      /* A Class value no class answers: `searched_model.none` where the method
         returns `Story` or `Comment` and neither defines `self.none`. CRuby
         raises NoMethodError when it is reached; say that rather than the
         node dump. A name Module/Class itself has is a spinel gap instead. */
      if (rvt == TY_CLASS) {
        static const char *const module_surface[] = {
          "new", "allocate", "superclass", "name", "to_s", "inspect", "ancestors",
          "instance_methods", "public_instance_methods", "private_instance_methods",
          "instance_method", "method_defined?", "public_method_defined?",
          "private_method_defined?", "protected_method_defined?", "const_get",
          "const_set", "const_defined?", "constants", "class_variable_get",
          "class_variable_set", "class_variables", "class_eval", "module_eval",
          "class_exec", "module_exec", "include?", "included_modules", "define_method",
          "alias_method", "attr_accessor", "attr_reader", "attr_writer", "subclasses",
          "attached_object", "private_constant", "module_function", "include",
          "extend", "prepend", "remove_method", "undef_method", "<", "<=", ">", ">=",
          "<=>", "==", "===", "hash", "freeze", NULL };
        int ncc = 0;
        comp_cmethod_candidates(c, mname, &ncc);
        if (ncc == 0 && !builtin_object_method_known(mname) && !str_in(mname, module_surface)) {
          snprintf(msg, cap, "undefined method '%s' for a Class: no class in the program defines a class method '%s' (NoMethodError)", mname, mname);
          return CR_NOMETHOD;
        }
      }
      if (ty_is_object(rvt)) {
        int cid = ty_object_class(rvt);
        /* a name every object answers (`send`, `tap`, ...) is not the
           program's NoMethodError: that refusal is a spinel gap, and saying
           "undefined method 'send'" pointed at the wrong thing (#4850) */
        if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_native_class &&
            comp_method_in_chain(c, cid, mname, NULL) < 0 &&
            !builtin_object_method_known(mname)) {
          const char *cn = class_ruby_name(c, cid);
          snprintf(msg, cap, "undefined method '%s' for an instance of %s (NoMethodError)", mname, cn ? cn : "Object");
          return CR_NOMETHOD;
        }
      }
    }
    int n = snprintf(msg, cap, "unsupported %s: node %d (%s `%s`) recv=%s/ty%d argc=%d",
                     what, id, ty, mname,
                     recv >= 0 ? nt_type(c->nt, recv) : "-",
                     recv >= 0 ? (int)comp_ntype(c, recv) : -1, ac);
    if (ac > 0 && av && n > 0 && (size_t)n < cap)
      snprintf(msg + n, cap - (size_t)n, " arg0ty%d", (int)comp_ntype(c, av[0]));
  }
  else
    snprintf(msg, cap, "unsupported %s: node %d (%s)", what, id, ty ? ty : "?");
  return CR_GAP;
}

/* A reader of the call plan's refusal (CP_REFUSE): the plan refuses node id
   by the decision `from`, so codegen raises it in the plan's words (site: the
   --plan-check reader name). Returns when the plan does not. */
void sg_refused_def(Compiler *c, int def) {
  if (def >= 0 && def < c->nt->count && nt_int(c->nt, def, "sg_needs_self_refused", 0))
    unsupported_feature(c, def, "singleton method that needs a self, on a receiver that is not one user-class instance");
}

void refuse_from_plan(Compiler *c, int id, int from, const char *site) {
  const CallPlan *p = cplan_refuse(c, id);
  if (p->dispatch != CP_REFUSE || p->rfrom != from) return;
  if (g_plan_check && !g_unsup_probe) cplan_served(site);
  unsupported_feature(c, id, p->msg);
}

/* A send with a runtime name, a runtime const_get and a binding call are
   refused by passes that run ahead of emission (reject_runtime_send and
   its kin), outside the per-method probe --defer-refusals defers refusals
   in: so the flag still stopped the build of a program that only names one
   (actionpack's polymorphic route helpers), though it never runs the line.
   Under the flag those passes step aside and the same refusal, with the
   same words, is raised here, where the call is emitted. */
void refuse_prepass_at_emit(Compiler *c, int id) {
  /* `...` forwarded to `new` on a class value that could be a Struct or
     Data class while an initialize takes keywords: left to be refused here
     by the `...` desugar, with or without the flag */
  if (nt_int(c->nt, id, "fwd_new_class_value", 0))
    unsupported_feature(c, id, "`...` forwarded to `new` on a class value, where an initialize "
                               "takes keywords and a Struct or Data class could be constructed");
  if (!defer_refusals()) return;
  refuse_from_plan(c, id, CRF_SEND, "refuse-send");
  refuse_from_plan(c, id, CRF_CONST_GET, "refuse-const-get");
  refuse_from_plan(c, id, CRF_BINDING, "refuse-binding");
}

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what) {
  /* Silent emittability probe (dynamic-send arm selection): unwind without a
     diagnostic, the caller just drops this arm. */
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  refuse_at(c, id);
  /* Ruby-map the diagnostic (#1338): a codegen gap reports against the source
     line the parser stamped (the same position the #line machinery uses), so
     the message is anchored to the .rb file instead of an opaque node id.
     Falls back to the bare form when the position wasn't stamped. */
  int ln; const char *file = unsup_pos(c, id, &ln);
  char msg[2400];
  unsup_message(c, id, what, g_emitting_class_id, msg, sizeof msg);
  /* Back to the driver's per-unit recovery point (when armed), abandoning
     this unit's discarded output, so one run surfaces every gap; else out.
     `unsupported` thus never returns. */
  unsup_leave(file, ln, msg);
}

const char *c_type_name(TyKind t) {
  /* a builtin kind's C type is its ty_traits row's (types.c); an object
     array is the pointer array, a user object has none here */
  if (ty_is_obj_array(t)) return "sp_PtrArray *";
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->ctype : NULL;
}
int is_scalar_ret(TyKind t) {
  /* a builtin kind's ty_traits row says (types.c); a user object and an
     object array are pointers a method can return */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->scalar_ret : (ty_is_object(t) || ty_is_obj_array(t));
}
int const_has_flag(const LocalVar *lv) {
  return lv && lv->const_early && (lv->type == TY_NIL || is_scalar_ret(lv->type));
}
void emit_const_flag_set(const LocalVar *lv, const char *key, Buf *b, int indent) {
  if (!const_has_flag(lv)) return;
  emit_indent(b, indent);
  buf_printf(b, "cst_%s__set = 1;\n", key);
}
/* The name CRuby shows for the constant node id reads: a path as written, a
   bare name qualified by the class it is written in. A collision-qualified
   key (Outer__Inner) shows its leaf. */
static void const_read_name(Compiler *c, int id, char *out, size_t cap) {
  const NodeTable *nt = c->nt;
  const char *segs[16]; int n = 0;
  for (int k = id; k >= 0 && n < 16; k = nt_kind(nt, k) == NK_ConstantPathNode ? nt_ref(nt, k, "parent") : -1) {
    /* a receiver that is no constant (a class held in a variable) has no name here */
    const char *nm = k == id || nt_kind(nt, k) == NK_ConstantReadNode || nt_kind(nt, k) == NK_ConstantPathNode
                     ? nt_str(nt, k, "name") : NULL;
    const char *us = nm ? strstr(nm, "__") : NULL;
    while (us && strstr(us + 2, "__")) us = strstr(us + 2, "__");
    if (nm) segs[n++] = us ? us + 2 : nm;
  }
  out[0] = 0;
  const char *cref = nt_kind(nt, id) == NK_ConstantReadNode ? nt_str(nt, id, "const_cref") : NULL;
  if (cref) {
    /* the class the analysis found it written in; "" at the top level */
    if (*cref) snprintf(out, cap, "%s::", cref);
  }
  else if (n == 1 && nt_kind(nt, id) != NK_ConstantPathNode) {
    Scope *s = comp_scope_of(c, id);
    int cls = s && s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cls >= 0 && cls < c->nclasses && class_ruby_name(c, cls)) snprintf(out, cap, "%s::", class_ruby_name(c, cls));
  }
  for (int i = n - 1; i >= 0; i--) {
    size_t l = strlen(out);
    snprintf(out + l, cap - l, "%s%s", segs[i], i ? "::" : "");
  }
}
/* The receiver of constant path node id when it is a value (a class held in
   a variable, a call): -1 for a constant or no receiver. */
static int const_value_recv(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int par = nt_kind(nt, id) == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1;
  if (par < 0 || nt_kind(nt, par) == NK_ConstantReadNode || nt_kind(nt, par) == NK_ConstantPathNode) return -1;
  return comp_ntype(c, par) == TY_CLASS || comp_ntype(c, par) == TY_POLY ? par : -1;
}
/* ... and one whose evaluation is more than a read (a call): the read then
   evaluates it once, into a temp, and names the class in its error by that
   value. A read receiver (a variable, self) is named where the error is raised. */
static int const_recv_held(Compiler *c, int id) {
  int par = const_value_recv(c, id);
  return par >= 0 && !subtree_is_pure_read(c, par) ? par : -1;
}
static void emit_const_recv_class(Compiler *c, int par, Buf *b) {
  if (comp_ntype(c, par) == TY_POLY) { buf_puts(b, "sp_unbox_class("); emit_boxed(c, par, b); buf_puts(b, ")"); }
  else emit_expr(c, par, b);
}
void emit_const_check_open(Compiler *c, int id, const LocalVar *lv, Buf *b) {
  if (!const_has_flag(lv)) return;
  int par = const_value_recv(c, id);
  if (par >= 0) {
    const NodeTable *nt = c->nt;
    const char *leaf = nt_str(nt, id, "name");
    const char *us = leaf ? strstr(leaf, "__") : NULL;
    while (us && strstr(us + 2, "__")) us = strstr(us + 2, "__");
    int t = const_recv_held(c, id) >= 0 ? ++g_tmp : -1;
    if (t >= 0) { buf_printf(b, "({ sp_Class _t%d = ", t); emit_const_recv_class(c, par, b); buf_puts(b, "; "); }
    else buf_puts(b, "(");
    buf_printf(b, "(cst_%s__set ? (void)0 : sp_raise_cls(\"NameError\", sp_sprintf(\"uninitialized constant %%s::%s\", %s(",
               lv->name, us ? us + 2 : leaf, comp_class_display_fn(c));
    if (t >= 0) buf_printf(b, "_t%d", t);
    else emit_const_recv_class(c, par, b);
    buf_puts(b, ")))), ");
    return;
  }
  char nm[256];
  const_read_name(c, id, nm, sizeof nm);
  buf_printf(b, "((cst_%s__set ? (void)0 : sp_raise_cls(\"NameError\", \"uninitialized constant %s\")), ", lv->name, nm);
}
void emit_const_check_close(Compiler *c, int id, const LocalVar *lv, Buf *b) {
  if (!const_has_flag(lv)) return;
  buf_puts(b, const_recv_held(c, id) >= 0 ? "; })" : ")");
}
/* A call on a flagged constant read through a receiver that runs (`pick::BUF
   << "y"`): the receiver and the flag test run once, into a pointer to the
   slot, and the call reads and writes through it (the lowering of a String
   mutator names its receiver twice). Declares the pointer in b and marks the
   receiver node with its temp; the caller emits the call and unmarks. -1
   when the call is no such one. */
static int const_recv_hold(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int r = nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "receiver") : -1;
  if (r < 0 || nt_kind(nt, r) != NK_ConstantPathNode || nt_int(nt, r, "const_ptr", 0)) return -1;
  const char *nm = nt_str(nt, r, "name");
  LocalVar *lv = nm ? comp_const(c, nm) : NULL;
  if (!const_has_flag(lv) || const_recv_held(c, r) < 0) return -1;
  int t = ++g_tmp;
  emit_ctype(c, lv->type, b);
  buf_printf(b, " *_p%d = ", t);
  emit_constant_slot(c, r, b);
  buf_puts(b, "; ");
  nt_node_set_int((NodeTable *)nt, r, "const_ptr", t);
  return r;
}
int emit_const_recv_wrap(Compiler *c, int id, Buf *b) {
  Buf pre; memset(&pre, 0, sizeof pre);
  int r = const_recv_hold(c, id, &pre);
  if (r < 0) { free(pre.p); return 0; }
  buf_puts(b, "({ "); buf_puts(b, pre.p ? pre.p : ""); free(pre.p);
  emit_call(c, id, b);
  nt_node_set_int((NodeTable *)c->nt, r, "const_ptr", 0);
  buf_puts(b, "; })");
  return 1;
}
/* The same for a call that is a statement: the pointer is held in a block of its own. */
int emit_const_recv_stmt_wrap(Compiler *c, int id, Buf *b, int indent, int tail) {
  Buf pre; memset(&pre, 0, sizeof pre);
  int r = const_recv_hold(c, id, &pre);
  if (r < 0) { free(pre.p); return 0; }
  emit_indent(b, indent);
  buf_puts(b, "{ "); buf_puts(b, pre.p ? pre.p : ""); buf_puts(b, "\n"); free(pre.p);
  if (tail) emit_stmt_tail(c, id, b, indent + 1);
  else emit_stmt(c, id, b, indent + 1);
  nt_node_set_int((NodeTable *)c->nt, r, "const_ptr", 0);
  emit_indent(b, indent);
  buf_puts(b, "}\n");
  return 1;
}
/* The plain read of a constant's slot, or its address: behind its test the
   read stays an lvalue (a String mutator assigns to it), `*(test, &cst_NAME)`.
   The text may be emitted again by a caller that repeats it (the holder's), so
   asking it for no receiver (own 0) leaves a variable receiver unevaluated and
   unnamed. */
void emit_const_slot_read(Compiler *c, int id, const LocalVar *lv, const char *key, int slot, int own, Buf *b) {
  if (!const_has_flag(lv)) { buf_printf(b, "%scst_%s", slot ? "&" : "", key); return; }
  int again = !own && const_recv_held(c, id) >= 0;
  if (again) {
    /* the name alone: the receiver is not this text's to run */
    buf_puts(b, slot ? "(" : "(*(");
    buf_printf(b, "(cst_%s__set ? (void)0 : sp_raise_cls(\"NameError\", \"uninitialized constant %s\")), &cst_%s)",
               lv->name, key, key);
    buf_puts(b, slot ? "" : ")");
    return;
  }
  buf_puts(b, slot ? "" : "(*");
  emit_const_check_open(c, id, lv, b);
  buf_printf(b, "&cst_%s", key);
  emit_const_check_close(c, id, lv, b);
  buf_puts(b, slot ? "" : ")");
}
/* native binding (Path B): map a spinel type spec to the C type at the ABI
   boundary. any -> the boxed value; string -> the runtime string; scalars
   pass by value. */
/* Kinds whose C representation is a struct passed BY VALUE (see c_type_name).
   A struct never implicitly converts to or from anything else in C, so a
   parameter and an argument that disagree here can never be the same call --
   unlike the numeric scalars, where int-into-float is an ordinary conversion. */
int ty_is_struct_valued(TyKind t) {
  /* a builtin kind's ty_traits row says (types.c); a user object's
     by-value class is asked of the class (comp_ty_value_obj) */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->struct_valued : 0;
}

const char *native_c_type(const char *spec) {
  if (!spec) return "void";
  if (sp_streq(spec, "any"))    return "sp_RbVal";
  if (sp_streq(spec, "ptr"))    return "void *";   /* raw pointer passthrough */
  if (sp_streq(spec, "string")) return "const char *";
  if (sp_streq(spec, "text"))   return "const char *";   /* write payload: the operand's #to_s */
  if (sp_streq(spec, "string?")) return "const char *";  /* nullable; call site wraps */
  if (sp_streq(spec, "nstring")) return "const char *";   /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return "const char *";   /* borrowed C string; call site dups */
  if (sp_streq(spec, "cbinstr")) return "const char *";  /* borrowed C bytes; call site dups sp_ffi_bin_len of them */
  if (sp_streq(spec, "regexp")) return "mrb_regexp_pattern *";
  if (sp_streq(spec, "int"))    return "sp_int";
  if (sp_streq(spec, "float"))  return "double";
  /* sp_bool, NOT int: a package's C function returns sp_bool (_Bool, one
     byte), so prototyping it here as int is a mismatched declaration. The
     caller then reads a full register where the callee only wrote its low
     byte -- gcc happened to zero the rest, ubuntu clang did not, and
     StringIO.new("x").closed? answered true. */
  if (sp_streq(spec, "bool"))   return "sp_bool";
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return "void";
  return "sp_RbVal";
}

const char *ffi_c_type(const char *spec) {
  const FfiSpecInfo *info = ffi_spec_lookup(spec);
  return info ? info->c_type : "void";
}

/* The C name an ffi_func's extern is declared and called under. The prototype
   is built from the spec types, which need not match a header that also
   declares the symbol (fopen's FILE * is our void *, strchr's char * our
   const char *), and C rejects two declarations of one name with different
   types. So the extern takes a private name per ffi_func and binds it to the
   real symbol with an asm label (SP_FFI_SYM in the emitted prologue): no
   header declaration, function-like macro or second module binding the same
   symbol under other specs can conflict with it. */
void ffi_extern_name(Compiler *c, int fi, Buf *out) {
  const char *sym = c->ffi_funcs[fi].csym ? c->ffi_funcs[fi].csym : c->ffi_funcs[fi].name;
  buf_printf(out, "sp_ffi_f%d_%s", fi, sym);
}

/* The C type of one ffi_callback argument, used to build the trampoline's own
   pointer type. A :ptr callback arg is `const void*` -- the near-universal shape
   of C comparator/visitor callbacks (qsort, bsearch, ...) -- so the generated
   trampoline's type matches the header's declaration exactly (no
   incompatible-function-pointer error). */
const char *ffi_cb_arg_ctype(const char *spec) {
  if (sp_streq(spec, "ptr")) return "const void *";
  return ffi_c_type(spec);
}
/* Write into `out` the C test for "local `en` currently holds nil", and answer
   1. Answers 0 when the slot has no nil representation: every value the type
   can hold is truthy, and its zero is indistinguishable from a real one, so
   `x ||= v` on it genuinely is a no-op.

   The pointer-shaped kinds are exactly the ones declare_local initialises to
   NULL, so a slot still holding NULL has never been assigned -- which is nil,
   not "already truthy". Reading it as truthy is what dropped the assignment in
   `text ||= [...].join(" ")` (#3388). */
/* The initial value of a local's slot: its type's zero, or the type's nil
   when a `||=` writes the local or a read can run before any write
   (#3388). */
const char *local_init_value(Compiler *c, LocalVar *lv) {
  /* an Integer or Float slot that holds its nil starts nil where a read can
     run before any write, its zero otherwise */
  if (slot_is_oint(lv)) {
    int unset = (lv->or_written || lv->maybe_unset) && !lv->is_param && !lv->is_block_param;
    return unset ? oint_nil(lv->type) : lv->type == TY_FLOAT ? "sp_ofloat_of(0.0)" : "sp_oint_of(0)";
  }
  if ((lv->or_written || lv->maybe_unset) && !lv->is_param && !lv->is_block_param) {
    const char *nv = nil_value(lv->type);
    if (nv) return nv;
  }
  /* A value-type object is stored INLINE (sp_X, not sp_X *), so its zero is a
     zeroed struct. default_value answers the blanket "NULL" for every object
     type, which declares `sp_K lv_r = NULL;` -- an invalid initializer, and a
     C build that stops. declare_local has had this arm all along; the inlined
     path reached the shared helper instead. The shape that finds it is the
     resource idiom: `def self.open; r = new; begin; yield r; ensure; r.close;
     end; end` on a class small enough to be a value type. */
  if (comp_ty_value_obj(c, lv->type)) return "{0}";
  return lv->type == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, lv->type);
}
/* A value landing in a slot of type `slot`. An Integer or Float slot that
   also sees nil is a nullable scalar (ty_unify's nil join), an oint that
   holds its nil beside the value: a bare `nil`, or a nil-typed expression
   (a void call, an always-nil method), is spelled as the oint's nil, where
   emit_expr renders the numeric 0 that reads as a real value. Every other
   slot takes emit_expr. */
void emit_expr_slot(Compiler *c, int node, TyKind slot, Buf *b) {
  if (node >= 0 && (slot == TY_INT || slot == TY_FLOAT)) {
    const char *sent = oint_nil(slot);   /* a slot that takes nil is an sp_oint */
    if (nt_kind(c->nt, node) == NK_NilNode) { buf_puts(b, sent); return; }
    TyKind vt = repr_of(c, node).as_ty;
    if (vt == TY_NIL || vt == TY_VOID) {
      buf_puts(b, "({ (void)("); emit_expr(c, node, b); buf_printf(b, "); %s; })", sent);
      return;
    }
  }
  store_check(c, node, slot, "a value into a typed slot", b);
  emit_expr(c, node, b);
}
/* A value written into a typed scalar slot -- an Integer or Float array's
   element, a typed accumulator -- from an expression the inference typed
   boxed: under --int-overflow=promote an arithmetic node whose operands are
   not constants is poly (it may promote), while the slot it lands in stays
   typed (a typed Array does not widen), so the value is converted at the
   sink; a Bignum that does not fit raises there. Any other expression is
   emitted as it is. `text` is the already-rendered expression. */
void emit_typed_sink_text(Compiler *c, int node, TyKind slot, const char *text, Buf *b) {
  Repr vr = repr_of(c, node);
  TyKind vt = vr.as_ty;
  if (vr.kind == RK_BOXED && poly_sink_unbox_fn(slot)) buf_printf(b, "%s(%s)", poly_sink_unbox_fn(slot), text);
  else if (vt == TY_BIGINT && slot == TY_INT) buf_printf(b, "sp_bigint_to_int(%s)", text);
  /* A block's value into a typed element (`fill { ... }`, a collect
     accumulator) is written as it is: where its class differs from the
     element's, the answer is the array widened by the inference, not a
     refusal here. The check reports it. */
  else {
    if (node >= 0 && slot != TY_UNKNOWN) store_check(c, node, slot, "a typed element sink", b);
    buf_puts(b, text);
  }
}
/* ---- The store check (--check-stores) ----

   Every value the emitter writes into a C slot -- a local, a temp, a field,
   an element, an argument, a return -- has a C type, and so does the slot.
   Where the two differ the store has to convert (box, unbox, a numeric
   conversion, a handle wrap), and each emitter decides that for itself. One
   that writes the value raw into a slot of another C type emits C that does
   not build, or, between two pointer types, reads one struct's memory as
   another's. --check-stores reports each such store at its Ruby line, with
   the two C types and the construct, and marks the spot in the C with a
   comment; it changes nothing else in the output. The stores report through
   store_check where they write the value as it is. */

/* The kind of the C value emit_expr renders for `node`: its inferred type --
   for a `yield`, the type at this call site, since the block spliced here
   answers its own (yield_site_type) -- except for the untyped nodes the
   emitter still renders in a definite C type. An empty `[]` with no element
   type is built as the method's array return kind, else as an Integer array
   (the ArrayNode arm of emit_expr), and an empty `{}` as a String-keyed
   boxed-value hash (its HashNode arm). */
TyKind store_value_kind(Compiler *c, int node) {
  if (node < 0) return TY_UNKNOWN;
  TyKind t = yield_site_type(c, node);
  /* a parenthesized value, `case ({})`, is rendered as its one statement */
  if (t == TY_UNKNOWN && nt_kind(c->nt, node) == NK_ParenthesesNode) {
    int in = unwrap_parens(c, node);
    if (in != node) return store_value_kind(c, in);
  }
  NodeKind k = nt_kind(c->nt, node);
  if (t == TY_UNKNOWN && (k == NK_ArrayNode || k == NK_HashNode)) {
    int n = 0;
    nt_arr(c->nt, node, "elements", &n);
    if (n == 0 && k == NK_HashNode) return TY_STR_POLY_HASH;
    if (n == 0) return ty_is_array(g_ret_type) && array_kind(g_ret_type) ? g_ret_type : TY_INT_ARRAY;
  }
  return t;
}

/* The C value class of a kind, whether a store fits as it is: repr.c
   (repr_store_class, repr_store_fits) */
#define store_class(c, t) repr_store_class((c), (t))
int store_fits(Compiler *c, TyKind from, TyKind to) { return repr_store_fits(c, from, to); }

/* Report the raw store of `node` (rendered as a `from` value) into a slot of
   kind `slot`, when it does not fit: once per node and site, on stderr at the
   node's Ruby line, and as a comment in `b` where the value is about to be
   written. A silent emittability probe's output is thrown away, so it
   reports nothing. */
void store_check_kind(Compiler *c, int node, TyKind from, TyKind slot, const char *what, Buf *b) {
  if (!g_check_stores || g_unsup_probe || store_fits(c, from, slot)) return;
  /* a nil literal is 0 as it is written: NULL, false or a carrier slot's
     zero, which no C compiler rejects; only another nil value is reported */
  if (from == TY_NIL && node >= 0 && nt_kind(c->nt, node) == NK_NilNode &&
      (store_class(c, slot) == SC_PTR || store_class(c, slot) == SC_ARITH)) return;
  static int *seen = NULL;
  static const char **seen_what = NULL;
  static int nseen = 0, capseen = 0;
  for (int k = 0; k < nseen; k++)
    if (seen[k] == node && seen_what[k] == what) goto mark;
  if (nseen == capseen) {
    capseen = capseen ? capseen * 2 : 64;
    seen = realloc(seen, sizeof *seen * (size_t)capseen);
    seen_what = realloc(seen_what, sizeof *seen_what * (size_t)capseen);
  }
  seen[nseen] = node; seen_what[nseen] = what; nseen++;
  {
    Buf fb, tb;
    memset(&fb, 0, sizeof fb); memset(&tb, 0, sizeof tb);
    emit_ctype(c, from, &fb); emit_ctype(c, slot, &tb);
    int ln; const char *file = unsup_pos(c, node, &ln);
    const char *fk = ty_is_object(from) ? class_ruby_name(c, ty_object_class(from)) : ty_name(from);
    fprintf(stderr, "spinel: %s:%d: warning: store check: %s: %s value (%s, %s) written as it is into a slot of %s\n",
            file, ln, what, fk ? fk : "?", fb.p ? fb.p : "void", nt_type(c->nt, node), tb.p ? tb.p : "void");
    free(fb.p); free(tb.p);
  }
mark:
  if (b) buf_printf(b, "/* store check: %s */", what);
}

void store_check(Compiler *c, int node, TyKind slot, const char *what, Buf *b) {
  if (!g_check_stores) return;
  store_check_kind(c, node, store_value_kind(c, node), slot, what, b);
}

/* ---- emit_coerce: a value into a typed slot ----

   The one place a store whose value may not fit its slot converts it, or
   refuses the program. A value that fits (store_fits) is written as it is,
   so a store that was right already emits the C it did. Otherwise the
   conversion depends on what the slot is to Ruby:

   CO_HOLD     the slot holds the Ruby value itself -- a Complex component,
               a Rational's numerator, a receiver -- so only its C
               representation may change (an Integer widens into a
               Bignum slot), never its class or its value;
   CO_CONVERT  the slot is a conversion Ruby makes itself -- the Float
               operand of a Float method, a duration -- so the value
               converts as Ruby converts it: an Integer past 64 bits or a
               Rational to its nearest double, any value to its truthiness
               for a boolean flag.

   A store no conversion keeps right is refused at compile time, naming the
   construct (`what`), the class it was given and the slot's C type. That is
   the rule of #6179: what Spinel compiles works, or it is refused; it never
   emits C that does not build, or a store that reads the wrong value. */
/* --repr-check (R6): each store emit_coerce makes records its form
   (CoerceForm), compared with repr_coerce_form's prediction. A store
   emit_coerce_text makes for emit_coerce's last arm hands its form up; one
   it makes on its own is compared with repr_coerce_text_form's. */
static int rcc_depth;
static int rcc_text_form = -1;
static void rcc_note(Compiler *c, int node, TyKind from, TyKind slot, int how, int form, int text) {
  if (!g_repr_check || form < 0) return;
  int want = text ? repr_coerce_text_form(c, node, from, slot, how) : repr_coerce_form(c, node, slot, how);
  if (want == form) return;
  const char *nty = node >= 0 ? nt_type(c->nt, node) : NULL;
  fprintf(stderr, "repr-check: %s: node %d %s %s->%s: emitted %s, predicted %s\n",
          view_depth() > 0 ? "coerce-view" : "coerce-conflict", node, nty ? nty : "?",
          ty_name(text ? from : store_value_kind(c, node)), ty_name(slot),
          repr_coerce_form_name(form), repr_coerce_form_name(want));
}
#define RCC(form) rcc_note(c, node, TY_UNKNOWN, slot, how, (form), 0)
#define RCCT(form) do { if (g_repr_check) { rcc_text_form = (form); \
    if (rcc_depth == 0) rcc_note(c, node, from, slot, how, (form), 1); } } while (0)

void emit_coerce_text(Compiler *c, int node, TyKind from, TyKind slot, int how,
                      const char *text, const char *what, Buf *b) {
  /* the form is repr_coerce_text_form's (repr.c) */
  switch (repr_coerce_text_form(c, node, from, slot, how)) {
  case CF_FIT:
    buf_puts(b, text);
    RCCT(CF_FIT);
    return;
  case CF_BOX:
    /* (a String into a boxed slot the rule shares: emit_boxed's lift) */
    if (repr_share_rule(c) && node >= 0 && c->poly_strbuf_lift[node] && from == TY_STRING) {
      buf_puts(b, "sp_poly_strbuf_lift(");
      emit_boxed_text(c, from, text, b);
      buf_puts(b, ")");
    }
    else emit_boxed_text(c, from, text, b);
    RCCT(CF_BOX); return;
  case CF_NIL_SENT:
    /* A value with no C type of its own -- a call that answers nothing, a
       raise -- is evaluated for its effect, and the slot takes its nil */
    buf_printf(b, "((void)(%s), %s)", text, raise_tail_value_c(c, slot));
    RCCT(CF_NIL_SENT);
    return;
  case CF_INT2BIG:
    buf_printf(b, "sp_bigint_new_int(%s)", text);
    RCCT(CF_INT2BIG);
    return;
  case CF_CONVERT:
    /* a Bignum or a Rational operand a Float slot converts, as Ruby does */
    buf_printf(b, "%s(%s)", from == TY_BIGINT ? "sp_bigint_to_double" : "sp_rational_to_f", text);
    RCCT(CF_CONVERT);
    return;
  default:
    break;
  }
  char msg[512];
  Buf tb; memset(&tb, 0, sizeof tb);
  emit_ctype(c, slot, &tb);
  const char *cn = from == TY_BIGINT ? "an Integer past 64 bits"
                 : from == TY_RATIONAL ? "a Rational" : from == TY_COMPLEX ? "a Complex"
                 : from == TY_EXCEPTION ? "an Exception" : from == TY_POLY ? "a value of any class"
                 : from == TY_BOOL ? "true or false" : NULL;
  const char *rn = cn ? NULL : conv_cls_name_of(c, from);
  snprintf(msg, sizeof msg, "%s given %s%s, which no conversion keeps in its %s slot",
           what, rn ? "a " : "", cn ? cn : rn ? rn : "a value of another class",
           tb.p ? tb.p : "C");
  free(tb.p);
  unsupported_feature(c, node, msg);
}

void emit_coerce(Compiler *c, int node, TyKind slot, int how, const char *what, Buf *b) {
  /* repr_coerce_plan decides the form (repr.c), in this order: a boolean
     flag's truthiness, an untyped empty container built at the slot's kind,
     a value that fits written as it is (store_fits first, as cheap as it
     was: the hot path), a boxed slot, an empty literal of another kind, a
     nil literal as the slot's nil, an Integer widened into a Bignum, a
     boxed value unboxed, and the conversions emit_coerce_text makes or
     refuses. */
  TyKind from = TY_UNKNOWN;
  if (oint_kind(slot) && node_may_be_nil(c, node)) refuse_nil_store(c, node, slot, what);
  int plan = repr_coerce_plan(c, node, slot, how, &from);
  switch (plan) {
  case CF_FIT:
    emit_expr(c, node, b);
    RCC(CF_FIT);
    return;
  case CF_CONVERT:
    /* A boolean a builtin takes as a flag (`report_on_exception = v`) is the
       value's truthiness, whatever its class: nil and false are false, 0 and
       "" are true (emit_cond) */
    if (how == CO_CONVERT && slot == TY_BOOL) { emit_cond(c, node, b); RCC(CF_CONVERT); return; }
    break;
  case CF_EMPTY_LIT:
    /* An untyped empty container (a bare Array.new / Hash.new) is built at
       the slot's kind ahead of the fit, which an untyped value always
       passes: the bare `Array.new` went into a Float array slot as the
       general Array it renders as; an empty `[]` or `{}` of another kind
       than the slot's is built at the slot's */
    if (emit_empty_literal_as(c, node, slot, b)) { RCC(CF_EMPTY_LIT); return; }
    break;
  case CF_BOX:
    /* A boxed slot takes any value boxed, as it is */
    if (slot == TY_POLY) { emit_boxed(c, node, b); RCC(CF_BOX); return; }
    break;
  case CF_NIL_SENT:
    /* nil literal into a slot with a nil of its own: the slot's nil itself
       (an Integer or Float slot here refuses it) */
    if (from == TY_NIL && nt_kind(c->nt, node) == NK_NilNode) {
      if (oint_kind(slot)) refuse_nil_store(c, node, slot, what);
      buf_puts(b, raise_tail_value_c(c, slot)); RCC(CF_NIL_SENT); return;
    }
    break;
  case CF_INT2BIG:
    /* An Integer into a Bignum slot is the same Ruby value in the wide
       representation, its nil flag kept as nil (emit_bigint_operand) */
    emit_bigint_operand_ext(c, node, b); RCC(CF_INT2BIG); return;
  case CF_POLY_RHS:
    /* A boxed value into a typed slot is unboxed, as the plain writes unbox
       it: a scalar or a String through its conversion (emit_poly_rhs_coerced,
       nil kept as the slot's nil) */
    if (emit_poly_rhs_coerced(c, slot, node, b)) { RCC(CF_POLY_RHS); return; }
    break;
  case CF_CHECKED_UNBOX: {
    /* a container, an object or a Bignum through the checked unbox, which
       converts or raises for a value of another class rather than reading
       its memory, and a Class from its boxed form. A struct-valued or other
       handle slot has no checked unbox, and is refused below. */
    Buf vb; memset(&vb, 0, sizeof vb);
    emit_expr(c, node, &vb);
    emit_unbox_nilable_text(c, slot, vb.p ? vb.p : "sp_box_nil()", b);
    free(vb.p);
    RCC(CF_CHECKED_UNBOX);
    return;
  }
  case CF_STRBUF_HANDLE:
    /* --share-strings: a String into a shared handle slot is the handle the
       value is (a variable that is one) or a fresh handle around it; a
       variable that is no handle is refused rather than copied
       (emit_strbuf_ivar_store) */
    emit_strbuf_ivar_store(c, 1, node, b);
    RCC(CF_STRBUF_HANDLE);
    return;
  default:
    break;
  }
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, node, &vb);
  rcc_depth++;
  emit_coerce_text(c, node, from, slot, how, vb.p ? vb.p : "", what, b);
  rcc_depth--;
  free(vb.p);
  RCC(rcc_text_form);
}

int local_nil_test(Compiler *c, LocalVar *lv, const char *ref, Buf *out) {
  if (!lv) return 0;
  TyKind t = lv->type;
  /* sp_int 0 and 0.0 are real values, so the slot only distinguishes nil when
     it is an oint declared nil -- which declare_local does exactly when
     a `||=` writes the local (or_written). */
  int nil_init = (lv->or_written || lv->maybe_unset) && !lv->is_param && !lv->is_block_param;
  /* ...or when some write leaves nil in it (`a = nil; a ||= 10`): the nil
     join keeps such a slot an Integer, and its nil is the oint's nil flag */
  if (lv->nullable_int) nil_init = 1;
  if (t == TY_INT || t == TY_FLOAT) {
    if (!nil_init || !slot_is_oint(lv)) return 0;
    buf_printf(out, "%s.nil", ref);
    return 1;
  }
  /* any other builtin kind: its ty_traits row's nil_test_local (types.c),
     NULL for a value kind with no in-band nil (and for POLY, BOOL and
     SYMBOL, whose callers test their own sentinel before reaching here) */
  const TyTraits *tr = ty_traits_of(t);
  if (tr) {
    if (!tr->nil_test_local) return 0;
    ty_traits_render(tr->nil_test_local, ref, out);
    return 1;
  }
  /* a user object is a pointer whose nil is NULL, unless its class is a
     value type; an object array is a pointer too */
  if (comp_ty_value_obj(c, t)) return 0;
  if (is_scalar_ret(t)) { buf_printf(out, "!%s", ref); return 1; }
  return 0;
}
/* The dead value closing a `({ ...; sp_raise_cls(...); V; })` arm. The raise
   never returns, so V only has to type-check in the slot: an UNKNOWN result
   flows as poly (default_value's "0" would not assign to sp_RbVal), and a
   Range wants its brace form. */
const char *raise_tail_value(TyKind t) {
  /* a builtin kind's ty_traits row's zero_tail (types.c): its zero, and a
     boxed nil for an untyped value; a user object's zero otherwise */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->zero_tail : default_value(t);
}

/* Write a ty_traits rendering (types.h) with `expr` for $e and, for $t, the
   one fresh temp the form takes, numbered on its first use. */
void ty_traits_render(const char *cell, const char *expr, Buf *b) {
  int tb = 0;
  for (const char *p = cell; *p; p++) {
    if (p[0] == '$' && p[1] == 'e') { buf_puts(b, expr); p++; continue; }
    if (p[0] == '$' && p[1] == 't') { if (!tb) tb = ++g_tmp; buf_printf(b, "%d", tb); p++; continue; }
    buf_printf(b, "%c", *p);
  }
}

/* Compiler-aware form: a by-value object class's C representation is a bare
   struct, where default_value's NULL would be ill-typed C. */
/* The TypeError Array#* raises for a count that is neither a String (join)
   nor convertible to an Integer (repeat), or NULL when the argument's type
   may be either. */
const char *array_times_type_error(TyKind at) {
  if (at == TY_NIL) return "no implicit conversion from nil to integer";
  if (ty_is_array(at) || ty_is_obj_array(at)) return "no implicit conversion of Array into Integer";
  if (ty_is_hash(at)) return "no implicit conversion of Hash into Integer";
  if (at == TY_SYMBOL) return "no implicit conversion of Symbol into Integer";
  if (at == TY_RANGE || at == TY_FLOAT_RANGE || at == TY_STR_RANGE)
    return "no implicit conversion of Range into Integer";
  return NULL;
}

const char *raise_tail_value_c(Compiler *c, TyKind t) {
  /* a value-type object's zero is its struct's (default_value_from_compiler) */
  if (ty_is_object(t) && comp_ty_value_obj(c, t)) return default_value_from_compiler(c, t);
  return raise_tail_value(t);
}

/* The value a typed slot holds when Ruby's answer is nil: a declared but
   unassigned local, a `next` with no value, an if with no else, a case no
   arm matched, a bare return. For an Integer or a Float that is its oint's
   nil, since the nil join of ty_unify makes such a slot a nullable scalar
   that holds its nil beside the value; 0 read as a truthy number there
   (the String's NULL was its nil already). A counter an emitter starts at
   zero writes the literal. */
/* default_value, for a caller that holds the Compiler: a value-type object is
   a struct, so its nil slot is the zeroed struct, where a pointer object's is
   NULL. default_value itself cannot tell the two apart from the TyKind alone. */
const char *default_value_from_compiler(Compiler *c, TyKind t) {
  int cid = ty_is_object(t) ? ty_object_class(t) : -1;
  if (cid >= 0 && cid < c->nclasses && comp_ty_value_obj(c, t)) {
    /* rotate: one static buffer would make two of these in a single
       buf_printf read the same text, and nothing in the signature says so */
    static char buf[4][128];
    static int slot;
    char *out = buf[slot++ & 3];
    snprintf(out, sizeof buf[0], "(sp_%s){0}", c->classes[cid].c_name);
    return out;
  }
  return default_value(t);
}

const char *default_value(TyKind t) {
  /* a builtin kind's zero is its ty_traits row's (types.c); a Hash, a user
     object and an object array are pointers, NULL */
  const TyTraits *tr = ty_traits_of(t);
  if (tr) return tr->zero;
  return (ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t)) ? "NULL" : "0";
}
/* An Integer node that can never hold nil, by its shape alone: no
   analysis is trusted (the nil analysis misses some sources, and the nil
   tests the emitters used to make unconditionally were what hid
   that). A literal; the result of an arithmetic or bitwise operator, which
   raises on a nil operand and otherwise answers a number; a local whose every
   write is one of those. Printing it, testing it for nil or truthiness,
   converting it or asking it a predicate then needs no nil test:
   a plain Integer that equals INTPTR_MIN is that number (-2**63 in wrap mode,
   a bitboard's top square), not nil (#7612). A parameter, a return, an ivar,
   an element or any other source keeps the tests. Under
   --int-overflow=promote every Integer read is asked (repr_nil_scalar). */
static const char *const plain_int_ops[] = {
  "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", NULL
};
static int plain_int_op(Compiler *c, const char *nm) {
  for (int i = 0; plain_int_ops[i]; i++)
    if (!strcmp(nm, plain_int_ops[i])) {
      /* a program that reopens Integer with the operator owns its answer */
      int ci = comp_class_index(c, "Integer");
      return ci < 0 || comp_method_in_chain(c, ci, nm, NULL) < 0;
    }
  return 0;
}
typedef struct { int node; } PlainWrite;
static PlainWrite *pw_list; static int pw_n, pw_cap;
static const NodeTable *pw_nt; static int pw_count = -1;
static int plain_expr(Compiler *c, int n, int depth);
/* every local write of the program, once per node table */
static void plain_writes_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (pw_nt == nt && pw_count == nt->count) return;
  pw_n = 0;
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_LocalVariableWriteNode && k != NK_LocalVariableOperatorWriteNode &&
        k != NK_LocalVariableOrWriteNode && k != NK_LocalVariableAndWriteNode &&
        k != NK_LocalVariableTargetNode) continue;
    if (pw_n == pw_cap) { pw_cap = pw_cap ? pw_cap * 2 : 1024; pw_list = realloc(pw_list, sizeof *pw_list * (size_t)pw_cap); }
    pw_list[pw_n++].node = id;
  }
  pw_nt = nt; pw_count = nt->count;
}
static int plain_local(Compiler *c, LocalVar *lv, const char *name, int depth) {
  if (!lv || lv->type != TY_INT || lv->is_param || lv->is_block_param || lv->rbs_seeded ||
      lv->nullable_int || lv->box_nullable || lv->maybe_unset || lv->or_written || lv->is_cell || g_promote_mode)
    return 0;
  if (lv->plain_int == 2) return 1;
  if (lv->plain_int == 3) return 0;
  if (lv->plain_int == 1) return 1;   /* a write that reads the local itself (x = x + 1): the others decide */
  if (depth > 8) return 0;
  lv->plain_int = 1;
  plain_writes_build(c);
  const NodeTable *nt = c->nt;
  int ok = 1, saw = 0;
  for (int i = 0; i < pw_n && ok; i++) {
    int w = pw_list[i].node;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || strcmp(wn, name)) continue;
    Scope *ws = comp_scope_of(c, w);
    if (!ws || scope_local(ws, wn) != lv) continue;
    saw = 1;
    NodeKind k = nt_kind(nt, w);
    if (k == NK_LocalVariableWriteNode) ok = plain_expr(c, nt_ref(nt, w, "value"), depth + 1);
    else if (k == NK_LocalVariableOperatorWriteNode) {
      const char *op = nt_str(nt, w, "binary_operator");
      ok = op && plain_int_op(c, op) && comp_ntype(c, w) == TY_INT;
    }
    else ok = 0;
  }
  lv->plain_int = (ok && saw) ? 2 : 3;
  return lv->plain_int == 2;
}
static int plain_expr(Compiler *c, int n, int depth) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  n = unwrap_parens(c, n);
  if (n < 0 || comp_ntype(c, n) != TY_INT) return 0;
  switch (nt_kind(nt, n)) {
  case NK_IntegerNode: return 1;
  case NK_LocalVariableReadNode: {
    const char *nm = nt_str(nt, n, "name");
    Scope *s = nm ? comp_scope_of(c, n) : NULL;
    return nm && s && plain_local(c, scope_local(s, nm), nm, depth);
  }
  case NK_LocalVariableWriteNode: return plain_expr(c, nt_ref(nt, n, "value"), depth + 1);
  /* `c ? a : b` and if/else: plain when every branch's last value is, and
     there is an else (a missing one answers nil) */
  case NK_IfNode: case NK_ElseNode: case NK_StatementsNode: {
    NodeKind k = nt_kind(nt, n);
    int st = k == NK_StatementsNode ? n : nt_ref(nt, n, "statements");
    int sub = k == NK_IfNode ? nt_ref(nt, n, "subsequent") : -1;
    if (k == NK_IfNode && sub < 0) return 0;
    int bn = 0; const int *body = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
    if (bn < 1 || !plain_expr(c, body[bn - 1], depth + 1)) return 0;
    return k != NK_IfNode || plain_expr(c, sub, depth + 1);
  }
  /* `a || b` and `a && b` answer one of their operands: plain when both are */
  case NK_OrNode: case NK_AndNode:
    return plain_expr(c, nt_ref(nt, n, "left"), depth + 1) && plain_expr(c, nt_ref(nt, n, "right"), depth + 1);
  case NK_CallNode: {
    const char *nm = nt_str(nt, n, "name");
    int recv = nt_ref(nt, n, "receiver");
    /* An Integer sum either returns a number or raises, even at INTPTR_MIN. */
    if (nm && recv >= 0 && is_sum_name(nm) && ty_is_array(repr_of(c, recv).as_ty) &&
        !call_is_safe_nav(nt, n) && cplan_user(c, n)->dispatch == CP_NONE) return 1;
    if (!nm || recv < 0 || nt_ref(nt, n, "block") >= 0) return 0;
    if (call_is_safe_nav(nt, n)) return 0;   /* nil&.+(1) is nil */
    int args = nt_ref(nt, n, "arguments"), argc = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    /* a collection's or String's size: a count, never nil (unless the program
       reopens the class with its own) */
    if (argc == 0 && (!strcmp(nm, "size") || !strcmp(nm, "length") || !strcmp(nm, "bytesize"))) {
      TyKind rk = comp_ntype(c, recv);
      const char *cn = rk == TY_STRING ? "String" : ty_is_array(rk) ? "Array" : ty_is_hash(rk) ? "Hash" : NULL;
      if (!cn) return 0;
      int ci = comp_class_index(c, cn);
      return ci < 0 || comp_method_in_chain(c, ci, nm, NULL) < 0;
    }
    /* under promote the receiver of an Integer ~ is boxed, and ~ still
       answers a number */
    if (comp_ntype(c, recv) != TY_INT &&
        !(g_promote_mode && argc == 0 && !strcmp(nm, "~") && comp_ntype(c, n) == TY_INT)) return 0;
    if (argc == 0 && (!strcmp(nm, "~") || !strcmp(nm, "-@") || !strcmp(nm, "+@"))) {
      int ci = comp_class_index(c, "Integer");
      if (ci >= 0 && comp_method_in_chain(c, ci, nm, NULL) >= 0) return 0;
      /* ~ of any word is a number; + and - hand a nil operand's word on
         (+nil raises in CRuby, and in a C int is the identity), so they
         are plain when their operand is */
      return !strcmp(nm, "~") || plain_expr(c, recv, depth + 1);
    }
    /* an Integer's own conversion and neighbours: to_i / to_int hand the word
       on (plain when the receiver is); abs, succ and pred compute a number */
    if (argc == 0 && (!strcmp(nm, "to_i") || !strcmp(nm, "to_int") || !strcmp(nm, "abs") ||
                      !strcmp(nm, "succ") || !strcmp(nm, "pred"))) {
      int ci = comp_class_index(c, "Integer");
      if (ci >= 0 && comp_method_in_chain(c, ci, nm, NULL) >= 0) return 0;
      return nm[0] == 't' ? plain_expr(c, recv, depth + 1) : 1;
    }
    return plain_int_op(c, nm) && argc == 1 && comp_ntype(c, argv[0]) == TY_INT;
  }
  default: return 0;
  }
}
/* a local (written under `name`) that never holds nil, as int_value_plain
   asks of its reads: for the target of `x += 1` */
int int_local_plain(Compiler *c, LocalVar *lv, const char *name) {
  if (g_promote_mode) return 0;
  return plain_local(c, lv, name, 0);
}
int int_value_plain(Compiler *c, int node) {
  if (g_promote_mode) return 0;
  return plain_expr(c, node, 0);
}
/* the shape alone under --int-overflow=promote, where every Integer slot is
   boxed: a plain value boxed into one is the number even at INTPTR_MIN */
int int_value_plain_promote(Compiler *c, int node) {
  return g_promote_mode && plain_expr(c, node, 0);
}
/* Ruby truthiness of a slot `ref` of type `t`, as a C condition: the scalar
   kinds hold nil in a form C does not read as false (an Integer or Float
   oint's nil flag, a Symbol's (sp_sym)-1). */
void emit_slot_truthy(TyKind t, const char *ref, Buf *b) {
  switch (t) {
  /* an Integer or Float `ref` is its sp_oint (emit_sentinel_bind) */
  case TY_INT: case TY_FLOAT: buf_printf(b, "(!(%s).nil)", ref); break;
  case TY_SYMBOL: buf_printf(b, "(%s != (sp_sym)-1)", ref); break;
  case TY_POLY:   buf_printf(b, "(sp_poly_truthy(%s))", ref); break;
  default:        buf_printf(b, "(%s)", ref); break;
  }
}
/* Hold a nullable Integer or Float operand in a fresh temp -- `sp_oint _tN =
   <node>; ` -- so a read that has to ask for its nil (emit_slot_truthy)
   evaluates it once. `ref` receives the temp's name; the caller opens and
   closes the block or statement expression around it. */
void emit_sentinel_bind(Compiler *c, TyKind t, int node, char *ref, size_t cap, Buf *b) {
  snprintf(ref, cap, "_t%d", ++g_tmp);
  if (oint_kind(t)) { buf_printf(b, "%s %s = ", oint_ctype(t), ref); emit_oint_expr(c, node, t, b); }
  else { emit_ctype(c, t, b); buf_printf(b, " %s = ", ref); emit_expr(c, node, b); }
  buf_puts(b, "; ");
}
/* The box for an element an Integer or Float array hands to a poly container
   (a zip or product row, a splat into a mixed literal or a rest parameter, a
   lazy stream): sp_box_int_nf / sp_box_float_nf, whose first argument is the
   array's nilbits, read once ahead of the loop by the caller. Where an
   element's nil bit is set it boxes as the nil it is; an array a computed
   index wrote past the end holds one analyze cannot see, so the static mark
   no longer decides. */
const char *typed_elem_box_fn(TyKind t) {
  return t == TY_INT_ARRAY ? "sp_box_int_nf" : "sp_box_float_nf";
}
/* The store an Integer or Float array (`k`, "Int" / "Float") takes the value
   `node` with: "_nilable" where a nil can land -- a literal nil, a boxed
   value (whose nil converts to the element's nil bit), an element a builtin
   copies, or a value with an oint form -- so the store takes an sp_oint and
   keeps the array's nil bitmap in step; "" for everything else, a plain
   value that is never nil. Any other kind of array has no flag. */
const char *nil_store_sfx(Compiler *c, const char *k, int node) {
  if (!k || (!sp_streq(k, "Int") && !sp_streq(k, "Float"))) return "";
  if (node == NIL_STORE_BOXED) return "_nilable";   /* a boxed element, converted */
  if (node < 0) return "";
  Repr r = repr_of(c, node);
  TyKind t = r.as_ty;
  if (t == TY_NIL || r.kind == RK_BOXED || t == TY_UNKNOWN) return "_nilable";
  if (t != TY_INT && t != TY_FLOAT) return "";
  /* a value with an oint form stores with its nil (the _nilable store takes
     an sp_oint: emit_elem_store_value) */
  return node_has_oint_form(c, node) ? "_nilable" : "";
}
/* The value node `node` stored into an Integer / Float array (kind k) as
   the store nil_store_sfx picked takes it: an sp_oint for the `_nilable`
   store, the plain element otherwise. */
void emit_elem_store_value(Compiler *c, const char *k, int node, Buf *b) {
  TyKind et = sp_streq(k, "Float") ? TY_FLOAT : TY_INT;
  /* a boxed value is checked against the element's kind: nil is the kind's
     nil, anything else foreign is refused (sp_poly_elem_i / _f, #4481) */
  Repr sr = node >= 0 ? repr_of(c, node) : (Repr){0};
  if (node >= 0 && (sr.kind == RK_BOXED || sr.as_ty == TY_POLY)) {
    buf_puts(b, et == TY_FLOAT ? "sp_poly_elem_f(" : "sp_poly_elem_i("); emit_boxed(c, node, b); buf_puts(b, ")");
    return;
  }
  if (nil_store_sfx(c, k, node)[0]) emit_oint_expr(c, node, et, b);
  else emit_coerce(c, node, et, CO_HOLD, "an Array element", b);
}
/* The C text asking whether the Integer or Float array `arr` (C text; `node`
   its Ruby expression, of kind `t`) may hold nil: "1" where analyze marked
   the array, else whether it has a nil bitmap at all (SP_MAY_NIL). */
void emit_may_nil_text(Compiler *c, int node, TyKind t, const char *arr, Buf *b) {
  if (node >= 0 && nullable_int_elem_array(c, node)) buf_puts(b, "1");
  else buf_printf(b, "sp_%sArray_may_nil(%s)", t == TY_INT_ARRAY ? "Int" : "Float", arr);
}
/* An array literal of Integers or Floats none of which can be nil at run
   time (no splat, no element nil_store_sfx would flag): it is born without
   may_nil, so a copy of it (`[0] * 8192`) has nothing to hand on. */
int typed_array_lit_flag_free(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node >= 0) node = unwrap_parens(c, node);
  if (node < 0 || nt_kind(nt, node) != NK_ArrayNode) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  for (int k = 0; els && k < en; k++) {
    TyKind t = comp_ntype(c, els[k]);
    if (nt_kind(nt, els[k]) == NK_SplatNode || (t != TY_INT && t != TY_FLOAT) || *nil_store_sfx(c, "Int", els[k]))
      return 0;
  }
  return 1;
}
/* A node in one of the Ruby builtins (builtins/enumerable.rb: take_while,
   partition, flat_map, ...). They copy their receiver's elements one by one,
   and an element read is typed as a number even where the receiver holds nil
   (a gap a computed index wrote past the end), so their stores take the
   flag-setting form and their boxes the _or_nil one, whatever the element. */
/* The methods Kernel defines private (CRuby's Kernel.private_instance_methods;
   lib/spinel_rt.h keeps the runtime's copy): an explicit receiver that does not
   define one gets "private method 'select' called for ...", not "undefined
   method". Answers the format's head, with the name and `for` in place. */
const char *nomethod_head(const char *name) {
  static const char *const k[] = { "Array", "Complex", "Float", "Hash", "Integer", "Rational", "String", "abort", "at_exit", "autoload", "autoload?", "binding", "block_given?", "caller", "caller_locations", "catch", "eval", "exec", "exit", "exit!", "fail", "fork", "format", "gets", "global_variables", "iterator?", "lambda", "load", "local_variables", "loop", "open", "p", "pp", "print", "printf", "proc", "putc", "puts", "raise", "rand", "readline", "readlines", "require", "require_relative", "select", "set_trace_func", "sleep", "spawn", "sprintf", "srand", "syscall", "system", "test", "throw", "trace_var", "trap", "untrace_var", "warn", NULL };
  for (int i = 0; name && k[i]; i++) if (sp_streq(name, k[i])) return "private method '%s' called for";
  return "undefined method '%s' for";
}
int enum_builtin_node(Compiler *c, int node) {
  Scope *s = node >= 0 ? comp_scope_of(c, node) : NULL;
  return s && s->name && !strncmp(s->name, "__enum_", 7);
}
/* The walk of a builtin Enumerable definition's receiver (call `id` in a
   per-site copy, reporting `name`'s walk name), held in _t<t>: a String,
   Symbol, Integer or Float whose class defines that method at another
   count (String#partition takes one argument, Enumerable's none) is
   CRuby's ArgumentError for the count the call the copy serves passed,
   read off the instance arity table, where the walk raised NoMethodError.
   Tested only for a receiver that is no object, which the walk would
   reject anyway. */
void emit_walk_arity_raise(Compiler *c, int id, int recv, const char *name, int t, int indent, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *wn = enum_walk_name(c, id, recv, name);
  if (wn == name) return;
  Scope *s = comp_scope_of(c, id);
  int site = s && s->def_node >= 0 ? (int)nt_int(nt, s->def_node, "enum_site", -1) : -1;
  if (site < 0 || nt_kind(nt, site) != NK_CallNode) return;
  int a = nt_ref(nt, site, "arguments"), an = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  /* the call rewritten onto the copy passes its receiver first */
  const char *sn = nt_str(nt, site, "name");
  if (sn && strncmp(sn, "__enum_", 7) == 0 && nt_ref(nt, site, "receiver") < 0) {
    if (an < 1) return;
    av++; an--;
  }
  for (int i = 0; i < an; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if (k == NK_SplatNode || k == NK_KeywordHashNode || k == NK_BlockArgumentNode) return;
  }
  int has_blk = nt_ref(nt, site, "block") >= 0;
  const struct { const char *cls, *test; } kinds[] = {
    { "String", "_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)" },
    { "Symbol", "_t%d.tag == SP_TAG_SYM" },
    { "Integer", "_t%d.tag == SP_TAG_INT || _t%d.tag == SP_TAG_BIGINT" },
    { "Float", "_t%d.tag == SP_TAG_FLT" },
  };
  Buf g; memset(&g, 0, sizeof g);
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    char exp[64];
    if (!builtin_arity_expected(kinds[i].cls, wn, has_blk, an, exp, sizeof exp)) continue;
    char test[160];
    snprintf(test, sizeof test, kinds[i].test, t, t);   /* a test names the temp once or twice */
    buf_printf(&g, " if (%s) sp_raise_cls(\"ArgumentError\", "
                   "\"wrong number of arguments (given %d, expected %s)\");", test, an, exp);
  }
  /* behind the walk's own rejection test, once per call before the loop:
     a receiver that walks (an object other than a String's handle) pays
     only that test */
  if (g.p) {
    emit_indent(b, indent);
    buf_printf(b, "if (SP_UNLIKELY(_t%d.tag != SP_TAG_OBJ || sp_poly_is_strbuf(_t%d))) {%s }\n", t, t, g.p);
  }
  free(g.p);
}
/* The name a walk over a value that is no collection reports. Inside one of
   builtins/enumerable.rb's definitions (`v.minmax` is `__enum_minmax(v)`),
   the walk of its own receiver is that method's: CRuby says
   "undefined method 'minmax' for an instance of Integer", where the walk
   named `each`. Anything else keeps its own name. */
const char *enum_walk_name(Compiler *c, int id, int recv, const char *name) {
  Scope *s = id >= 0 ? comp_scope_of(c, id) : NULL;
  if (!s || !s->name || strncmp(s->name, "__enum_", 7) != 0) return name;
  if (recv >= 0) {
    NodeKind rk = nt_kind(c->nt, recv);
    const char *rn = rk == NK_LocalVariableReadNode ? nt_str(c->nt, recv, "name") : NULL;
    if (rk != NK_SelfNode && !(rn && sp_streq(rn, "__self"))) return name;
  }
  static char buf[4][96];
  static int k;
  char *o = buf[k++ & 3];
  snprintf(o, 96, "%s", s->name + 7);
  /* a clone's name carries `__<n>` (`__enum_any?__12`) */
  char *cut = strstr(o + 1, "__");
  if (cut) *cut = 0;
  return o[0] ? o : name;
}
/* The C type of class `cid`'s instances. A `native_struct` carries the name
   its declaration gave -- which need not be derived from the Ruby class name
   (`native_struct "Store", "sp_X509_Store"`) -- and every other class is the
   `sp_<c_name>` struct the generator defines for it. Four rotating buffers so
   one format string can name two classes. */
const char *class_ctype(Compiler *c, int cid) {
  static char bufs[4][160];
  static int turn = 0;
  if (cid < 0 || cid >= c->nclasses) return "void";
  ClassInfo *ci = &c->classes[cid];
  if (ci->is_native_class && ci->c_struct) return ci->c_struct;
  char *out = bufs[turn++ & 3];
  snprintf(out, sizeof bufs[0], "sp_%s", ci->c_name ? ci->c_name : "");
  return out;
}
void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* value-type classes are stored inline (sp_X); others are heap pointers */
    buf_printf(b, "%s %s", class_ctype(c, cid), c->classes[cid].is_value_type ? "" : "*");
  }
  else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}
/* The element stamp a pointer array is boxed with (#4486): the runtime's one
   cls_id for pointer arrays is type-erased, so the box carries what the
   elements are for the poly paths to read them back. */
const char *ptr_array_stamp(Compiler *c, TyKind t) {
  static char buf[64];
  (void)c;
  if (t == TY_INT_ARRAY_ARRAY)   return "SP_PTR_ELEM_INT_ROWS, -1";
  if (t == TY_FLOAT_ARRAY_ARRAY) return "SP_PTR_ELEM_FLT_ROWS, -1";
  snprintf(buf, sizeof buf, "SP_PTR_ELEM_OBJ, %d", ty_obj_array_class(t));
  return buf;
}
/* comp_ntype through fold_seed_kind, which owns the rule (see types.c). */
TyKind fold_seed_ntype(Compiler *c, int node) {
  return fold_seed_kind(comp_ntype(c, node), nt_type(c->nt, node));
}
/* sum(seed) through the boxed fold. The receiver is boxed into a ROOTED temp
   before the seed runs: a seed that allocates -- a Rational, a Bignum -- can
   collect a receiver array the same statement just built, and the fold then
   walked an empty one. Rooting also fixes the order, which is Ruby's: the
   receiver first, then the seed, each evaluated exactly once. Shared by the
   typed-array, Hash and poly-receiver call sites, which each had the hazard. */
void emit_poly_sum_seed(Compiler *c, int recv, int seed, Buf *b) {
  int tr = ++g_tmp, ts = ++g_tmp;
  buf_puts(b, "({ ");
  tr = hold_operand(c, recv, TY_POLY, 1, tr, 1, " ", b);
  if (seed >= 0) ts = hold_operand(c, seed, TY_POLY, 1, ts, 1, " ", b);
  else buf_printf(b, "sp_RbVal _t%d = sp_box_int(0); ", ts);
  buf_printf(b, "sp_poly_sum_seed(_t%d, _t%d); })", tr, ts);
}
/* A call that never hands back a value: a receiverless raise or fail, or a
   method the program defines whose every path raises, which the analyzer
   types void (`def version = raise NotImplementedError` in a base class no
   subclass overrides). Its value sits in a position that wants one --
   `"v#{m.version}"`, `m.version == v` -- only on paper: control leaves
   through the raise. TY_VOID
   alone does not say so, since a bare puts, print, p or warn is typed void
   too and does return (nil), so the call must reach a method whose return
   the analyzer settled as void. */
int call_never_returns(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_CallNode) return 0;
  int recv = nt_ref(nt, id, "receiver");
  /* a call on such a receiver never runs, whatever its own type
     (`m.version - 1 > 2`: the `-` raises before `>` is reached) */
  if (recv >= 0 && call_never_returns(c, recv)) return 1;
  if (comp_ntype(c, id) != TY_VOID) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  if (recv < 0 && (is_raise_alias(nm))) return 1;
  int mi = -1;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) mi = comp_self_call_mi(c, id, nm);
  else {
    Repr rr = repr_of(c, recv);
    TyKind rt = rr.as_ty;
    if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), nm, NULL);
    else if (rr.kind == RK_BOXED) {
      /* a boxed receiver dispatches to whichever class defines the name;
         every one of them must raise (a builtin answering it would have
         given the call that builtin's type, not void) */
      int any = 0;
      for (int ci = 0; ci < c->nclasses; ci++) {
        int dc = -1, m = comp_method_in_chain(c, ci, nm, &dc);
        if (m < 0 || dc != ci) continue;
        if (c->scopes[m].ret != TY_VOID) return 0;
        any = 1;
      }
      return any;
    }
  }
  return mi >= 0 && c->scopes[mi].ret == TY_VOID;
}
/* The runtime conversion of a typed array to the poly array; NULL for a kind
   that has none. */
const char *array_to_poly_fn(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "sp_IntArray_to_poly";
    case TY_FLOAT_ARRAY: return "sp_FloatArray_to_poly";
    case TY_STR_ARRAY:   return "sp_StrArray_to_poly_fmt";
    default:             return NULL;
  }
}
/* The kind of the slot the next emit_block_value_into writes, when it is not
   the tail's own (a `then` whose value joined a `next` arm of another array
   kind, #4747); that one call consumes it. */
TyKind g_bv_dest_ty = TY_UNKNOWN;
const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}
/* Storage prefix for a loop over the array itself. A nested numeric table is
   an sp_PtrArray of row pointers, so the walk is sp_PtrArray_length / _get
   (void* converts to the row pointer). Object arrays share that storage but
   are not walked here. */
const char *array_iter_kind(TyKind t) {
  if (t == TY_POLY_ARRAY) return "Poly";
  if (ty_is_ptr_array(t)) return "Ptr";   /* a numeric table's rows, or objects */
  return array_kind(t);
}
void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}
void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}
/* Marker byte before string-literal data: 0xff is an immutable rodata literal
   (frozen? false, value semantics); `frozen` -- set from the node's `fzl` flag
   when its file has `# frozen_string_literal: true` -- switches to 0xf1 so
   `frozen?` is true and mutation raises FrozenError. Synthesized strings
   (symbol names, ivar names, ...) go through emit_str_literal, which is never
   frozen: the pragma only affects literals written in the source. */
/* A frozen literal is one static object per distinct content, defined at
   file scope, so every occurrence of the same bytes is the same object: CRuby
   with frozen string literals gives `"x".equal?("x")` true, and a
   compare_by_identity Hash keyed by "x" in two methods has one entry.

   The 0xf1 marker promises a REAL sp_str_hdr immediately in front of the data
   (hash cache, mutation guards), so every frozen literal -- single or an
   adjacent-literal fold -- carries this header (#1749). `ascii7` says every
   byte is below 0x80, which is what lets the runtime index the literal by
   byte (sp_str_fixed_width wants the bit AND a known length, #4239); it is
   read off the escaped text, where a byte from 0x80 up is a `\2xx` or `\3xx`
   escape, so two spellings of the same bytes can never disagree on it.

   A site emits a reference; the definitions are kept here, keyed by the
   escaped text (emit_c_escaped_n and the interpolation fold escape the same
   way), and fzl_emit_defs writes the ones the finished unit still names. */
typedef struct { char *esc; size_t esc_len, raw_len; unsigned hash; } FzlLit;
static FzlLit *g_fzl;
static int g_fzl_n, g_fzl_cap;
static int *g_fzl_slot;            /* open addressing over g_fzl, -1 empty */
static int g_fzl_slots;

static unsigned fzl_hash(const char *s, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
  return h;
}
static void fzl_rehash(void) {
  int ns = g_fzl_slots ? g_fzl_slots * 2 : 1024;
  free(g_fzl_slot);
  g_fzl_slot = malloc(sizeof(int) * (size_t)ns);
  for (int i = 0; i < ns; i++) g_fzl_slot[i] = -1;
  g_fzl_slots = ns;
  for (int k = 0; k < g_fzl_n; k++) {
    int i = (int)(g_fzl[k].hash & (unsigned)(ns - 1));
    while (g_fzl_slot[i] >= 0) i = (i + 1) & (ns - 1);
    g_fzl_slot[i] = k;
  }
}
static int fzl_esc_ascii7(const char *e, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (e[i] == '\\') { i++; if (i < n && (e[i] == '2' || e[i] == '3')) return 0; }
  return 1;
}
void emit_frozen_literal(Buf *b, const char *esc, size_t esc_len, size_t raw_len) {
  if (!esc) esc = "";
  unsigned h = fzl_hash(esc, esc_len);
  if ((g_fzl_n + 1) * 2 > g_fzl_slots) fzl_rehash();
  int i = (int)(h & (unsigned)(g_fzl_slots - 1));
  for (; g_fzl_slot[i] >= 0; i = (i + 1) & (g_fzl_slots - 1)) {
    FzlLit *f = &g_fzl[g_fzl_slot[i]];
    if (f->hash == h && f->esc_len == esc_len && !memcmp(f->esc, esc, esc_len)) break;
  }
  int id = g_fzl_slot[i];
  if (id < 0) {
    if (g_fzl_n == g_fzl_cap) {
      g_fzl_cap = g_fzl_cap ? g_fzl_cap * 2 : 256;
      g_fzl = realloc(g_fzl, sizeof *g_fzl * (size_t)g_fzl_cap);
    }
    id = g_fzl_n++;
    g_fzl[id].esc = malloc(esc_len + 1);
    memcpy(g_fzl[id].esc, esc, esc_len);
    g_fzl[id].esc[esc_len] = 0;
    g_fzl[id].esc_len = esc_len; g_fzl[id].raw_len = raw_len; g_fzl[id].hash = h;
    g_fzl_slot[i] = id;
  }
  buf_printf(b, "((char *)_fzl_%d.d)", id);
}
/* --repr-check, with handles: a frozen literal built into a handle by a
   constructor that copies it. sp_String_new_shared answers the literal's own
   handle (sp_String_literal_handle) and sp_String_new_unfrozen is the copy a
   `+"lit"` asks for; any other sp_String_new* taking the literal itself would
   make a new String per evaluation. Reported as a conflict, which the
   sharing verifier (tools/share_verify.rb) fails on. */
static int fzl_ctor_is(const char *s, size_t n, const char *name) {
  return strlen(name) == n && strncmp(s, name, n) == 0;
}
static void fzl_check_handles(const char *t) {
  static const char lit[] = "((char *)_fzl_";
  for (const char *p = t; p && (p = strstr(p, lit)); p += sizeof lit - 1) {
    if (p == t || p[-1] != '(') continue;
    const char *e = p - 1, *s = e;
    while (s > t && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
    size_t n = (size_t)(e - s);
    if (strncmp(s, "sp_String_new", 13) != 0 || fzl_ctor_is(s, n, "sp_String_new_shared") ||
        fzl_ctor_is(s, n, "sp_String_new_unfrozen")) continue;
    fprintf(stderr, "repr-check: conflict: frozen literal _fzl_%ld is built into a new handle by %.*s\n",
            strtol(p + sizeof lit - 1, NULL, 10), (int)n, s);
  }
}
/* The definitions of the literals the unit `t` names, in the order they were
   first met. A literal only a discarded emission named (a speculative arm, an
   abandoned unit) is left out. Resets the table for the next unit. With
   handles, the slot the header links to is a static of its own: the header
   taking the address of its own object would stop the C compiler from reading
   the bytes as constant (a `putc "|"` was no longer folded). */
void fzl_emit_defs(const char *t, Buf *out, int handles) {
  if (handles && g_repr_check) fzl_check_handles(t);
  char *used = calloc((size_t)g_fzl_n + 1, 1);
  for (const char *p = t; p && (p = strstr(p, "_fzl_")); ) {
    p += 5;
    if (!isdigit((unsigned char)*p)) continue;
    long k = 0;
    while (isdigit((unsigned char)*p)) k = k * 10 + (*p++ - '0');
    if (k < g_fzl_n) used[k] = 1;
  }
  for (int k = 0; k < g_fzl_n; k++) {
    FzlLit *f = &g_fzl[k];
    if (used[k] && handles)
      buf_printf(out, "static sp_StringLiteral _fzl_%d_slot = { NULL, NULL };\n"
                      "static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _fzl_%d = "
                      "{ { (sp_str_hdr *)((char *)&_fzl_%d_slot + 1), %zu%s, %zu, 0 }, 0xf1, \"%s\" };\n",
                 k, f->raw_len + 1, k, k, f->raw_len + 1,
                 fzl_esc_ascii7(f->esc, f->esc_len) ? " | SP_STR_SIZE_ASCII7" : "",
                 f->raw_len, f->esc);
    else if (used[k])
      buf_printf(out, "static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _fzl_%d = "
                      "{ { NULL, %zu%s, %zu, 0 }, 0xf1, \"%s\" };\n",
                 f->raw_len + 1, k, f->raw_len + 1,
                 fzl_esc_ascii7(f->esc, f->esc_len) ? " | SP_STR_SIZE_ASCII7" : "",
                 f->raw_len, f->esc);
    free(f->esc);
  }
  free(used);
  free(g_fzl); g_fzl = NULL; g_fzl_n = g_fzl_cap = 0;
  free(g_fzl_slot); g_fzl_slot = NULL; g_fzl_slots = 0;
}
static int round_kw_elem(Compiler *c, const RoundKw *o, int e, int *is_splat, int *opaque) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, o->node, "elements", &n);
  if (e >= n) return -1;
  int key = nt_ref(nt, els[e], "key");
  const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
  int kn_ok = kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"));
  *is_splat = key < 0;
  *opaque = key >= 0 && !kn_ok;
  return nt_ref(nt, els[e], "value");
}

void round_kw_read(Compiler *c, int kwh, RoundKw *o) {
  const NodeTable *nt = c->nt;
  memset(o, 0, sizeof *o);
  o->node = kwh;
  o->half = -1;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  char names[256]; names[0] = 0;
  for (int e = 0; e < n; e++) {
    int val = nt_ref(nt, els[e], "value");
    if (val < 0) continue;              /* nothing to evaluate and nothing to say */
    o->nelem = e + 1;
    int key = nt_ref(nt, els[e], "key");
    if (key < 0) { o->nsplat++; continue; }
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && sp_streq(kn, "half")) { o->half = val; continue; }  /* a repeated key: the last wins */
    const char *ks = (!kn && kty && sp_streq(kty, "StringNode")) ? nt_str(nt, key, "content") : NULL;
    if (!kn && !ks) continue;           /* unreadable: claim nothing */
    if (o->nunknown < 8) {
      size_t at = strlen(names);
      snprintf(names + at, sizeof names - at, "%s%s%s%s", o->nunknown ? ", " : "",
               kn ? ":" : "\"", kn ? kn : ks, kn ? "" : "\"");
    }
    o->nunknown++;
  }
  if (n) o->nelem = n;
  if (o->nunknown)
    snprintf(o->unknown, sizeof o->unknown, "unknown keyword%s: %s",
             o->nunknown > 1 ? "s" : "", names);
}

/* Evaluate every keyword value for its side effects and say nothing else --
   the reject paths, where CRuby has still built the hash before deciding the
   call cannot be made. */
void emit_round_kw_effects(Compiler *c, const RoundKw *kw, Buf *b) {
  for (int e = 0; e < kw->nelem; e++) {
    int sp_, op_; int v = round_kw_elem(c, kw, e, &sp_, &op_);
    if (v < 0) continue;
    buf_puts(b, "(void)("); emit_boxed(c, v, b); buf_puts(b, "); ");
  }
}

/* Bind what CRuby evaluates before a `round`-family call decides anything --
   every keyword value, in source order -- then settle the tie-break mode and
   raise for an unknown keyword, which CRuby does only once the whole hash has
   been read. Emits into an already-open statement expression; answers the
   temp holding the mode, or -1 when the hash names none. */
int emit_round_kw_binds(Compiler *c, const RoundKw *kw, Buf *b) {
  int thalf = -1, has_splat = 0;
  int *kt = calloc((size_t)kw->nelem + 1, sizeof *kt);
  if (!kt) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int e = 0; e < kw->nelem; e++) {
    int is_splat, opaque; int v = round_kw_elem(c, kw, e, &is_splat, &opaque);
    int t = kt[e] = ++g_tmp;
    if (v < 0) { buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); (void)_t%d; ", t, t); continue; }
    t = kt[e] = hold_operand(c, v, TY_POLY, 1, t, 1, " ", b);
    if (is_splat) has_splat = 1;
    else if (!opaque && v == kw->half) thalf = t;
  }
  if (kw->nunknown) {
    buf_puts(b, "sp_raise_cls(\"ArgumentError\", ");
    emit_str_literal(b, kw->unknown);
    buf_puts(b, "); ");
  }
  if (!has_splat) { free(kt); return thalf; }
  /* a `**` source is read in its place, so a `half:` on either side of it
     wins by being later, as it does in the hash the call really builds */
  int tm = ++g_tmp;
  buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); ", tm, tm);
  for (int e = 0; e < kw->nelem; e++) {
    int is_splat, opaque; int v = round_kw_elem(c, kw, e, &is_splat, &opaque);
    int t = kt[e];
    if (v < 0) continue;
    if (is_splat)
      buf_printf(b, "{ sp_RbVal _s%d = sp_round_half_kwsplat(_t%d);"
                    " if (_s%d.tag != SP_TAG_NIL) _t%d = _s%d; } ",
                 t, t, t, tm, t);
    else if (!opaque && v == kw->half)
      buf_printf(b, "_t%d = _t%d; ", tm, t);
  }
  free(kt);
  return tm;
}

void emit_str_literal_n(Buf *b, const char *content, size_t len, int frozen) {
  const char *mk = frozen ? "\\xf1" : "\\xff";
  /* A frozen literal must carry a REAL sp_str_hdr: the 0xf1 marker promises
     one immediately in front of the data (sp_str_hash caches the FNV hash
     through it, mutation guards and frozen? key off the marker). Baking the
     marker onto a bare rodata literal made that header read/write land in
     whatever rodata precedes the literal -- a garbage cached hash, so a
     Hash#[] with the literal key missed entries whose equal-content keys
     were built at runtime (#1749; ASAN: global-buffer-overflow). Emit a
     static header+marker+data object instead: the layout matches a heap
     string exactly (hdr | marker | bytes), the hash cache write hits our
     own static storage, and next=NULL keeps it off the sweep list.
     Shared literal definitions use a tagged link to their handle slot. */
  if (frozen) {
    Buf e; memset(&e, 0, sizeof e);
    if (content && len) emit_c_escaped_n(&e, content, len);
    emit_frozen_literal(b, e.p, e.len, content ? len : 0);
    free(e.p);
    return;
  }
  if (!content || len == 0) { buf_printf(b, "(&(\"%s\")[1])", mk); return; }
  /* NUL-containing strings: use sp_str_from_bytes with explicit byte count.
     The heap string it builds is writable (0xfe), so a frozen literal is
     sealed with sp_str_freeze_val (flips the heap marker to 0xf1 in place). */
  if (len > strlen(content)) {
    /* A FROZEN NUL-containing literal is immortal (sp_str_sweep never frees a
       0xf1 string), so build it once into a call-site-local static and reuse it.
       This avoids re-allocating it on every evaluation -- which, besides the
       churn, made the literal a GC-triggering sibling that could sweep an
       unrooted operand mid-expression (e.g. the receiver in
       `data[8, 8].delete("\0")`, a use-after-free in doom's WAD name parse). */
    if (frozen) {
      static int g_binlit_ctr = 0;
      int id = g_binlit_ctr++;
      buf_printf(b, "({ static const char *_binlit_%d; _binlit_%d ? _binlit_%d : "
                    "(_binlit_%d = sp_str_freeze_val(sp_str_from_bytes(\"", id, id, id, id);
      emit_c_escaped_n(b, content, len);
      buf_printf(b, "\", %zu))); })", len);
      return;
    }
    buf_puts(b, "sp_str_from_bytes(\"");
    emit_c_escaped_n(b, content, len);
    buf_printf(b, "\", %zu)", len);
    return;
  }
  buf_printf(b, "(&(\"%s\" \"", mk);
  emit_c_escaped_n(b, content, len);
  buf_puts(b, "\")[1])");
}
void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content), 0);
}
/* Ruby-source string literal (a StringNode): unlike the internal-constant
   emitter above, each OCCURRENCE gets its own static array, so two textually
   equal literals are two distinct objects and `equal?` (pointer identity)
   answers like CRuby. The plain `("\xff" "abc")` form let the C compiler merge
   equal literals into one address, making `"abc".equal?("abc")` true. A
   re-evaluated occurrence (a literal in a loop) still yields one address --
   the frozen-string-literal semantics spinel's immutable strings already have.
   A frozen literal is one object per content (_fzl_N), and a NUL-containing
   one is built by sp_str_from_bytes. */
void emit_str_literal_src(Buf *b, const char *content, size_t len, int frozen) {
  static int g_slit_ctr = 0;
  if (frozen || (content && len > strlen(content))) {
    emit_str_literal_n(b, content, len, frozen);
    return;
  }
  int lid = g_slit_ctr++;
  buf_printf(b, "({ static const char _slit_%d[] = \"\\xff\" \"", lid);
  if (content && len) emit_c_escaped_n(b, content, len);
  buf_printf(b, "\"; &_slit_%d[1]; })", lid);
}
/* Emit a catch/throw tag expression; returns the tag KIND (0 = a Symbol,
   matched by name; 1 = an object, matched by pointer identity; 2 = a
   String, matched by its pointer; 3 = a shared String, matched by its
   handle). A String matches by identity, as equal? does: by content an
   equal copy met the original, where CRuby raises UncaughtThrowError. A
   literal is one object per content, as in CRuby. */
int emit_catch_tag(Compiler *c, int id, Buf *b) {
  const char *ty = nt_type(c->nt, id);
  if (ty && sp_streq(ty, "SymbolNode")) { emit_str_literal(b, nt_str(c->nt, id, "value")); return 0; }
  Repr tr = repr_of(c, id);
  TyKind t = tr.as_ty;
  /* a variable holding the shared handle is that String, whichever face
     its read is typed as: a read of the handle hands out a copy */
  char href[256];
  if ((t == TY_STRBUF || t == TY_STRING) && strbuf_slot_ref(c, id, href, sizeof href)) {
    buf_printf(b, "((const char *)(void *)%s)", href);
    return 3;
  }
  if (t == TY_STRBUF)
    unsupported(c, id, "catch/throw with a shared String tag that no variable holds (bind it to a local first)");
  if (t == TY_STRING) {
    emit_expr(c, id, b);
    return 2;
  }
  if (t == TY_SYMBOL) {
    buf_puts(b, "sp_sym_to_s("); emit_expr(c, id, b); buf_puts(b, ")");
    return 0;
  }
  if (ty_is_object(t)) {
    /* a non-symbol object tag matches by identity: carry its pointer. An
       object that is nil (a NULL pointer) is the nil tag, so it meets a nil
       held in a boxed value */
    int tp = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = (const char *)(void *)(", tp); emit_expr(c, id, b);
    buf_printf(b, "); _t%d ? _t%d : (const char *)&sp_catch_nil_tag; })", tp, tp);
    return 1;
  }
  if (tr.kind == RK_BOXED) {
    /* A boxed tag is whatever the value is: a Symbol or a String matches by
       name, an object by identity. The kind is the value's at run time, so
       the caller gets the sp_RbVal and -1, and asks sp_catch_tag_of for the
       tag and the kind together. Reading the payload as an object pointer
       made a Symbol element of an Array (`throw TAGS[1]`) a kind-1 tag whose
       pointer was the symbol's id, and it matched nothing (#4523). */
    emit_expr(c, id, b);
    return -1;
  }
  if (t == TY_INT) {
    /* an Integer tag: CRuby matches by identity, which for a Fixnum is value
       equality -- carry the value in the pointer slot and match by identity. */
    buf_puts(b, "(const char *)(intptr_t)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  /* nil, true and false are single objects in CRuby, so they match by
     identity: each is the address of its own marker. */
  if (t == TY_NIL) {
    buf_puts(b, "((void)("); emit_expr(c, id, b); buf_puts(b, "), (const char *)&sp_catch_nil_tag)");
    return 1;
  }
  if (t == TY_BOOL) {
    buf_puts(b, "(("); emit_expr(c, id, b);
    buf_puts(b, ") ? (const char *)&sp_catch_true_tag : (const char *)&sp_catch_false_tag)");
    return 1;
  }
  /* A Float/Bignum tag would emit a non-pointer (or a struct) into the
     const char* tag slot -- invalid C. These are vanishingly rare as
     catch/throw tags; reject loudly rather than miscompile. */
  unsupported(c, id, "catch/throw with a Float or Bignum tag (use a Symbol, String, Integer, nil, boolean, or object tag)");
  return 0;
}
/* A key whose static kind can never be in a typed hash's table: a String
   or a user object looked up in an Integer-keyed Hash, a Float where
   1.eql?(1.0) is false, nil in any of them. Hash looks a key up by #hash and
   #eql? and converts nothing, so the lookup is a plain miss in CRuby, not a
   TypeError -- and not the raw pointer in the sp_int slot that stopped the C
   build ({1 => 2}.dig("a"), .fetch("a"), .except(obj)). The same kinds a
   poly key of another tag already misses on. A user object is a miss
   without asking whether its class defines #eql? and #hash: a typed table
   holds no objects, so nothing in it can be eql? to one. The by-value kinds
   (a Range, a Time, a Complex, a Rational...) are asked of their traits
   row: none is a String, a Symbol or an Integer, and Complex(1, 0).eql?(1)
   is false. */
int hash_key_misses(Compiler *c, int key, TyKind kt) {
  TyKind actual = comp_ntype(c, key);
  if (kt == TY_POLY || actual == kt || actual == TY_POLY || actual == TY_UNKNOWN) return 0;
  if (kt == TY_STRING && actual == TY_STRBUF) return 0;
  return actual == TY_NIL || actual == TY_BOOL || actual == TY_INT ||
         actual == TY_BIGINT || actual == TY_FLOAT || actual == TY_SYMBOL ||
         actual == TY_STRING || actual == TY_STRBUF || actual == TY_REGEX ||
         ty_is_struct_valued(actual) || ty_is_array(actual) || ty_is_hash(actual) ||
         ty_is_object(actual);
}

/* nil looked up in an Integer-keyed table as a stored key: never now. An
   Integer key that can be nil takes the _okey entry points, where nil is a
   key of its own, so no sp_int key stands for it. */
int hash_nil_key_stored(Compiler *c, int key, TyKind kt) {
  (void)c; (void)key; (void)kt;
  return 0;   /* an Integer key has no nil word any more (A5) */
}

/* An Integer-keyed table looked up with a key of another class
   (hash_key_misses): no sp_int names a key no entry has, so the lookup takes
   the table's _okey entry point with a nil sp_oint key, which matches no
   entry (the miss answers as each op does for a missing key). */
int hash_okey_miss(Compiler *c, int key, TyKind kt) {
  return kt == TY_INT && hash_key_misses(c, key, kt);
}
/* that key: evaluated for its effects, then the nil no entry matches */
void emit_hash_okey(Compiler *c, int key, Buf *b) {
  buf_puts(b, "({ (void)("); emit_expr(c, key, b); buf_puts(b, "); sp_oint_nil(); })");
}

/* ... and a key that may not be an Integer at run time -- a boxed one, or
   an Integer that can be nil -- takes the same entry points with a key
   that is nil where it matches no entry: a boxed key of another class, a
   nil key (CRuby looks nil up and misses; it is no TypeError) */
int hash_okey_form(Compiler *c, int key, TyKind kt) {
  if (kt != TY_INT) return 0;
  if (hash_key_misses(c, key, kt)) return 1;
  TyKind at = comp_ntype(c, key);
  if (repr_of(c, key).kind == RK_BOXED) return 1;
  return at == TY_INT && node_is_oint(c, key);
}
/* a lookup site's three parts for such a key: the key temp's C type, the
   op's name suffix, and the key itself */
const char *hash_key_ctype(Compiler *c, int key, TyKind kt) {
  return hash_okey_form(c, key, kt) ? "sp_oint" : c_type_name(kt);
}
const char *hash_okey_sfx(Compiler *c, int key, TyKind kt) {
  return hash_okey_form(c, key, kt) ? "_okey" : "";
}
void emit_hash_key_o(Compiler *c, int key, TyKind kt, Buf *b) {
  if (!hash_okey_form(c, key, kt)) { emit_hash_key(c, key, kt, b); return; }
  if (hash_key_misses(c, key, kt)) { emit_hash_okey(c, key, b); return; }
  if (repr_of(c, key).kind == RK_BOXED) {
    buf_puts(b, "({ sp_RbVal _hk = "); emit_boxed(c, key, b);
    buf_puts(b, "; _hk.tag == SP_TAG_INT ? sp_oint_of(_hk.v.i) : sp_oint_nil(); })");
    return;
  }
  emit_oint_expr(c, key, TY_INT, b);
}

/* --share-strings: String Array value v as a PolyArray holding each
   element boxed as a handle of its own (a slot whose elements the rule
   shares, or a fresh Array an iterator consumes: share_node_fresh_elems) */
void emit_str_array_handles(Compiler *c, int v, Buf *b) {
  int ta = ++g_tmp, tp = ++g_tmp, ti = ++g_tmp;
  buf_printf(b, "({ sp_StrArray *_t%d = ", ta);
  emit_expr(c, v, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++)"
                " sp_PolyArray_push(_t%d, sp_box_nullable_obj(sp_String_new_shared(sp_StrArray_get(_t%d, _t%d)),"
                " SP_BUILTIN_STRBUF)); _t%d; })",
             ta, tp, tp, ti, ti, ta, ti, tp, ta, ti, tp);
}
/* An iterator's receiver n: a fresh String Array whose elements the rule
   shares (share_node_fresh_elems)? Then it is consumed as the PolyArray of
   handles a local's Array would be. */
int iter_src_as_handles(Compiler *c, int n) {
  return c->share_strings && comp_ntype(c, n) == TY_STR_ARRAY && share_node_fresh_elems(c, n);
}
/* A retaining iterator call `id` (select, reject, find_all, the in-place
   filters) over a fresh sharing String Array `recv`: the receiver is
   consumed as the PolyArray of handles, and the answer is one too
   (share_iter_answers_handles types it so). */
int iter_filter_src_as_handles(Compiler *c, int id, int recv) {
  return share_iter_answers_handles(c, id) && iter_src_as_handles(c, recv);
}
/* emit_boxed for an Enumerator's source, as iter_src_as_handles has it */
void emit_boxed_iter_src(Compiler *c, int n, Buf *b) {
  if (!iter_src_as_handles(c, n)) { emit_boxed(c, n, b); return; }
  buf_puts(b, "sp_box_nullable_obj((void *)(");
  emit_str_array_handles(c, n, b);
  buf_puts(b, "), SP_BUILTIN_POLY_ARRAY)");
}
/* --share-strings: is block parameter lv, bound to an element of kind et, a
   slot the scope holds as the shared handle (TY_STRBUF) over a String
   element? Then each element is a fresh String the handle wraps, bound
   into the slot, and no et-typed shadow declaration replaces it: the body's
   reads were settled against the handle. */
int elem_param_is_handle(const LocalVar *lv, TyKind et) {
  return lv && lv->type == TY_STRBUF && et == TY_STRING;
}
/* A variable name an ENV snapshot read takes (desugar_env_enum marks it,
   or its splat) raises CRuby's ArgumentError for an embedded NUL, as ENV[]
   does: the check's text around the name. */
const char *env_key_open(const NodeTable *nt, int n) { return nt_int(nt, n, "env_key", 0) ? "sp_env_chk(" : ""; }
const char *env_key_close(const NodeTable *nt, int n) { return nt_int(nt, n, "env_key", 0) ? ", 0)" : ""; }
static void emit_hash_key_value(Compiler *c, int key, TyKind kt, Buf *b);
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  if (kt != TY_STRING) { emit_hash_key_value(c, key, kt, b); return; }
  buf_puts(b, env_key_open(c->nt, key));
  emit_hash_key_value(c, key, kt, b);
  buf_puts(b, env_key_close(c->nt, key));
}
static void emit_hash_key_value(Compiler *c, int key, TyKind kt, Buf *b) {
  int kboxed = repr_of(c, key).kind == RK_BOXED;
  if (hash_key_misses(c, key, kt)) {
    /* an Integer key no entry equals goes through the _okey entry points
       (hash_okey_miss), which each caller asks first: one that reaches here
       has none, and refuses at compile time rather than raise a TypeError
       CRuby would not */
    if (kt == TY_INT) {
      unsupported_feature(c, key, "a key of another class on an Integer-keyed Hash in this method");
      buf_puts(b, "0");
      return;
    }
    /* evaluate the key for its effects, then answer the value no key equals */
    buf_puts(b, "({ (void)(");
    emit_expr(c, key, b);
    if (kt == TY_STRING)      buf_puts(b, "); (const char *)0; })");
    else                      buf_puts(b, "); (sp_sym)-1; })");
    return;
  }
  /* A Symbol key on a String-keyed hash used to coerce to its name, a
     leftover of an older Hash.new{} model (its hash is PolyPoly now): it
     made `h[:a]` find "a"'s entry and `h.delete(:a)` remove it (#4531).
     :a != "a", so it is a miss like any other kind mismatch above. */
  if (kboxed && kt != TY_POLY) {
    /* The union member is only valid when the tag agrees. A call site reached
       with a key of another kind -- the same method called with a String and
       with a Float -- read a Float's bits as a `const char *` and dereferenced
       them (#3810). A key of the wrong kind is simply not in the table, so
       answer a value no key can equal and let the lookup miss. */
    buf_puts(b, "({ sp_RbVal _hk = ");
    emit_boxed(c, key, b);
    /* A shared-string handle is `==` to the immediate string with the same
       bytes and now hashes alike, so it is a key that IS in the table: deref
       it rather than answering the no-key sentinel (#4279). */
    if (kt == TY_STRING)      buf_puts(b, "; _hk = sp_poly_strbuf_deref(_hk); _hk.tag == SP_TAG_STR ? _hk.v.s : (const char *)0; })");
    else if (kt == TY_SYMBOL) buf_puts(b, "; _hk.tag == SP_TAG_SYM ? (sp_sym)_hk.v.i : (sp_sym)-1; })");
    else                      buf_puts(b, "; sp_poly_hkey_i(_hk); })");   /* another kind raises (A5) */
    return;
  }
  if (kt == TY_POLY && !kboxed) {
    /* PolyPolyHash key: box the typed value into sp_RbVal */
    emit_boxed(c, key, b);
    return;
  }
  /* a read marked to hand out a shared String's handle (a dynamic call's
     argument the read also is, at another round) keys a String-keyed hash by
     a copy of its bytes, which publishes nothing on the side channel */
  if (kt == TY_STRING && comp_ntype(c, key) == TY_STRBUF && repr_of(c, key).handle) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_String *_t%d = ", t);
    emit_expr(c, key, b);
    buf_printf(b, "; _t%d ? sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1])) : NULL; })", t, t);
    return;
  }
  emit_expr(c, key, b);
}
/* 1 when `kwh` is a keyword list made only of `**` spreads (`m(**a, **b)`).
   Such a list may be empty at run time, and an empty spread passes no
   argument at all (`m(1, **{})` is `m(1)`). */
int kwh_only_spreads(const NodeTable *nt, int kwh) {
  if (kwh < 0 || nt_kind(nt, kwh) != NK_KeywordHashNode) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  if (en == 0) return 0;
  for (int e = 0; e < en; e++)
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode) return 0;
  return 1;
}
int unwrap_parens(Compiler *c, int id) {
  while (id >= 0) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "ParenthesesNode")) break;
    int body = nt_ref(c->nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
    if (n != 1) break;
    id = bd[0];
  }
  return id;
}

/* Walk a String `<<` chain (`(s << a) << b`) down to its base, peeling
   parens. Fills chain[] outermost-first with each link's argument (at most
   64) and stores the base node in *base. Returns the link count. */
int str_append_chain(Compiler *c, int recv, int *chain, int *base) {
  const NodeTable *nt = c->nt;
  int nchain = 0; int cur = recv;
  while (nchain < 64) {
    cur = unwrap_parens(c, cur);
    const char *cty = nt_type(nt, cur);
    if (!cty || !sp_streq(cty, "CallNode")) break;
    const char *cnm = nt_str(nt, cur, "name");
    int crecv = nt_ref(nt, cur, "receiver");
    if (!cnm || !sp_streq(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
    int cargs = nt_ref(nt, cur, "arguments");
    int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
    if (cac != 1) break;
    chain[nchain++] = cav[0];
    cur = crecv;
  }
  *base = cur;
  return nchain;
}

/* 1 when the receiver is a range whose begin endpoint is statically a Float
   -- directly a (possibly parenthesized) RangeNode or through a
   sole-assignment local. CRuby raises TypeError "can't iterate from Float"
   when enumerating such a range (an int begin with a float end iterates
   fine); the int-backed sp_Range would otherwise silently truncate. */
int range_float_begin(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rn = unwrap_parens(c, recv);
  if (rn < 0) return 0;
  const char *rty = nt_type(nt, rn);
  if (!rty || !sp_streq(rty, "RangeNode")) {
    rn = local_sole_range_node(c, rn);
    if (rn < 0) return 0;
  }
  int lo = nt_ref(nt, rn, "left");
  return lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
}
const char *int_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_int_add";
  if (sp_streq(op, "-"))  return "sp_int_sub";
  if (sp_streq(op, "*"))  return "sp_int_mul";
  if (sp_streq(op, "/"))  return "sp_idiv";
  if (sp_streq(op, "%"))  return "sp_imod";
  if (sp_streq(op, "**")) return "sp_int_pow";
  return NULL;
}
const char *bigint_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_bigint_add";
  if (sp_streq(op, "-"))  return "sp_bigint_sub";
  if (sp_streq(op, "*"))  return "sp_bigint_mul";
  if (sp_streq(op, "/"))  return "sp_bigint_div";
  if (sp_streq(op, "%"))  return "sp_bigint_mod";
  if (sp_streq(op, "&"))  return "sp_bigint_and";
  if (sp_streq(op, "|"))  return "sp_bigint_or";
  if (sp_streq(op, "^"))  return "sp_bigint_xor";
  return NULL;
}
/* True if any user exception subclass overrides #message or #to_s, so the
   default exception message/to_s path must dispatch to the user method rather
   than reporting the stored message (which defaults to the class name). */
int exc_has_user_msg_override(Compiler *c) {
  /* a builtin exception's reopening defining either (`class StandardError;
     def to_s`) serves every exception under it */
  if (any_exc_reopen(c))
    for (int i = 0; i < c->nclasses; i++) {
      if (!class_is_exc_reopen(c, i)) continue;
      for (int k = 0; k < 2; k++) {
        int mi = comp_method_in_chain(c, i, k ? "to_s" : "message", NULL);
        if (mi >= 0 && c->scopes[mi].class_id == i && (TyKind)c->scopes[mi].ret == TY_STRING) return 1;
      }
    }
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    /* Only a string-returning override is dispatched (see codegen_program),
       so gate on TY_STRING to match -- otherwise a non-string override would
       route every exception query through an empty dispatcher for nothing. */
    int mi_msg = comp_method_in_chain(c, i, "message", NULL);
    if (mi_msg >= 0 && (TyKind)c->scopes[mi_msg].ret == TY_STRING)
      return 1;
    int mi_tos = comp_method_in_chain(c, i, "to_s", NULL);
    if (mi_tos >= 0 && (TyKind)c->scopes[mi_tos].ret == TY_STRING)
      return 1;
  }
  return 0;
}

/* Does an exception's #message or #to_s override of the program's own call
   `super`? That reaches Exception's, the stored message, which a body cannot
   hand on as the stored handle yet (its answer is the stored bytes' copy), so
   a shared route over such a class stays refused. Asked once. */
int exc_text_override_calls_super(Compiler *c) {
  if (c->exc_text_super) return c->exc_text_super == 2;
  c->exc_text_super = 1;
  for (int i = 0; i < c->nclasses && c->exc_text_super == 1; i++) {
    if (!class_is_exc_subclass(c, i) && !class_is_exc_reopen(c, i)) continue;
    for (int k = 0; k < 2; k++) {
      int mi = comp_method_in_class(c, i, k ? "to_s" : "message");
      if (mi >= 0 && scope_calls_super(c, mi)) { c->exc_text_super = 2; break; }
    }
  }
  return c->exc_text_super == 2;
}

/* An override that answers something other than a String: Exception#message
   is #to_s, so such a class carries a non-string value out of #message and the
   const char * dispatchers cannot represent it. A boxed pair of dispatchers is
   emitted instead, and the call sites type as poly (#3868). */
int exc_has_nonstring_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    for (int k = 0; k < 2; k++) {
      int mi = comp_method_in_chain(c, i, k ? "to_s" : "message", NULL);
      if (mi < 0) continue;
      TyKind rk = (TyKind)c->scopes[mi].ret;
      if (rk != TY_STRING && rk != TY_UNKNOWN) return 1;
    }
  }
  return 0;
}

/* The class an index-taking Array method was handed instead of an index, or
   NULL when the argument can serve as one. Ruby converts a Float through
   #to_int and takes a Range where the method has a slice form; a String,
   Symbol, Array, Hash, nil or boolean is a TypeError. A poly argument stays on
   the runtime path: its class is not settled here (#3923, #3924, #3925). */
const char *array_index_bad_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int ir = nt_ref(nt, id, "receiver");
  const char *inm = nt_str(nt, id, "name");
  int ia = nt_ref(nt, id, "arguments");
  int ic = 0; const int *iv = ia >= 0 ? nt_arr(nt, ia, "arguments", &ic) : NULL;
  TyKind irt = ir >= 0 ? comp_ntype(c, ir) : TY_UNKNOWN;
  if (ir < 0 || !inm || ic < 1 || !iv || iv[0] < 0) return NULL;
  if (!(ty_is_array(irt) || ty_is_obj_array(irt))) return NULL;
  if (user_defines_or_reads(c, inm)) return NULL;
  static const char *const IDX[] = { "at", "fetch", "first", "last", "take",
                                     "drop", "insert", "dig", "values_at",
                                     "rotate", "[]", "slice", "[]=", NULL };
  static const char *const SLICE_OK[] = { "[]", "slice", "[]=", "values_at", NULL };
  int is_idx = 0, range_ok = 0;
  for (int k = 0; IDX[k]; k++) if (sp_streq(inm, IDX[k])) { is_idx = 1; break; }
  if (!is_idx) return NULL;
  for (int k = 0; SLICE_OK[k]; k++) if (sp_streq(inm, SLICE_OK[k])) { range_ok = 1; break; }
  TyKind at4 = comp_ntype(c, iv[0]);
  if (at4 == TY_STRING || at4 == TY_STRBUF) return "String";
  if (at4 == TY_SYMBOL) return "Symbol";
  if (ty_is_array(at4) || ty_is_obj_array(at4)) return "Array";
  if (ty_is_hash(at4)) return "Hash";
  if (at4 == TY_NIL) return "nil";
  if (at4 == TY_BOOL) return "Boolean";
  if ((at4 == TY_RANGE || at4 == TY_FLOAT_RANGE || at4 == TY_STR_RANGE) && !range_ok)
    return "Range";
  return NULL;
}
/* A method on a REOPENED builtin whose composed C name would be a runtime
   function of the same spelling: sp_String is the shared-mutable String's own
   type, so `class String; def length` emitted an sp_String_length(const char *)
   beside the runtime's sp_String_length(sp_String *) and the program did not
   compile at all. The same holds for dup, freeze, insert, prepend, replace and
   the rest of that family. A reopened method of one of those names takes an
   `_oc` suffix on the class stem instead; every site that composes the name
   goes through here or through mc_reopen_cls below. */
static const char *const sp_rt_string_fns[] = {
  "append", "append_bin", "append_n", "cstr", "dup", "fin", "freeze",
  "insert", "is_frozen", "length", "new", "new_len", "new_shared",
  "prepend", "replace", "set_bin", NULL };
const char *mc_reopen_cls(Compiler *c, int class_id, const char *mname) {
  static char buf[128];
  const char *stem = c->classes[class_id].c_name;
  /* The runtime owns dozens of sp_Range_* / sp_Time_* / sp_File_* /
     sp_Class_* functions and grows more; rather than track the names, a
     reopen of these takes the `_oc` stem for every method. */
  {
    const char *rn = c->classes[class_id].name;
    if (rn && (is_range_or_time_class(rn) ||
               io_family_class(c, class_id) || sp_streq(rn, "Class"))) {
      snprintf(buf, sizeof buf, "%s_oc", stem);
      return buf;
    }
  }
  if (mname && sp_streq(c->classes[class_id].name, "String")) {
    for (int i = 0; sp_rt_string_fns[i]; i++)
      if (sp_streq(mname, sp_rt_string_fns[i])) {
        snprintf(buf, sizeof buf, "%s_oc", stem);
        return buf;
      }
  }
  return stem;
}

const char *mc(const char *name) {
  static char buf[256];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof buf - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    /* operator characters map to distinct tokens so that, e.g., `&` and `|`
       (or `<<` and `>>`) don't mangle to the same C identifier */
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      case '+': tok = "_plus";  break;
      case '-': tok = "_minus"; break;
      case '*': tok = "_star";  break;
      case '/': tok = "_slash"; break;
      case '%': tok = "_pct";   break;
      case '<': tok = "_lt";    break;
      case '>': tok = "_gt";    break;
      case '&': tok = "_amp";   break;
      case '|': tok = "_bar";   break;
      case '^': tok = "_caret"; break;
      case '~': tok = "_tilde"; break;
      case '@': tok = "_at";    break;
      case '[': tok = "_lb";    break;
      case ']': tok = "_rb";    break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  /* A method is emitted as sp_<Class>_<mc(name)>, and sp_<Class>_new is the
     generated constructor: an instance method named `new` redefined it, and
     the call bound to the zero-argument constructor (#4829). Every user of a
     method's C name goes through here, so the rename is seen consistently;
     the class-side `self.new` (sp_<Class>_s_new__m) moves with it. The GC
     scan function, the other generated helper a method name could hit, is
     named sp_<Class>__gc_scan for the same reason. */
  if (strcmp(buf, "new") == 0) return "new__m";
  return buf;
}

/* The names a top-level Ruby method must not take: generated from the runtime
   sources by the Makefile (build/csrc/sp_rt_names.h), because the set is a fact
   about those sources and a hand-kept copy of it goes stale -- `def gcd` and
   `def gets` collided with sp_gcd and sp_gets while the list said nothing. */
#include "sp_rt_names.h"

/* The mangled name of a top-level method: `mc(name)`, with an `rb_` infix when
   the plain form would sit in the runtime's own namespace.

   The segment compared is the whole name when it carries no underscore. A
   prefix names both a runtime NAMESPACE (sp_sym_intern, sp_int_add) and, for
   about a dozen of them, a runtime IDENTIFIER of its own -- the typedefs
   sp_sym and sp_int, sp_raise, sp_thread. Looking only in front of an
   underscore protected the namespace and left the identifier open, so `def
   sym` collided with the typedef and the program failed to build on a C
   diagnostic that never mentions the name the author wrote. */
const char *mc_top(Compiler *c, const char *name) {
  static char buf[272];
  const char *m = mc(name);
  /* A class or module of the same name owns sp_<Name> as its typedef, and
     `def Foo` beside `class Foo` is ordinary Ruby -- URI(), Integer(),
     Array() are all a method sharing a name with a type. Without this the two
     collide and the program fails to build on a C diagnostic that never
     mentions either. Same remedy as the runtime clash below. */
  if (c && comp_class_index(c, name) >= 0) {
    snprintf(buf, sizeof buf, "rb_%s", m);
    return buf;
  }
  const char *us = strchr(m, '_');
  size_t seg = (us && us > m) ? (size_t)(us - m) : strlen(m);
  if (seg > 0) {
    for (int i = 0; SP_RT_PREFIXES[i]; i++) {
      if (strlen(SP_RT_PREFIXES[i]) != seg) continue;
      if (strncmp(m, SP_RT_PREFIXES[i], seg) != 0) continue;
      snprintf(buf, sizeof buf, "rb_%s", m);
      return buf;
    }
  }
  return m;
}
/* Mangle an ivar/struct-member name (sans leading '@') to a valid C field
   identifier, so a Struct/Data member like `verbose?` becomes iv_verbose_p
   rather than the illegal iv_verbose? (#3110). Same character map as mc(), but
   a rotating buffer ring so the 2-4 uses of a field name on one buf_printf line
   don't clobber each other. A no-op for the usual all-identifier ivar names. */
const char *iv_c(const char *name) {
  static char bufs[8][256];
  static int which = 0;
  char *buf = bufs[(which++) & 7];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof bufs[0] - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  /* A long name keeps a prefix plus a hash of the whole name, so every field
     reference fits the fixed buffers its callers format it into and names
     sharing a long prefix stay distinct. */
  if (j > IV_C_MAX) {
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    snprintf(buf + IV_C_MAX - 17, 18, "_%016llx", h);
  }
  return buf;
}
/* scope_is_shadowed asks every later scope, and emission asks it per method
   and per call site, which made the C emission grow with the square of the
   program (the scale test's codegen leg). Once analysis has settled the
   scopes (g_scopes_settled, set by codegen_program), the answers for all of
   them come from one backward pass, kept until the scope count changes. */
int g_scopes_settled = 0;
static const Compiler *g_shadow_c;
static int g_shadow_n = -1;
static unsigned char *g_shadow_tab;
typedef struct { const char *name; int class_id, is_cm, used, any, d1, multi; } ShadowKey;
static unsigned shadow_hash(const char *nm, int cls, int cm) {
  unsigned h = 2166136261u;
  for (const char *p = nm; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
  return h ^ ((unsigned)cls * 2654435761u) ^ (unsigned)cm;
}
static int scope_is_shadowed_scan(Compiler *c, int s);
static void shadow_tab_build(Compiler *c) {
  int n = c->nscopes;
  free(g_shadow_tab);
  g_shadow_tab = (unsigned char *)calloc((size_t)(n > 0 ? n : 1), 1);
  size_t cap = 16; while (cap < (size_t)n * 2 + 2) cap *= 2;
  ShadowKey *tab = (ShadowKey *)calloc(cap, sizeof(ShadowKey));
  if (!g_shadow_tab || !tab) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = n - 1; k >= 0; k--) {
    Scope *sc = &c->scopes[k];
    if (!sc->name) continue;
    /* by the scope's own class, whatever an instance_exec block has moved
       (comp_scope_own_class): the move defines nothing in the class it
       moves to, and the table then reads the same whenever it is built */
    int own_cm, own = comp_scope_own_class(c, k, &own_cm);
    int cls = own < 0 ? -1 : own, cm = own_cm ? 1 : 0;
    size_t i = shadow_hash(sc->name, cls, cm) & (cap - 1);
    while (tab[i].used && !(tab[i].class_id == cls && tab[i].is_cm == cm && sp_streq(tab[i].name, sc->name)))
      i = (i + 1) & (cap - 1);
    ShadowKey *e = &tab[i];
    if (!e->used) { e->used = 1; e->name = sc->name; e->class_id = cls; e->is_cm = cm; e->d1 = -1; }
    int def = sc->def_node >= 0 && nt_kind(c->nt, sc->def_node) == NK_DefNode ? sc->def_node : -1;
    if (cls >= 0) g_shadow_tab[k] = (unsigned char)e->any;
    else g_shadow_tab[k] = (unsigned char)(def >= 0 && e->d1 >= 0 && (e->d1 != def || e->multi));
    e->any = 1;
    if (cls < 0 && def >= 0) {
      if (e->d1 < 0) e->d1 = def;
      else if (e->d1 != def) e->multi = 1;
    }
  }
  free(tab);
  g_shadow_c = c; g_shadow_n = n;
}
int scope_is_shadowed(Compiler *c, int s) {
  if (c->scopes[s].c_name) return 0;
  if (!g_scopes_settled) return scope_is_shadowed_scan(c, s);
  if (g_shadow_c != c || g_shadow_n != c->nscopes) shadow_tab_build(c);
  return g_shadow_tab[s];
}
static int scope_is_shadowed_scan(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (sc->c_name) return 0;
  if (!sc->name) return 0;
  /* a redefined top-level method: only a later `def` of the same name
     shadows it, and comp_method_index answers that one. Emitting both was
     a C redefinition when the signatures matched. */
  if (sc->class_id < 0) {
    if (sc->def_node < 0 || nt_kind(c->nt, sc->def_node) != NK_DefNode) return 0;
    for (int k = s + 1; k < c->nscopes; k++) {
      Scope *o = &c->scopes[k];
      if (o->class_id < 0 && o->is_cmethod == sc->is_cmethod && o->name &&
          sp_streq(o->name, sc->name) && o->def_node >= 0 && o->def_node != sc->def_node &&
          nt_kind(c->nt, o->def_node) == NK_DefNode) return 1;
    }
    return 0;
  }
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && sp_streq(o->name, sc->name)) return 1;
  }
  return 0;
}
/* True when scope `s` is emitted as a standalone `sp_Class_method` function, so
   a poly-dispatch `case` arm may call it without dangling at link. Mirrors the
   emission gate in codegen.c exactly: a yielding method is inlined at each call
   site (no symbol exists), and a pruned/shadowed/transplanted method is never
   defined. A dispatch arm that targets a scope failing this test references an
   undefined symbol (issues #1583 yields, #1576 pruned). */
/* ---- proc forms of yielding methods (#3399) ----
   A yielding method has no symbol: it is inlined at each call site with the
   block spliced in. A poly dispatch has no call site to splice into, so the
   methods it names get a SECOND definition -- an ordinary function taking the
   block as an sp_Proc * -- and the dispatch points at that. The analyzer makes
   it (make_yield_proc_forms) as a clone scope; the helpers below find it. */

/* Is `node` a read of something that already holds its object (a local, an
   ivar, self, a constant)? Anything else -- a constructor, a method call --
   may hand back a fresh object whose only reference is the C temporary the
   caller keeps it in, and a call on it that allocates must root that
   temporary first. */
int expr_is_held_ref(Compiler *c, int node) {
  NodeKind k = nt_kind(c->nt, node);
  return k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode ||
         k == NK_SelfNode || k == NK_ConstantReadNode;
}
/* Is `node` a read of an object that a root reaches where the read ends: a
   variable, a constant or self; a plain field read off such a read (an
   attr_reader's or a Struct member's slot), or a class's own reader called
   with no receiver; an element of such an Array, by an Integer index; a
   conditional, a `case` or a `begin` whose every arm is one such read of
   its own type, or an `||` or `&&` of two? Once its conditions and its
   index have run, such a read allocates nothing and calls nothing of the
   program's, so what it read is still held where it ends. `recv[i]` is an
   element read only while the program gives Array no `[]` of its own
   (array_index_is_own): that one is what the call reaches, and its answer
   is a call's result. An index that only reads leaves any receiver's Array
   where it was; one that runs code does too if the receiver is a variable
   that code cannot give another Array (read_rebound_by, which does not
   look at a constant). */
static int read_is_held(Compiler *c, int node);
/* Does `[]` on an Array reach a method of the program's? Only one in the
   chain of the program's own reopening of Array can be: a `[]` another class
   defines (a Grid's) is never what an Array receiver calls. */
static int array_index_is_own(Compiler *c) {
  int ci = comp_class_index(c, "Array");
  return ci >= 0 && (comp_method_in_chain(c, ci, "[]", NULL) >= 0 ||
                     comp_reader_in_chain(c, ci, "[]", NULL));
}
/* one statement, a held read of type t */
static int arm_is_held(Compiler *c, int stmts, TyKind t) {
  int n = 0;
  const int *body = stmts >= 0 ? nt_arr(c->nt, stmts, "body", &n) : NULL;
  return n == 1 && comp_ntype(c, body[0]) == t && read_is_held(c, body[0]);
}
static int else_is_held(Compiler *c, int els, TyKind t) {
  return els >= 0 && nt_kind(c->nt, els) == NK_ElseNode &&
         arm_is_held(c, nt_ref(c->nt, els, "statements"), t);
}
static int read_is_held(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  node = unwrap_parens(c, node);
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  TyKind t = comp_ntype(c, node);
  if (expr_is_held_ref(c, node) || k == NK_ConstantPathNode ||
      k == NK_GlobalVariableReadNode || k == NK_ClassVariableReadNode)
    return 1;
  if (k == NK_IfNode) {
    int sub = nt_ref(nt, node, "subsequent");
    if (!arm_is_held(c, nt_ref(nt, node, "statements"), t)) return 0;
    /* an elsif is the if of its own arm */
    if (sub >= 0 && nt_kind(nt, sub) == NK_IfNode) return comp_ntype(c, sub) == t && read_is_held(c, sub);
    return else_is_held(c, sub, t);
  }
  if (k == NK_UnlessNode)
    return arm_is_held(c, nt_ref(nt, node, "statements"), t) &&
           else_is_held(c, nt_ref(nt, node, "else_clause"), t);
  if (k == NK_CaseNode) {
    int nw = 0;
    const int *whens = nt_arr(nt, node, "conditions", &nw);
    for (int w = 0; w < nw; w++)
      if (!arm_is_held(c, nt_ref(nt, whens[w], "statements"), t)) return 0;
    return nw > 0 && else_is_held(c, nt_ref(nt, node, "else_clause"), t);
  }
  if (k == NK_BeginNode)
    return nt_ref(nt, node, "rescue_clause") < 0 && nt_ref(nt, node, "ensure_clause") < 0 &&
           nt_ref(nt, node, "else_clause") < 0 && arm_is_held(c, nt_ref(nt, node, "statements"), t);
  if (k == NK_OrNode || k == NK_AndNode) {
    int l = nt_ref(nt, node, "left"), r = nt_ref(nt, node, "right");
    return l >= 0 && r >= 0 && comp_ntype(c, l) == t && comp_ntype(c, r) == t &&
           read_is_held(c, l) && read_is_held(c, r);
  }
  if (k == NK_CallNode) {
    int recv = nt_ref(nt, node, "receiver"), alloc = 0;
    if (recv < 0) return implicit_self_is_field_read(c, node);
    if (!read_is_held(c, recv)) return 0;
    if (call_is_field_read(c, node, &alloc)) return !alloc;
    const char *nm = nt_str(nt, node, "name");
    int a = nt_ref(nt, node, "arguments"), ac = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    if (!nm || !sp_streq(nm, "[]") || ac != 1 || nt_ref(nt, node, "block") >= 0 ||
        !(ty_is_array(comp_ntype(c, recv)) || ty_is_obj_array(comp_ntype(c, recv))) ||
        comp_ntype(c, av[0]) != TY_INT || array_index_is_own(c))
      return 0;
    if (subtree_is_pure_read(c, av[0])) return 1;
    recv = unwrap_parens(c, recv);
    NodeKind rk = nt_kind(nt, recv);
    return (rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode ||
            rk == NK_GlobalVariableReadNode || rk == NK_ClassVariableReadNode) &&
           !read_rebound_by(c, recv, av[0]);
  }
  return 0;
}
/* Is `node` a typed Array that something else holds while sp_typed_to_poly
   boxes it? One a root reaches (read_is_held), or an Array literal, which
   sits in a rooted temp. Any other source is boxed by
   sp_typed_to_poly_unheld. */
int typed_array_src_held(Compiler *c, int node) {
  node = unwrap_parens(c, node);
  if (node < 0) return 0;
  return read_is_held(c, node) || nt_kind(c->nt, node) == NK_ArrayNode;
}
/* The proc-form clone of scope `s`, or -1. Made in analyze (make_yield_proc_forms):
   a second scope named "<name>#pf" on the same class, holding an independently
   typed copy of the body whose yields answer poly. */
int scope_proc_form_of(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return -1;
  Scope *sc = &c->scopes[s];
  if (!sc->name || !sc->yields) return -1;
  char pfname[192];
  snprintf(pfname, sizeof pfname, "%s#pf", sc->name);
  int pi = sc->class_id < 0 ? comp_method_index(c, pfname)
         : (sc->is_cmethod ? comp_cmethod_in_class : comp_method_in_class)(c, sc->class_id, pfname);
  if (pi < 0 || !c->scopes[pi].is_proc_form) return -1;
  return pi;
}
/* Is proc form `s` one some call can reach? The poly dispatch that names it
   is emitted after the reachability pass, so the clone's own flag cannot say;
   but its source method is reached by name wherever a call could take the
   clone, and a source nothing calls leaves the clone dead, with callees the
   pass rightly left out: emitted anyway, it named functions that were never
   written (#5117). */
int proc_form_live(Compiler *c, int s) {
  Scope *pf = &c->scopes[s];
  if (!pf->is_proc_form || pf->reachable) return 1;
  if (pf->class_id < 0) return 1;
  int si = proc_form_source(c, s);
  return si < 0 || c->scopes[si].reachable;
}
/* The method proc form `s` was cloned from, or -1. */
int proc_form_source(Compiler *c, int s) {
  Scope *pf = &c->scopes[s];
  const char *nm = pf->name;
  const char *h = nm ? strstr(nm, "#pf") : NULL;
  if (!pf->is_proc_form || !h) return -1;
  char src[192];
  size_t n = (size_t)(h - nm);
  if (n >= sizeof src) return -1;
  memcpy(src, nm, n); src[n] = 0;
  if (pf->class_id < 0) return comp_method_index(c, src);
  return (pf->is_cmethod ? comp_cmethod_in_class : comp_method_in_class)(c, pf->class_id, src);
}
/* Is `new` call `id`, which runs yielding initialize `initm`, a step of a
   cycle of constructors (mark_ctor_cycles)? Its own scope, or the method a
   clone holding it was made from, is on the same cycle as initm. */
int ctor_site_on_cycle(Compiler *c, int id, int initm) {
  if (initm < 0 || initm >= c->nscopes || !c->scopes[initm].ctor_cycle) return 0;
  int s = id >= 0 && id < c->nt->count ? c->nscope[id] : -1;
  if (s >= 0 && s < c->nscopes && c->scopes[s].is_proc_form) s = proc_form_source(c, s);
  return s >= 0 && s < c->nscopes && c->scopes[s].ctor_cycle == c->scopes[initm].ctor_cycle;
}
int scope_needs_proc_form(Compiler *c, int s) {
  return scope_proc_form_of(c, s) >= 0;
}
/* A module whose instance methods a TOP-LEVEL `include` makes callable: the
   bare-call path emits a direct call to the module's own function, so that
   function has to exist even though including the module into a class also
   copied it away (#3795). */
/* Whether method `mi`'s own body reads or writes an instance variable, in any
   form: a multiple-assignment target and `&&=` included. The top-level
   include path asks it twice, to inline and to refuse, and the two lists it
   replaced drifted apart once already. */
int scope_uses_ivars(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  for (int nid = 0; nid < nt->count; nid++) {
    if (c->nscope[nid] != mi) continue;
    switch (nt_kind(nt, nid)) {
      case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode:
      case NK_InstanceVariableOperatorWriteNode: case NK_InstanceVariableOrWriteNode:
      case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableTargetNode:
        return 1;
      default: break;
    }
  }
  return 0;
}

int scope_toplevel_included(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  if (sc->is_cmethod || sc->class_id < 0) return 0;
  for (int i = 0; i < c->ntoplevel_includes; i++)
    if (c->toplevel_includes[i] == sc->class_id) return 1;
  return 0;
}

int scope_has_callable_symbol(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  return sc->reachable && !sc->yields &&
         (!sc->is_transplanted_source || scope_toplevel_included(c, s)) &&
         !scope_is_shadowed(c, s);
}
/* What a keyword flag's value says at compile time: 1 for a literal Ruby
   treats as true (true, a number, a String, a Symbol, a container -- any
   literal but false and nil), 0 for false or nil, -1 when only the run time
   knows. A flag such as `chomp:` was read as true for the TrueNode kind
   alone, so `chomp: 1` did not chomp. */
int kw_flag_static(Compiler *c, int node) {
  const char *t = node >= 0 ? nt_type(c->nt, node) : NULL;
  if (!t) return 0;
  if (sp_streq(t, "FalseNode") || sp_streq(t, "NilNode")) return 0;
  if (sp_streq(t, "TrueNode") || sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
      sp_streq(t, "StringNode") || sp_streq(t, "InterpolatedStringNode") ||
      sp_streq(t, "SymbolNode") || sp_streq(t, "ArrayNode") || sp_streq(t, "HashNode") ||
      sp_streq(t, "RegularExpressionNode") || sp_streq(t, "RangeNode"))
    return 1;
  return -1;
}
/* The C truth of a keyword flag into `out`: "1"/"0" from kw_flag_static, or
   the run-time test in parentheses. An absent flag is "0". */
void emit_kw_flag(Compiler *c, int node, Buf *out) {
  int f = kw_flag_static(c, node);
  if (f >= 0) { buf_printf(out, "%d", f); return; }
  buf_puts(out, "("); emit_cond(c, node, out); buf_puts(out, ")");
}
int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) {
      const char *kn = nt_str(nt, key, "value");
      if (kn && sp_streq(kn, name)) return nt_ref(nt, els[i], "value");
    }
  }
  return -1;
}
int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  if (t == TY_FLOAT_RANGE) return 6;
  if (t == TY_STR_RANGE) return 7;
  return 0;
}
/* The program the class tests below read its reopenings from (set by
   codegen_program): ty_matches_class takes no Compiler, and is asked in
   emitters all through the code generator. */
const Compiler *g_tmc_c = NULL;
/* Does class index k include program module `mod`, directly or through a
   module it includes? */
/* The modules class index k includes, by the `include` statements of its
   bodies as written (a module with no methods transplants nothing, so the
   analysis's included_mods misses it) and by what the analysis recorded. */
static int tmc_body_includes(const Compiler *c, int k, int mod) {
  const NodeTable *nt = c->nt;
  const char *kn = c->classes[k].name, *mn = c->classes[mod].name;
  if (!kn || !mn) return 0;
  static const NodeKind HK[] = { NK_ClassNode, NK_ModuleNode };
  for (int h = 0; h < 2; h++)
    for (int m = comp_kind_first((Compiler *)c, HK[h]); m >= 0; m = comp_kind_next((Compiler *)c, m)) {
      if (nt_kind(nt, m) != HK[h]) continue;
      int cp = nt_ref(nt, m, "constant_path");
      const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      if (!cn || !sp_streq(cn, kn)) continue;
      int body = nt_ref(nt, m, "body"), bn = 0;
      const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int i = 0; i < bn; i++) {
        if (nt_kind(nt, bb[i]) != NK_CallNode || nt_ref(nt, bb[i], "receiver") >= 0) continue;
        const char *cl = nt_str(nt, bb[i], "name");
        if (!cl || !sp_streq(cl, "include")) continue;
        int args = nt_ref(nt, bb[i], "arguments"), an = 0;
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        for (int a = 0; a < an; a++) {
          NodeKind ak = nt_kind(nt, av[a]);
          const char *an2 = ak == NK_ConstantReadNode || ak == NK_ConstantPathNode ? nt_str(nt, av[a], "name") : NULL;
          if (an2 && sp_streq(an2, mn)) return 1;
        }
      }
    }
  return 0;
}
static int tmc_includes(const Compiler *c, int k, int mod, int depth) {
  if (k < 0 || k >= c->nclasses || depth > 16) return 0;
  if (tmc_body_includes(c, k, mod)) return 1;
  const ClassInfo *ci = &c->classes[k];
  for (int m = 0; m < ci->nincluded_mods; m++) {
    int x = ci->included_mods[m];
    if (x == mod || tmc_includes(c, x, mod, depth + 1)) return 1;
  }
  /* through a module this class includes by statement */
  for (int x = 0; x < c->nclasses; x++)
    if (x != k && x != mod && comp_class_is_module((Compiler *)c, (ClassInfo *)&c->classes[x]) &&
        tmc_body_includes(c, k, x) && tmc_includes(c, x, mod, depth + 1)) return 1;
  return 0;
}
/* Is program module `cn` among the ancestors of the builtin class `self_cls`
   by way of a program reopening of it or of a builtin ancestor (`class Hash;
   include DeepMergeable; end`, `class Numeric; include M; end`, `module
   Enumerable; include M; end`)? */
static int tmc_prog_module_in_builtin(const char *self_cls, const char *cn) {
  const Compiler *c = g_tmc_c;
  if (!c || !cn) return 0;
  int mod = comp_class_index((Compiler *)c, cn);
  if (mod < 0 || !comp_class_is_module((Compiler *)c, &c->classes[mod])) return 0;
  const char *chain[8]; int n = 0;
  chain[n++] = self_cls;
  if (sp_streq(self_cls, "Integer") || sp_streq(self_cls, "Float") ||
      sp_streq(self_cls, "Complex") || sp_streq(self_cls, "Rational")) chain[n++] = "Numeric";
  if (sp_streq(self_cls, "Array") || sp_streq(self_cls, "Hash") || sp_streq(self_cls, "Range") ||
      sp_streq(self_cls, "Enumerator")) chain[n++] = "Enumerable";
  if (sp_streq(self_cls, "String") || sp_streq(self_cls, "Symbol") || sp_streq(self_cls, "Time") ||
      n > 1 && sp_streq(chain[1], "Numeric")) chain[n++] = "Comparable";
  chain[n++] = "Object";
  chain[n++] = "Kernel";
  for (int i = 0; i < n; i++) {
    int k = comp_class_index((Compiler *)c, chain[i]);
    if (k >= 0 && tmc_includes(c, k, mod, 0)) return 1;
  }
  return 0;
}
/* Is program module `mod` mixed into some builtin class (or builtin
   module) by a reopening of it, directly or through another module? */
int builtin_reopen_includes_module(Compiler *c, int mod) {
  if (mod < 0 || mod >= c->nclasses || !comp_class_is_module(c, &c->classes[mod])) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    const char *n = c->classes[k].name;
    if (!n || builtin_class_id(n) == 0) continue;
    if (tmc_includes(c, k, mod, 0)) return 1;
  }
  return 0;
}
int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  switch (t) {
  case TY_STRING: case TY_STRBUF: self_cls = "String"; break;
  case TY_INT: case TY_BIGINT: self_cls = "Integer"; break;
  case TY_FLOAT: self_cls = "Float"; break;
  case TY_SYMBOL: self_cls = "Symbol"; break;
  case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: self_cls = "Range"; break;
  case TY_NIL: self_cls = "NilClass"; break;
  case TY_BOOL: self_cls = "Boolean"; break; /* true/false split handled at call site */
  case TY_FIBER: self_cls = "Fiber"; break;
  case TY_THREAD: self_cls = "Thread"; break;
  case TY_QUEUE: self_cls = "Queue"; break;
  case TY_MUTEX: self_cls = "Mutex"; break;
  case TY_CONDVAR: self_cls = "ConditionVariable"; break;
  case TY_ENUMERATOR: self_cls = "Enumerator"; break;
  case TY_TIME: self_cls = "Time"; break;
  case TY_COMPLEX: self_cls = "Complex"; break;
  case TY_RATIONAL: self_cls = "Rational"; break;
  case TY_REGEX: self_cls = "Regexp"; break;
  case TY_MATCHDATA: self_cls = "MatchData"; break;
  case TY_PROC: self_cls = "Proc"; break;
  case TY_RANDOM: self_cls = "Random"; break;
  case TY_IO: self_cls = "IO"; break;
  default: break;
  }
  if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  if (!self_cls) return -1;
  if (sp_streq(cn, self_cls)) return 1;
  if (exact) return 0;
  if (is_object_root(cn)) return 1;
  if (sp_streq(cn, "Comparable") && (t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
                                     t == TY_FLOAT || t == TY_SYMBOL || t == TY_TIME ||
                                     t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Numeric") && (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
                                  t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE ||
                                     t == TY_ENUMERATOR)) return 1;
  /* a program module the program mixed into this builtin by reopening it */
  if (tmc_prog_module_in_builtin(self_cls, cn)) return 1;
  return 0;
}

/* `if (frozen) raise FrozenError` guard preceding an ivar store on a
   freeze-observed class ("can't modify frozen <Name>: <inspect>"); emits
   nothing when instances of the class are never frozen. `selfexpr` is a C
   expression for the instance pointer, evaluated twice (bind a temp first
   when it has effects). */
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b) {
  if (cid < 0 || cid >= c->nclasses) return;
  if (!c->classes[cid].freeze_observed || c->classes[cid].is_value_type) return;
  const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
  /* an Array subclass instance is frozen as its Array is (#7449); a Hash
     subclass instance, as its Hash is, by the header bit */
  if (c->classes[cid].ary_root > 0 && !c->classes[cid].ary_hash) buf_printf(b, "if ((%s)->ary.frozen) ", selfexpr);
  else buf_printf(b, "if (sp_gc_is_frozen((void *)%s)) ", selfexpr);
  buf_printf(b,
      "sp_raise_frozen_obj(sp_box_obj((void *)%s, %d), (&(\"\\xff\" \"can't modify frozen %s\")[1])); ",
      selfexpr, cid, rn);
}

/* `_t<tmp>` when the node was already evaluated into that temp, or the
   node itself when tmp is -1. */
void emit_node_or_tmp(Compiler *c, int node, int tmp, Buf *b) {
  if (tmp >= 0) buf_printf(b, "_t%d", tmp);
  else emit_expr(c, node, b);
}
/* Root a variable whose C type came from a TyKind. A boxed-poly variable is
   an sp_RbVal, whose first word is a tag rather than a pointer, so it has to
   be rooted through the rbval macro -- rooting it as a raw pointer hands the
   mark walker a small integer and segfaults under GC pressure. Sites that
   emit a temp from a type the inference chose keep getting this wrong one at
   a time, so they go through here; a site that names its variable takes the
   `_var` form, and one that emits around the root asks `ty_gc_rootable`
   first. */
void emit_gc_root_var(Compiler *c, TyKind t, const char *name, Buf *b) {
  if (!ty_gc_rootable(c, t)) return;
  /* a String slot takes the string form: a builder's buffer (marker 0xfd) is
     kept alive by the handle in front of it, which only sp_mark_string
     reaches; the object form's header walk skips the buffer, and reads a
     freed one once the handle is gone (see emit_local_decl) */
  if (t == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(%s);", name);
  else if (t == TY_STRING) buf_printf(b, "SP_GC_ROOT_STR(%s);", name);
  else buf_printf(b, "SP_GC_ROOT(%s);", name);
}
/* stmt_may_rebind_local's walk over one statement: each local name the
   subtree at `id` writes, targets or takes as a block or method parameter
   goes into names[], each node reached is marked as the statement's, and the
   answer is 1 when it binds a local it cannot name: a numbered parameter, or
   a parameter-like node of a kind the node table does not list (an `it`
   parameter) with no name. A listed node is named by its kind; an unlisted
   one that carries a name counts as binding it. */
static int stmt_wr_walk(Compiler *c, int stmt, int id, const char ***names, int *n, int *cap) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  if (id < c->stmt_wr_cap) c->stmt_wr_mark[id] = stmt;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_NumberedParametersNode) return 1;
  int lv_bind = k == NK_LocalVariableWriteNode || k == NK_LocalVariableOperatorWriteNode ||
                k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
                k == NK_LocalVariableTargetNode;
  int param = k == NK_RequiredParameterNode || k == NK_OptionalParameterNode ||
              k == NK_RestParameterNode || k == NK_KeywordRestParameterNode ||
              k == NK_OptionalKeywordParameterNode || k == NK_BlockParameterNode;
  if (k == NK_NONE) {
    const char *ty = nt_type(nt, id);
    if (!ty) return 0;
    if (nt_str(nt, id, "name")) param = 1;
    else if (strstr(ty, "Parameter")) return 1;
  }
  if (lv_bind || param) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm && lv_bind) return 1;
    if (nm) {
      if (*n + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *names = realloc(*names, sizeof **names * (size_t)*cap);
        if (!*names) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      (*names)[(*n)++] = nm;
    }
  }
  int opaque = 0;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) opaque |= stmt_wr_walk(c, stmt, nt_ref_at(nt, id, i), names, n, cap);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int k = 0;
    const int *ids = nt_arr_at(nt, id, i, &k);
    for (int j = 0; j < k; j++) opaque |= stmt_wr_walk(c, stmt, ids[j], names, n, cap);
  }
  return opaque;
}

/* See codegen_internal.h. The statement's names are collected on its first
   question and kept, so each statement is walked once however many temps
   ask about it. */
int stmt_may_rebind_local(Compiler *c, int stmt, int read, const char *name) {
  if (stmt < 0 || read < 0 || !name) return 1;
  int need = c->nt->count;
  if (stmt >= need || read >= need) return 1;
  if (need > c->stmt_wr_cap) {
    int cap = need + need / 2 + 64;
    c->stmt_wr_state = realloc(c->stmt_wr_state, (size_t)cap);
    c->stmt_wr_names = realloc(c->stmt_wr_names, sizeof *c->stmt_wr_names * (size_t)cap);
    c->stmt_wr_mark = realloc(c->stmt_wr_mark, sizeof *c->stmt_wr_mark * (size_t)cap);
    if (!c->stmt_wr_state || !c->stmt_wr_names || !c->stmt_wr_mark) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = c->stmt_wr_cap; i < cap; i++) { c->stmt_wr_state[i] = 0; c->stmt_wr_names[i] = NULL; c->stmt_wr_mark[i] = -1; }
    c->stmt_wr_cap = cap;
  }
  if (!c->stmt_wr_state[stmt]) {
    const char **names = NULL; int n = 0, ncap = 0;
    int opaque = stmt_wr_walk(c, stmt, stmt, &names, &n, &ncap);
    if (!names) names = calloc(1, sizeof *names);
    else names[n] = NULL;
    c->stmt_wr_names[stmt] = names;
    c->stmt_wr_state[stmt] = opaque ? 2 : 1;
  }
  if (c->stmt_wr_state[stmt] == 2) return 1;
  /* a read outside the statement (an inlined body spliced into it) is
     bound by code the walk did not see */
  if (c->stmt_wr_mark[read] != stmt) return 1;
  for (const char **q = c->stmt_wr_names[stmt]; q && *q; q++)
    if (sp_streq(*q, name)) return 1;
  return 0;
}

void emit_gc_root_tmp(Compiler *c, TyKind t, int tmp, Buf *b) {
  char name[24]; snprintf(name, sizeof name, "_t%d", tmp);
  emit_gc_root_var(c, t, name, b);
}
/* Whether a variable of this kind takes a root at all. A value-type object
   lives in the variable itself, not behind it: rooting one hands the mark
   walker the struct's first field. */
int ty_gc_rootable(Compiler *c, TyKind t) {
  return needs_root(t) && !comp_ty_value_obj(c, t);
}
/* Whether a temp of this kind holds a reference a collection has to see:
   one ty_gc_rootable takes, or one of the by-value kinds that carry
   Strings inside -- a String Range's two ends, a value object's String
   fields (a keyword value keeps the by-value layout). declare_local_named
   roots a local of those kinds field by field. */
int ty_gc_holds_refs(Compiler *c, TyKind t) {
  if (ty_gc_rootable(c, t) || t == TY_STR_RANGE) return 1;
  if (!comp_ty_value_obj(c, t)) return 0;
  ClassInfo *vc = &c->classes[ty_object_class(t)];
  for (int i = 0; i < vc->nivars; i++)
    if (vc->ivar_types[i] == TY_STRING) return 1;
  return 0;
}
/* Root a temp of a kind ty_gc_holds_refs answers: as emit_gc_root_tmp, or
   each String a by-value kind carries, the way declare_local_named roots
   a local of that kind. */
/* The roots a String Range by value adds, built --share-strings, beyond its
   two endpoint strings: the handles it keeps (#8321). `ref` is the struct. */
void emit_srange_handle_roots(Compiler *c, const char *ref, Buf *b) {
  if (repr_share_rule(c)) buf_printf(b, " SP_GC_ROOT(%s.hf); SP_GC_ROOT(%s.hl);", ref, ref);
}
/* The mark lines of a String Range slot by value (`ref` the slot: an
   object's ivar, a global, a constant, a class variable), which the walker
   cannot follow: its two endpoint strings and, built --share-strings, the
   handles it keeps (#8321). */
void emit_srange_marks(Compiler *c, const char *ref, Buf *b) {
  buf_printf(b, "  sp_mark_string(%s.first);\n", ref);
  buf_printf(b, "  sp_mark_string(%s.last);\n", ref);
  if (repr_share_rule(c)) buf_printf(b, "  sp_gc_mark(%s.hf);\n  sp_gc_mark(%s.hl);\n", ref, ref);
}
void emit_gc_root_tmp_refs(Compiler *c, TyKind t, int tmp, Buf *b) {
  if (t == TY_STR_RANGE) {
    buf_printf(b, "SP_GC_ROOT_STR(_t%d.first); SP_GC_ROOT_STR(_t%d.last);", tmp, tmp);
    char ref[32]; snprintf(ref, sizeof ref, "_t%d", tmp);
    emit_srange_handle_roots(c, ref, b);
    return;
  }
  if (!comp_ty_value_obj(c, t)) { emit_gc_root_tmp(c, t, tmp, b); return; }
  ClassInfo *vc = &c->classes[ty_object_class(t)];
  const char *sep = "";
  for (int i = 0; i < vc->nivars; i++)
    if (vc->ivar_types[i] == TY_STRING) {
      buf_printf(b, "%sSP_GC_ROOT(_t%d.iv_%s);", sep, tmp, iv_c(vc->ivars[i] + 1));
      sep = " ";
    }
}

/* An arm that hoists its receiver into `_tN` and then evaluates arguments
   holds the receiver in nothing while they run, nor while the call itself
   allocates. This renders the receiver as the initialiser of `_tN` and
   follows it with a root on `_tN`, so the receiver is held until the arm's
   statement expression ends. When this receiver node is in the override
   table (g_argov_node), the operand-order rewrite or an arm that
   re-dispatches through a temp has already declared and rooted the temp the
   receiver renders as, in front of the call, and a second root would only
   repeat it. */
void emit_recv_rooted(Compiler *c, int recv, int t, const char *rootm, Buf *b) {
  emit_expr(c, recv, b);
  int bound = 0;
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == recv) bound = 1;
  if (bound) buf_puts(b, "; ");
  else buf_printf(b, "; %s(_t%d); ", rootm, t);
}

/* A compound literal is not a GC root. Evaluate and root each operand
   before the next one's setup or value can allocate; only the held values
   go into the array passed to the builtin. The caller owns the scope. With
   `tmps`, each operand's temp goes there, and one the nil arm's head ran is
   read from the head's temp (head_held_read), or boxed from it with no root
   of its own (head_held_box); without, the temps are first + i. */
int emit_rooted_arg_list(Compiler *c, const int *argv, int argc,
                         const char *ctype, const char *root,
                         void (*emit)(Compiler *, int, Buf *), int *tmps, Buf *b) {
  int first = g_tmp + 1;
  g_tmp += argc;
  for (int i = 0; i < argc; i++) {
    Buf pre = {0}, val = {0};
    emit_split_pre(c, argv[i], emit, &pre, &val);
    if (pre.len) buf_puts(b, pre.p);
    int held = tmps ? head_held_read(c, argv[i], val.p) : -1;
    if (tmps) tmps[i] = held >= 0 ? held : first + i;
    if (held < 0 && tmps && emit == emit_boxed && head_held_box(c, argv[i]))
      buf_printf(b, "%s _t%d = %s; ", ctype, first + i, val.p);
    else if (held < 0)
      buf_printf(b, "%s _t%d = %s; %s(_t%d); ", ctype, first + i, val.p, root, first + i);
    free(pre.p); free(val.p);
  }
  return first;
}

/* Boxed key-list builtins also hold the receiver before their keys. */
void emit_rooted_key_call(Compiler *c, const char *fn, const char *recv,
                          const int *argv, int argc, Buf *b) {
  int tr = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d); ", tr, recv, tr);
  int *ta = calloc((size_t)argc + 1, sizeof *ta);
  if (!ta) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  emit_rooted_arg_list(c, argv, argc, "sp_RbVal", "SP_GC_ROOT_RBVAL", emit_boxed, ta, b);
  buf_printf(b, "%s(_t%d, %d, (sp_RbVal[]){", fn, tr, argc);
  for (int i = 0; i < argc; i++) buf_printf(b, "%s_t%d", i ? ", " : "", ta[i]);
  if (!argc) buf_puts(b, "sp_box_nil()");
  buf_puts(b, "}); })");
  free(ta);
}

void emit_main_exit(Buf *b) {
  if (g_uses_threads) buf_puts(b, "sp_sched_drain(); ");
  buf_puts(b, "_sp_main_rc = sp_at_exit_run(0); return; }\n");
}

void emit_retf_return(int eid, int has_retval, Buf *b) {
  if (g_c_ret_void && g_ret_type == TY_UNKNOWN) { buf_printf(b, "if (_retf%d) { ", eid); emit_main_exit(b); }
  /* A fiber body is `static void`: returning the value there is a C
     constraint violation (GCC 14 rejects it), and the value had nowhere to
     go anyway -- a void function's caller cannot read it. Drop it and
     return, which is what the generated code already did in practice. */
  else if (has_retval && g_c_ret_void) buf_printf(b, "if (_retf%d) return;\n", eid);
  else if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d;\n", eid, eid);
  else if (g_in_proc_body && g_result_var && g_result_poly)
    buf_printf(b, "if (_retf%d) { %s = sp_box_nil(); return 0; }\n", eid, g_result_var);
  else if (g_c_ret_void) buf_printf(b, "if (_retf%d) return;\n", eid);
  else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil();\n", eid);
  else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0;\n", eid);
  /* a proc body's C function returns sp_int (the value rides
     _sp_proc_poly_ret), so a bare `return;` there is a mismatch the other
     way -- and leaves the returned int indeterminate. Its tail returns 0. */
  else if (g_in_proc_body) buf_printf(b, "if (_retf%d) return 0;\n", eid);
  else buf_printf(b, "if (_retf%d) return;\n", eid);
}

/* ---- The arity ArgumentError: one builder for every binder ----
   CRuby words a positional count its callee cannot take the same way
   whichever path bound the call (rb_arity_error_new): "wrong number of
   arguments (given G, expected E)", E being `N` for an exact count, `N..M`
   with optionals and `N+` past a rest -- and for a callee with required
   keywords argument_arity_error (vm_args.c) appends "; required keyword:
   k" / "; required keywords: k, j", naming every one of them whether the
   call supplied it or not. Each binder spelled its own copy, and the
   copies drifted: the splat and gather counts, lambdas and the poly arms
   left the keywords out. The counts the compiler knows are written here;
   those only the run time knows go through sp_raise_arity (lib/sp_exc.c),
   which takes the same suffix. max < 0 is no upper bound. */
void arity_expected(char *out, size_t n, int min, int max) {
  if (min == max) snprintf(out, n, "%d", min);
  else if (max < 0) snprintf(out, n, "%d+", min);
  else snprintf(out, n, "%d..%d", min, max);
}
void arity_message(char *out, size_t n, int given, int min, int max, const char *kw) {
  char exp[48];
  arity_expected(exp, sizeof exp, min, max);
  snprintf(out, n, "wrong number of arguments (given %d, expected %s%s)", given, exp, kw ? kw : "");
}
static int param_is_required_kw(const NodeTable *nt, int p) {
  const char *t = nt_type(nt, p);
  return t && sp_streq(t, "RequiredKeywordParameterNode");
}
/* The required-keyword suffix of a parameter list: a def's ParametersNode or
   a block's BlockParametersNode. "" when it has none. */
void arity_kw_suffix(const NodeTable *nt, int params, char *out, size_t n) {
  out[0] = 0;
  if (params >= 0 && nt_kind(nt, params) == NK_BlockParametersNode)
    params = nt_ref(nt, params, "parameters");
  if (params < 0) return;
  int nk = 0; const int *kws = nt_arr(nt, params, "keywords", &nk);
  int nreq = 0;
  for (int i = 0; i < nk; i++) if (param_is_required_kw(nt, kws[i])) nreq++;
  if (!nreq) return;
  size_t off = (size_t)snprintf(out, n, "; required keyword%s: ", nreq > 1 ? "s" : "");
  for (int i = 0, first = 1; i < nk && off < n; i++) {
    if (!param_is_required_kw(nt, kws[i])) continue;
    /* a block parameter's rename suffix (`k__bp3`) is ours, not the name */
    const char *kn = nt_str(nt, kws[i], "name");
    if (!kn) kn = "?";
    int kl = block_param_is_renamed(kn) ? (int)block_param_written_len(kn) : (int)strlen(kn);
    off += (size_t)snprintf(out + off, n - off, "%s%.*s", first ? "" : ", ", kl, kn);
    first = 0;
  }
}
/* The suffix of method (or block) scope `m`, read off its own definition. */
void scope_arity_kw_suffix(Compiler *c, const Scope *m, char *out, size_t n) {
  out[0] = 0;
  if (!m || m->def_node < 0) return;
  arity_kw_suffix(c->nt, nt_ref(c->nt, m->def_node, "parameters"), out, n);
}
/* The raise of a count only the run time knows, the C expression `given`,
   as one statement. */
void emit_arity_raise(Buf *b, const char *given, int min, int max, const char *kw) {
  buf_printf(b, "sp_raise_arity(%s, %d, %d, ", given, min, max);
  if (kw && kw[0]) buf_printf(b, "\"%s\")", kw);
  else buf_puts(b, "NULL)");
}
/* The check and raise of such a count, as one statement. */
void emit_arity_check(Buf *b, const char *given, int min, int max, const char *kw) {
  buf_printf(b, "sp_arity_check(%s, %d, %d, ", given, min, max);
  if (kw && kw[0]) buf_printf(b, "\"%s\")", kw);
  else buf_puts(b, "NULL)");
}
/* Does method (or block) scope `m` say `**nil`? It refuses any keyword --
   CRuby's "no keywords accepted", ahead of its count -- where a method
   declaring no keyword parameter would take them as a positional Hash. */
int scope_refuses_keywords(Compiler *c, const Scope *m) {
  /* A `def w(...)` the __fwd_N model binds (a yielding parent reached by
     `super(...)` or `new(...)`) hands every keyword on to its target, which
     refuses them when it says `**nil`: the forwarder refuses them for it,
     as its synthesized keyword params would carry them nowhere. */
  for (int hops = 0; m && m->fwd_target1 > 0 && hops < 32; hops++) m = &c->scopes[m->fwd_target1 - 1];
  if (!m || m->def_node < 0) return 0;
  int pn = nt_ref(c->nt, m->def_node, "parameters");
  if (pn >= 0 && nt_kind(c->nt, pn) == NK_BlockParametersNode) pn = nt_ref(c->nt, pn, "parameters");
  int kr = pn >= 0 ? nt_ref(c->nt, pn, "keyword_rest") : -1;
  return kr >= 0 && nt_type(c->nt, kr) && sp_streq(nt_type(c->nt, kr), "NoKeywordsParameterNode");
}

/* ---- The keyword ArgumentErrors, one builder beside the arity one ----
   CRuby names the keywords a call gets wrong in one message each way
   (argument_kw_error, vm_args.c): "missing keyword: :a" / "missing
   keywords: :a, :b" for the required ones the call leaves out, and then
   "unknown keyword(s): ..." for the keys naming no parameter, every one as
   its #inspect writes it. `names` is that list, joined by ", ". The run
   time spells the same through sp_raise_kw_error. */
void kw_error_message(char *out, size_t n, const char *kind, int count, const char *names) {
  snprintf(out, n, "%s keyword%s: %s", kind, count > 1 ? "s" : "", names);
}
/* Appends one name, already inspected, to such a list. */
void kw_names_add(char *list, size_t n, int *count, const char *inspected) {
  size_t l = strlen(list);
  if (l < n) snprintf(list + l, n - l, "%s%s", *count ? ", " : "", inspected);
  (*count)++;
}
/* An identifier as a Symbol names one: a letter, `_` or a byte of a
   non-ASCII character first, then those or digits, and with `suffix` one
   trailing `?`, `!` or `=`. */
static int sym_ident(const char *s, int suffix) {
  unsigned char ch = (unsigned char)*s;
  if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch >= 0x80)) return 0;
  for (s++; (ch = (unsigned char)*s) != 0; s++)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
          ch == '_' || ch >= 0x80)) break;
  if (suffix && (*s == '?' || *s == '!' || *s == '=')) s++;
  return *s == 0;
}

/* Does Symbol#inspect show `name` bare, as `:name`? The run time's rule
   (sp_sym_simple_p), which kw_key_inspect's messages have to match: an
   identifier with an optional trailing `?`, `!` or `=`, a `$`, `@` or `@@`
   name, or an operator method's name; anything else #inspect quotes. An
   operator was quoted, so `f(a: 1, "+": 2)` raised `unknown keyword: :"+"`
   where CRuby says `:+`. */
int sym_name_plain(const char *s) {
  if (!s || !*s) return 0;
  if (*s == '$') return sym_ident(s + 1, 0);
  if (*s == '@') return sym_ident(s + (s[1] == '@' ? 2 : 1), 0);
  if (sym_ident(s, 1)) return 1;
  static const char *const ops[] = {
    "+", "-", "*", "/", "%", "**", "==", "===", "!=", "=~", "!~",
    "<", "<=", ">", ">=", "<=>", "<<", ">>", "&", "|", "^", "~",
    "!", "+@", "-@", "[]", "[]=", "`", NULL };
  for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return 1;
  return 0;
}
/* A literal key as #inspect writes it: a Symbol `:name`, quoted where the
   name is no identifier (`:"a b"`), a String quoted, with the escapes
   String#inspect makes. */
void kw_key_inspect(const char *kn, int is_sym, char *out, size_t n) {
  size_t o = 0;
  int quote = !is_sym || !sym_name_plain(kn);
  o += (size_t)snprintf(out + o, n - o, "%s%s", is_sym ? ":" : "", quote ? "\"" : "");
  for (const unsigned char *p = (const unsigned char *)kn; *p && o < n; p++) {
    const char *esc = NULL; char hx[8];
    switch (*p) {
      case '"': esc = quote ? "\\\"" : NULL; break;
      case '\\': esc = "\\\\"; break;
      case '\n': esc = "\\n"; break;
      case '\t': esc = "\\t"; break;
      case '\r': esc = "\\r"; break;
      case '\f': esc = "\\f"; break;
      case '\v': esc = "\\v"; break;
      case '\b': esc = "\\b"; break;
      case '\a': esc = "\\a"; break;
      case 0x1b: esc = "\\e"; break;
      case '#': esc = (p[1] == '{' || p[1] == '$' || p[1] == '@') ? "\\#" : NULL; break;
      default:
        if (*p < 0x20 || *p == 0x7f) { snprintf(hx, sizeof hx, "\\x%02X", *p); esc = hx; }
    }
    if (esc) o += (size_t)snprintf(out + o, n - o, "%s", esc);
    else out[o++] = (char)*p;
  }
  if (o < n) snprintf(out + o, n - o, "%s", quote ? "\"" : "");
  else out[n - 1] = 0;
}

/* An emitter asked to write into g_pre itself, after a partial line
   (`buf_printf(g_pre, "sp_int _t3 = "); emit_int_expr(c, v, g_pre)`): what
   the expression hoists would land in the middle of that line -- an
   argument through a method with a default (`with_index(w(1))`), a value
   stored through a setter -- and the C did not build. The value is emitted
   into a side buffer, the hoisted statements are spliced in where the
   line starts, and the value is appended to the line. */
void emit_into_pre_line(Compiler *c, void (*fn)(Compiler *, int, Buf *), int node) {
  Buf *pre = g_pre;
  size_t ls = pre->len;
  while (ls > 0 && pre->p[ls - 1] != '\n') ls--;
  Buf val; memset(&val, 0, sizeof val);
  Buf hoist; memset(&hoist, 0, sizeof hoist);
  g_pre = &hoist;
  fn(c, node, &val);
  g_pre = pre;
  if (hoist.len > 0) {
    size_t tail = pre->len - ls;
    char *saved = malloc(tail + 1);
    memcpy(saved, pre->p + ls, tail); saved[tail] = 0;
    pre->len = ls; pre->p[ls] = 0;
    buf_puts(pre, hoist.p);
    if (pre->len > 0 && pre->p[pre->len - 1] != '\n') buf_puts(pre, "\n");
    buf_putn(pre, saved, tail);
    free(saved);
  }
  if (val.p) buf_puts(pre, val.p);
  free(val.p); free(hoist.p);
}

/* ---- nil out of band: sp_oint / sp_ofloat (codegen_internal.h) ---- */

int oint_kind(TyKind t) { return t == TY_INT || t == TY_FLOAT; }
const char *oint_ctype(TyKind t) { return t == TY_FLOAT ? "sp_ofloat" : "sp_oint"; }
const char *oint_nil(TyKind t)   { return t == TY_FLOAT ? "sp_ofloat_nil()" : "sp_oint_nil()"; }
const char *oint_of(TyKind t)    { return t == TY_FLOAT ? "sp_ofloat_of" : "sp_oint_of"; }
const char *oint_val(TyKind t)   { return t == TY_FLOAT ? "sp_ofloat_val" : "sp_oint_val"; }
const char *oint_arg(TyKind t)   { return t == TY_FLOAT ? "sp_ofloat_arg" : "sp_oint_arg"; }
const char *oint_box(TyKind t)   { return t == TY_FLOAT ? "sp_box_ofloat" : "sp_box_oint"; }
const char *oint_unbox(TyKind t) { return t == TY_FLOAT ? "sp_unbox_ofloat" : "sp_unbox_oint"; }

/* A local's slot holds its nil beside the value when the analysis says a
   write can leave nil in it (nullable_int), when only its boxing has to
   answer nil (box_nullable, a parameter bound from an unset ivar), when a
   call passes a literal nil to the proc parameter (nil_passed), or when a
   read can run before any write (or_written / maybe_unset: the slot starts
   nil). A parameter is bound at entry, so the last two do not apply. */
int slot_is_oint(const LocalVar *lv) {
  if (!lv || !oint_kind(lv->type)) return 0;
  /* the receiver parameter of a builtin written in Ruby (`__int_digits(self,
     ...)`, enumerable.rb's inline frames): a nil receiver is a NoMethodError
     naming the method, raised where the call unwraps it (sp_oint_val), so
     the slot itself is plain */
  if (lv->is_param && lv->name && sp_streq(lv->name, "__self")) return 0;
  if (lv->nullable_int || lv->box_nullable || lv->nil_passed) return 1;
  return (lv->or_written || lv->maybe_unset) && !lv->is_param && !lv->is_block_param;
}

void emit_slot_ctype(Compiler *c, const LocalVar *lv, Buf *b) {
  if (slot_is_oint(lv)) buf_puts(b, oint_ctype(lv->type));
  else emit_ctype(c, lv->type, b);
}

/* An instance ivar carries its nil as a byte of the object's iv__nilb (1:
   nil) when a write can leave nil in it (ivar_nullable_int) or initialize
   does not assign it (an unset ivar reads nil). A byte of its own, not a bit
   of a shared word: a store to one ivar's nil is a plain byte store, with no
   read-modify-write of the word its neighbours' bits share. A subclass
   shares its parent's ivar indexes (inherit_members keeps the prefix), and
   the bytes are numbered over the ivar indexes the whole hierarchy keeps a
   nil for (class_nilbyte_map), so the array sits at the same offset and with
   the same numbering in every struct of it and is no longer than it needs. */
static int class_root(Compiler *c, int cid);
static int ivar_has_nilbit_own(Compiler *c, int cid, int iv);
/* A field is one slot down its class family (a subclass lays the parent's
   fields first): a nil bit any class of the family keeps for it, every
   class keeps, so a parent's method that writes the field clears the bit a
   subclass's constructor seeded */
int ivar_has_nilbit(Compiler *c, int cid, int iv) {
  if (cid < 0 || cid >= c->nclasses) return 0;
  ClassInfo *ci = &c->classes[cid];
  if (iv < 0 || iv >= ci->nivars || !oint_kind(ci->ivar_types[iv])) return 0;
  if (ivar_has_nilbit_own(c, cid, iv)) return 1;
  int root = class_root(c, cid);
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid) continue;
    ClassInfo *ck = &c->classes[k];
    if (iv >= ck->nivars || !ck->ivars[iv] || !ci->ivars[iv] || strcmp(ck->ivars[iv], ci->ivars[iv]) != 0) continue;
    if (class_root(c, k) != root) continue;
    if (ivar_has_nilbit_own(c, k, iv)) return 1;
  }
  return 0;
}
/* Class#allocate runs no initialize, so every field of the instance it
   makes starts nil: the classes some `allocate` can make (a constant
   receiver names its class and the subclasses; any other receiver, every
   class). Memo per node table: -1 none, -2 every class, else a bitmap. */
static int class_is_allocated(Compiler *c, int cid) {
  static const NodeTable *memo_nt = NULL;
  static int memo_n = 0, any = 0;
  static unsigned char *named = NULL;
  const NodeTable *nt = c->nt;
  if (memo_nt != nt || memo_n != c->nclasses) {
    memo_nt = nt; memo_n = c->nclasses; any = 0;
    free(named);
    named = (unsigned char *)calloc((size_t)(memo_n > 0 ? memo_n : 1), 1);
    if (!named) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    NT_FOREACH_KIND(nt, NK_CallNode, u) {
      const char *nm = nt_str(nt, u, "name");
      if (!nm || !sp_streq(nm, "allocate")) continue;
      int r = nt_ref(nt, u, "receiver");
      int rc = r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode ? comp_class_index(c, nt_str(nt, r, "name")) : -1;
      if (rc >= 0) named[rc] = 1; else any = 1;
    }
  }
  if (any) return 1;
  for (int k = cid, hop = 0; k >= 0 && hop < 64; k = c->classes[k].parent, hop++)
    if (named[k]) return 1;
  return 0;
}
static int ivar_has_nilbit_own(Compiler *c, int cid, int iv) {
  if (cid < 0 || cid >= c->nclasses) return 0;
  ClassInfo *ci = &c->classes[cid];
  if (iv < 0 || iv >= ci->nivars || !oint_kind(ci->ivar_types[iv])) return 0;
  if (ci->ivar_nullable_int && ci->ivar_nullable_int[iv]) return 1;
  if (class_is_allocated(c, cid)) return 1;
  /* a Struct / Data member is assigned by the generated constructor */
  if ((ci->is_struct || ci->is_data) && iv < ci->nmembers) return 0;
  /* in a class whose presence the program reads (presence_family), an ivar
     whose presence is read off its value (ivs_kind 1: one a module's
     initialize between super calls leaves unset, as well as one initialize
     never assigns) reads nil until written */
  if (c->pres_seen && ci->presence_family > 0 && ivs_kind(c, cid, ci->ivars[iv]) == 1) return 1;
  return !ivar_assigned_in_initialize(c, cid, ci->ivars[iv]);
}
static int class_root(Compiler *c, int cid) {
  int hop = 0;
  while (cid >= 0 && c->classes[cid].parent >= 0 && hop++ < 64) cid = c->classes[cid].parent;
  return cid;
}
/* A class family's nil bytes: the ivar indexes some class of the family
   keeps a nil byte for, numbered in order. Every struct of the family gets
   the same numbering, so a byte sits at one offset across it whichever class
   writes it. Memo per family root: map[iv] is the byte (-1: none), n its
   length; count the bytes. */
typedef struct { int *map; int n; int count; } NilByteMap;
static const NilByteMap *class_nilbyte_map(Compiler *c, int cid) {
  static NilByteMap *memo = NULL;
  static int memo_n = -1;
  static const Compiler *memo_c = NULL;
  static const NilByteMap none = { NULL, 0, 0 };
  if (cid < 0 || cid >= c->nclasses) return &none;
  if (memo_n != c->nclasses || memo_c != c) {
    if (memo) for (int i = 0; i < memo_n; i++) free(memo[i].map);
    free(memo);
    memo_n = c->nclasses; memo_c = c;
    memo = (NilByteMap *)calloc((size_t)(memo_n > 0 ? memo_n : 1), sizeof(NilByteMap));
    if (!memo) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < memo_n; i++) memo[i].count = -1;
  }
  int root = class_root(c, cid);
  NilByteMap *m = &memo[root];
  if (m->count >= 0) return m;
  int maxn = 0;
  for (int k = 0; k < c->nclasses; k++)
    if (class_root(c, k) == root && c->classes[k].nivars > maxn) maxn = c->classes[k].nivars;
  m->n = maxn;
  m->map = (int *)malloc(sizeof(int) * (size_t)(maxn > 0 ? maxn : 1));
  if (!m->map) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int iv = 0; iv < maxn; iv++) m->map[iv] = -1;
  int count = 0;
  for (int iv = 0; iv < maxn; iv++) {
    for (int k = 0; k < c->nclasses; k++) {
      if (class_root(c, k) != root || iv >= c->classes[k].nivars) continue;
      if (ivar_has_nilbit(c, k, iv)) { m->map[iv] = count++; break; }
    }
  }
  m->count = count;
  return m;
}
int ivar_nilbit_index(Compiler *c, int cid, int iv) {
  const NilByteMap *m = class_nilbyte_map(c, cid);
  return iv >= 0 && iv < m->n ? m->map[iv] : -1;
}
int class_nilbyte_count(Compiler *c, int cid) { return class_nilbyte_map(c, cid)->count; }
void ivar_nilbit_test(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap) {
  int k = ivar_nilbit_index(c, cid, iv);
  if (k < 0) { snprintf(out, cap, "(0)"); return; }   /* no nil byte: never nil */
  snprintf(out, cap, "(%siv__nilb[%d])", obj, k);
}
void ivar_nilbit_set(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap) {
  int k = ivar_nilbit_index(c, cid, iv);
  if (k < 0) { snprintf(out, cap, "(void)0"); return; }
  snprintf(out, cap, "%siv__nilb[%d] = 1", obj, k);
}
void ivar_nilbit_clear(Compiler *c, int cid, int iv, const char *obj, char *out, size_t cap) {
  int k = ivar_nilbit_index(c, cid, iv);
  if (k < 0) { snprintf(out, cap, "(void)0"); return; }
  snprintf(out, cap, "%siv__nilb[%d] = 0", obj, k);
}
/* The nil byte takes the flag `nil` (0 or 1, as an oint's .nil is): one byte
   store, with no branch between setting and clearing it */
void ivar_nilbit_assign(Compiler *c, int cid, int iv, const char *obj, const char *nil, char *out, size_t cap) {
  int k = ivar_nilbit_index(c, cid, iv);
  if (k < 0) { snprintf(out, cap, "(void)0"); return; }
  snprintf(out, cap, "%siv__nilb[%d] = %s", obj, k, nil);
}

/* The ivar an InstanceVariableReadNode / write names, as the read emitter
   resolves it (emit_ivar_cvar_gvar_expr): the object's field of class
   `*cid` (kind 1), or the class-level / top-level static civ_C_x (kind 2);
   0 when the class has no such slot. */
int ivar_node_slot(Compiler *c, int node, int *cid, int *iv) {
  HolderRef h;
  *cid = *iv = -1;
  Scope *cs = comp_scope_of(c, node);
  const char *nm = nt_str(c->nt, node, "name");
  if (!nm) return 0;
  /* inside an instance_eval / instance_exec splice: the receiver's class
     (the analysis's map for the node, else the splice being emitted) */
  int iec = ie_class_of(c, node);
  if (iec < 0 && cs && cs->class_id < 0 && !cs->is_cmethod) iec = g_ie_class_id;
  if (iec >= 0) {
    for (int k = iec; k >= 0; k = c->classes[k].parent) {
      int i = comp_ivar_index(&c->classes[k], nm);
      if (i >= 0) { *cid = k; *iv = i; return 1; }
    }
    return 0;
  }
  /* a class body outside any def: the class-level static */
  if (cs && cs->class_id < 0 && !cs->is_cmethod && g_class_body_id >= 0) {
    int i = comp_ivar_index(&c->classes[g_class_body_id], nm);
    if (i < 0) return 0;
    *cid = g_class_body_id; *iv = i; return 2;
  }
  if (!holder_of_node_in(c, node, g_class_body_id, &h) || h.idx < 0) return 0;
  *cid = h.cid; *iv = h.idx;
  return h.cls_slot ? 2 : 1;
}
/* The head of a Range walk, `for (VAR = first; ...; VAR += step)`, whose
   bounds may sit at the ends of sp_int: `i >= last` with last at INTPTR_MIN
   never ended, and `i += 1` past INTPTR_MAX overflowed. The test runs ahead
   of the step on the distance left, in unsigned arithmetic, and the step is
   not taken past the last value. `first`, `step` and `last` (the inclusive
   end) are C texts read once each iteration; `decl` declares VAR in the
   head, else the guard is declared on its own line first. */
void emit_range_walk_head(Buf *b, int indent, const char *var, int decl, const char *first,
                          const char *step, const char *last) {
  int g = ++g_tmp;
  if (!decl) { emit_indent(b, indent); buf_printf(b, "sp_int _g%d = (%s > 0 ? %s <= %s : %s >= %s);\n", g, step, first, last, first, last); emit_indent(b, indent); }
  buf_printf(b, "for (%s%s = %s", decl ? "sp_int " : "", var, first);
  if (decl) buf_printf(b, ", _g%d = (%s > 0 ? %s <= %s : %s >= %s)", g, step, var, last, var, last);
  buf_printf(b, "; _g%d; _g%d = (%s > 0 ? (uintptr_t)(%s) - (uintptr_t)(%s) >= (uintptr_t)(%s)"
                " : (uintptr_t)(%s) - (uintptr_t)(%s) >= (uintptr_t)0 - (uintptr_t)(%s)), %s = _g%d ? %s + %s : %s) {\n",
             g, g, step, last, var, step, var, last, step, var, g, var, step, var);
}

/* An Integer literal as a C constant. INT64_MIN has no literal of its own:
   `-9223372036854775808LL` negates a constant too wide for long long (C reads
   it unsigned, a -Werror), so it is spelled as the expression. */
void emit_int_lit(Buf *b, long long v) {
  if (v == INT64_MIN) buf_puts(b, "(-9223372036854775807LL - 1)");
  else buf_printf(b, "%lldLL", v);
}

int ivar_read_slot_is_oint(Compiler *c, int node) {
  int cid, iv;
  int kind = ivar_node_slot(c, node, &cid, &iv);
  if (kind == 1) return ivar_has_nilbit(c, cid, iv);
  if (kind == 2) return civ_is_oint(c, cid, iv);
  return 0;
}

int gvar_seeded_before_read(Compiler *c, const char *gname);   /* analyze_internal.h */
/* A global starts nil and keeps it until its first write: its static is an
   sp_oint when a read can see that nil (not seeded before every read) or a
   write leaves nil in it. A class-level or top-level ivar static starts
   nil too, and nothing seeds it: always an oint. A class variable read
   before its write is a NameError in Ruby, so only a nil write makes one. */
int gvar_is_oint(Compiler *c, const LocalVar *g) {
  if (!g || !oint_kind(g->type)) return 0;
  if (g->nullable_int) return 1;
  return !gvar_seeded_before_read(c, g->name);
}
int civ_is_oint(Compiler *c, int cid, int iv) {
  if (cid < 0 || cid >= c->nclasses || iv < 0 || iv >= c->classes[cid].nivars) return 0;
  return oint_kind(c->classes[cid].ivar_types[iv]);
}
int cvar_is_oint(Compiler *c, int cid, int idx) {
  if (cid < 0 || cid >= c->nclasses || idx < 0 || idx >= c->classes[cid].ncvars) return 0;
  ClassInfo *ci = &c->classes[cid];
  return oint_kind(ci->cvar_types[idx]) && ci->cvar_nullable_int && ci->cvar_nullable_int[idx];
}

/* The reader method `nm` resolved on class cid: the ivar it answers, when
   that ivar carries a nil bit (its read is an sp_oint). */
static int reader_ivar_has_nilbit(Compiler *c, int cid, const char *nm) {
  int defc = -1;
  if (cid < 0 || cid >= c->nclasses || !nm || !comp_reader_in_chain(c, cid, nm, &defc)) return 0;
  char ivb[300];
  snprintf(ivb, sizeof ivb, "@%s", comp_resolve_alias(c, cid, nm));
  int k = defc >= 0 ? defc : cid;
  int iv = comp_ivar_index(&c->classes[k], ivb);
  return iv >= 0 && ivar_has_nilbit(c, k, iv);
}

/* the number of parent hops from class cid up to class k (a large value
   when k is not an ancestor) */
static int class_chain_depth(Compiler *c, int cid, int k) {
  int d = 0;
  for (int x = cid; x >= 0 && x < c->nclasses; x = c->classes[x].parent, d++) if (x == k) return d;
  return 1 << 20;
}

/* A call on an object of class `cid` dispatches to `mi` or to a subclass's
   own `nm`: can one of them answer nil -- its answer is an oint, or it
   answers nil itself (`def nop = nil` beside an override answering 7)? The
   dispatch switch then holds the oint (g_disp_ro). */
static int dispatch_answers_nil(Compiler *c, int cid, const char *nm, int mi) {
  if (method_ret_is_oint(&c->scopes[mi])) return 1;
  int any_nil = c->scopes[mi].ret == TY_NIL || c->scopes[mi].ret == TY_VOID, overridden = 0;
  int nd = 0; const int *ds = comp_descendants(c, cid, &nd);
  for (int k = 0; k < nd; k++) {
    int odef = -1, omi = comp_method_in_chain(c, ds[k], nm, &odef);
    if (omi < 0 || omi == mi || odef != ds[k]) continue;
    overridden = 1;
    if (method_ret_is_oint(&c->scopes[omi])) return 1;
    if (c->scopes[omi].ret == TY_NIL || c->scopes[omi].ret == TY_VOID) any_nil = 1;
  }
  return overridden && any_nil;
}

int file_stat_nil_call(Compiler *c, int v);   /* analyze.c */
static int node_is_oint_asked(Compiler *c, int node);
/* Codegen asks this while a view overrides a node's type (a re-entered
   call on its receiver boxed), and the analysis it asks (nullable_int_value)
   re-derives a receiver's type: asked as a pure read, as repr_of asks
   (an_pure_read_begin), so the derived type is not recorded over the
   view's. */
int node_is_oint(Compiler *c, int node) {
  an_pure_read_begin();
  int r = node_is_oint_asked(c, node);
  an_pure_read_end();
  return r;
}
static int node_is_oint_asked(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NilNode) return 1;
  TyKind t = comp_ntype(c, node);
  if (!oint_kind(t)) return 0;
  switch (k) {
  case NK_LocalVariableReadNode: {
    /* the slot's own oint, unless a nil guard narrowed this read */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && slot_is_oint(lv) && nullable_int_value(c, node);
  }
  case NK_InstanceVariableReadNode: {
    /* inside an instance_eval / instance_exec splice whose receiver's class
       has no such ivar (or a receiver that is no object): the read is nil
       there, whatever the analysis says of the name elsewhere */
    if (oint_kind(comp_ntype(c, node))) {
      if (g_ie_nil_ivars) return 1;
      Scope *ics = comp_scope_of(c, node);
      int iec = ie_class_of(c, node);
      if (iec < 0 && ics && ics->class_id < 0 && !ics->is_cmethod) iec = g_ie_class_id;
      int icid, iiv;
      if (iec >= 0 && !ivar_node_slot(c, node, &icid, &iiv)) return 1;
    }
    return nullable_int_value(c, node);
  }
  case NK_ClassVariableReadNode:
  case NK_GlobalVariableReadNode:
    return nullable_int_value(c, node);
  case NK_LocalVariableAndWriteNode: {
    /* `x &&= v` answers the slot: its oint where the slot holds its nil
       (the untaken arm answers the nil the slot held) */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && slot_is_oint(lv);
  }
  case NK_InstanceVariableWriteNode:
    /* `@x = v` as an expression answers the slot it wrote: its oint where
       the field carries a nil bit (or the static is an oint); the `||=` /
       `&&=` / `op=` value forms answer by the analysis (default) */
    return ivar_read_slot_is_oint(c, node);
  case NK_GlobalVariableWriteNode:
  case NK_GlobalVariableOrWriteNode:
  case NK_GlobalVariableAndWriteNode:
  case NK_GlobalVariableOperatorWriteNode: {
    /* a global's write answers its static: the oint where that is one */
    const char *gn = nt_str(nt, node, "name");
    const char *grn = gn && gn[0] == '$' ? comp_resolve_gvar(c, gn + 1) : gn;
    LocalVar *g = grn ? comp_gvar(c, grn) : NULL;
    return g && gvar_is_oint(c, g);
  }
  case NK_ClassVariableWriteNode:
  case NK_ClassVariableOrWriteNode:
  case NK_ClassVariableAndWriteNode:
  case NK_ClassVariableOperatorWriteNode: {
    HolderRef h;
    if (!holder_of_node_in(c, node, g_class_body_id, &h)) return 0;
    return cvar_is_oint(c, h.cid, h.idx);
  }
  case NK_IndexOrWriteNode:
  case NK_IndexAndWriteNode:
    /* `h[k] ||= v`: the slot read with its nil (the `&&=` arm keeps it) */
    return nullable_int_value(c, node);
  case NK_ParenthesesNode: {
    /* `(expr)` is its expression's form */
    int u = unwrap_parens(c, node);
    return u != node && u >= 0 ? node_is_oint(c, u) : nullable_int_value(c, node);
  }
  case NK_CallOrWriteNode:
  case NK_CallAndWriteNode: {
    /* `o.x ||= v` / `o.x &&= v` answers what the reader answers on the arm
       that does not assign: the reader's oint when the reader has one (a
       method written out answering its nil, an attr reader over a nil bit) */
    int r = nt_ref(nt, node, "receiver");
    const char *nm = nt_str(nt, node, "name");
    TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    if (r < 0 || !nm || !ty_is_object(rt)) return nullable_int_value(c, node);
    int cid = ty_object_class(rt), rmi = -1;
    int rk = comp_resolve_member(c, cid, nm, 0, NULL, &rmi);
    if (rk == SP_MEMBER_METHOD && rmi >= 0) return method_ret_is_oint(&c->scopes[rmi]);
    /* an attr's `||=` stores a nil field's replacement and answers it: nil
       only where the value can be */
    if (rk == SP_MEMBER_ATTR && k == NK_CallOrWriteNode) {
      int v = nt_ref(nt, node, "value");
      return v >= 0 && (nt_kind(nt, v) == NK_NilNode || node_has_oint_form(c, v));
    }
    if (rk == SP_MEMBER_ATTR) return reader_ivar_has_nilbit(c, cid, nm);
    return nullable_int_value(c, node);
  }
  case NK_CallNode: {
    /* `r&.m`: nil when r is -- except on the safe-navigation emitter's
       re-entry for the same node (g_sn_skip), which emits the plain call on
       the guarded receiver and wraps it itself */
    const char *sop = nt_str(nt, node, "call_operator");
    if (sop && sp_streq(sop, "&.") && g_sn_skip != node) {
      /* nil when the receiver is -- only where a guard stands (a receiver
         that cannot be nil, `3&.fdiv(2)`, has neither guard nor nil) */
      if (sn_guard_pending(c, node)) return 1;
    }
    const char *nm = nt_str(nt, node, "name");
    if (!nm) return 0;
    /* a File::Stat field and File.size?: the runtime answers the oint */
    if (file_stat_nil_call(c, node)) return 1;
    /* `o.x = v` as a value answers v, whatever the writer returns */
    if (call_is_setter_assign(nt, node)) {
      int sa = nt_ref(nt, node, "arguments"), san = 0;
      const int *sav = sa >= 0 ? nt_arr(nt, sa, "arguments", &san) : NULL;
      if (sav && san >= 1) return nt_kind(nt, sav[san - 1]) == NK_NilNode || nullable_int_value(c, sav[san - 1]);
    }
    int blk = nt_ref(nt, node, "block");
    int r = nt_ref(nt, node, "receiver");
    TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    int a2 = nt_ref(nt, node, "arguments"), an2 = 0;
    if (a2 >= 0) nt_arr(nt, a2, "arguments", &an2);
    /* `x.then { ... }` answers its block: a nil tail or a `next` that can
       hand nil makes it nil */
    if ((sp_streq(nm, "then") || sp_streq(nm, "yield_self")) && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      if (bn > 0 && (node_has_oint_form(c, bs[bn - 1]) || nt_kind(nt, bs[bn - 1]) == NK_NilNode)) return 1;
      return bb >= 0 && block_next_may_be_nil(c, bb, 0);
    }
    /* `a[i] = v` on an Integer / Float array answers v as it was stored:
       with its nil (a nil, a boxed value, an oint) -- emit_array_call's
       store; so does `h[k] = v` on an Integer-valued typed Hash, which
       stores it through _oset (D3b-ii) */
    if (sp_streq(nm, "[]=") && r >= 0 && an2 >= 2 &&
        (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH)) {
      int sa = nt_ref(nt, node, "arguments"), san = 0;
      const int *sav = nt_arr(nt, sa, "arguments", &san);
      int lv = sav[san - 1];
      return nt_kind(nt, lv) == NK_NilNode || repr_of(c, lv).kind == RK_BOXED || node_has_oint_form(c, lv);
    }
    /* `x.tap { ... }` answers its receiver, in the receiver's form */
    if (sp_streq(nm, "tap") && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && r >= 0 && oint_kind(rt)) return node_is_oint(c, r);
    /* unary `+` hands an Integer / Float its operand: the operand's form */
    if (sp_streq(nm, "+@") && an2 == 0 && r >= 0 && oint_kind(rt)) return node_is_oint(c, r);
    /* a proc's result comes back boxed and is unboxed with its nil */
    if (is_call_or_yield(nm) && r >= 0 && rt == TY_PROC) return 1;
    /* `o.instance_eval { ... }` spliced over an object answers its body's
       last expression in that expression's form */
    TyKind iert = rt;
    if (r < 0) { Scope *ies = comp_scope_of(c, node); if (ies && ies->class_id >= 0 && !ies->is_cmethod) iert = ty_object(ies->class_id); }
    /* over a boxed receiver the splice runs per class, and a receiver with no
       such ivar (nil, a builtin, Object.new) reads it nil: an Integer / Float
       tail can be nil -- so too over a typed receiver that is no object of
       the program's (nil, a number, a String) */
    if ((sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec")) && blk >= 0 && r >= 0 &&
        (rt == TY_POLY || (rt != TY_UNKNOWN && !ty_is_object(rt))) && nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      return bn > 0 && oint_kind(comp_ntype(c, bs[bn - 1]));
    }
    if ((sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec")) && blk >= 0 &&
        ty_is_object(iert) && comp_method_in_chain(c, ty_object_class(iert), nm, NULL) < 0 &&
        nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      return (bn > 0 && node_is_oint(c, bs[bn - 1])) || (bb >= 0 && block_next_may_be_nil(c, bb, 0));
    }
    /* a method the program defines, resolved as the call emitter resolves
       it: its C function answers an oint iff method_ret_is_oint */
    int mi = -1;
    /* the builtin a reopened IO class overrides, emitted as that builtin
       (emit_io_builtin_call): the builtin's own rules below */
    int io_bi = g_io_skip_reopen && g_io_skip_node == node;
    if (io_bi) ;
    else if (r < 0) mi = comp_self_call_mi(c, node, nm);
    else if (ty_is_object(rt)) {
      int cid = ty_object_class(rt);
      int mdef = -1, rdef = -1;
      mi = comp_method_in_chain(c, cid, nm, &mdef);
      /* an attr reader is its ivar's read: the ivar's oint when the ivar
         carries a nil bit, the plain field otherwise (whatever the reader's
         own return mark says: the field has no other form) -- unless a
         method written out nearer in the chain overrides the reader */
      if (an2 == 0 && blk < 0 && comp_reader_in_chain(c, cid, nm, &rdef) &&
          (mi < 0 || (mdef != cid &&
                      class_chain_depth(c, cid, rdef < 0 ? cid : rdef) <= class_chain_depth(c, cid, mdef < 0 ? cid : mdef))))
        return reader_ivar_has_nilbit(c, cid, nm);
    }
    else if (nt_kind(nt, r) == NK_ConstantReadNode) {
      int rci = comp_class_index(c, nt_str(nt, r, "name"));
      if (rci >= 0) mi = comp_cmethod_in_chain(c, rci, nm, NULL);
    }
    /* a builtin value's method the program reopened its class with
       (`class Symbol; def mark = ...`) */
    if (mi < 0 && !io_bi && r >= 0 && rt != TY_POLY && rt != TY_UNKNOWN && !ty_is_object(rt))
      mi = comp_builtin_kind_reopen_mi(c, rt, nm);
    /* a method whose return widened past the call's (a yielding method
       answering its block, typed per call site): the call's own analysis */
    if (mi >= 0 && (c->scopes[mi].ret == TY_POLY || c->scopes[mi].ret == TY_UNKNOWN)) return nullable_int_value(c, node);
    /* the dispatch to a subclass's override, on an object receiver or on
       an instance method's self */
    if (mi >= 0 && oint_kind(comp_ntype(c, node))) {
      int dcid = -1;
      if (r >= 0 && ty_is_object(rt)) dcid = ty_object_class(rt);
      else if (r < 0) { Scope *self = comp_scope_of(c, node); if (self && !self->is_cmethod) dcid = self->class_id; }
      if (dcid >= 0 && dispatch_answers_nil(c, dcid, nm, mi)) return 1;
    }
    if (mi >= 0) return method_ret_is_oint(&c->scopes[mi]);
    /* `<=>` answers nil for an incomparable operand: the analysis's answer
       where both sides are of one comparable kind (numbers, Strings,
       Symbols); any other pairing can answer nil at run time */
    /* a native class's method declared to answer an Integer or a Float */
    if (native_call_ret_plain_num(c, node)) return 0;
    if (sp_streq(nm, "<=>") && an2 == 1) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      TyKind at = a1a && a1n == 1 ? comp_ntype(c, a1a[0]) : TY_UNKNOWN;
      /* a builtin receiver with no `<=>` of its own (a Regexp, a Proc, a
         Queue, an exception) takes a reopened Object's: its answer's form */
      if (r >= 0 && rt != TY_POLY && rt != TY_UNKNOWN && !ty_is_object(rt) && !oint_kind(rt) &&
          rt != TY_BIGINT && rt != TY_RATIONAL && rt != TY_STRING && rt != TY_SYMBOL && rt != TY_TIME &&
          !ty_is_array(rt)) {
        int oc = comp_class_index(c, "Object");
        int omi = oc >= 0 ? comp_method_in_chain(c, oc, nm, NULL) : -1;
        if (omi >= 0) return method_ret_is_oint(&c->scopes[omi]);
      }
      int num_l = rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT || rt == TY_RATIONAL;
      int num_r = at == TY_INT || at == TY_FLOAT || at == TY_BIGINT || at == TY_RATIONAL;
      /* a Float side can be NaN; a non-literal String side can be nil */
      int same = (num_l && num_r && rt != TY_FLOAT && at != TY_FLOAT) ||
                 (rt == TY_STRING && at == TY_STRING) || (rt == TY_SYMBOL && at == TY_SYMBOL) ||
                 (rt == TY_TIME && at == TY_TIME);
      return same ? nullable_int_value(c, node) : 1;
    }
    /* `s[0]` / `s[:a]` / `s["a"]` / `s.dig(:a)` on a Struct or Data: the
       member's read, with its nil where the member carries a nil bit */
    if ((sp_streq(nm, "[]") || sp_streq(nm, "dig")) && an2 == 1 && r >= 0 && ty_is_object(rt)) {
      int scid = ty_object_class(rt);
      ClassInfo *sci = &c->classes[scid];
      if (sci->is_struct || sci->is_data) {
        int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
        const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
        int mix = -1;
        if (a1a && a1n == 1) {
          NodeKind ak = nt_kind(nt, a1a[0]);
          if (ak == NK_IntegerNode && !sp_streq(nm, "dig")) {
            long long ix = nt_int(nt, a1a[0], "value", 0);
            if (ix < 0) ix += sci->nmembers;
            if (ix >= 0 && ix < sci->nmembers) mix = (int)ix;
          }
          else if (ak == NK_SymbolNode || ak == NK_StringNode) {
            const char *mn = ak == NK_SymbolNode ? nt_str(nt, a1a[0], "value") : nt_str(nt, a1a[0], "content");
            char ivb[300]; snprintf(ivb, sizeof ivb, "@%s", mn ? mn : "");
            int iv = mn ? comp_ivar_index(sci, ivb) : -1;
            if (iv >= 0 && iv < sci->nmembers) mix = iv;
          }
        }
        if (mix >= 0) return ivar_has_nilbit(c, scid, mix);
      }
    }
    /* `o.instance_variable_get(:@x)` on a typed object: the field's read,
       with its nil where the field carries a nil bit (as an attr reader);
       `o.remove_instance_variable(:@x)` answers the same read */
    if ((sp_streq(nm, "instance_variable_get") || sp_streq(nm, "remove_instance_variable")) &&
        an2 == 1 && r >= 0 && ty_is_object(rt)) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      if (a1a && a1n == 1) {
        NodeKind ak = nt_kind(nt, a1a[0]);
        const char *sn = ak == NK_SymbolNode ? nt_str(nt, a1a[0], "value")
                       : ak == NK_StringNode ? nt_str(nt, a1a[0], "content") : NULL;
        if (sn) {
          char ivb[300]; snprintf(ivb, sizeof ivb, "%s%s", sn[0] == '@' ? "" : "@", sn);
          int cid = ty_object_class(rt);
          int iv = comp_ivar_index(&c->classes[cid], ivb);
          if (iv >= 0) return ivar_has_nilbit(c, cid, iv);
        }
      }
    }
    /* `o.instance_variable_set(:@x, v)` answers v: its form */
    if (sp_streq(nm, "instance_variable_set") && an2 == 2 && r >= 0) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      return a1a && a1n == 2 && (nt_kind(nt, a1a[1]) == NK_NilNode || node_has_oint_form(c, a1a[1]));
    }
    /* a receiver that stayed poly: the dispatch answers an oint when a
       target can answer nil (the analysis's dispatch set) -- except on the
       safe-navigation re-entry (g_sn_skip), where the analysis mark belongs
       to the `&.` as a whole and the inner emitter's own form decides */
    if ((rt == TY_POLY || (r < 0 && !ty_is_object(rt))) && g_sn_skip != node) {
      if (nullable_int_value(c, node)) return 1;
    }
    /* `Integer(x, exception: false)` / `Float(x, exception: false)`: nil on
       a failed conversion */
    if (r < 0 && (sp_streq(nm, "Integer") || sp_streq(nm, "Float")) && an2 >= 2) {
      int a2v = nt_ref(nt, node, "arguments"), a2n = 0;
      const int *a2a = a2v >= 0 ? nt_arr(nt, a2v, "arguments", &a2n) : NULL;
      if (a2a && a2n >= 2 && nt_kind(nt, a2a[a2n - 1]) == NK_KeywordHashNode) return 1;
    }
    /* a class-level attribute reader over an oint static (`Cfg.level`, or
       `level` in the class's own class method) answers the static */
    {
      int sgc = -1;
      if (r >= 0 && (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode))
        sgc = comp_class_index(c, nt_str(nt, r, "name"));
      else if (r < 0 || nt_kind(nt, r) == NK_SelfNode) {
        Scope *ss = comp_scope_of(c, node);
        if (ss && ss->is_cmethod) sgc = ss->class_id;
      }
      if (sgc >= 0 && an2 == 0 && comp_method_in_chain(c, sgc, nm, NULL) < 0) {
        ClassInfo *sgi = &c->classes[sgc];
        const char *rn = comp_resolve_alias(c, sgc, nm);
        if (!rn) rn = nm;
        if (comp_is_sg_reader(sgi, rn) && comp_is_sg_civ(sgi, rn)) {
          char ivb[300]; snprintf(ivb, sizeof ivb, "@%s", rn);
          int iv = comp_ivar_index(sgi, ivb);
          return iv >= 0 && civ_is_oint(c, sgc, iv);
        }
      }
    }
    /* a class's own methods (File.delete, IO::Buffer.size_of) are no container's */
    if (r >= 0 && (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode) &&
        !oint_kind(rt) && rt != TY_COMPLEX && rt != TY_RATIONAL && rt != TY_BIGINT &&
        !ty_is_array(rt) && !ty_is_hash(rt) && rt != TY_RANGE && rt != TY_STRING &&   /* a constant holding a number or a container (Float::INFINITY, FREE = []) is a value */
        !sp_streq(nm, "world_readable?") && !sp_streq(nm, "world_writable?"))   /* File.world_readable?(f): nil when it is not */
      return 0;
    /* the runtime functions that answer an sp_oint (RUNTIME-API.md) */
    if ((sp_streq(nm, "bsearch") || sp_streq(nm, "bsearch_index")) && blk >= 0) return 1;
    if ((sp_streq(nm, "nonzero?") && oint_kind(rt)) ||
        (sp_streq(nm, "infinite?") && (rt == TY_FLOAT || rt == TY_INT || rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_BIGINT)))
      return 1;
    /* a Method object's call runs its target: the target's own answer */
    if (r >= 0 && rt == TY_METHOD && is_method_invoke(nm)) {
      int mn = method_recv_node(c, r);
      int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      /* the direct arms (a self-less target called by name, a bound target
         called through emit_bound_method_call) answer the target's own
         form; the fn-cast arm answers the plain value */
      /* every arm (a self-less target by name, emit_bound_method_call, the
         fn cast) answers the target's own form */
      if (target >= 0) return method_ret_is_oint(&c->scopes[target]);
      return 0;
    }
    /* a search with its needle; without one the call is an arity error, no nil */
    if (rt == TY_STRING && an2 >= 1 && (sp_streq(nm, "index") || sp_streq(nm, "rindex") ||
                                        sp_streq(nm, "byteindex") || sp_streq(nm, "byterindex"))) return 1;
    /* String#getbyte past the end, IO#getbyte at end of file: nil */
    if (sp_streq(nm, "getbyte") && r >= 0 && !oint_kind(rt)) return 1;
    if (sp_streq(nm, "exitstatus") || sp_streq(nm, "termsig") ||
        sp_streq(nm, "world_readable?") || sp_streq(nm, "world_writable?")) return 1;
    if (r >= 0 && ty_is_array(rt) &&
        (sp_streq(nm, "index") || sp_streq(nm, "rindex") || sp_streq(nm, "delete_at") || sp_streq(nm, "delete") ||
         ((sp_streq(nm, "pop") || sp_streq(nm, "shift")) && an2 == 0))) return 1;
    if (r >= 0 && ty_is_hash(rt) && sp_streq(nm, "delete")) return 1;
    if (r >= 0 && rt == TY_MATCHDATA && (sp_streq(nm, "bytebegin") || sp_streq(nm, "byteend"))) return 1;
    if (is_range_bound_reader(nm) && r >= 0 &&
        (rt == TY_MATCHDATA || rt == TY_RANGE || rt == TY_FLOAT_RANGE)) return 1;
    if (r >= 0 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)) {
      /* an Integer / Float array's element answers: nil when empty or missing */
      if (an2 == 0 && blk < 0 &&
          (sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "sample") ||
           sp_streq(nm, "min") || sp_streq(nm, "max"))) return 1;
      if (sp_streq(nm, "slice!") && an2 == 1) return 1;
      if (sp_streq(nm, "find_index")) return 1;
      /* a seedless fold, a comparator min / max: nil over an empty receiver */
      if (blk >= 0 && an2 == 0 && (is_reduce_alias(nm) || sp_streq(nm, "min") || sp_streq(nm, "max"))) return 1;
    }
    if (r >= 0 && (rt == TY_RANGE || rt == TY_FLOAT_RANGE) && an2 == 0 && blk < 0 &&
        (sp_streq(nm, "min") || sp_streq(nm, "max"))) return 1;
    /* `s.unpack1("q")` past the input's end is nil */
    if (r >= 0 && sp_streq(nm, "unpack1")) return 1;
    /* a String range has no size (nil); an Enumerator's find_index / index
       answers nil on a miss */
    if (r >= 0 && rt == TY_STR_RANGE && an2 == 0 && (sp_streq(nm, "size"))) return 1;
    if (r >= 0 && rt == TY_ENUMERATOR && (sp_streq(nm, "find_index") || sp_streq(nm, "index"))) return 1;
    if (r >= 0 && ty_is_hash(rt) && sp_streq(nm, "dig") && an2 >= 1) return 1;
    /* fetch with a nil default answers that nil */
    if (r >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && sp_streq(nm, "fetch") && an2 == 2) {
      int fv = nt_ref(nt, node, "arguments"), fn = 0;
      const int *fa = fv >= 0 ? nt_arr(nt, fv, "arguments", &fn) : NULL;
      if (fa && fn == 2 && (nt_kind(nt, fa[1]) == NK_NilNode || node_is_oint(c, fa[1]))) return 1;
    }
    /* an element read that can miss, a fold or a search over a container
       that can hold nil: the analysis's answer, for a container receiver */
    if (r >= 0 && (ty_is_array(rt) || ty_is_hash(rt) || rt == TY_RANGE) &&
        (sp_streq(nm, "[]") || (sp_streq(nm, "slice") && an2 == 1) || sp_streq(nm, "at") ||
         sp_streq(nm, "fetch") || sp_streq(nm, "dig") ||
         sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "min") || sp_streq(nm, "max") ||
         sp_streq(nm, "sum") || sp_streq(nm, "sample") || sp_streq(nm, "find_index") ||
         is_find_alias(nm) || is_reduce_alias(nm)))
      return nullable_int_value(c, node);
    return 0;
  }
  default:
    return nullable_int_value(c, node);
  }
}

int g_want_oint = 0;

/* The slot a leaf read names, when that slot is an oint: the local, the
   instance ivar with a nil bit, the oint static. */
static int leaf_slot_is_oint(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_LocalVariableReadNode) {
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    return slot_is_oint(s ? scope_local(s, ln) : NULL);
  }
  if (k == NK_InstanceVariableReadNode) return ivar_read_slot_is_oint(c, node);
  if (k == NK_ClassVariableReadNode) {
    HolderRef h;
    return holder_of_node_in(c, node, g_class_body_id, &h) && cvar_is_oint(c, h.cid, h.idx);
  }
  if (k == NK_GlobalVariableReadNode) {
    const char *gn = nt_str(nt, node, "name");
    const char *rn = gn ? comp_resolve_gvar(c, gn + 1) : NULL;
    return gvar_is_oint(c, rn ? comp_gvar(c, rn) : NULL);
  }
  return 0;
}

/* the method being emitted answers an sp_oint / sp_ofloat (method_ret_is_oint
   of its scope; set beside g_ret_type) */
int g_ret_oint = 0;

/* The right-hand side of a store into ivar iv of class cid (an object
   field with a nil bit; `obj` is the receiver prefix, "self->") from node
   v, with the bit kept in step: a value that can be nil (an oint form, a
   boxed value, a nil) stores its value and copies its nil into the bit; a
   plain value clears the bit. A statement-expression whose value is the
   field's. */
void emit_ivar_value_nilbit(Compiler *c, int cid, int iv, const char *obj, int v, Buf *b) {
  TyKind t = c->classes[cid].ivar_types[iv];
  char bs[160], bc[160];
  ivar_nilbit_set(c, cid, iv, obj, bs, sizeof bs);
  ivar_nilbit_clear(c, cid, iv, obj, bc, sizeof bc);
  int to = ++g_tmp;
  if (v < 0 || nt_kind(c->nt, v) == NK_NilNode) { buf_printf(b, "({ %s; (%s)0; })", bs, c_type_name(t)); return; }
  Repr r = repr_of(c, v);
  if (node_has_oint_form(c, v) || r.kind == RK_BOXED || r.as_ty == TY_NIL || r.as_ty == TY_VOID ||
      r.as_ty == TY_UNKNOWN || (oint_kind(r.as_ty) && r.as_ty != t)) {
    buf_printf(b, "({ %s _t%d = ", oint_ctype(t), to);
    /* a boxed value into a slot an --rbs seed pins: the seed's assertion
       (nil passes) before the narrowing, as the plain slot's write has it */
    const char *ivn = c->classes[cid].ivars[iv];
    if ((r.kind == RK_BOXED || r.as_ty == TY_POLY) && ivn && class_ivar_pinned(&c->classes[cid], ivn)) {
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, v, &rb);
      Buf ck; memset(&ck, 0, sizeof ck);
      emit_rbs_checked_text(c, t, ivn, rb.p ? rb.p : "sp_box_nil()", &ck);
      buf_printf(b, "%s(%s)", oint_unbox(t), ck.p ? ck.p : "sp_box_nil()");
      free(rb.p); free(ck.p);
    }
    else if (r.as_ty == TY_UNKNOWN) { buf_printf(b, "%s(", oint_unbox(t)); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_oint_expr(c, v, t, b);
    char nf[32], ba[200];
    snprintf(nf, sizeof nf, "_t%d.nil", to);
    ivar_nilbit_assign(c, cid, iv, obj, nf, ba, sizeof ba);
    buf_printf(b, "; %s; _t%d.v; })", ba, to);
    return;
  }
  buf_printf(b, "({ %s _t%d = ", c_type_name(t), to);
  emit_coerce(c, v, t, CO_HOLD, "an instance variable write", b);
  buf_printf(b, "; %s; _t%d; })", bc, to);
}
/* the same for a value already rendered as an sp_oint / sp_ofloat text */
void emit_ivar_text_nilbit(Compiler *c, int cid, int iv, const char *obj, const char *otext, Buf *b) {
  TyKind t = c->classes[cid].ivar_types[iv];
  int to = ++g_tmp;
  char nf[32], ba[200];
  snprintf(nf, sizeof nf, "_t%d.nil", to);
  ivar_nilbit_assign(c, cid, iv, obj, nf, ba, sizeof ba);
  buf_printf(b, "({ %s _t%d = %s; %s; _t%d.v; })", oint_ctype(t), to, otext, ba, to);
}

/* The right-hand side of `@x = nil` for an Integer or Float ivar named by
   write node `id`: a field with a nil bit sets it (the value is its 0), an
   oint static takes its nil, a plain field (nothing marks it) the TypeError
   a nil raises where a number is wanted. */
void emit_ivar_nil_store(Compiler *c, int id, TyKind t, Buf *b) {
  int cid, iv, k = ivar_node_slot(c, id, &cid, &iv);
  if (k == 1 && ivar_has_nilbit(c, cid, iv)) {
    char pfx[128], bs[200];
    snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref);
    ivar_nilbit_set(c, cid, iv, pfx, bs, sizeof bs);
    buf_printf(b, "({ %s; (%s)0; })", bs, c_type_name(t));
  }
  else if (k == 2 && civ_is_oint(c, cid, iv)) buf_puts(b, oint_nil(t));
  else refuse_nil_store(c, id, t, "an instance variable write");
}

/* nil out of band, the backstop: a nil, or a value whose nil is still
   beside it, must never land in a plain sp_int / sp_float slot. Where the
   analysis left such a slot unflagged the emission refuses here, at compile
   time, rather than store the 0 a plain read would take for a number. */
__attribute__((noreturn)) void refuse_nil_store(Compiler *c, int node, TyKind t, const char *where) {
  char msg[400];
  snprintf(msg, sizeof msg, "nil written into a non-nullable %s slot: %s",
           t == TY_FLOAT ? "Float" : "Integer", where ? where : "a store");
  unsupported_feature(c, node, msg);
}
/* The node's value can be nil: a nil literal, a nil-typed value, or an
   Integer / Float producer whose nil is still beside the value (node_is_oint,
   unless the node is bound to a hoisted plain temp). */
int node_may_be_nil(Compiler *c, int node) {
  if (node < 0) return 0;
  if (nt_kind(c->nt, node) == NK_NilNode) return 1;
  TyKind t = comp_ntype(c, node);
  if (t == TY_NIL) return 1;
  if (!oint_kind(t)) return 0;
  for (int i = g_n_argov - 1; i >= 0; i--)
    if (g_argov_node[i] == node) return g_argov_oint[i] && nullable_int_value(c, node);
  /* an oint producer the analysis proved never nil (`[a].first`: the
     runtime function's form, not a nil) is unwrapped at the store, which
     keeps the proof honest at run time (sp_oint_arg) */
  return node_is_oint(c, node) && nullable_int_value(c, node);
}

/* A conditional's result slot (an if / case / begin / `||` in value
   position): an Integer or Float one holds its nil when an arm can answer
   nil (node_is_oint of the conditional). The C type and the dead value. */
int cond_res_oint(Compiler *c, int id, TyKind res) {
  return oint_kind(res) && node_is_oint(c, id);
}
void emit_res_ctype(Compiler *c, TyKind res, int res_o, Buf *b) {
  if (res_o && oint_kind(res)) buf_puts(b, oint_ctype(res)); else emit_ctype(c, res, b);
}
const char *res_zero(Compiler *c, TyKind res, int res_o) {
  if (res_o && oint_kind(res)) return oint_nil(res);
  return res == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, res);
}
/* the result slot a tail statement fills answers an oint (beside
   g_result_ty / g_result_var) */
int g_result_oint = 0;

/* `@x ||= v` / `&&=` at write node `id` (ivar type t, slot text ref): the
   field's nil bit or the oint static decides the test and keeps the bit */
void emit_ivar_orw_value(Compiler *c, int id, TyKind t, int elems_handle, const char *ref, int v, int is_or,
                         const char *mark, Buf *b) {
  int cid, iv, k = oint_kind(t) ? ivar_node_slot(c, id, &cid, &iv) : 0;
  char pfx[128], nt[300];
  /* the expression's form (node_is_oint): the field beside its bit, or the
     oint static, where the value can be nil; the value otherwise */
  int want_o = oint_kind(t) && node_is_oint(c, id);
  if (k == 1 && ivar_has_nilbit(c, cid, iv)) {
    snprintf(pfx, sizeof pfx, "%s%s", g_self, g_self_deref);
    ivar_nilbit_test(c, cid, iv, pfx, nt, sizeof nt);
    if (want_o) buf_puts(b, "({ (void)(");
    emit_slot_orw_value_o(c, t, 0, ref, v, is_or, nt, cid, iv, pfx, mark, b);
    if (want_o) buf_printf(b, "); ((%s){ %s, %s != 0 }); })", oint_ctype(t), ref, nt);
  }
  else if (k == 2 && civ_is_oint(c, cid, iv)) {
    snprintf(nt, sizeof nt, "%s.nil", ref);
    if (!want_o) buf_puts(b, "(");
    emit_slot_orw_value_o(c, t, 0, ref, v, is_or, nt, -1, -1, NULL, mark, b);
    if (!want_o) buf_puts(b, ").v");
  }
  else emit_slot_orw_value(c, t, elems_handle, ref, v, is_or, mark, b);
}
/* `obj.x ||= v` through the attribute's field (class cid, ivar iv, receiver
   prefix pfx such as "_t3->") */
void emit_attr_orw_value(Compiler *c, int cid, int iv, const char *pfx, TyKind t, int elems_handle,
                         const char *ref, int v, int is_or, const char *mark, Buf *b) {
  char nt[300];
  if (oint_kind(t) && iv >= 0 && ivar_has_nilbit(c, cid, iv)) {
    ivar_nilbit_test(c, cid, iv, pfx, nt, sizeof nt);
    emit_slot_orw_value_o(c, t, 0, ref, v, is_or, nt, cid, iv, pfx, mark, b);
  }
  else emit_slot_orw_value(c, t, elems_handle, ref, v, is_or, mark, b);
}
/* The nil test of ivar iv of class cid behind `ref` ("self->iv_x", the
   field) for an or-write statement: the bit, the oint's flag, or NULL */
int ivar_orw_niltest(Compiler *c, int id, const char *ref, char *out, size_t cap, int *cid, int *iv, char *pfx, size_t pcap) {
  int k = ivar_node_slot(c, id, cid, iv);
  if (k == 1 && ivar_has_nilbit(c, *cid, *iv)) {
    snprintf(pfx, pcap, "%s%s", g_self, g_self_deref);
    ivar_nilbit_test(c, *cid, *iv, pfx, out, cap);
    return 1;
  }
  if (k == 2 && civ_is_oint(c, *cid, *iv)) { snprintf(out, cap, "%s.nil", ref); return 2; }
  return 0;
}

/* set while a value is emitted only for its effects (`p` of a nil-typed
   call): a nil read there is no unwrap */
int g_value_discarded = 0;
void emit_slot_nil_read(Compiler *c, TyKind t, Buf *b) {
  if (oint_kind(t) && g_value_discarded) { buf_puts(b, t == TY_FLOAT ? "0.0" : "0"); return; }
  if (oint_kind(t)) { buf_printf(b, "%s(%s)", oint_arg(t), oint_nil(t)); return; }
  const char *nv = nil_value(t);
  buf_puts(b, nv ? nv : default_value_from_compiler(c, t));
}

int node_has_oint_form(Compiler *c, int node) {
  return node >= 0 && (node_is_oint(c, node) || leaf_slot_is_oint(c, node));
}

void emit_oint_expr(Compiler *c, int node, TyKind t, Buf *b) {
  if (b == g_pre) {
    /* as emit_into_pre_line, with the kind threaded through */
    Buf tmp; memset(&tmp, 0, sizeof tmp);
    emit_oint_expr(c, node, t, &tmp);
    buf_puts(b, tmp.p ? tmp.p : "");
    free(tmp.p);
    return;
  }
  if (!oint_kind(t)) t = TY_INT;
  if (node < 0) { buf_puts(b, oint_nil(t)); return; }
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NilNode) { buf_puts(b, oint_nil(t)); return; }
  Repr r = repr_of(c, node);
  TyKind vt = r.as_ty;
  /* the call inside parentheses (`(table(x))`) */
  int vc = node;
  while (vc >= 0 && (nt_kind(nt, vc) == NK_ParenthesesNode || nt_kind(nt, vc) == NK_StatementsNode)) {
    int pb = nt_kind(nt, vc) == NK_ParenthesesNode ? nt_ref(nt, vc, "body") : vc;
    int pn = 0; const int *pv = pb >= 0 && nt_kind(nt, pb) == NK_StatementsNode ? nt_arr(nt, pb, "body", &pn) : NULL;
    vc = pv && pn == 1 ? pv[0] : (pb >= 0 && nt_kind(nt, pb) != NK_StatementsNode ? pb : -1);
  }
  /* a value with no C type of its own (a call on a method that answers
     nothing included): evaluated for its effect, then nil */
  if (vt == TY_NIL || vt == TY_VOID ||
      ((vt == TY_UNKNOWN || vt == TY_POLY) && vc >= 0 && nt_kind(nt, vc) == NK_CallNode && call_names_only_void_methods(c, vc))) {
    buf_puts(b, "((void)("); emit_expr(c, node, b); buf_printf(b, "), %s)", oint_nil(t));
    return;
  }
  /* a boxed value, or an untyped one (the gate's raising token): unboxed
     with its nil */
  if (r.kind == RK_BOXED || vt == TY_POLY || vt == TY_UNKNOWN) {
    Buf ub; memset(&ub, 0, sizeof ub);
    emit_expr(c, node, &ub);
    const char *ut = ub.p ? ub.p : "sp_box_nil()";
    /* a conditional whose condition folds is its live arm alone; a raise
       there has no box to open, so it runs, and the nil after it is never
       reached (as the plain tail runs it, emit_tail_value_1) */
    if (strncmp(ut, "(sp_raise_cls(", 14) == 0 || strncmp(ut, "(sp_exc_stage_key(", 18) == 0 ||
        (strncmp(ut, "((void)(", 8) == 0 && text_diverges(ut)))
      buf_printf(b, "({ (void)%s; %s; })", ut, oint_nil(t));
    else buf_printf(b, "%s(%s)", oint_unbox(t), ut);
    free(ub.p);
    return;
  }
  /* an Integer into a Float slot (or the reverse) converts the value */
  if (oint_kind(vt) && vt != t) {
    if (node_is_oint(c, node) || leaf_slot_is_oint(c, node)) {
      int tn = ++g_tmp;
      buf_printf(b, "({ %s _t%d = ", oint_ctype(vt), tn);
      emit_oint_expr(c, node, vt, b);
      buf_printf(b, "; _t%d.nil ? %s : %s((%s)_t%d.v); })", tn, oint_nil(t), oint_of(t), c_type_name(t), tn);
    }
    else {
      buf_printf(b, "%s((%s)(", oint_of(t), c_type_name(t)); emit_expr(c, node, b); buf_puts(b, "))");
    }
    return;
  }
  if (node_is_oint(c, node) || leaf_slot_is_oint(c, node)) {
    /* the bare producer: emit_expr consumes the flag for this node */
    g_want_oint = 1;
    emit_expr(c, node, b);
    g_want_oint = 0;
    return;
  }
  buf_printf(b, "%s(", oint_of(t)); emit_expr(c, node, b); buf_puts(b, ")");
}

/* Ruby truthiness of an Integer or Float node: a plain one is always
   truthy (evaluated for its effect); one with an oint form is truthy
   exactly when it is not nil. */
void emit_oint_truthy(Compiler *c, int node, TyKind t, Buf *b) {
  if (node_has_oint_form(c, node)) {
    buf_puts(b, "(!("); emit_oint_expr(c, node, t, b); buf_puts(b, ").nil)");
  }
  else { buf_puts(b, "(("); emit_expr(c, node, b); buf_puts(b, "), 1)"); }
}

/* emit_oint_unwrap_ck's op naming an arithmetic operator's right operand:
   sp_oint_opnd's coercion TypeError */
const char g_ck_opnd[] = "(operand)";

/* The oint of `node` unwrapped at once: an operator's receiver (`op`,
   sp_oint_val's NoMethodError), a strict argument (op NULL, sp_oint_arg's
   TypeError) or an operator's right operand (g_ck_opnd, sp_oint_opnd's
   "nil can't be coerced"): the unwrap around its oint. */
void emit_oint_unwrap_ck(Compiler *c, int node, TyKind t, const char *op, Buf *b) {
  Buf side; memset(&side, 0, sizeof side);
  emit_oint_expr(c, node, t, &side);
  if (op == g_ck_opnd) buf_printf(b, "%s(%s)", t == TY_FLOAT ? "sp_ofloat_opnd" : "sp_oint_opnd", side.p ? side.p : "");
  else if (op) buf_printf(b, "%s(%s, \"%s\")", oint_val(t), side.p ? side.p : "", op);
  else buf_printf(b, "%s(%s)", oint_arg(t), side.p ? side.p : "");
  free(side.p);
}

/* An Integer key of a boxed receiver's [] / []= / index op-write that can be
   nil goes to the runtime boxed (sp_poly_index_poly / sp_poly_set_poly),
   its nil with it: CRuby calls [] on the receiver before the key is
   converted, so a nil receiver's NoMethodError comes first, an Array's
   TypeError comes from the runtime, and a Hash takes nil as a key.
   Unwrapped ahead of the call (sp_oint_arg), the key's TypeError ran first
   and a Hash's nil key raised. */
int poly_int_key_boxed(Compiler *c, int key) {
  return key >= 0 && comp_ntype(c, key) == TY_INT && node_has_oint_form(c, key);
}

void emit_scalar_operand_op(Compiler *c, int node, const char *op, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (oint_kind(t) && cmp_operand_may_be_nil(c, node)) { emit_oint_unwrap_ck(c, node, t, op, b); return; }
  emit_scalar_operand(c, node, t == TY_FLOAT ? "0.0" : "0", b);
}
