/* In-memory AST node table for the C Spinel compiler.
 *
 * Mirrors the text node-table schema emitted by spinel_parse.c
 * (N/S/I/F/R/A lines): every node has a type string and a set of named
 * fields. Fields come in four flavors that match the text tags:
 *   S  string field   (e.g. CallNode "name" = "puts")
 *   I  int field      (e.g. IntegerNode "value" = 1)
 *   R  ref field      (a child node id, -1 if absent)
 *   A  array field    (a list of child node ids)
 * plus an F "content" string (floats, raw literals).
 *
 * The parser (sp_parse_file, src/spinel_parse.c) fills the table as it
 * walks Prism's tree, through the nt_put_* calls below; nt_dump prints it
 * in the text form for --dump-ast.
 */
#ifndef SPINEL_NODE_TABLE_H
#define SPINEL_NODE_TABLE_H

#include <stddef.h>
#include <stdio.h>
#include "types.h"   /* sp_streq -- used by the node-table accessors below */

/* Field lookups are by NAME and are the compiler's hottest operation
   (nt_str alone: ~160M calls and ~20% of self time on a 37k-line compile).
   Each field carries a 2-byte discriminator -- first character plus length --
   so the scan rejects a non-matching key with one 16-bit compare instead of
   dereferencing its string. The query side folds to a constant: the callers
   pass string literals, and the accessors below are inline, so
   sp_field_disc("receiver") is computed at compile time. */
#define SP_FIELD_DISC(k, klen) ((unsigned)(unsigned char)(k)[0] | ((unsigned)(klen) << 8))
static inline unsigned sp_field_disc(const char *k) {
  return SP_FIELD_DISC(k, __builtin_strlen(k));
}
/* A call rename nt_node_swap_str or nt_node_restore_str made: the name it
   replaced and the one it set, or cut when either did not fit. */
typedef struct { char from[32], to[32]; unsigned char cut; } NtRename;
#define NT_RENAME_LOG 64
typedef struct { char *key; unsigned disc; char *val; size_t val_len; } SpStrField;
typedef struct { char *key; unsigned disc; long long val; }    SpIntField;
typedef struct { char *key; unsigned disc; int ref; }          SpRefField;
typedef struct { char *key; unsigned disc; int *ids; int n; }  SpArrField;

typedef struct {
  char *type;          /* node type string ("CallNode"), NULL if unset */
  char *content;       /* F content, NULL if none */
  SpStrField *s; int ns, cs;
  SpIntField *i; int ni, ci;
  SpRefField *r; int nr, cr;
  SpArrField *a; int na, ca;
  int kind;            /* cached NodeKind+1; 0 = not yet computed (see nt_kind) */
} SpNode;

/* Parse inputs for each closed autoload registration. The Compiler's copy
   adds the load record's slot and declaration ownership. */
typedef struct { int node, file, slot, first; char *name, *path; } AutoloadRegistration;

typedef struct {
  AutoloadRegistration *autoloads; int nautoloads, cautoloads;
  SpNode *nodes;
  int count;           /* number of allocated node slots */
  int node_cap;        /* capacity of `nodes`; >= count (for append growth) */
  int root_id;
  int has_cext_decls; /* syntactic opt-in; avoids a declaration scan in ordinary programs */
  char *source_file;   /* SOURCE_FILE path (unescaped), NULL if none */
  char **files;        /* FILE id -> path (unescaped); for multi-file #line maps */
  int nfiles;          /* length of `files` */
  unsigned char *file_defers; /* FILE id -> 1 when the file's `# spinel: defer-refusals`
                          magic comment turns its refusals into raises where they
                          run (nt_put_file_defers); nfile_defers its length */
  int nfile_defers;
  unsigned char *file_tier;   /* FILE id -> the resolver's SP_LIB_* tier of a
                          library file (nt_put_file_tier); 0 (SP_LIB_NONE) for
                          the program's own; nfile_tier its length */
  int nfile_tier;
  /* Under --load-model=graph (#9898) each file the program loads is a root:
     its ProgramNode, by graph id (0 is the entry, the table's root). None
     under the inline model, whose one root is root_id (nt_root). */
  int *file_roots;
  int nfile_roots;
  /* Bumped whenever a node is added or a string field is rewritten, i.e. by
     exactly the edits that can change which node carries which name. Caches
     keyed on node identity or name can revalidate against it. */
  unsigned version;
  /* The parts of version that a local variable read appended by
     nt_new_local_read, a call renamed by nt_node_swap_str or
     nt_node_restore_str (nt_stamp_holds), and a subtree appended by
     nt_clone_subtree bumped: a clone touches no node below the count it
     found, so an index of the nodes by name extends over the clones when
     every bump since its build was one (an_calls_named_first). */
  unsigned read_version, rename_version, clone_version;
  /* The last NT_RENAME_LOG of the renames rename_version counts, by that
     count: the name each replaced and the one it set (nt_renames_spare). */
  NtRename rename_log[NT_RENAME_LOG];
  /* Bumped by every edit of a node, an int, ref or array field's included,
     which version leaves out. infer_type keeps an earlier query's answers
     only while neither it nor version has moved (#8852). */
  unsigned long edits;
  /* Per node (node_table.c): where a node appended from orig_lo on came
     from and the pass that appended it (nt_origin), and the scratch
     nt_clone_subtree marks its subtree with (a node is in the clone being
     made while its mark is clone_gen), so a clone costs its subtree, not
     the table. orig_lo is 0 until nt_track_origins. */
  struct NtAux *aux;
  int aux_cap, orig_lo;
  unsigned clone_gen;
  /* how many nodes each pass has given each origin (node_table.c), for the
     k nt_origin answers */
  struct NtCount *counts;
  int counts_cap, ncounts;
  /* the analysis pass appending nodes now (nt_set_pass), NULL outside one */
  const char *pass;
  /* The ids of each kind, ascending (nt_nodes_of_kind): kind k's are
     kind_ids[k][0 .. kind_n[k]), over the nodes below kind_count; NULL
     until the first ask. kind_moved notes the nodes retyped since the last
     ask, as (id, kind listed under) pairs; kind_live counts the open
     NT_FOREACH_KIND loops, and kind_retired holds the arrays they may still
     be reading. */
  int **kind_ids;
  int *kind_n, *kind_cap;
  int kind_count;
  int *kind_moved, nkind_moved, ckind_moved;
  int kind_live;
  int **kind_retired, nkind_retired, ckind_retired;
  /* the class or module body that holds each node (fwd_lex_parent), made as
     far as its questions need; NULL until the first */
  struct NtHolders *holders;
} NodeTable;

/* An index of the tree kept on the table it was made of: for each node, the
   class or module whose body holds it as a statement (or as the def a
   visibility call wraps), the first such in node order before it. It stays
   valid while the table's count, version and edits are the ones it saw
   (ver, edits, cnt), and an entry counts when it carries the stamp `cur`, so
   an edit clears nothing. The holders are entered in node order, statement by
   statement, only as far as a question needs: the holders before `hi`, and
   the first `k` statements of that one's body `bv`, of `bn` statements (-1:
   not read yet). Freed with the table. */
struct NtHolders {
  int *par;
  unsigned *stamp;
  int cap;
  unsigned cur;
  int hi, k;
  const int *bv;
  int bn;
  unsigned ver;
  unsigned long edits;
  int cnt;
};

/* The roots of the program's files, entry first (see file_roots). */
static inline int nt_nroots(const NodeTable *nt) { return nt->nfile_roots ? nt->nfile_roots : 1; }
static inline int nt_root(const NodeTable *nt, int i) { return nt->nfile_roots ? nt->file_roots[i] : nt->root_id; }


/* Node-type kinds. The parser emits the type as a string ("CallNode"); to
   avoid an strcmp ladder on every per-node dispatch in the (iterated) analysis
   passes, each node caches an integer kind, computed once by nt_kind.
   The list is src/node_kinds.inc, generated from Prism's node list (and
   sorted, as nt_kind_resolve searches it by name), so every node type
   Prism has is here and a new Prism version cannot leave one out; the
   compiler's own types are named in tools/gen_node_kinds.rb. A type that is
   not on the list answers NK_NONE. */
#include "node_kinds.inc"

typedef enum {
  NK_NONE = 0,
#define X(n) NK_##n,
  SP_NODE_KINDS(X)
#undef X
  NK__COUNT
} NodeKind;

/* Work accounting for the scaling test (make scale-test): with SP_WORK_COUNT
   defined, every node access and kind-list step counts one unit, and the
   compiler prints the total at exit. The count is deterministic, so the test
   can compare two program sizes without timing anything; a pass that rescans
   the table per node shows up as a ratio far above the size ratio
   (rubys/roundhouse#72). Off in every normal build: the macro is empty. */
#ifdef SP_WORK_COUNT
#define NT_WORK() (g_nt_work++)
#else
#define NT_WORK() ((void)0)
#endif

/* Integer node-type, computed once per node and cached. NK_NONE if the node
   has no type or an unrecognized one. */
/* Slow path: resolve the type string to a kind and cache it in the node. */
NodeKind nt_kind_resolve(const NodeTable *nt, int id);
/* The kind is cached in the node as kind+1 after the first resolve, so the
   steady-state cost must be a load and a compare -- an out-of-line call here
   measured SLOWER than the inlined byte compare it replaced (the old
   `sp_streq(nt_type(...), "CallNode")` usually differs on the first byte). */
static inline NodeKind nt_kind(const NodeTable *nt, int id) {
  NT_WORK();
  if (id < 0 || id >= nt->count) return NK_NONE;
  int k = nt->nodes[id].kind;
  return k ? (NodeKind)(k - 1) : nt_kind_resolve(nt, id);
}

/* Node ids of a given kind, in ascending id order (the table's kind index).
   Lets a pass iterate only the nodes it cares about instead of scanning the
   whole table and filtering by kind. *count receives the number of ids. */
const int *nt_nodes_of_kind(const NodeTable *nt, NodeKind k, int *count);

/* Iterate the ids of kind KIND (ascending), binding each to a fresh `int IDV`:
     NT_FOREACH_KIND(nt, NK_CallNode, id) { ... use id ... }
   Replaces `for (int id=0; id<nt->count; id++) { if (kind!=K) continue; ... }`.
   The inner one-shot loop scopes IDV; nesting/multiple uses don't collide. */
typedef struct { const NodeTable *nt; const int *ids; int n, i, id; } NtKindIter;
/* A loop body may append or retype nodes (a desugar pass does) and, through
   anything that asks for a kind's ids again, replace the array the iterator
   is reading. The table keeps a count of its live iterators and retires a
   superseded id array instead of freeing it while one is open; the
   iterator's cleanup (run at the loop's end, a break or a return alike) is
   what lets the retired arrays go. */
void nt_kind_iter_open(const NodeTable *nt);
void nt_kind_iter_close(NtKindIter *it);
static inline NtKindIter nt_kind_iter_begin(const NodeTable *nt, NodeKind k) {
  NtKindIter it; it.nt = nt; it.ids = nt_nodes_of_kind(nt, k, &it.n); it.i = 0; it.id = -1;
  nt_kind_iter_open(nt);
  return it;
}
static inline int nt_kind_iter_next(NtKindIter *it) {
  NT_WORK();
  if (it->i >= it->n) return 0;
  it->id = it->ids[it->i++];
  return 1;
}
#define NT_FOREACH_KIND(NT, KIND, IDV) \
  for (NtKindIter _it_##IDV __attribute__((cleanup(nt_kind_iter_close))) = nt_kind_iter_begin((NT), (KIND)); nt_kind_iter_next(&_it_##IDV); ) \
    for (int IDV = _it_##IDV.id, _once_##IDV = 1; _once_##IDV; _once_##IDV = 0)

/* ---- filling a table from the parser ----
   The parser's walk numbers the nodes as it reaches them and hands each
   node's type and fields over in the order it finds them. A put to an id
   past the end extends the table to it; a node's fields keep their arrival
   order, a second field of the same key is appended after the first (which
   the accessors answer), and a second type or content replaces the first. */
NodeTable *nt_new_table(void);
void nt_put_type(NodeTable *nt, int id, const char *type);
void nt_put_str(NodeTable *nt, int id, const char *key, const char *val, size_t len);  /* copied */
void nt_put_int(NodeTable *nt, int id, const char *key, long long val);
void nt_put_ref(NodeTable *nt, int id, const char *key, int child);
void nt_put_arr(NodeTable *nt, int id, const char *key, int *ids /*owned*/, int n);
void nt_put_content(NodeTable *nt, int id, const char *content);  /* copied */
/* Record the source path, or the path of file id `fid` (copied). */
void nt_put_source_file(NodeTable *nt, const char *path);
void nt_put_file(NodeTable *nt, int fid, const char *path);

/* Print the table in its text form (docs/internals/AST.md): the ROOT,
   SOURCE_FILE and FILE header lines, then each node's N line followed by
   its S, I, F, R and A lines, in id order. --dump-ast writes it. */
void nt_dump(const NodeTable *nt, FILE *out);
/* A hash of every node's type, content and fields (SPINEL_LOWER_CHECK). */
unsigned long long nt_content_hash(const NodeTable *nt);

void nt_add_autoload(NodeTable *nt, int node, int file, const char *name, const char *path);
void nt_free(NodeTable *nt);
/* Renumber the nodes to an order: order[k] is the id node k had (node_table.c). */
void nt_renumber(NodeTable *nt, const int *order);

/* Deep-clone the subtree rooted at `root`; returns the new root id (or -1).
   Appends nodes and grows nt->count -- parallel per-node arrays must be
   resized to match afterward. */
int nt_clone_subtree(NodeTable *nt, int root);
/* The ids of the subtrees at roots[0..n), each once, in walk order (a node
   two roots share is listed once): a malloc'd list the caller frees, its
   length in *out. A walk of those subtrees alone. */
int *nt_subtree_ids(NodeTable *nt, const int *roots, int n, int *out);
/* Where node `id` came from, for a node appended after nt_track_origins:
   the node it is a copy of (*how 'c'), or the node it was first linked
   under (*how 'r', *a the ref field's index; *how 'a', *a the array
   field's index). *pass is a hash of the name of the pass that appended it
   (nt_set_pass; 0 outside one), and *k counts from 1 the nodes that pass
   gave this same origin. -1 for any other node, or one not linked anywhere
   yet. comp_node_tag names an appended node from this, so the name is the
   same whenever it is asked and whichever passes ran before. */
int nt_origin(const NodeTable *nt, int id, char *how, int *a, unsigned *pass, int *k);
void nt_track_origins(NodeTable *nt);   /* from the nodes appended next on */
const char *nt_set_pass(NodeTable *nt, const char *pass);   /* the one before */
void nt_subtree_blank(NodeTable *nt, int id);   /* every node of the subtree a NilNode */
void nt_swap_nodes(NodeTable *nt, int a, int b);
/* Copy the subtree at root of src into dst; the new root's id, or -1. */
int nt_import_subtree(NodeTable *dst, const NodeTable *src, int root);
/* Are a call's arguments all positional -- no splat, no block argument? (A
   braceless trailing hash is one positional argument to the builtin surface.)
   The face table's rows describe positional calls, and the two halves that
   read it treat the other shapes alike. */
int nt_call_args_plain(const NodeTable *nt, int id);

/* ---- synthetic node construction ----
   Append a freshly-typed node and populate its fields, for desugaring AST
   in-place (e.g. a forwarded `&callable` into an equivalent block). Each
   nt_new_node grows nt->count; like nt_clone_subtree, callers must resize the
   parallel per-node arrays (comp_grow_node_arrays) afterward. The set_* helpers
   overwrite a field of the same key if present, else append it. */
int  nt_new_node(NodeTable *nt, const char *type);                 /* new id, -1 on OOM */
void nt_node_set_type(NodeTable *nt, int id, const char *type);    /* retype in place */
void nt_node_reset(NodeTable *nt, int id, const char *type);
void nt_node_set_str(NodeTable *nt, int id, const char *key, const char *val);
/* A temporary rename (an emitter re-entering under another spelling): the
   swap sets key to a copy of val and answers the string it held, NULL when
   the node had none, which the restore puts back. Unlike nt_node_set_str it
   frees nothing a caller may still hold: a name read before the rename stays
   valid through it and after. */
char *nt_node_swap_str(NodeTable *nt, int id, const char *key, const char *val);
void nt_node_restore_str(NodeTable *nt, int id, const char *key, char *held);
void nt_node_set_int(NodeTable *nt, int id, const char *key, long long val);
void nt_node_set_ref(NodeTable *nt, int id, const char *key, int child);
void nt_node_set_arr(NodeTable *nt, int id, const char *key, const int *ids, int n);
int  nt_new_int(NodeTable *nt, long long v);                     /* a new IntegerNode */
/* Are the nodes appended at or after id `from` all local variable reads?
   Such a read is a leaf, so a whole-tree index built before it was appended
   (one that reads no local variable read) is still the index of the table. */
int  nt_appended_only_local_reads(const NodeTable *nt, int from);
/* A new local variable read of name, counted in read_version. */
int  nt_new_local_read(NodeTable *nt, const char *name);
/* The table's version when an index was built, with its two counted parts.
   nt_stamp_holds answers whether every version bump since was a local
   variable read appended (nt_new_local_read) or, with renames, a call's name
   swapped or restored: an index that reads neither (nor, with renames, any
   call's name) is then still the index of the table. */
typedef struct { unsigned version, reads, renames; } NtStamp;
NtStamp nt_stamp(const NodeTable *nt);
int  nt_stamp_holds(const NodeTable *nt, NtStamp s, int renames);
/* nt_stamp_holds with renames, where each rename since also replaced and
   set names `reads` answers 0 for: an index that reads a call's name only
   through `reads` is then still the index of the table. 0 as well when the
   renames since outran the log. */
int  nt_renames_spare(const NodeTable *nt, NtStamp s, int (*reads)(const char *name));

/* Accessors. id must be in [0, nt->count). Out-of-range ids return the
   given defaults so callers can walk freely without bounds checks. */
/* Resolve a `node_file` id to its source path, or NULL if out of range.
   Used by codegen to emit `#line N "path"` directives in multi-file builds. */
const char *nt_file_path(const NodeTable *nt, int fid);
/* The file's `# spinel: defer-refusals` magic comment: set, and asked */
void nt_put_file_defers(NodeTable *nt, int fid);
int nt_file_defers(const NodeTable *nt, int fid);
/* Where `require` found a file (the parser's sp_lib_lookup): lib/, a `-I`
   root, builtins/ or a pre-installed package; SP_LIB_NONE for a file of
   the program's own (the entry, a require_relative, a load). */
enum { SP_LIB_NONE, SP_LIB_LIB, SP_LIB_ROOT, SP_LIB_BUILTIN, SP_LIB_PACKAGE };
/* The file's tier: set, and asked */
void nt_put_file_tier(NodeTable *nt, int fid, int tier);
int nt_file_tier(const NodeTable *nt, int fid);

const char *nt_type(const NodeTable *nt, int id);          /* NULL if unset */
static inline const char *nt_str(const NodeTable *nt, int id, const char *key) {
  NT_WORK();
  if (id < 0 || id >= nt->count) return NULL;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->ns; j++)
    if (nd->s[j].disc == d && sp_streq(nd->s[j].key, key)) return nd->s[j].val;
  return NULL;
}
/* Overwrite an existing string field's value (no-op if the key is absent).
   Returns 1 if the field was found and updated. */
int         nt_set_str(NodeTable *nt, int id, const char *key, const char *val);
static inline size_t nt_str_len(const NodeTable *nt, int id, const char *key) {
  if (id < 0 || id >= nt->count) return 0;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->ns; j++)
    if (nd->s[j].disc == d && sp_streq(nd->s[j].key, key)) return nd->s[j].val_len;
  return 0;
}
static inline long long nt_int(const NodeTable *nt, int id, const char *key, long long dflt) {
  NT_WORK();
  if (id < 0 || id >= nt->count) return dflt;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->ni; j++)
    if (nd->i[j].disc == d && sp_streq(nd->i[j].key, key)) return nd->i[j].val;
  return dflt;
}
/* PM_PARAMETER_FLAGS_REPEATED_PARAMETER (bit 2 of a parameter node's
   "flags"): the node repeats a name an earlier parameter of the same list
   bound, which Ruby allows for an underscore-prefixed name. */
#define NT_PARAM_REPEATED 4
static inline int nt_param_repeated(const NodeTable *nt, int id) {
  return (nt_int(nt, id, "flags", 0) & NT_PARAM_REPEATED) != 0;
}
/* Prism's node flags that flatten_node keeps in a node's "flags" integer: a
   RangeNode's, a WhileNode's and UntilNode's, and a regexp literal's. Their
   bits overlap (a range's EXCLUDE_END, a loop's BEGIN_MODIFIER and a
   regexp's IGNORE_CASE are all 4), so a bit is read through the predicate for
   its node kind, which answers 0 for any other kind, never by testing
   "flags" against a number. spinel_parse.c asserts each value below against
   Prism's own constant. */
#define NT_RANGE_EXCLUDE_END 4     /* PM_RANGE_FLAGS_EXCLUDE_END */
#define NT_LOOP_BEGIN_MODIFIER 4   /* PM_LOOP_FLAGS_BEGIN_MODIFIER */
#define NT_RE_IGNORE_CASE 4        /* PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE */
#define NT_RE_EXTENDED 8           /* PM_REGULAR_EXPRESSION_FLAGS_EXTENDED */
#define NT_RE_MULTI_LINE 16        /* PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE */
#define NT_RE_ONCE 32              /* PM_REGULAR_EXPRESSION_FLAGS_ONCE */
#define NT_RE_EUC_JP 64            /* PM_REGULAR_EXPRESSION_FLAGS_EUC_JP */
#define NT_RE_ASCII_8BIT 128       /* PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT */
#define NT_RE_WINDOWS_31J 256      /* PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J */
#define NT_RE_UTF_8 512            /* PM_REGULAR_EXPRESSION_FLAGS_UTF_8 */
/* A range literal that excludes its end (`a...b`). */
static inline int nt_range_excl(const NodeTable *nt, int id) {
  return nt_kind(nt, id) == NK_RangeNode && (nt_int(nt, id, "flags", 0) & NT_RANGE_EXCLUDE_END) != 0;
}
/* A range a desugar builds says whether it excludes its end the way the
   parser does. */
static inline void nt_range_set_excl(NodeTable *nt, int id, int excl) {
  nt_node_set_int(nt, id, "flags", excl ? NT_RANGE_EXCLUDE_END : 0);
}
/* A `while` or `until` loop that tests after its body (`begin ... end while
   c`): the body runs once before the first test. */
static inline int nt_loop_post(const NodeTable *nt, int id) {
  NodeKind k = nt_kind(nt, id);
  return (k == NK_WhileNode || k == NK_UntilNode) &&
         (nt_int(nt, id, "flags", 0) & NT_LOOP_BEGIN_MODIFIER) != 0;
}
/* A regexp literal's option bits, a mask of the NT_RE_* values (0 for any
   other node). */
static inline int nt_regexp_opts(const NodeTable *nt, int id) {
  NodeKind k = nt_kind(nt, id);
  if (k != NK_RegularExpressionNode && k != NK_InterpolatedRegularExpressionNode) return 0;
  return (int)nt_int(nt, id, "flags", 0);
}
/* PM_CALL_NODE_FLAGS_SAFE_NAVIGATION: the call is written `recv&.name`
   (also on the call-operator write nodes, `recv&.name += 1`). The parser
   exports it as the `safe_nav` flag, set only when Prism set it, and a
   desugar that turns the call into a plain one clears it, as it clears
   `vcall`. A node without the field is not a safe navigation. */
static inline int nt_safe_nav(const NodeTable *nt, int id) {
  return nt_int(nt, id, "safe_nav", 0) != 0;
}
static inline void nt_set_safe_nav(NodeTable *nt, int id, int on) {
  if (on || nt_safe_nav(nt, id)) nt_node_set_int(nt, id, "safe_nav", on ? 1 : 0);
}
/* PM_CALL_NODE_FLAGS_VARIABLE_CALL: the call is a bare identifier (`foo`,
   no receiver, no parentheses, no arguments), which Ruby resolves as a local
   variable first and only then as a method. The parser exports it as the
   `vcall` flag, set only when Prism set it, and a desugar that gives the call
   a receiver clears it with nt_set_vcall. A node without the field is not a
   variable call. */
static inline int nt_vcall(const NodeTable *nt, int id) {
  return nt_int(nt, id, "vcall", 0) != 0;
}
static inline void nt_set_vcall(NodeTable *nt, int id, int on) {
  if (on || nt_vcall(nt, id)) nt_node_set_int(nt, id, "vcall", on ? 1 : 0);
}
/* PM_CALL_NODE_FLAGS_IGNORE_VISIBILITY: a receiverless call or a literal
   `self` receiver may call a private method. Parenthesized self does not.
   Index targets deny it: CRuby refuses private []= in masgn and for.
   Desugars preserve this permission when spelling the same call differently,
   set it for a synthesized self call, and clear it for a different receiver. */
static inline int nt_ignore_visibility(const NodeTable *nt, int id) {
  return nt_int(nt, id, "ignore_visibility", 0) != 0;
}
static inline void nt_set_ignore_visibility(NodeTable *nt, int id, int on) {
  nt_node_set_int(nt, id, "ignore_visibility", on ? 1 : 0);
}
/* A synthesized implicit-self call spells its receiver explicitly while
   retaining the private-call permission of the receiverless form. An
   explicit denial from a public_send rewrite stays denied. */
static inline void nt_set_self_receiver(NodeTable *nt, int id, int receiver) {
  nt_node_set_ref(nt, id, "receiver", receiver);
  nt_set_ignore_visibility(nt, id, nt_ignore_visibility(nt, id));
}
/* PM_CALL_NODE_FLAGS_ATTRIBUTE_WRITE: the call is written as an assignment,
   `recv.name = v`, `recv.name=(v)` or `recv[i] = v`, so its value is the
   argument whatever the writer returns. The parser exports it as the
   `attr_write` flag, set only when Prism set it, and a desugar that builds
   such an assignment sets it with nt_set_attr_write (a desugar that makes
   the call plain clears it). `recv.send(:name=, v)` and `recv.[]=(i, v)` are
   not assignments, and a node without the field is not one. */
static inline int nt_attr_write(const NodeTable *nt, int id) {
  return nt_int(nt, id, "attr_write", 0) != 0;
}
static inline void nt_set_attr_write(NodeTable *nt, int id, int on) {
  if (on || nt_attr_write(nt, id)) nt_node_set_int(nt, id, "attr_write", on ? 1 : 0);
}
static inline int nt_ref(const NodeTable *nt, int id, const char *key) {
  NT_WORK();
  if (id < 0 || id >= nt->count) return -1;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->nr; j++)
    if (nd->r[j].disc == d && sp_streq(nd->r[j].key, key)) return nd->r[j].ref;
  return -1;
}
static inline const int *nt_arr(const NodeTable *nt, int id, const char *key, int *out_n) {
  NT_WORK();
  if (out_n) *out_n = 0;
  if (id < 0 || id >= nt->count) return NULL;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->na; j++)
    if (nd->a[j].disc == d && sp_streq(nd->a[j].key, key)) {
      if (out_n) *out_n = nd->a[j].n;
      return nd->a[j].ids;
    }
  return NULL;
}
/* ---- an argument list's shape ----
   Prism sets ArgumentsNodeFlags on an ArgumentsNode (CONTAINS_SPLAT,
   CONTAINS_MULTIPLE_SPLATS, CONTAINS_KEYWORDS, CONTAINS_KEYWORD_SPLAT,
   CONTAINS_FORWARDING). The table does not keep them: the desugars rewrite
   argument lists (a `foo(*a)` turned into a literal list, arguments added by
   a splice, a SplatNode retyped), so a bit exported at parse time would
   describe the list as written, not as it is. These predicates ask the list
   itself, each time, so they answer from its current children after any
   rewrite and keep nothing to invalidate. A list is a few elements, and the
   scan costs what reading a cached field would: a lookup of the node's
   array, then one kind load per element. A cache on the ArgumentsNode
   would also answer for the whole list, where callers ask about a prefix
   or a tail of it, and would have to be dropped by every edit of the
   array and every retyping of an argument node.

   They take the list the caller already holds, an `arguments` array and its
   length (a call's `arguments`, a yield's, a super's, an index write's; also
   a slice of one), and answer 0 for a missing list. A call's `&blk` is not
   in the list: it is the call's `block`, a BlockArgumentNode
   (nt_call_has_block_arg), except `&:name` for a word-character name or a
   unary operator, which the parser makes a literal block; `&:+`, `&:&` and
   the other binary operators stay a BlockArgumentNode. */
#define NT_ARG_SPLAT 1        /* SplatNode: `*a`, and the anonymous `*` */
#define NT_ARG_KEYWORDS 2     /* KeywordHashNode: `k: v`, `**h` (a trailing hash without braces) */
#define NT_ARG_BLOCK 4        /* BlockArgumentNode as a list element: defensive, Prism and the desugars put `&blk` on the call's block, and the loops that tested for one keep asking */
#define NT_ARG_FORWARDING 8   /* ForwardingArgumentsNode: `...` */
static inline unsigned nt_arg_shape(NodeKind k) {
  switch (k) {
  case NK_SplatNode: return NT_ARG_SPLAT;
  case NK_KeywordHashNode: return NT_ARG_KEYWORDS;
  case NK_BlockArgumentNode: return NT_ARG_BLOCK;
  case NK_ForwardingArgumentsNode: return NT_ARG_FORWARDING;
  default: return 0;
  }
}
/* Is any of argv[0..argc) one of the shapes (a mask of NT_ARG_*)? The single
   shapes below name the common questions; a caller that asks for several
   at once ("no splat, keyword or block argument") passes the mask. */
static inline int nt_argv_has(const NodeTable *nt, const int *argv, int argc, unsigned shapes) {
  if (!argv) return 0;
  for (int i = 0; i < argc; i++)
    if (nt_arg_shape(nt_kind(nt, argv[i])) & shapes) return 1;
  return 0;
}
/* PM_ARGUMENTS_NODE_FLAGS_CONTAINS_SPLAT: does the call spread an array into
   its positionals (`*args`)? */
static inline int nt_argv_has_splat(const NodeTable *nt, const int *argv, int argc) {
  return nt_argv_has(nt, argv, argc, NT_ARG_SPLAT);
}
/* How many of the arguments are splats (CONTAINS_MULTIPLE_SPLATS is
   `> 1`). */
static inline int nt_argv_splat_count(const NodeTable *nt, const int *argv, int argc) {
  int n = 0;
  for (int i = 0; argv && i < argc; i++)
    if (nt_kind(nt, argv[i]) == NK_SplatNode) n++;
  return n;
}
/* CONTAINS_KEYWORDS: does the call pass keyword arguments -- a literal
   `k: v` or a `**h`? A sole keyword hash counts too (the Struct/Data member
   form, `d.class.new(x: 1)`, is built as the static `D.new(x: 1)` is). */
static inline int nt_argv_has_keywords(const NodeTable *nt, const int *argv, int argc) {
  return nt_argv_has(nt, argv, argc, NT_ARG_KEYWORDS);
}
/* CONTAINS_FORWARDING: does the call forward `...`? */
static inline int nt_argv_has_forwarding(const NodeTable *nt, const int *argv, int argc) {
  return nt_argv_has(nt, argv, argc, NT_ARG_FORWARDING);
}
/* The trailing keyword hash of the arguments (`f(a, k: 1)`), or -1. Prism
   puts the keywords after the positionals, so it is the last element. */
static inline int nt_argv_kwh(const NodeTable *nt, const int *argv, int argc) {
  return argv && argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
}
/* Is the argument list exactly `(...)`? */
static inline int nt_argv_only_forwarding(const NodeTable *nt, const int *argv, int argc) {
  return argv && argc == 1 && nt_kind(nt, argv[0]) == NK_ForwardingArgumentsNode;
}
/* Does a Hash or KeywordHash literal spread another hash (`**h`)? */
static inline int nt_hash_has_splat(const NodeTable *nt, int hash) {
  int n = 0; const int *el = hash >= 0 ? nt_arr(nt, hash, "elements", &n) : NULL;
  for (int e = 0; e < n; e++)
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) return 1;
  return 0;
}
/* CONTAINS_KEYWORD_SPLAT: does an argument's keyword hash carry a `**h`? */
static inline int nt_argv_has_kwsplat(const NodeTable *nt, const int *argv, int argc) {
  for (int i = 0; argv && i < argc; i++)
    if (nt_kind(nt, argv[i]) == NK_KeywordHashNode && nt_hash_has_splat(nt, argv[i])) return 1;
  return 0;
}
/* The call's `&blk` or `&`: its block when that is a BlockArgumentNode, -1
   for a literal block or none. */
static inline int nt_call_block_arg(const NodeTable *nt, int call) {
  int b = nt_ref(nt, call, "block");
  return nt_kind(nt, b) == NK_BlockArgumentNode ? b : -1;
}
static inline int nt_call_has_block_arg(const NodeTable *nt, int call) {
  return nt_call_block_arg(nt, call) >= 0;
}
/* The expression inside a run of parentheses that each hold exactly one
   statement: `((x))` is x. It answers the syntactic question, "is this one
   wrapped expression, and which?", so parentheses holding anything else
   (`()`, `(a; b)`) are not peeled and the answer is the ParenthesesNode
   itself, and any other node comes back as it is: a node never turns into -1.
   It says nothing of the expression's value, which `(a; b)` takes from b and
   `()` from nil; a caller that asks that does not use this. The statements
   are counted rather than read from Prism's MULTIPLE_STATEMENTS flag: a
   desugar builds ParenthesesNodes the parser never saw and edits a body in
   place, and the flag is not exported. */
static inline int nt_paren_single(const NodeTable *nt, int n) {
  while (nt_kind(nt, n) == NK_ParenthesesNode) {
    int body = nt_ref(nt, n, "body"), cnt = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &cnt) : NULL;
    if (cnt != 1) break;
    n = stmts[0];
  }
  return n;
}
/* A def's receiver `recv` as written inside its parentheses: `def (expr).m`,
   where Prism gives the receiver's own parentheses the expression itself
   as body; parentheses inside it hold statements as usual (nt_paren_single) */
static inline int nt_def_receiver(const NodeTable *nt, int recv) {
  if (nt_kind(nt, recv) == NK_ParenthesesNode) {
    int b = nt_ref(nt, recv, "body");
    if (b >= 0 && nt_kind(nt, b) != NK_StatementsNode) recv = b;
  }
  return nt_paren_single(nt, recv);
}
static inline int nt_kind_is_constant(NodeKind k) {
  return k == NK_ConstantReadNode || k == NK_ConstantPathNode;
}
int nt_pred_const(const NodeTable *nt, int pred);   /* a predicate's constant truth: 1, 0, -1 not constant */
const char *nt_content(const NodeTable *nt, int id);

/* Generic child iteration (for structural walks that don't know field
   names). Ref fields and array-field elements are the node's children. */
int        nt_num_refs(const NodeTable *nt, int id);
int        nt_ref_at(const NodeTable *nt, int id, int i);   /* ref value, may be -1 */
int        nt_num_arrs(const NodeTable *nt, int id);
const int *nt_arr_at(const NodeTable *nt, int id, int i, int *out_n);

/* The frame an eval's text is parsed in (sp_parse_snippet_in, in
   src/spinel_parse.c): the locals of the scopes around it, outermost
   first; the line the text starts on; the path `__FILE__` answers in it
   (NULL: "(eval at FILE:LINE)" of the site); and the site's line and file,
   which its nodes take for `#line` and the reports (site_line 0: none). */
typedef struct SnipScope { const char *const *names; int n; } SnipScope;
typedef struct SnipFrame {
  const SnipScope *scopes; int nscopes;
  long long line;
  const char *path;
  long long site_line; int site_file;
} SnipFrame;
NodeTable *sp_parse_snippet(const char *src);   /* compiler-written text: no frame */
NodeTable *sp_parse_snippet_in(const char *src, const SnipFrame *f);

/* What the driver's flags ask of the parse, the analysis and codegen:
   src/main.c sets them, spinel_parse.c defines them. A string is a path to
   write, or NULL. */
extern int g_opt_debug, g_opt_line_map, g_opt_positions, g_opt_warn_widen, g_opt_check_stores,
           g_opt_defer_refusals, g_opt_emit_types_keep_c;
extern const char *g_opt_emit_types, *g_opt_emit_rbs, *g_opt_emit_symbol_map, *g_opt_profile_symbol_map,
                  *g_opt_rbs_seed;

#endif
