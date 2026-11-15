/* method_table_check.h -- --method-table-check (#10866, step 1): the reads
   of the static method tables made outside the question.

   call_target is the one place a site asks what a call runs. Every other
   answer read of the tables (the 13 primitives below) is counted here,
   once per site: the node being inferred or emitted when it was made (a
   frame, set around infer_uncached and emit_expr_node, and carried by the
   emitter's unwind state), the side and the
   name. Reads inside a bracket (mtab_enter / mtab_leave: the question
   itself, the two surveys, and the owners of the candidate sets, which ask
   every class by design) are not counted. The report names each site by
   the family of the reader (reader_family) and lists the answers of the
   question that are the table's last definition for want of a proof
   (ML_TABLE). With the flag off a primitive pays one branch, and no C
   changes. */
#ifndef SPINEL_METHOD_TABLE_CHECK_H
#define SPINEL_METHOD_TABLE_CHECK_H

#include "compiler.h"

extern int g_method_table_check;   /* --method-table-check */

/* the table primitives (compiler.c) */
typedef enum {
  MT_METHOD_IN_CLASS, MT_CMETHOD_IN_CLASS, MT_METHOD_IN_CHAIN, MT_CMETHOD_IN_CHAIN,
  MT_SELF_METHOD_IN_CHAIN, MT_METHOD_INDEX, MT_MAIN_METHOD, MT_INCLUDED_METHOD_INDEX,
  MT_READER_IN_CHAIN, MT_WRITER_IN_CHAIN, MT_IS_UNDEFFED_IN_CHAIN,
  MT_METHOD_VIS_IN_CHAIN
} MtPrim;

/* the side a read asks */
enum { MTS_INST, MTS_CMETH, MTS_SELF };

/* the frame and the brackets open now: what a node frame saves and what an
   unwind to an abandoned emission restores (EmitUnitState) */
typedef struct { int frame, depth, build; } MtabMark;

struct MethodTableCheck *mtab_new(void);
void mtab_free(struct MethodTableCheck *m);
/* a read of cls's tables for name (cls < 0: the top-level functions) */
void mtab_read(Compiler *c, MtPrim prim, int cls, int side, const char *name);
/* the answer of call_target, noted where it is the table's last definition
   (ML_TABLE) for the MLW_* reason why */
void mtab_table(Compiler *c, int id, int cls, unsigned flags, const char *name, int why);
void mtab_depth(Compiler *c, int by);
void mtab_build_depth(Compiler *c, int by);
MtabMark mtab_mark(Compiler *c);
void mtab_unwind(Compiler *c, MtabMark was);
MtabMark mtab_frame_set(Compiler *c, int id);   /* node id is the frame now, outside any bracket; the state before */
MtabMark mtab_frame_row_owner(Compiler *c, int node, const char *name);   /* the owner of the row `name` above node is the frame; the state before */
MtabMark mtab_frame_keep(Compiler *c, int id);  /* node id is the frame now, the brackets open kept; the state before */
int mtab_framed(Compiler *c);                   /* a node is the frame now (the reads made are its own) */
void mtab_report(Compiler *c);

static inline void mtab_enter(Compiler *c) { if (g_method_table_check) mtab_depth(c, 1); }
static inline void mtab_leave(Compiler *c) { if (g_method_table_check) mtab_depth(c, -1); }
/* The window of a method list's builders (the refl_* functions that assemble a list's names and
   refl_emit_sym_array): a read made unbracketed inside it is the list's own read of a table, family lists; one
   made under the list's node outside it is the call's. */
static inline void mtab_build_enter(Compiler *c) { if (g_method_table_check) mtab_build_depth(c, 1); }
static inline void mtab_build_leave(Compiler *c) { if (g_method_table_check) mtab_build_depth(c, -1); }
/* The state to come back to after a pass (a step of an analysis phase): what mtab_mark answers, with no call
   with the flag off */
static inline MtabMark mtab_save(Compiler *c) { return g_method_table_check ? mtab_mark(c) : (MtabMark){ -1, 0, 0 }; }
/* The end of a pass: a bracket the pass left open (or closed once too often) is reported, as the check fails on it,
   and the state before the pass is back. */
void mtab_step_report(Compiler *c, MtabMark was, const char *pass);
static inline void mtab_step_end(Compiler *c, MtabMark was, const char *pass) { if (g_method_table_check) mtab_step_report(c, was, pass); }
/* A node frame: the brackets open around it cover their own reads only, so
   a node inferred or emitted inside one has its reads counted. */
static inline MtabMark mtab_push(Compiler *c, int id) { return g_method_table_check ? mtab_frame_set(c, id) : (MtabMark){ -1, 0, 0 }; }
static inline void mtab_pop(Compiler *c, MtabMark was) { if (g_method_table_check) mtab_unwind(c, was); }
/* The frame of a read that a pass, a plan or a walker makes about call id (it has the node in hand, but no
   inference or emission of it is open): the reads are the call's. A bracket open around it still covers them:
   the plan's own reads and the candidate owners' are not counted, whatever node they are made for. */
static inline MtabMark mtab_push_in(Compiler *c, int id) { return g_method_table_check ? mtab_frame_keep(c, id) : (MtabMark){ -1, 0, 0 }; }
/* The frame of what is asked about `node`, a constant of a pattern or a `when`: the node above it that holds the
   implied call `name` (its row), where the program's code does nothing for it with the flag off. */
static inline MtabMark mtab_push_row_owner(Compiler *c, int node, const char *name) { return g_method_table_check ? mtab_frame_row_owner(c, node, name) : (MtabMark){ -1, 0, 0 }; }

#endif
