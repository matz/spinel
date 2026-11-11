/* implied_calls.c -- the methods CRuby calls for a node that the program's
   text does not name (see implied_calls.h).

   The rows are generated from tools/implied_calls.yml (implied_calls.inc),
   sorted by node kind; this reads them. It is its own file because it is a
   reader of the node table, not an arm of the value-position iterator. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "implied_calls.h"
#include "analyze.h"
#include "value_pos.h"
#include "builtin_names.h"

enum { NTC_NONE, NTC_NONLITERAL, NTC_NO_ELSE, NTC_MULTI_TARGET, NTC_GVAR_PORT };
enum { NTA_SKIP, NTA_SELF, NTA_NONE };
enum { NTO_PATH, NTO_NODE, NTO_SELF, NTO_YIELDED, NTO_MATCHED, NTO_NONE };

typedef struct { const char *field; short kind; unsigned char list, chain; } NtiStep;
typedef struct {
  short kind;
  unsigned char side, cond, absent, word, nsteps;
  const char *name, *name_field;
  NtiStep step[3];
} NtiRow;

static const NtiRow nti_rows[] = {
#define NTI_PART 1
#include "implied_calls.inc"
#undef NTI_PART
};
#define NTI_ROWS ((int)(sizeof nti_rows / sizeof nti_rows[0]))

static int nti_literal_kind(NodeKind k) {
  switch (k) {
#define NTI_PART 2
#include "implied_calls.inc"
#undef NTI_PART
    return 1;
  default:
    return 0;
  }
}

static int nti_not_value_kind(NodeKind k) {
  switch (k) {
#define NTI_PART 3
#include "implied_calls.inc"
#undef NTI_PART
    return 1;
  default:
    return 0;
  }
}

/* Is node `id` a super: a call of the builtin of its def's name, when no
   ancestor of the program has the method (tools/implied_calls.yml, `super`)? */
int nt_implied_super(const NodeTable *nt, int id) {
  switch (nt_kind(nt, id)) {
#define NTI_PART 4
#include "implied_calls.inc"
#undef NTI_PART
    return 1;
  default:
    return 0;
  }
}

/* Is node n the value of a scalar literal? A sequence or parentheses stand
   for their last statement, nothing at all for nil. */
static int nti_literal(const NodeTable *nt, int n) {
  switch (vp_one(nt, n, VPS_NONE | VP_PARENS_ONLY, &n)) {
  case VP1_NODE: return nti_literal_kind(nt_kind(nt, n));
  case VP1_NIL: return 1;
  case VP1_MANY: return 0;
  }
  return 0;
}

/* A block of two or more parameters takes a yielded array apart, as do _1 and
   _2; an implicit rest (`|a, |`) is a row of its own. */
static int nti_multi_target(const NodeTable *nt, int id) {
  switch (nt_kind(nt, id)) {
  case NK_NumberedParametersNode:
    return nt_int(nt, id, "maximum", 0) >= 2;
  case NK_BlockParametersNode: {
    int params = nt_ref(nt, id, "parameters"), slots = 0, n = 0;
    if (params < 0) return 0;
    nt_arr(nt, params, "requireds", &n); slots += n;
    nt_arr(nt, params, "optionals", &n); slots += n;
    nt_arr(nt, params, "posts", &n); slots += n;
    int rest = nt_ref(nt, params, "rest");
    if (rest >= 0 && nt_kind(nt, rest) != NK_ImplicitRestNode) slots++;
    return slots >= 2;
  }
  default:
    return 0;
  }
}

static int nti_cond(const NodeTable *nt, int id, const NtiRow *r) {
  switch (r->cond) {
  case NTC_NO_ELSE: return nt_ref(nt, id, "else_clause") < 0;
  case NTC_MULTI_TARGET: return nti_multi_target(nt, id);
  case NTC_GVAR_PORT: return is_port_gvar_name(nt_str(nt, id, "name"));
  default: return 1;
  }
}

typedef struct { NtImplied *out; int max, n; const NtiRow *row; const char *name; } NtiSink;

static void nti_emit(NtiSink *s, int operand) {
  if (s->n < s->max) {
    s->out[s->n].name = s->name;
    s->out[s->n].operand = operand;
    s->out[s->n].side = s->row->side;
  }
  s->n++;
}

/* The nodes step i of row r reaches from `cur`: a node that is a value, and a
   literal only when the row allows one, is an operand. A missing field at the
   last step is the row's `absent`; a guard that does not hold, a missing
   field before the last step or an empty list say nothing. A last step that
   chains goes on from the node it reached while that is of the step's kind
   (the receivers down a call chain), each one an operand of its own, of any
   kind. */
static void nti_walk(const NodeTable *nt, int cur, int i, NtiSink *s) {
  const NtiRow *r = s->row;
  const NtiStep *st = &r->step[i];
  int last = i + 1 == r->nsteps;
  for (int hops = 0; hops < 256; hops++) {
    int n = 0, single = -1;
    const int *list;
    if (st->list) list = nt_arr(nt, cur, st->field, &n);
    else {
      single = nt_ref(nt, cur, st->field);
      list = &single;
      n = single >= 0;
    }
    if (!st->list && single < 0) {
      if (last && r->absent == NTA_SELF) nti_emit(s, NTI_SELF);
      else if (last && r->absent == NTA_NONE) nti_emit(s, NTI_NONE);
      return;
    }
    for (int k = 0; k < n; k++) {
      int node = list[k];
      if (node < 0 || (st->kind && !(last && st->chain) && nt_kind(nt, node) != (NodeKind)st->kind)) continue;
      if (!last) nti_walk(nt, node, i + 1, s);
      else if (!nti_not_value_kind(nt_kind(nt, node)) && (r->cond != NTC_NONLITERAL || !nti_literal(nt, node)))
        nti_emit(s, node);
    }
    if (!(last && st->chain) || single < 0 || nt_kind(nt, single) != (NodeKind)st->kind) return;
    cur = single;
  }
}

/* Does a node of kind k have rows? Most kinds have none: the search is for
   the few that do. */
static int nti_kind_has_rows(NodeKind k) {
  switch (k) {
#define NTI_PART 5
#include "implied_calls.inc"
#undef NTI_PART
    return 1;
  default:
    return 0;
  }
}

/* Can node `id` make an implied call: its kind has rows, or is a super */
int nt_implied_possible(const NodeTable *nt, int id) {
  return nti_kind_has_rows(nt_kind(nt, id)) || nt_implied_super(nt, id);
}

int nt_implied_calls(const NodeTable *nt, int id, NtImplied *out, int max) {
  NodeKind k = nt_kind(nt, id);
  int lo = 0, hi = NTI_ROWS;
  if (!nti_kind_has_rows(k)) return 0;
  while (lo < hi) {   /* the first row of kind k */
    int mid = (lo + hi) / 2;
    if (nti_rows[mid].kind < (int)k) lo = mid + 1;
    else hi = mid;
  }
  NtiSink s = { out, max, 0, NULL, NULL };
  for (int i = lo; i < NTI_ROWS && nti_rows[i].kind == (int)k; i++) {
    const NtiRow *r = &nti_rows[i];
    if (!nti_cond(nt, id, r)) continue;
    s.row = r;
    s.name = r->name ? r->name : nt_str(nt, id, r->name_field);
    if (!s.name) continue;
    switch (r->word) {
    case NTO_PATH: nti_walk(nt, id, 0, &s); break;
    case NTO_NODE: nti_emit(&s, id); break;
    case NTO_SELF: nti_emit(&s, NTI_SELF); break;
    case NTO_YIELDED: nti_emit(&s, NTI_YIELDED); break;
    case NTO_MATCHED: nti_emit(&s, NTI_MATCHED); break;
    default: nti_emit(&s, NTI_NONE); break;
    }
  }
  return s.n;
}

/* ---- --dump-implied-calls ---- */

/* The sides some row names `name` for (NTS_ bits; 0 when none does): the
   rows' fixed names, not a name a node holds (an operator write's operator,
   an attribute write's reader) */
unsigned nt_implied_name_sides(const char *name) {
  unsigned sides = 0;
  for (int i = 0; name && i < NTI_ROWS; i++)
    if (nti_rows[i].name && sp_streq(nti_rows[i].name, name)) sides |= nti_rows[i].side;
  return sides;
}

static void nti_dump_operand(const NodeTable *nt, int operand, FILE *out) {
  switch (operand) {
  case NTI_NONE: fputs("none", out); break;
  case NTI_SELF: fputs("self", out); break;
  case NTI_YIELDED: fputs("yielded", out); break;
  case NTI_MATCHED: fputs("matched", out); break;
  default: fprintf(out, "%s line %d", nt_type(nt, operand), (int)nt_int(nt, operand, "node_line", 0)); break;
  }
}

void implied_calls_dump(const NodeTable *nt, FILE *out) {
  NtImplied calls[64];
  for (int id = 0; id < nt->count; id++) {
    if (nt_int(nt, id, "node_file", 0) != 0 || nt_kind(nt, id) == NK_NONE) continue;
    int n = nt_implied_calls(nt, id, calls, 64);
    for (int i = 0; i < n && i < 64; i++) {
      fprintf(out, "line %d %s: %s on ", (int)nt_int(nt, id, "node_line", 0), nt_type(nt, id), calls[i].name);
      nti_dump_operand(nt, calls[i].operand, out);
      fprintf(out, " (%s)\n", calls[i].side == (NTS_INST | NTS_CLASS) ? "either" : calls[i].side == NTS_CLASS ? "class" : "inst");
    }
    if (n > 64) fprintf(out, "line %d %s: %d calls, only 64 shown\n", (int)nt_int(nt, id, "node_line", 0), nt_type(nt, id), n);
  }
}
