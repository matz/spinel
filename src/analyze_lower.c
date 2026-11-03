/* analyze_lower.c -- the load stage's lowerings, as rows of one table.

   A lowering rewrites one surface shape into the forms the passes below it
   read (`m(&nil)` into the blockless call, `x => pat` into a one-arm case,
   `|a, (b, c)|` into a flat parameter and an assignment). Each is a row of
   `lowerings`: the step name its appended nodes are named after (nt_set_pass),
   the kinds of node it enters at, its visit, which rewrites the shape at one
   node, and its left, which says whether that shape is at a node.

   lower_run is the one loop. It visits every node below the count at its
   entry, in ascending id, the nodes no root reaches included (a pass that
   reads the whole table afterwards reads what they hold). A row appends nodes
   past that count and never visits them, as the loops it replaces did not.
   A row that decides from the whole program whether it applies at all has a
   gate, asked once at the start of a run. A row that works on the statements
   of class and module bodies (LF_STMT) is entered at each statement, the
   owner and the body in the lowering context. A row that reads a node's
   parent asks lower_parents, which makes the map of every node's parent when
   a row first needs it in a run. A row that makes shapes it must itself lower
   runs again while a run changes the table (fix).

   Code a step splices into a body after rows have run (a hook's body, the
   text of a class_eval) is a span: lower_span runs over it every row that has
   run, in the order of the table, as their runs over the program would have,
   had the code been there. A row enters at the span's nodes and the nodes
   rows append for it; a row that enters at statements (LF_STMT) takes the
   span's statements, with the body they stand in as its owner and body; a row
   that names the locals of a scope (LF_SCOPE) takes each splice's statements
   as a scope of their own, apart from the body's (one call of a hook, one
   class_eval block); any other row whose kinds include that owner's visits
   the owner, as its run did. A gate asks the whole program, so a span asks it
   again once a step, and a yes runs the row on every span of the step. A gate
   that said 0 over the program and says yes after the splice (the splice
   brought in what it asks for, a def of ` say) makes code outside the spans
   the row's too: lower_settle runs the row over the program once more at the
   end of the step. So does every pass over the whole program (LF_PROGRAM),
   which a span cannot be asked of: once per step, however many spans it
   splices. Nothing in a span is rebuilt for the program: the answers a row
   keeps of the whole program (its gate's among them) last the step.

   SPINEL_LOWER_CHECK=1 checks the rows instead of compiling. After each row
   it reports a node still in the row's shape, and a second run of the row
   that changes the table: a lowering that is not idempotent cannot be
   applied again to code a later pass splices in. At the end of the stage it
   reports every row's shape still present, every row that changes the table
   when the rows run once more in order. The report goes to stderr and the
   compile stops there, as it does under --dump-load-record: a second run
   perturbs the table, so no C follows. */

#include "analyze_internal.h"

enum {
  LF_MARKS = 1,   /* sets marks only: no shape is eliminated, so no left */
  LF_STMT = 2,    /* enters at the statements of the bodies of its kinds, not at the nodes */
  LF_PROGRAM = 4, /* a pass over the whole program: visited once, with id -1, and nothing is asked of a node */
  LF_SCOPE = 8,   /* names the locals of the scope a node of its kinds opens: visited at the node, with the
                     scope's statements and the node that names it in the lowering context (lc->scope) */
  LF_FRAMES = 16  /* enters, besides at its kinds, at every definition whose body opens a frame (class_def_opens_frame):
                     self and definee are the class there, which is what a def, alias or define_method in the body asks */
};
enum { LW_MAX_KINDS = 6 };

typedef struct {
  const char *name;                 /* the step name (nt_set_pass) */
  unsigned flags;                   /* LF_* */
  NodeKind kinds[LW_MAX_KINDS];     /* the kinds it enters at; NK_NONE ends the list */
  LowerFn *visit;                   /* rewrites the shape at id: 1 when it did */
  LowerFn *left;                    /* the shape is at id; NULL: only a second run is checked */
  const char *shape;                /* the shape it eliminates, for the check's report */
  LowerGate *gate;                  /* NULL: none. Asked once at the start of a run; 0 skips it. */
  int fix;                          /* 0: one run. n: runs again while a run changed the table, n runs at most */
  LowerFn *span;                    /* LF_PROGRAM: its form over a span of the walk's (lower_span_ids), run as
                                       the splice is made, before the walk registers what it holds; NULL: none */
} Lowering;

/* In the order the stage runs the rows; a row run twice (nil_block_arg) is
   one row. Where a row must come before another, the order is in the
   stage's list (an_phase_desugar_register). */
static const Lowering lowerings[LW__COUNT] = {
  [LW_NIL_BLOCK_ARG] = { "desugar_nil_block_arg", 0, { NK_CallNode },
    desugar_nil_block_arg_at, desugar_nil_block_arg_left, "`m(&nil)`" },
  [LW_CONST_SET_WRITE] = { "desugar_const_set_write", 0, { NK_CallNode },
    desugar_const_set_write_at, desugar_const_set_write_left, "`M.const_set(:N, v)` with a literal name",
    desugar_const_set_write_gate },
  [LW_THREAD_START] = { "desugar_thread_start", 0, { NK_CallNode },
    desugar_thread_start_at, desugar_thread_start_left, "`Thread.start`, `Thread.fork`", desugar_thread_start_gate },
  [LW_MARSHAL_RESTORE] = { "desugar_marshal_restore", 0, { NK_CallNode },
    desugar_marshal_restore_at, desugar_marshal_restore_left, "`Marshal.restore`", desugar_marshal_restore_gate },
  [LW_SOCKET_OPEN] = { "desugar_socket_open", 0, { NK_CallNode },
    desugar_socket_open_at, desugar_socket_open_left, "a blockless `TCPSocket.open` and the other socket classes'", desugar_socket_open_gate },
  [LW_SYSTEM_OPTIONS] = { "desugar_system_options", 0, { NK_CallNode },
    desugar_system_options_at, desugar_system_options_left, "`system(cmd, out: ..)`", desugar_system_options_gate },
  [LW_IO_POPEN_BLOCK] = { "desugar_io_popen_block", 0, { NK_CallNode },
    desugar_io_popen_block_at, desugar_io_popen_block_left, "`IO.popen(cmd) { }` in read mode", desugar_io_popen_block_gate },
  [LW_USER_BACKTICK] = { "desugar_user_backtick", 0, { NK_XStringNode, NK_InterpolatedXStringNode },
    desugar_user_backtick_at, NULL, "a backtick literal in a program that defines ` itself", desugar_user_backtick_gate },
  [LW_LITERAL_UNDEF_METHOD] = { "desugar_literal_undef_method", LF_STMT | LF_FRAMES, { NK_NONE },
    desugar_literal_undef_method_at, desugar_literal_undef_method_left, "`undef_method :a` in a body" },
  [LW_EXPLICIT_INITIALIZE_CALLS] = { "desugar_explicit_initialize_calls", LF_PROGRAM, { NK_NONE },
    desugar_explicit_initialize_calls_at, NULL, "`initialize(x)` called in a method" },
  [LW_NOARG_DISPLAY_WRAPPERS] = { "desugar_noarg_display_wrappers", LF_STMT | LF_FRAMES, { NK_NONE },
    desugar_noarg_display_wrappers_at, desugar_noarg_display_wrappers_left, "`def inspect(i = 0)` in a body" },
  [LW_BIND_ATTR_HELPERS] = { "desugar_bind_attr_helpers", LF_STMT | LF_FRAMES, { NK_NONE },
    desugar_bind_attr_helpers_at, desugar_bind_attr_helpers_left, "`attr_reader :x` an `instance_method(:x)` names", desugar_bind_attr_helpers_gate },
  [LW_PAREN_BLOCK_ARG] = { "desugar_paren_block_arg", 0, { NK_BlockArgumentNode },
    desugar_paren_block_arg_at, desugar_paren_block_arg_left, "`&(e)`: a block argument in parentheses" },
  [LW_PAREN_OPERANDS] = { "desugar_paren_operands", 0,
    { NK_CallNode, NK_LocalVariableWriteNode, NK_InstanceVariableWriteNode, NK_ClassVariableWriteNode, NK_ConstantWriteNode },
    desugar_paren_operands_at, desugar_paren_operands_left, "`(e).m`, `x = (e)`: a receiver or value in parentheses" },
  [LW_SEND_CLASS_MAKERS] = { "desugar_send_class_makers", 0, { NK_CallNode },
    desugar_send_class_makers_at, desugar_send_class_makers_left, "`Struct.send(:new, ..)`, `Data.send(:define, ..)`" },
  [LW_SELF_SUPERCLASS] = { "desugar_self_superclass", LF_STMT | LF_FRAMES, { NK_NONE },
    desugar_self_superclass_at, desugar_self_superclass_left, "`class I < self` in a class body" },
  [LW_STRUCT_SUBCLASS_NEW_WRITE] = { "desugar_struct_subclass_new_write", 0, { NK_ConstantWriteNode },
    desugar_struct_subclass_new_write_at, desugar_struct_subclass_new_write_left, "`X = K.new(..)` where K < Struct" },
  [LW_REFUSE_SUPERCLASS_EXPRS] = { "refuse_superclass_exprs", 0, { NK_ClassNode },
    refuse_superclass_exprs_at, NULL, "a superclass that is not a constant" },
  [LW_RESCUE_BLOCK_BODIES] = { "desugar_rescue_block_bodies", 0, { NK_BlockNode },
    desugar_rescue_block_bodies_at, desugar_rescue_block_bodies_left, "a block whose body is not a StatementsNode" },
  [LW_MARK_SYM_PROC_BLOCKS] = { "mark_sym_proc_blocks", LF_MARKS, { NK_BlockNode },
    mark_sym_proc_blocks_at, NULL, NULL },
  [LW_BUILDER_BLOCK_PARAM] = { "desugar_builder_block_param", 0, { NK_ConstantWriteNode, NK_ConstantPathWriteNode },
    desugar_builder_block_param_at, desugar_builder_block_param_left, "a class builder's block parameter `|k|`" },
  [LW_BODY_LOCAL_SCOPES] = { "desugar_body_local_scopes", LF_SCOPE | LF_FRAMES, { NK_ClassNode, NK_ModuleNode, NK_SingletonClassNode },
    desugar_body_local_scopes_at, NULL, "a local of a class body" },
  [LW_ERRNO_ALIASES] = { "desugar_errno_aliases", 0, { NK_ConstantPathNode },
    desugar_errno_aliases_at, desugar_errno_aliases_left, "`Errno::EWOULDBLOCK`, an alias of another errno's class" },
  [LW_PAREN_DEF_BODY] = { "desugar_paren_def_body", 0, { NK_DefNode },
    desugar_paren_def_body_at, desugar_paren_def_body_left, "`def m = (a; b)`" },
  [LW_MARK_MATCH_RANGES] = { "mark_match_ranges", LF_MARKS, { NK_WhenNode, NK_CallNode },
    mark_match_ranges_at, NULL, NULL },
  /* after mark_match_ranges: it skips a range an index call has marked */
  [LW_MARK_RANGE_LOCALS] = { "mark_match_ranges", LF_MARKS, { NK_LocalVariableWriteNode },
    mark_range_locals_at, NULL, NULL },
  [LW_DUPLICATE_UNDERSCORE_PARAMS] = { "desugar_duplicate_underscore_params", 0, { NK_DefNode, NK_BlockNode, NK_LambdaNode },
    desugar_duplicate_underscore_params_at, desugar_duplicate_underscore_params_left, "a parameter list that binds one `_x` twice" },
  [LW_ENCODING_QUERIES] = { "desugar_encoding_queries", 0, { NK_CallNode },
    desugar_encoding_queries_at, desugar_encoding_queries_left, "`Encoding.default_internal`, `Encoding.find(\"utf-8\")`" },
  [LW_ALIAS_METHOD_STRING_NAMES] = { "desugar_alias_method_string_names", 0, { NK_CallNode },
    desugar_alias_method_string_names_at, desugar_alias_method_string_names_left, "`alias_method \"a\", \"b\"`" },
  [LW_ALIAS_METHOD_VALUES] = { "desugar_alias_method_values", LF_STMT | LF_FRAMES, { NK_SingletonClassNode },
    desugar_alias_method_values_at, desugar_alias_method_values_left, "a value-used `alias_method` in a body" },
  [LW_REASSIGNED_BLOCK_PARAMS] = { "desugar_reassigned_block_params", 0, { NK_BlockNode },
    desugar_reassigned_block_params_at, desugar_reassigned_block_params_left, "a block parameter the body assigns" },
  [LW_ITSELF_RECEIVER] = { "desugar_itself_receiver", 0, { NK_CallNode },
    desugar_itself_receiver_at, desugar_itself_receiver_left, "`x.itself` on a variable", desugar_itself_receiver_gate },
  [LW_TAP_VALUE_WRITE] = { "desugar_tap_value_write", 0, { NK_LocalVariableWriteNode },
    desugar_tap_value_write_at, NULL, "`x = e.tap { }`" },
  [LW_BLK_PARAM_WRITES] = { "desugar_blk_param_writes", 0, { NK_DefNode },
    desugar_blk_param_writes_at, desugar_blk_param_writes_left, "a write to a method's `&blk` parameter" },
  [LW_REST_PARAM_WRITES] = { "desugar_rest_param_writes", 0, { NK_DefNode },
    desugar_rest_param_writes_at, desugar_rest_param_writes_left, "a write to a method's `*args` parameter" },
  [LW_YIELD_IN_CLOSURE] = { "desugar_yield_in_closure", 0, { NK_DefNode },
    desugar_yield_in_closure_at, desugar_yield_in_closure_left, "a `yield` inside a proc or lambda literal" },
  [LW_SAFE_NAV_ATTR_WRITE] = { "desugar_safe_nav_attr_write", 0,
    { NK_CallOperatorWriteNode, NK_CallOrWriteNode, NK_CallAndWriteNode },
    desugar_safe_nav_attr_write_at, desugar_safe_nav_attr_write_left, "`r&.a op= v`" },
  [LW_CONST_ATTR_OP_ASSIGN] = { "desugar_const_attr_op_assign", 0,
    { NK_CallOperatorWriteNode, NK_CallOrWriteNode, NK_CallAndWriteNode },
    desugar_const_attr_op_assign_at, desugar_const_attr_op_assign_left, "`Klass.a op= v`" },
  [LW_STORED_ENUM_EACH] = { "desugar_stored_enum_each", 0, { NK_CallNode },
    desugar_stored_enum_each_at, NULL, "`e.each { }` on a stored enumerator" },
  [LW_SCOPE_NUMBERED_BLOCK_PARAMS] = { "scope_numbered_block_params", LF_PROGRAM, { NK_NONE },
    scope_numbered_block_params_at, NULL, "two blocks of one scope with numbered parameters" },
  [LW_SINGLETON_CLASS_DEFINE_METHOD] = { "desugar_singleton_class_define_method", LF_STMT | LF_FRAMES, { NK_NONE },
    desugar_singleton_class_define_method_at, desugar_singleton_class_define_method_left, "`singleton_class.define_method` in a body" },
  [LW_EMPTY_HASH_COMPARE_BY_IDENTITY] = { "desugar_empty_hash_compare_by_identity", 0, { NK_CallNode },
    desugar_empty_hash_compare_by_identity_at, desugar_empty_hash_compare_by_identity_left, "`{}.compare_by_identity`" },
  [LW_DEFINE_METHOD_PROC_ARG] = { "desugar_define_method_proc_arg", LF_STMT | LF_FRAMES, { NK_SingletonClassNode },
    desugar_define_method_proc_arg_at, NULL, "`define_method(:m, proc)` in a body" },
  [LW_DEFINE_METHOD_CAPTURES] = { "desugar_define_method_captures", LF_FRAMES, { NK_ProgramNode, NK_ClassNode, NK_ModuleNode, NK_SingletonClassNode },
    desugar_define_method_captures_at, NULL, "a body local a define_method block captures" },
  [LW_DEFINE_METHOD_KEYWORDS] = { "desugar_define_method_keywords", LF_STMT | LF_FRAMES, { NK_SingletonClassNode },
    desugar_define_method_keywords_at, NULL, "a `define_method` block with keyword or rest parameters" },
  [LW_NAME_ANON_BLOCK_KWREST] = { "name_anon_block_kwrest", 0, { NK_BlockNode, NK_LambdaNode },
    name_anon_block_kwrest_at, name_anon_block_kwrest_left, "an anonymous `**` block parameter" },
  [LW_RENAME_SHADOWING_BLOCK_PARAMS] = { "rename_shadowing_block_params", LF_PROGRAM, { NK_NONE },
    rename_shadowing_block_params_at, NULL, "a block parameter that shadows a local", NULL, 0,
    rename_shadowing_block_params_span },
  [LW_SYM_TO_PROC_CALL] = { "desugar_sym_to_proc_call", 0, { NK_CallNode },
    desugar_sym_to_proc_call_at, NULL, "`:m.to_proc.call(r, a)`" },
  [LW_ENUM_CHAIN_SHAPES] = { "desugar_enum_chain_shapes", 0, { NK_CallNode },
    desugar_enum_chain_shapes_at, NULL, "an iterator chain: each_char.with_index, each.with_object, ..", NULL, 2 },
  [LW_BUILTIN_KWSPLAT] = { "desugar_builtin_kwsplat", 0, { NK_CallNode },
    desugar_builtin_kwsplat_at, NULL, "`**h` into step, sample and the other keyword builtins", desugar_builtin_kwsplat_gate },
  [LW_RIGHTWARD_PATTERN] = { "desugar_rightward_pattern", 0, { NK_MatchRequiredNode },
    desugar_rightward_pattern_at, desugar_rightward_pattern_left, "`x => pattern`" },
  [LW_MATCH_PREDICATE] = { "desugar_match_predicate", 0, { NK_MatchPredicateNode },
    desugar_match_predicate_at, desugar_match_predicate_left, "`x in pattern`" },
  [LW_SORT_BY_WITH_INDEX] = { "desugar_sort_by_with_index", 0, { NK_CallNode },
    desugar_sort_by_with_index_at, desugar_sort_by_with_index_left, "`sort_by.with_index { }`" },
  [LW_FORWARDING_TO_REST_CALLEE] = { "desugar_forwarding_to_rest_callee", LF_PROGRAM, { NK_NONE },
    desugar_forwarding_to_rest_callee_at, NULL, "`def m(...) = f(*a, **k)` forwarding to a rest callee" },
  [LW_ANON_BLOCK_PARAM] = { "desugar_anon_block_param", 0, { NK_DefNode },
    desugar_anon_block_param_at, desugar_anon_block_param_left, "an anonymous `&`, `*` or `**` method parameter" },
  [LW_BLOCK_IMPLICIT_REST] = { "desugar_block_implicit_rest", 0, { NK_BlockParametersNode },
    desugar_block_implicit_rest_at, desugar_block_implicit_rest_left, "`|x,|` outside a lambda" },
  [LW_MULTI_VALUE_JUMP] = { "desugar_multi_value_jump", 0,
    { NK_ReturnNode, NK_BreakNode, NK_NextNode, NK_YieldNode, NK_CallNode },
    desugar_multi_value_jump_at, desugar_multi_value_jump_left, "`return *a, b`: a jump or call handing out several values" },
  [LW_BLOCK_DESTRUCTURE_PARAMS] = { "desugar_block_destructure_params", 0, { NK_BlockNode, NK_LambdaNode, NK_DefNode },
    desugar_block_destructure_params_at, desugar_block_destructure_params_left, "`|a, (b, c)|`: a destructuring parameter" },
  [LW_FOR_NONLOCAL_INDEX] = { "desugar_for_nonlocal_index", 0, { NK_ForNode },
    desugar_for_nonlocal_index_at, desugar_for_nonlocal_index_left, "`for $g in xs`: a `for` index that is not a local" },
  [LW_ENUM_FOR_WITH_ARGS] = { "desugar_enum_for_with_args", 0, { NK_DefNode },
    desugar_enum_for_with_args_at, NULL, "`return enum_for(:m, args)`" },
  [LW_ENUMERATOR_PRODUCE] = { "desugar_enumerator_produce", 0, { NK_CallNode },
    desugar_enumerator_produce_at, desugar_enumerator_produce_left, "`Enumerator.produce(x) { }`" },
  [LW_PARAM_DEFAULT_ASSIGNS_LOCAL] = { "desugar_param_default_assigns_local", 0, { NK_DefNode },
    desugar_param_default_assigns_local_at, NULL, "a parameter default that writes a local the body reads" },
  [LW_RECURSIVE_PARAM_DEFAULTS] = { "desugar_recursive_param_defaults", LF_PROGRAM, { NK_NONE },
    desugar_recursive_param_defaults_at, NULL, "a default that calls its own method" },
};

/* ---- the kinds a row enters at ---- */

typedef struct { unsigned long long w[(NK__COUNT + 63) / 64]; } KindSet;

static KindSet kind_set_of(const Lowering *w) {
  KindSet s;
  memset(&s, 0, sizeof s);
  for (int k = 0; k < LW_MAX_KINDS && w->kinds[k]; k++) s.w[w->kinds[k] >> 6] |= 1ull << (w->kinds[k] & 63);
  return s;
}

static int kind_set_has(const KindSet *s, NodeKind k) { return (int)(s->w[k >> 6] >> (k & 63) & 1); }

/* Does row w enter at node id: a node of its kinds (set), or for a row that
   enters at frames (LF_FRAMES) a definition that opens one */
static int lower_enters(const NodeTable *nt, const Lowering *w, const KindSet *set, int id) {
  return kind_set_has(set, nt_kind(nt, id)) || ((w->flags & LF_FRAMES) && class_def_opens_frame(nt, id));
}

/* ---- the parent map ---- */

/* Every node's parent (an_parent_map's: -1 for a root and for the root of a
   subtree no root reaches), made when a row first asks within a run and kept
   for the rest of it, as the maps the rows made for themselves were. A run
   after one that left the table as it found it asks the same map. */
const int *lower_parents(Compiler *c, LowerCtx *lc) {
  const NodeTable *nt = c->nt;
  if (!lc->parent_live) {
    lc->parent_live = 1;
    if (lc->parent && lc->parent_n == nt->count && lc->parent_version == nt->version && lc->parent_edits == nt->edits)
      return lc->parent;
    free(lc->parent);
    lc->parent_n = nt->count;
    lc->parent = an_parent_map(nt);
    if (!lc->parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    lc->parent_version = nt->version;
    lc->parent_edits = nt->edits;
  }
  return lc->parent;
}

/* ---- the driver ---- */

int lower_stmt_at(const NodeTable *nt, const LowerCtx *lc, int stmt) {
  int n = 0;
  const int *st = nt_arr(nt, lc->body, "body", &n);
  for (int i = 0; i < n; i++) if (st[i] == stmt) return i;
  return -1;
}

/* A row's run starts afresh: its scratch, and the parent map checked
   against the table at its first ask (a run before it, or the run a check
   follows, may have edited the table after asking for the map). */
static void lower_scratch_reset(LowerCtx *lc) {
  lc->parent_live = 0;
  lc->serial = 0;
  /* a span's row keeps what the step's first span made it ask of the whole
     program: the program's names and answers (lower_settle drops them) */
  if (!lc->span) {
    for (int i = 0; i < 4; i++) lc->prog_memo[i] = -1;
    for (int i = 0; i < lc->nused; i++) free(lc->used[i]);
    free(lc->used);
    lc->used = NULL;
    lc->nused = 0;
    lc->used_built = 0;
  }
  if (!lc->span) free(lc->ids);   /* a span's are its gate's, kept for the step (span_ids) */
  lc->ids = NULL;
  lc->nids = 0;
  free(lc->aliases);
  lc->aliases = NULL;
  lc->naliases = 0;
}

/* The statements sv[0..sn) of the body of `owner`, for a row that enters at
   statements: a copy, so a statement a visit inserts is not visited */
static int lower_stmts(Compiler *c, LowerCtx *lc, const Lowering *w, int owner, int body, const int *sv, int sn,
                       int ask, int *first) {
  if (sn <= 0) return 0;
  int n = 0;
  int *st = malloc(sizeof(int) * (size_t)sn);
  if (!st) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(st, sv, sizeof(int) * (size_t)sn);
  for (int i = 0; i < sn; i++) {
    lc->owner = owner;
    lc->body = body;
    int r = ask ? w->left(c, lc, st[i]) : w->visit(c, lc, st[i]);
    if (r && first && *first < 0) *first = st[i];
    n += ask ? r != 0 : r;
  }
  free(st);
  return n;
}

/* A row that names a scope's locals (LF_SCOPE), at `owner`: the scope of
   the statements sv[0..sn), which `scope` names */
static int lower_scope(Compiler *c, LowerCtx *lc, const Lowering *w, int owner, int scope, const int *sv, int sn,
                       int ask, int *first) {
  lc->scope = scope;
  lc->scope_stmts = sv;
  lc->scope_n = sn;
  int r = ask ? w->left(c, lc, owner) : w->visit(c, lc, owner);
  lc->scope_stmts = NULL;
  lc->scope_n = 0;
  if (r && first && *first < 0) *first = owner;
  return ask ? r != 0 : r;
}

/* A row's site at node id, one of its kinds */
static int lower_site(Compiler *c, LowerCtx *lc, const Lowering *w, int id, int ask, int *first) {
  const NodeTable *nt = c->nt;
  if (w->flags & LF_SCOPE) {
    int body = class_def_body(nt, id);
    return body >= 0 ? lower_scope(c, lc, w, id, id, &body, 1, ask, first) : 0;
  }
  if (w->flags & LF_STMT) {
    int body = class_def_body(nt, id);
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
    int sn = 0;
    const int *sv = nt_arr(nt, body, "body", &sn);
    return lower_stmts(c, lc, w, id, body, sv, sn, ask, first);
  }
  int r = ask ? w->left(c, lc, id) : w->visit(c, lc, id);
  if (r && first && *first < 0) *first = id;
  return ask ? r != 0 : r;
}

static int lower_span_sites(Compiler *c, LowerCtx *lc, const Lowering *w);
/* Every site of a row, in the order the loops it replaces took them: with
   `ask` 0 its visit rewrites each and the answer is how many it did; with 1
   its left answers for each and the answer is how many are in the shape
   (*first the first of them). A row that enters at nodes takes the nodes below
   the count at entry (the current count when asking) that are of its kinds,
   in ascending id. A row that enters at statements (LF_STMT) takes the nodes
   of its kinds, each a body owner, in ascending id, and the statements the
   owner's body held when it was reached, in order: a statement a visit
   inserts is not visited, one it moves is visited where it stood. */
static int lower_sites(Compiler *c, LowerCtx *lc, const Lowering *w, int ask, int *first) {
  const NodeTable *nt = c->nt;
  KindSet set = kind_set_of(w);
  int n = 0, end = ask ? nt->count : lc->n0;
  if (w->flags & LF_PROGRAM) {
    /* what a whole-program pass rewrote is not counted; whether it changed the table is */
    unsigned long edits = nt->edits;
    int r = ask ? 0 : w->visit(c, lc, -1);
    return r ? r : nt->edits != edits;
  }
  if (lc->span && !ask) return lower_span_sites(c, lc, w);
  for (int id = 0; id < end; id++)
    if (lower_enters(nt, w, &set, id)) n += lower_site(c, lc, w, id, ask, first);
  return n;
}

/* A row's gate, over the program: lower_span asks again a gate that said 0 */
static int lower_gate(Compiler *c, LowerCtx *lc, const Lowering *w) {
  int yes = w->gate(c, lc) != 0;
  lc->gate_no[w - lowerings] = !yes;
  return yes;
}

/* A row's gate in a span, asked of the program once a step, at the first
   span that reaches the row; the ids it collects last the step. A gate that
   said 0 over the program and says yes now leaves the row to lower_settle
   as well. */
static int lower_span_gate(Compiler *c, LowerCtx *lc, const Lowering *w) {
  int row = (int)(w - lowerings);
  if (!lc->span_gate[row]) {
    int yes = w->gate(c, lc) != 0;
    lc->span_gate[row] = yes ? 2 : 1;
    lc->span_ids[row] = lc->ids;
    lc->span_nids[row] = lc->nids;
    if (yes && lc->gate_no[row]) lc->settle[row] = 1;
  }
  lc->ids = lc->span_ids[row];
  lc->nids = lc->span_nids[row];
  return lc->span_gate[row] == 2;
}

/* One run of a row: its gate, its sites. The arrays that follow the table grow
   once, at the end. */
static int lower_once(Compiler *c, LowerCtx *lc, const Lowering *w) {
  lc->n0 = c->nt->count;
  lower_scratch_reset(lc);
  if (w->gate && !(lc->span ? lower_span_gate(c, lc, w) : lower_gate(c, lc, w))) return 0;
  int rewrote = lower_sites(c, lc, w, 0, NULL);
  comp_grow_node_arrays(c);
  return rewrote;
}

/* A row with a run cap (fix) runs again while a run changed the table, up to
   that many runs. */
static int lower_pass(Compiler *c, LowerCtx *lc, const Lowering *w) {
  const NodeTable *nt = c->nt;
  unsigned long edits = nt->edits;
  int count = nt->count, rewrote = lower_once(c, lc, w);
  for (int run = 1; run < w->fix && (nt->edits != edits || nt->count != count); run++) {
    edits = nt->edits;
    count = nt->count;
    rewrote += lower_once(c, lc, w);
  }
  return rewrote;
}

/* ---- SPINEL_LOWER_CHECK ---- */

void lower_say(LowerCtx *lc, const char *row, const char *what) {
  lc->findings++;
  fprintf(stderr, "spinel: lower check: %s: %s\n", row, what);
}

static void lower_where(const NodeTable *nt, int id, char *buf, size_t n) {
  long long ln = nt_int(nt, id, "node_line", 0);
  const char *file = nt_file_path(nt, (int)nt_int(nt, id, "node_file", 0));
  if (!file || !*file) file = nt->source_file;
  if (ln > 0) snprintf(buf, n, "%s:%lld", file && *file ? file : "source.rb", ln);
  else snprintf(buf, n, "no line");
}

/* The nodes of a row's kinds that are still in the shape it eliminates; the
   first is reported. */
static void lower_check_left(Compiler *c, LowerCtx *lc, int row, const char *when) {
  const Lowering *w = &lowerings[row];
  if (!w->left) return;
  const NodeTable *nt = c->nt;
  lc->n0 = nt->count;
  lower_scratch_reset(lc);
  if (w->gate && !w->gate(c, lc)) return;   /* the row does not apply to this program */
  int first = -1, n = lower_sites(c, lc, w, 1, &first);
  if (!n) return;
  char where[600], what[1000];
  lower_where(nt, first, where, sizeof where);
  snprintf(what, sizeof what, "%d node(s) still in the shape %s %s (first %s)", n, w->shape, when, where);
  lower_say(lc, w->name, what);
}

/* Run the row again over the table the first run left, and report a change.
   The row's own hash and count are what is compared. */
static void lower_check_again(Compiler *c, LowerCtx *lc, int row) {
  const Lowering *w = &lowerings[row];
  const NodeTable *nt = c->nt;
  unsigned long long h = nt_content_hash(nt);
  int n = nt->count;
  int again = lower_pass(c, lc, w);
  if (nt_content_hash(nt) == h) return;
  char what[200];
  snprintf(what, sizeof what, "a second run rewrote %d node(s) and changed the table (%d -> %d nodes)", again, n, nt->count);
  lower_say(lc, w->name, what);
}

/* A row is about to blank the subtree at `root`, which it has taken out of the
   tree. Report a node of it that a root still reaches. */
void lower_check_unreachable(Compiler *c, LowerCtx *lc, const char *row, int root) {
  const NodeTable *nt = c->nt;
  int cap = nt_nroots(nt) + 64, top = 0;   /* every root, then the walk's growth */
  int *st = malloc(sizeof(int) * (size_t)cap);
  unsigned char *seen = calloc((size_t)nt->count + 1, 1);
  if (!st || !seen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int pass = 0; pass < 2; pass++) {
    /* pass 0 marks what the roots reach, pass 1 walks the subtree and asks */
    if (pass == 0) for (int r = 0; r < nt_nroots(nt); r++) st[top++ ] = nt_root(nt, r);
    else st[top++] = root;
    while (top > 0) {
      int id = st[--top];
      if (id < 0 || id >= nt->count) continue;
      if (pass == 0) { if (seen[id]) continue; seen[id] = 1; }
      else if (seen[id]) {
        char what[120];
        snprintf(what, sizeof what, "a node (%d) of the subtree it blanked at %d is still in the tree", id, root);
        lower_say(lc, row, what);
        break;
      }
      int nr = nt_num_refs(nt, id), na = nt_num_arrs(nt, id);
      for (int i = 0; i < nr + na; i++) {
        int n = 1, one = 0;
        const int *ch = &one;
        if (i < nr) one = nt_ref_at(nt, id, i); else ch = nt_arr_at(nt, id, i - nr, &n);
        for (int e = 0; e < n; e++) {
          if (top == cap) { cap *= 2; st = realloc(st, sizeof(int) * (size_t)cap); if (!st) { fprintf(stderr, "spinel: out of memory\n"); exit(1); } }
          st[top++] = ch[e];
        }
      }
    }
    top = 0;
  }
  free(st);
  free(seen);
}

void lower_begin(Compiler *c, LowerCtx *lc) {
  (void)c;
  memset(lc, 0, sizeof *lc);
  const char *e = getenv("SPINEL_LOWER_CHECK");
  lc->check = e && *e && *e != '0';
}

int lower_run(Compiler *c, LowerCtx *lc, int row) {
  NodeTable *nt = (NodeTable *)c->nt;
  const Lowering *w = &lowerings[row];
  lc->ran[row] = 1;
  const char *was = nt_set_pass(nt, w->name);
  int rewrote = lower_pass(c, lc, w);
  lc->rewrote[row] += rewrote;
  if (lc->check) {
    lower_check_left(c, lc, row, "after its run");
    lower_check_again(c, lc, row);
  }
  nt_set_pass(nt, was);
  return rewrote;
}

static void lower_release(LowerCtx *lc) {
  lc->span = NULL;
  lower_scratch_reset(lc);
  free(lc->parent);
  free(lc->later);
  blkp_step_free(lc->blkp);
}

/* ---- spans ---- */

typedef struct LowerSpan {
  int owner, body;      /* the frame: the body the statements stand in, and its owner */
  const int *stmts;     /* the spliced statements */
  const int *scopes;    /* per statement, the node that names its scope: one per splice */
  int nstmts;
  int *nodes;           /* their nodes, ascending */
  int nnodes;
  int count0;           /* the count when the span was handed over: the rows' own nodes are past it */
} LowerSpan;

static int int_cmp(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

/* The nodes of the subtrees at stmts, ascending: a walk of the span alone */
static void lower_span_collect(NodeTable *nt, LowerSpan *s) {
  s->nodes = nt_subtree_ids(nt, s->stmts, s->nstmts, &s->nnodes);
  qsort(s->nodes, (size_t)s->nnodes, sizeof(int), int_cmp);
}

int *lower_span_ids(const LowerCtx *lc, int *n) {
  const LowerSpan *s = lc->span;
  int m = s->nnodes + (lc->n0 > s->count0 ? lc->n0 - s->count0 : 0);
  int *ids = malloc(sizeof(int) * (size_t)(m > 0 ? m : 1));
  if (!ids) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(ids, s->nodes, sizeof(int) * (size_t)s->nnodes);
  for (int id = s->count0; id < lc->n0; id++) ids[s->nnodes + id - s->count0] = id;
  *n = m;
  return ids;
}

/* A row that visits a span's owner, in a span of the walk's: left to
   lower_settle, which visits each owner once for all the spans of its body
   (the walk splices one call at a time, and the visit takes the whole
   body). A pair already noted for the row's last owner is not noted again. */
static void lower_later(LowerCtx *lc, int row, int owner) {
  int k = lc->nlater - 1;
  while (k >= 0 && lc->later[2 * k] != row) k--;
  if (k >= 0 && lc->later[2 * k + 1] == owner) return;
  if (lc->nlater == lc->clater) {
    lc->clater = lc->clater ? lc->clater * 2 : 16;
    lc->later = realloc(lc->later, sizeof(int) * 2 * (size_t)lc->clater);
    if (!lc->later) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  lc->later[2 * lc->nlater] = row;
  lc->later[2 * lc->nlater++ + 1] = owner;
}

/* A row's sites in the span: the frame's owner where the row enters at its
   kind (only the spliced statements, for a row that enters at statements;
   each splice's, as a scope of its own, for a row that names a scope's
   locals), then the span's nodes and the ones the rows appended for it,
   ascending */
static int lower_span_sites(Compiler *c, LowerCtx *lc, const Lowering *w) {
  const NodeTable *nt = c->nt;
  const LowerSpan *s = lc->span;
  KindSet set = kind_set_of(w);
  int n = 0;
  if (s->owner >= 0 && lower_enters(nt, w, &set, s->owner)) {
    if (w->flags & LF_STMT) n += lower_stmts(c, lc, w, s->owner, s->body, s->stmts, s->nstmts, 0, NULL);
    else if (w->flags & LF_SCOPE)
      for (int i = 0, j; i < s->nstmts; i = j) {
        for (j = i + 1; j < s->nstmts && s->scopes[j] == s->scopes[i]; j++) {}
        n += lower_scope(c, lc, w, s->owner, s->scopes[i], s->stmts + i, j - i, 0, NULL);
      }
    else if (lc->span_now) lower_later(lc, (int)(w - lowerings), s->owner);
    else n += lower_site(c, lc, w, s->owner, 0, NULL);
  }
  for (int i = 0; i < s->nnodes; i++)
    if (s->nodes[i] < lc->n0 && lower_enters(nt, w, &set, s->nodes[i])) n += lower_site(c, lc, w, s->nodes[i], 0, NULL);
  for (int id = s->count0; id < lc->n0; id++)
    if (lower_enters(nt, w, &set, id)) n += lower_site(c, lc, w, id, 0, NULL);
  return n;
}

/* Statements stmts[0..n), which a step has put into `body` (owner's), lowered
   by every row that has run, in the order of the table; scopes[i] names the
   scope of stmts[i], the same node for the statements of one splice. A
   whole-program row is left to lower_settle. The nodes the rows rewrote. */
int lower_span(Compiler *c, LowerCtx *lc, int owner, int body, const int *stmts, const int *scopes, int n) {
  NodeTable *nt = (NodeTable *)c->nt;
  int any = 0;
  for (int row = 0; row < LW__COUNT && !any; row++) any = lc->ran[row];
  if (!any || n <= 0) return 0;
  if (!lc->span_step) {   /* the step's first span: its rows ask the program afresh */
    lower_scratch_reset(lc);
    lc->span_step = 1;
  }
  LowerSpan s = { owner, body, stmts, scopes, n, NULL, 0, nt->count };
  lower_span_collect(nt, &s);
  lc->span = &s;
  int rewrote = 0;
  for (int row = 0; row < LW__COUNT; row++) {
    if (!lc->ran[row]) continue;
    const Lowering *w = &lowerings[row];
    if (w->flags & LF_PROGRAM) lc->settle[row] = 1;
    if ((w->flags & LF_PROGRAM) && !(lc->span_now && w->span)) continue;
    const char *was = nt_set_pass(nt, w->name);
    int r;
    if (w->flags & LF_PROGRAM) {   /* its span form: the span's nodes, the count at its entry */
      lc->n0 = nt->count;
      lower_scratch_reset(lc);
      r = w->span(c, lc, -1);
      comp_grow_node_arrays(c);
    }
    else r = lower_pass(c, lc, w);
    lc->rewrote[row] += r;
    rewrote += r;
    nt_set_pass(nt, was);
  }
  lc->span = NULL;
  lc->ids = NULL;
  lc->nids = 0;
  free(s.nodes);
  return rewrote;
}

/* The end of a step that lowered spans: each row they left runs once more
   over the program, in the order of the table, and the answers the spans
   kept are dropped. A row left an owner of the walk's spans visits it once
   (lower_later). A row with a span form has already run on the walk's
   spans, before the walk registered what they hold; under
   SPINEL_LOWER_CHECK its run here must change nothing. */
void lower_settle(Compiler *c, LowerCtx *lc) {
  if (!lc->span_step) return;
  NodeTable *nt = (NodeTable *)c->nt;
  int now = lc->span_now;
  lc->span_step = 0;
  lc->span_now = 0;
  lower_scratch_reset(lc);
  blkp_step_free(lc->blkp);
  lc->blkp = NULL;
  for (int row = 0; row < LW__COUNT; row++) {
    free(lc->span_ids[row]);
    lc->span_ids[row] = NULL;
    lc->span_gate[row] = 0;
  }
  for (int row = 0; row < LW__COUNT; row++) {
    for (int k = 0; k < lc->nlater; k++) {
      if (lc->later[2 * k] != row) continue;
      const char *was = nt_set_pass(nt, lowerings[row].name);
      lc->n0 = nt->count;
      lower_scratch_reset(lc);
      lc->rewrote[row] += lower_site(c, lc, &lowerings[row], lc->later[2 * k + 1], 0, NULL);
      comp_grow_node_arrays(c);
      nt_set_pass(nt, was);
    }
    if (!lc->settle[row]) continue;
    lc->settle[row] = 0;
    unsigned long long h = lc->check && now && lowerings[row].span ? nt_content_hash(nt) : 0;
    lower_run(c, lc, row);
    if (h && nt_content_hash(nt) != h)
      lower_say(lc, lowerings[row].name, "its run at the end of the walk changed the table, after the walk registered what its spans hold");
  }
  free(lc->later);
  lc->later = NULL;
  lc->nlater = lc->clater = 0;
}

int lower_row_once(Compiler *c, int row) {
  LowerCtx lc;
  lower_begin(c, &lc);
  lc.check = 0;   /* the check is of the stage's run */
  int rewrote = lower_run(c, &lc, row);
  lower_release(&lc);
  return rewrote;
}

/* The end of the stage: every row's shape, then the rows once more in the
   order of the table. */
static void lower_check_end(Compiler *c, LowerCtx *lc) {
  const NodeTable *nt = c->nt;
  for (int row = 0; row < LW__COUNT; row++) lower_check_left(c, lc, row, "at the end of the stage");
  for (int row = 0; row < LW__COUNT; row++) {
    unsigned long long h = nt_content_hash(nt);
    int n = nt->count;
    int again = lower_pass(c, lc, &lowerings[row]);
    if (nt_content_hash(nt) == h) continue;
    char what[200];
    snprintf(what, sizeof what, "run once more after the stage it rewrote %d node(s) and changed the table (%d -> %d nodes)",
             again, n, nt->count);
    lower_say(lc, lowerings[row].name, what);
  }
  fprintf(stderr, "spinel: lower check: %d rows, %d finding(s); rewrote:", LW__COUNT, lc->findings);
  for (int row = 0; row < LW__COUNT; row++)
    if (lc->rewrote[row] > 0) fprintf(stderr, " %s(%d)", lowerings[row].name, lc->rewrote[row]);
  fprintf(stderr, "\n");
}

void lower_finish(Compiler *c, LowerCtx *lc) {
  if (lc->check) {
    lower_check_end(c, lc);
    exit(0);
  }
  lower_release(lc);
}
