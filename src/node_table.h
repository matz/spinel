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
 * The single-binary compiler loads this directly from the parser's text
 * output (sp_parse_file_to_text) -- no on-disk intermediate.
 */
#ifndef SPINEL_NODE_TABLE_H
#define SPINEL_NODE_TABLE_H

#include <stddef.h>
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

typedef struct {
  SpNode *nodes;
  int count;           /* number of allocated node slots */
  int node_cap;        /* capacity of `nodes`; >= count (for append growth) */
  int root_id;
  char *source_file;   /* SOURCE_FILE path (unescaped), NULL if none */
  char **files;        /* FILE id -> path (unescaped); for multi-file #line maps */
  int nfiles;          /* length of `files` */
  /* Bumped whenever a node is added or a string field is rewritten, i.e. by
     exactly the edits that can change which node carries which name. Caches
     keyed on node identity or name can revalidate against it. */
  unsigned version;
} NodeTable;


/* Node-type kinds. The parser emits the type as a string ("CallNode"); to
   avoid an strcmp ladder on every per-node dispatch in the (iterated) analysis
   passes, each node caches an integer kind, computed once by nt_kind. */
#define SP_NODE_KINDS(X) \
  X(AliasGlobalVariableNode) \
  X(AliasMethodNode) \
  X(AlternationPatternNode) \
  X(AndNode) \
  X(ArrayNode) \
  X(ArrayPatternNode) \
  X(AssocNode) \
  X(AssocSplatNode) \
  X(BackReferenceReadNode) \
  X(BeginNode) \
  X(BlockArgumentNode) \
  X(BlockNode) \
  X(BlockParameterNode) \
  X(BlockParametersNode) \
  X(BreakNode) \
  X(CallAndWriteNode) \
  X(CallNode) \
  X(CallOrWriteNode) \
  X(CallTargetNode) \
  X(CapturePatternNode) \
  X(CaseMatchNode) \
  X(CaseNode) \
  X(ClassNode) \
  X(ClassVariableAndWriteNode) \
  X(ClassVariableOperatorWriteNode) \
  X(ClassVariableOrWriteNode) \
  X(ClassVariableReadNode) \
  X(ClassVariableTargetNode) \
  X(ClassVariableWriteNode) \
  X(ConstantAndWriteNode) \
  X(ConstantOperatorWriteNode) \
  X(ConstantOrWriteNode) \
  X(ConstantPathAndWriteNode) \
  X(ConstantPathNode) \
  X(ConstantPathOperatorWriteNode) \
  X(ConstantPathOrWriteNode) \
  X(ConstantPathTargetNode) \
  X(ConstantPathWriteNode) \
  X(ConstantReadNode) \
  X(ConstantTargetNode) \
  X(ConstantWriteNode) \
  X(DefNode) \
  X(DefinedNode) \
  X(ElseNode) \
  X(EmbeddedStatementsNode) \
  X(FalseNode) \
  X(FloatNode) \
  X(ForNode) \
  X(ForwardingSuperNode) \
  X(GlobalVariableAndWriteNode) \
  X(GlobalVariableOperatorWriteNode) \
  X(GlobalVariableOrWriteNode) \
  X(GlobalVariableReadNode) \
  X(GlobalVariableTargetNode) \
  X(GlobalVariableWriteNode) \
  X(HashNode) \
  X(HashPatternNode) \
  X(IfNode) \
  X(ImaginaryNode) \
  X(InNode) \
  X(IndexAndWriteNode) \
  X(IndexOperatorWriteNode) \
  X(IndexOrWriteNode) \
  X(IndexTargetNode) \
  X(InstanceVariableAndWriteNode) \
  X(InstanceVariableOperatorWriteNode) \
  X(InstanceVariableOrWriteNode) \
  X(InstanceVariableReadNode) \
  X(InstanceVariableTargetNode) \
  X(InstanceVariableWriteNode) \
  X(IntegerNode) \
  X(InterpolatedRegularExpressionNode) \
  X(InterpolatedStringNode) \
  X(InterpolatedSymbolNode) \
  X(InterpolatedXStringNode) \
  X(KeywordHashNode) \
  X(KeywordRestParameterNode) \
  X(LambdaNode) \
  X(LocalVariableAndWriteNode) \
  X(LocalVariableOperatorWriteNode) \
  X(LocalVariableOrWriteNode) \
  X(LocalVariableReadNode) \
  X(LocalVariableTargetNode) \
  X(LocalVariableWriteNode) \
  X(MatchPredicateNode) \
  X(MatchRequiredNode) \
  X(ModuleNode) \
  X(MultiTargetNode) \
  X(MultiWriteNode) \
  X(NextNode) \
  X(NilNode) \
  X(NumberedParametersNode) \
  X(NumberedReferenceReadNode) \
  X(OperatorWriteNode) \
  X(OptionalKeywordParameterNode) \
  X(OptionalParameterNode) \
  X(OrNode) \
  X(ParametersNode) \
  X(ParenthesesNode) \
  X(PinnedExpressionNode) \
  X(PinnedVariableNode) \
  X(PostExecutionNode) \
  X(PreExecutionNode) \
  X(RangeNode) \
  X(RationalNode) \
  X(RedoNode) \
  X(RegularExpressionNode) \
  X(RequiredParameterNode) \
  X(RescueModifierNode) \
  X(RescueNode) \
  X(RestParameterNode) \
  X(RetryNode) \
  X(ReturnNode) \
  X(SelfNode) \
  X(SingletonClassNode) \
  X(SourceEncodingNode) \
  X(SourceFileNode) \
  X(SourceLineNode) \
  X(SplatNode) \
  X(StatementsNode) \
  X(StringNode) \
  X(SuperNode) \
  X(SymbolNode) \
  X(TrueNode) \
  X(UndefNode) \
  X(UnlessNode) \
  X(UntilNode) \
  X(WhileNode) \
  X(XStringNode) \
  X(YieldNode)

typedef enum {
  NK_NONE = 0,
#define X(n) NK_##n,
  SP_NODE_KINDS(X)
#undef X
  NK__COUNT
} NodeKind;

/* Integer node-type, computed once per node and cached. NK_NONE if the node
   has no type or an unrecognized one. */
/* Slow path: resolve the type string to a kind and cache it in the node. */
NodeKind nt_kind_resolve(const NodeTable *nt, int id);
/* The kind is cached in the node as kind+1 after the first resolve, so the
   steady-state cost must be a load and a compare -- an out-of-line call here
   measured SLOWER than the inlined byte compare it replaced (the old
   `sp_streq(nt_type(...), "CallNode")` usually differs on the first byte). */
static inline NodeKind nt_kind(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return NK_NONE;
  int k = nt->nodes[id].kind;
  return k ? (NodeKind)(k - 1) : nt_kind_resolve(nt, id);
}

/* Node ids of a given kind, in ascending id order (cached per node table).
   Lets a pass iterate only the nodes it cares about instead of scanning the
   whole table and filtering by kind. *count receives the number of ids. */
const int *nt_nodes_of_kind(const NodeTable *nt, NodeKind k, int *count);

/* Iterate the ids of kind KIND (ascending), binding each to a fresh `int IDV`:
     NT_FOREACH_KIND(nt, NK_CallNode, id) { ... use id ... }
   Replaces `for (int id=0; id<nt->count; id++) { if (kind!=K) continue; ... }`.
   The inner one-shot loop scopes IDV; nesting/multiple uses don't collide. */
typedef struct { const int *ids; int n, i, id; } NtKindIter;
/* A loop body may append nodes (a desugar pass does) and, through anything
   that asks for a kind's ids again, rebuild the cache the iterator is reading.
   The cache keeps a count of live iterators and retires a superseded id
   array instead of freeing it while one is open; the iterator's cleanup
   (run at the loop's end, a break or a return alike) is what lets the
   retired arrays go. */
void nt_kind_iter_open(void);
void nt_kind_iter_close(NtKindIter *it);
static inline NtKindIter nt_kind_iter_begin(const NodeTable *nt, NodeKind k) {
  NtKindIter it; it.ids = nt_nodes_of_kind(nt, k, &it.n); it.i = 0; it.id = -1;
  nt_kind_iter_open();
  return it;
}
static inline int nt_kind_iter_next(NtKindIter *it) {
  if (it->i >= it->n) return 0;
  it->id = it->ids[it->i++];
  return 1;
}
#define NT_FOREACH_KIND(NT, KIND, IDV) \
  for (NtKindIter _it_##IDV __attribute__((cleanup(nt_kind_iter_close))) = nt_kind_iter_begin((NT), (KIND)); nt_kind_iter_next(&_it_##IDV); ) \
    for (int IDV = _it_##IDV.id, _once_##IDV = 1; _once_##IDV; _once_##IDV = 0)

/* Build a node table from the parser's text AST (NUL-terminated). The
   buffer is consumed read-only; the table owns its own copies. Returns
   NULL on allocation failure. */
NodeTable *nt_load_text(const char *text);

void nt_free(NodeTable *nt);

/* Deep-clone the subtree rooted at `root`; returns the new root id (or -1).
   Appends nodes and grows nt->count -- parallel per-node arrays must be
   resized to match afterward. */
int nt_clone_subtree(NodeTable *nt, int root);
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
void nt_node_set_int(NodeTable *nt, int id, const char *key, long long val);
void nt_node_set_ref(NodeTable *nt, int id, const char *key, int child);
void nt_node_set_arr(NodeTable *nt, int id, const char *key, const int *ids, int n);

/* Accessors. id must be in [0, nt->count). Out-of-range ids return the
   given defaults so callers can walk freely without bounds checks. */
/* Resolve a `node_file` id to its source path, or NULL if out of range.
   Used by codegen to emit `#line N "path"` directives in multi-file builds. */
const char *nt_file_path(const NodeTable *nt, int fid);

const char *nt_type(const NodeTable *nt, int id);          /* NULL if unset */
static inline const char *nt_str(const NodeTable *nt, int id, const char *key) {
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
  if (id < 0 || id >= nt->count) return dflt;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->ni; j++)
    if (nd->i[j].disc == d && sp_streq(nd->i[j].key, key)) return nd->i[j].val;
  return dflt;
}
static inline int nt_ref(const NodeTable *nt, int id, const char *key) {
  if (id < 0 || id >= nt->count) return -1;
  const SpNode *nd = &nt->nodes[id];
  unsigned d = sp_field_disc(key);
  for (int j = 0; j < nd->nr; j++)
    if (nd->r[j].disc == d && sp_streq(nd->r[j].key, key)) return nd->r[j].ref;
  return -1;
}
static inline const int *nt_arr(const NodeTable *nt, int id, const char *key, int *out_n) {
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
const char *nt_content(const NodeTable *nt, int id);       /* NULL */

/* Generic child iteration (for structural walks that don't know field
   names). Ref fields and array-field elements are the node's children. */
int        nt_num_refs(const NodeTable *nt, int id);
int        nt_ref_at(const NodeTable *nt, int id, int i);   /* ref value, may be -1 */
int        nt_num_arrs(const NodeTable *nt, int id);
const int *nt_arr_at(const NodeTable *nt, int id, int i, int *out_n);

#include "happy_c.h"

#endif
