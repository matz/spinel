/* Compile-time evaluation over the node table.
 *
 * Code a class body runs once, when its file loads, often computes only
 * from what the program spells (a constant table, literal arguments):
 *
 *   StatusMessage.each { |code, message|
 *     const_set(message.gsub(/ /, ''), Class.new(code < 400 ? Success : Error))
 *   }
 *
 * This evaluator runs such code ahead of time. It is pure: given a node
 * and an environment (the block's locals, the classes the evaluation has
 * made so far) it answers a compile-time value (ct_value.h), or 0 where the
 * answer is not known for certain -- it writes nothing to the node table
 * and registers nothing. What the code does to the program (a constant it
 * sets, a class it makes, an instance variable it sets on one, an element
 * it stores into a constant's Hash) is reported as a list of effects, in
 * the order the code performs them, for the caller to apply; and every
 * core method it ran is listed, so the caller can ask whether the program
 * changes it. Today the table-loop desugar (analyze_ct.c) turns the
 * effects into class-body statements; the load-phase elaborator will
 * apply them to its class and constant model directly.
 *
 * It follows: nil, true, false, Integer, String, Symbol and Array literals,
 * interpolation, the block's locals, `if` / ternary / `unless` / `case`
 * over Integer, String and Symbol values and Integer ranges, the methods
 * ct_value.c runs (mv_call, mv_sub), constants (as references), and
 * `Class.new(Constant)`. As statements: a write of a block local, those
 * conditionals, `const_set(name, value)`, `klass.instance_variable_set(:@x,
 * value)`, `Const[key] = value` and `value.freeze`. In macro mode (a class
 * method a class body calls, run with that class as self, ct_macro_call)
 * also self's `name`, `to_s` and `inspect`; as statements,
 * `define_method(name) { ... }` and `define_singleton_method`, effects
 * whose block the caller takes as written, `attr_reader` / `attr_writer` /
 * `attr_accessor` on self or on its singleton class, `class_eval(text)` /
 * `module_eval(text, file, line)` on self with text it computes, and a call
 * of another macro, run in a frame of its own; and a statement it does not
 * follow is an effect too, left for the caller to run as written. Anything
 * else answers 0. */
#ifndef SPINEL_CT_EVAL_H
#define SPINEL_CT_EVAL_H

#include "ct_value.h"
#include "node_table.h"

typedef enum {
  CT_VALUE = 0,  /* mv */
  CT_CONST,      /* the constant the node `node` reads (ConstantRead / ConstantPath) */
  CT_CLASS       /* the class `cls` the evaluation made (CtEnv.cls) */
} CtKind;

/* A String value's object -- frozen or not, a literal's or the
   evaluation's own -- is the value's (Mv.frozen, Mv.lit). */
typedef struct { CtKind k; Mv mv; int node; int cls; } CtVal;

/* a class `Class.new(parent)` made: `parent` is the constant node; `name`
   is NULL until a const_set names it */
typedef struct { int parent; char *name; } CtClass;

typedef enum {
  CT_FX_CLASS_NEW,   /* cls: a class made, with its parent */
  CT_FX_CONST_SET,   /* name = val in the module whose body runs; names: val is the class it names */
  CT_FX_IVAR_SET,    /* class cls's instance variable name = val */
  CT_FX_INDEX_SET,   /* recv[key] = val, recv a constant */
  /* macro mode; frame: the macro frame (CtEnv.frames) the effect ran in */
  CT_FX_DEFINE_METHOD,  /* self's method name (sg: its singleton method), its body block (the call at site's) */
  CT_FX_ATTR,        /* the attr_* method name, of self (sg: of its singleton class), for attrs[0..nattrs) */
  CT_FX_LOCAL,       /* site wrote the frame's local name = val */
  CT_FX_RESIDUAL,    /* statement site, not followed: it runs as written */
  CT_FX_EVAL         /* site, a class_eval / module_eval on self, runs text name as self's body's code */
} CtFxKind;
/* a local: its value, the statement that last wrote it (-1: a parameter)
   and how many writes ran */
typedef struct { char *name; CtVal v; int wr, nw; } CtLocal;

typedef struct {
  CtFxKind k; int site; char *name; int cls, names, block, frame, sg;
  int lam;     /* a define_method whose body is a lambda argument (block: the LambdaNode) */
  int vcode;   /* a macro's const_set whose value the run does not know: the value's node + 1 (0: val holds it) */
  /* ...and in that code, the name argument of each send on self the run
     knows (fold[i], a node of the macro's body), spelled foldv[i] */
  int *fold; char **foldv; int nfold;
  char **attrs; int nattrs;
  CtVal recv, key, val;
  /* a residual or a local write: the frame's locals as they stood when
     it ran, in the frame's order (names not owned) */
  CtLocal *snap; int nsnap;
} CtFx;

/* a core method the evaluation ran: on an instance of `cls`, or (a class
   method) on the class itself */
typedef struct { const char *cls; const char *meth; } CtRan;
/* a name the evaluator took a call on self to reach what the record says,
   with how many effects had run before it did */
typedef struct { char *name; int after; } CtRelied;
/* a macro frame: the def whose body ran for call `call` of frame parent's
   (-1: the first), its locals as the body left them */
typedef struct { int def, call, parent; CtLocal *loc; int nloc; } CtFrame;

typedef struct {
  CtLocal loc[64]; int nloc;
  CtClass *cls; int ncls, ccls;
  CtFx *fx; int nfx, cfx;
  CtRan *ran; int nran, cran;
  CtRelied *relied; int nrelied, crelied;
  /* macro mode: a class-body macro's frame, whose self is the class named
     self_name. self_core(host, m) answers whether a call of m on self
     reaches the core method (Module's), not one of the program's: only
     then does the evaluator run it (self's name, to_s, inspect) or follow
     it (define_method with a block) */
  const char *self_name;
  int (*self_core)(void *host, const char *meth);
  /* the def a call of meth on self finds when it is a macro the frame may
     run (one of the program's), or -1 */
  int (*self_macro)(void *host, const char *meth);
  void *host;
  CtFrame *frames; int nframes, cframes;
  int frame, depth;   /* the frame running, and how many run */
  /* macro mode: self's instance variables, as the class body left them
     before the call (ct_env_ivar_set) and as the run writes them; a value
     of MV_UNDEF is one not known. One not listed is unset (nil), or not
     known when ivar_open. */
  CtLocal *ivar; int nivar, civar, ivar_open;
  /* a `return` ran in the frame running (returning), with this value */
  CtVal ret; int returning;
} CtEnv;

/* the value of expression `node`, 1 when it is known */
int ct_eval(const NodeTable *nt, CtEnv *e, int node, CtVal *out);
/* run statements node `stmts` (a StatementsNode, or -1 for none), 1 when
   every statement was followed */
int ct_exec(const NodeTable *nt, CtEnv *e, int stmts);
/* bind local `name` (the block's parameters); 0 when the table is full */
int ct_env_set(CtEnv *e, const char *name, CtVal v);
/* local `name` of macro frame `frame`, as its body left it; NULL when it
   has none */
const CtLocal *ct_frame_local(const CtEnv *e, int frame, const char *name);
/* macro mode: run class-body call `call` of a macro on self in a frame of
   its own, the def self_macro names with its parameters bound to the call's
   values; 1 when every statement was followed or left as written */
int ct_macro_call(const NodeTable *nt, CtEnv *e, int call);
/* macro mode: self's instance variable `name` holds v before the call
   (an MV_UNDEF value: not known) */
void ct_env_ivar_set(CtEnv *e, const char *name, CtVal v);
/* forget the locals (a new block call), keeping the classes and effects */
void ct_env_new_call(CtEnv *e);
/* do a and b hold one String object? A copy of a value (a local's read,
   String#to_s, an element) shares the String's storage, and each String the
   evaluator makes has storage of its own, so the storage is the identity */
int ct_same_string(const CtVal *a, const CtVal *b);
/* note that core method cls#meth ran */
void ct_env_ran(CtEnv *e, const char *cls, const char *meth);
/* note that a call of meth on self was taken to reach what the record says,
   with the effects that ran so far before it */
void ct_env_relied(CtEnv *e, const char *meth);
void ct_env_free(CtEnv *e);

#endif
