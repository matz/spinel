# Spinel v2 Codegen - Ruby subset AOT compiler backend
#
# Reads text AST from spinel_parse.rb, generates standalone C code.
# Written in Spinel-compilable Ruby subset.
#
# Usage: ruby spinel_codegen.rb ast.txt output.c
#
# All data structures use parallel arrays (no arrays of objects).
# Node fields stored as parallel arrays indexed by integer node ID.

class Compiler
  attr_accessor :out


  def initialize
    @out_lines = "".split(",")
    @out = ""
    @deferred_tuple = ""
    @deferred_lambda = ""
    @indent = 0
    @temp_counter = 0
    @label_counter = 0
 # Body root tracked during scan_writer_calls so the
 # InstanceVariableWriteNode arm can look at the body for push
 # observations on a LVR rhs and derive a promoted array type
 # before pinning the ivar.
    @cur_writer_body = -1

 # ---- AST node storage (parallel arrays by node ID) ----
 # Use "".split(",") for StrArray init (v1 infers StrArray from split)
    @nd_type = "".split(",")
    @nd_name = "".split(",")
    @nd_value = []
    @nd_content = "".split(",")
    @nd_flags = []
    @nd_operator = "".split(",")
    @nd_binop = "".split(",")
    @nd_callop = "".split(",")
    @nd_unescaped = "".split(",")

 # Node references (integer node IDs, -1 = nil)
    @nd_receiver = []
    @nd_arguments = []
    @nd_body = []
    @nd_block = []
    @nd_parameters = []
    @nd_predicate = []
    @nd_subsequent = []
    @nd_else_clause = []
    @nd_left = []
    @nd_right = []
    @nd_constant_path = []
    @nd_superclass = []
    @nd_rest = []
 # ParametersNode#keyword_rest -- holds a KeywordRestParameterNode
 # (def f(**kw)) or NoKeywordsParameterNode (def f(**nil)).
    @nd_keyword_rest = []
    @nd_rescue_clause = []
    @nd_ensure_clause = []
    @nd_expression = []
    @nd_target = []
    @nd_pattern = []
    @nd_key = []
    @nd_reference = []
    @nd_collection = []

 # Node array fields: stored as comma-separated ID strings
    @nd_stmts = "".split(",")
    @nd_args = "".split(",")
    @nd_requireds = "".split(",")
    @nd_optionals = "".split(",")
    @nd_keywords = "".split(",")
    @nd_elements = "".split(",")
    @nd_parts = "".split(",")
    @nd_conditions = "".split(",")
    @nd_exceptions = "".split(",")
    @nd_targets = "".split(",")
    @nd_rights = "".split(",")
 # ParametersNode#posts -- required params after the splat
 # (def f(*r, x, y) → posts = [x, y]). Currently unused by codegen
 # (post-rest parameters aren't observed in test/), but the parser
 # emits the field so future tests get a proper AST.
    @nd_posts = "".split(",")
 # AliasMethodNode / AliasGlobalVariableNode -- parallel ref slots
 # for the new and old names (SymbolNode for methods, GlobalVariableReadNode
 # for globals).
    @nd_new_name = []
    @nd_old_name = []
 # UndefNode -- comma-separated child ids for the SymbolNode names.
    @nd_names = "".split(",")

 # Per-node inferred type, parallel to the other @nd_* arrays.
 # Empty string means "not yet annotated"; node_type falls back to
 # infer_type in that case (transparent during analysis). After
 # `freeze_analysis` runs, every reachable node gets a non-empty
 # entry so the codegen path becomes O(1) per node lookup instead
 # of recursively re-walking the subtree on every infer_type call.
    @nd_inferred_type = "".split(",")
 # 1 once analysis has converged and freeze_analysis has filled
 # @nd_inferred_type. Switches infer_type to consult the cache
 # first; analysis iterations themselves keep recomputing because
 # cached values would otherwise pin to stale converging types.
    @analysis_frozen = 0

 # Per-scope local-decls cache. Indexed by the body node id (bid)
 # of the scope: top-level main uses @root_id, method bodies use
 # @meth_body_ids[i], class instance methods use the bid stored
 # in @cls_meth_bodies[ci][bj], etc. Both arrays are pipe-joined
 # ("name1|name2|...") for compact IR transfer. Empty string at
 # @nd_scope_names[bid] means "no precomputed scope decls for this
 # bid"; codegen falls back to its own scan_locals path in that
 # case (block-iteration bodies, ad-hoc temp scopes, etc.).
    @nd_scope_names = "".split(",")
    @nd_scope_types = "".split(",")

    @nd_count = 0
    @root_id = 0

 # Issue: unresolved-call warnings deduped by "<mname>:<recv_type>"
 # so a hot call site that fails to resolve emits one warning, not N.
    @unresolved_call_warnings = "".split(",")

 # ---- Top-level methods (parallel arrays) ----
    @meth_names = "".split(",")
    @meth_param_names = "".split(",")
    @meth_param_types = "".split(",")
 # Per-param "deferred element" flag: "1" means at least one caller
 # passed an empty `[]` literal (or a local that itself was assigned
 # an empty literal). Used by the param body-push promotion pass
 # to decide whether the param's int_array can be safely
 # promoted to a concrete typed-array based on body usage.
    @meth_param_empty = "".split(",")
    @meth_return_types = "".split(",")
    @meth_body_ids = []
    @meth_has_defaults = "".split(",")
    @meth_rest_index = []

 # ---- Classes (parallel arrays) ----
    @cls_names = "".split(",")
    @cls_parents = "".split(",")
 # per-class list of included module
 # names, semicolon-separated. Modules live in @module_names so
 # the resolution from name -> id is deferred to codegen time.
    @cls_includes = "".split(",")
    @cls_ivar_names = "".split(",")
    @cls_ivar_types = "".split(",")
 # Per-ivar flag: was the ivar's first scanned write a definite
 # literal (IntegerNode / FloatNode / StringNode / ...)? Used to
 # distinguish concrete-literal writes from best-guess inference
 # so type unification only widens to poly when both writes are
 # definite — non-recognized CallNodes default to "int" through
 # infer_ivar_init_type and a naive trust of that produces
 # spurious disagreement.
    @cls_ivar_init_definite = "".split(",")
 # Per-(class, ivar) accumulator of distinct concrete writer
 # types observed by scan_writer_calls. After all writer-scan
 # iterations finish, slots with 2+ distinct entries widen to
 # poly. Observations are recorded with the scope active inside
 # scan_writer_calls (params declared with their iteratively-
 # widened ptypes), so e.g. `value` in `def write_any(value);
 # @id = value` resolves to its caller-pinned type rather than
 # the placeholder "int" outside that scope. Each entry is a
 # comma-separated list of distinct types per ivar; the outer
 # dimension is semicolon-separated and parallel to
 # `@cls_ivar_names[ci]`.
    @cls_ivar_observed_types = "".split(",")
 # Memoization for `find_lv_ivar_alias_in_ast`. Keyed by
 # `"<class_idx>:<lv_name>"`, value is the resolved ivar name (or
 # `""` when the LV has multiple sources / non-ivar writes).
    @lv_alias_cache = {}
 # Memoize cls_find_method_direct(ci, mname) which is called
 # heavily during the fixpoint and otherwise re-splits
 # @cls_meth_names[ci] on every call. Key = "<ci>:<mname>".
 # Invalidated on append_cls_meth (when @cls_meth_names mutates).
    @cls_meth_idx_cache = {}
 # Same shape for cls_method_return — top String#split caller.
 # Key = "<ci>:<mname>", value = the recorded return type.
 # Invalidated on append_cls_meth + at every refresh of
 # @cls_meth_returns (infer_all_returns line 12334).
    @cls_meth_return_cache = {}
 # Same shape for cls_ivar_type. @cls_ivar_types has ~20 mutation
 # sites scattered across record_ivar_observation /
 # update_ivar_type / refine_module_ivar_types / etc., so use a
 # version counter that each writer bumps. cls_ivar_type clears
 # the cache when versions disagree, so the cache is auto-fresh
 # per call without per-site invalidation.
    @cls_ivar_type_cache = {}
    @cls_ivar_types_version = 0
    @cls_ivar_type_cache_version = 0
 # Single-slot caches for the outer "|" split of joined per-class
 # method/param fields. cls_meth_ptypes_get / cls_meth_pnames_get /
 # cls_cmeth_*_get all do `field[ci].split("|")` then
 # `[midx].split(",")` — the outer split is repeated for every
 # (ci, midx) pair. Cache one ci's outer split per field;
 # consecutive calls with the same ci skip the outer split.
    @cmp_outer_ci = -1
    @cmp_outer_split = "".split(",")
    @cmp_outer_version = 0
    @cmn_outer_ci = -1
    @cmn_outer_split = "".split(",")
    @cmn_outer_version = 0
    @ccmp_outer_ci = -1
    @ccmp_outer_split = "".split(",")
    @ccmp_outer_version = 0
    @ccmn_outer_ci = -1
    @ccmn_outer_split = "".split(",")
    @ccmn_outer_version = 0
    @cls_meth_ptypes_version = 0
    @cls_meth_params_version = 0
    @cls_cmeth_ptypes_version = 0
    @cls_cmeth_params_version = 0
 # Top-level (script-scope) ivars. Lowered to `static` file-scope
 # globals because `main()` / top-level `def` bodies have no `self`.
    @toplevel_ivar_names = "".split(",")
    @toplevel_ivar_types = "".split(",")
    @cls_meth_names = "".split(",")
    @cls_meth_params = "".split(",")
    @cls_meth_ptypes = "".split(",")
    @cls_meth_returns = "".split(",")
    @cls_meth_bodies = "".split(",")
    @cls_meth_defaults = "".split(",")
 # Mirror of @meth_param_empty for class methods. Pipe-separated by
 # method, comma-separated by param. .
    @cls_meth_ptypes_empty = "".split(",")
    @cls_attr_readers = "".split(",")
    @cls_attr_writers = "".split(",")
    @cls_cmeth_names = "".split(",")
    @cls_cmeth_params = "".split(",")
    @cls_cmeth_ptypes = "".split(",")
    @cls_cmeth_returns = "".split(",")
    @cls_cmeth_bodies = "".split(",")
    @cls_cmeth_defaults = "".split(",")
 # Per-(class, cmeth) local scope tables. @nd_scope_names is
 # keyed by AST body id only; an inherited class method's body
 # is shared across subclasses, so the per-bid table gets
 # overwritten by whichever subclass scans last. These per-
 # (class, cmj) tables preserve subclass-specific local type
 # info (e.g. Comment.find's `result : sp_Comment *` vs
 # Article.find's `result : sp_Article *`). Outer separator
 # ";" indexes by cmj (mirrors @cls_cmeth_bodies); inner
 # separator "|" matches @nd_scope_names' format so the codegen
 # consumer can split into the same (lnames, ltypes) pair.
    @cls_cmeth_scope_names = "".split(",")
    @cls_cmeth_scope_types = "".split(",")
    @cls_is_value_type = []
 # SRA (scalar replacement of aggregates) eligibility flag per class.
 # Classes marked here can have their non-escaping instances replaced
 # with individual scalar locals. Distinct from value_type: SRA allows
 # attr_writer (mutation is rewritten to per-field assignment).
    @cls_is_sra = []

 # ---- Constants (parallel arrays) ----
    @const_names = "".split(",")
    @const_types = "".split(",")

 # ---- Class variables (@@var) ----
 # Per-(class,name) parallel arrays. Storage is a per-class C global
 # named `cvar_<ClassName>_<var>` (var without the @@ prefix).
 # Spinel's class-var lookup does NOT walk the inheritance chain --
 # each class's @@var is independent. CRuby's hierarchy-shared cvars
 # are a known footgun (mame, ko1, et al. publicly disrecommend
 # them); the simpler per-class semantics fit Spinel's compile-time
 # storage model better. Documented in the test fixtures.
    @cvar_names = "".split(",")
    @cvar_types = "".split(",")
 # Compile-time literal initializer per cvar, if the class-body
 # write was `@@x = <literal>`. "" means "use type-default". This
 # is necessary because Spinel doesn't run class-body statements
 # at startup, so any initializer that's not a fold-able literal
 # leaves the cvar at its type-default until first write.
    @cvar_init_values = "".split(",")
    @const_expr_ids = []
    @const_scope_names = "".split(",")

 # `redo` -- labeled-goto target stack. Each loop emitter pushes
 # a fresh label name when entering an iteration body and pops on
 # exit; a `redo` jumps to the top of the innermost label.
    @redo_label_stack = "".split(",")
    @redo_label_counter = 0

 # `alias $copy $orig` -- maps new gvar name to its target.
 # Populated by collect_all from AliasGlobalVariableNode
 # statements; consulted by sanitize_gvar / scan_features /
 # infer_type so $copy and $orig share storage.
    @galias_new = "".split(",")
    @galias_old = "".split(",")

 # `undef foo` -- per-(class, method-name) registry of removed
 # methods. Recorded by collect_class_method_undef; compile-time
 # enforcement of "call after undef fails" is currently a
 # documented out-of-scope.
    @undef_class_idx = []
    @undef_method = "".split(",")

 # `BEGIN { ... }` bodies, in source-encounter order. Hoisted to
 # the top of main() during emit_main.
    @pre_execution_blocks = []

 # `END { ... }` bodies, in source-encounter order. Each emits a
 # static C function; main() startup registers them via atexit()
 # which naturally invokes handlers LIFO -- matches CRuby's
 # reverse-of-source-order END execution.
    @post_execution_blocks = []

 # ---- Scope stack for local variables ----
    @scope_names = "".split(",")
    @scope_types = "".split(",")
 # Parallel to `@scope_names`: when a local was assigned directly
 # from an ivar read (`lv = @ivar`), record the ivar name here so
 # later sites that need ivar-side metadata (notably the
 # `<poly>[k]` narrowing in `compile_poly_method_call`) can
 # resolve through the alias. Empty string when the local has no
 # such alias (or had a non-ivar write since).
    @scope_ivar_alias = "".split(",")

 # Type-narrow stack for `is_a?`/`kind_of?` guards. While walking
 # the then-arm of `if v.is_a?(Hash)` or the truthy branch of
 # `v.is_a?(Hash) ? a : b`, the narrowed `(var_name,
 # narrowed_type)` is pushed here; find_var_type's top-down
 # lookup picks it up so infer_type / scan / codegen see the
 # narrowed type without per-pass plumbing.
    @type_narrow_names = "".split(",")
    @type_narrow_types = "".split(",")

    @current_class_idx = -1
    @current_method_name = ""
    @current_lexical_scope = ""
    @current_method_return = ""
    @current_method_block_param = ""
 # 1 when the wrapping C function being emitted has a `self`
 # binding (instance method, constructor synthesis). 0 for
 # class methods, module class methods, and top-level free
 # functions. Drives the bare-return-with-obj_<C>-return shape
 # in compile_return_stmt: only when has_self == 1 does a bare
 # `return` whose function returns obj_<C> lower to `return
 # self;`; otherwise it emits the type's default value.
    @current_method_has_self = 0
    @in_main = 0
    @in_loop = 0
    @hoisted_strlen_var = ""
    @hoisted_strlen_recv = ""
    @in_yield_method = 0
    @current_method_yield_arity = 1
    @in_gc_scope = 0
 # Set during the arity-0 instance_eval trampoline inlining so
 # receiverless calls in the spliced block body dispatch against
 # the rebound self (the .instance_eval receiver) instead of the
 # enclosing method's self.
    @instance_eval_self_var = ""
    @instance_eval_self_type = ""

 # During default-arg substitution, when the callee's default
 # expression is inlined into the caller, `self_arrow` consults
 # this override to route `self->iv_X` against the call's
 # explicit receiver instead of the caller's self.
    @self_override = ""

 # Yield/block tracking (parallel with meth_names / cls_meth_names)
    @meth_has_yield = []
    @cls_meth_has_yield = "".split(",")

 # Block function accumulator (emitted before forward decls)
    @block_funcs = ""
    @block_counter = 0

 # Feature flags
    @needs_gc = 0
    @needs_system = 0
    @needs_int_array = 0
    @needs_float_array = 0
    @tuple_types = "".split(",")
    @needs_str_array = 0
    @needs_str_int_hash = 0
    @needs_str_str_hash = 0
    @needs_int_str_hash = 0
    @needs_sym_int_hash = 0
    @needs_sym_str_hash = 0
    @needs_sym_intern = 0
    @needs_setjmp = 0
 # Stack of (class_var, msg_var) pairs naming the snapshot locals
 # emitted at the top of each rescue body. A bare `raise` inside a
 # rescue body re-raises with the snapshotted class+message rather
 # than fabricating a fresh RuntimeError. Empty outside any rescue.
    @rescue_cls_stack = "".split(",")
    @rescue_msg_stack = "".split(",")
    @rescue_depth = 0
 # Stack of ensure-clause node IDs (encoded as strings) currently in
 # scope. Each entry corresponds to an enclosing `begin..ensure..end`
 # whose body is being compiled. When `return` is emitted from inside
 # the body, each ensure body is replayed (innermost-first) before
 # the C `return`, so writebacks in `ensure` execute on early return.
    @ensure_stack = "".split(",")
 # Number of `sp_exc_top++` pushes emitted along the static
 # fall-through path leading to the current emit point but not
 # yet matched by an emitted `sp_exc_top--`. An early `return`
 # emits `sp_exc_top -= N` to balance them, so the caller doesn't
 # longjmp into our stale stack frame after we've returned.
    @setjmp_depth = 0
 # Counter used to mint unique snapshot variable names
 # (`_ensure_cls_<n>`, `_ensure_msg_<n>`) for re-raising the
 # in-flight exception after an ensure body runs on the
 # exception path of a `begin..ensure..end`.
    @ensure_emit_depth = 0
 # Exception variable bindings: parallel stacks of (var_name, cls_var).
 # A `rescue => e` binds `e` to the message string and registers it
 # here so that `e.message`, `e.class`, `e.to_s`, and `e.inspect`
 # dispatch correctly. Pushed at rescue body entry, popped at exit.
    @exc_var_names = "".split(",")
    @exc_var_cls_vars = "".split(",")
    @needs_mutable_str = 0
    @needs_rb_value = 0
    @needs_regexp = 0
    @needs_rand = 0
    @regexp_patterns = "".split(",")
    @regexp_flags = "".split(",")
 # Dynamic-regex (InterpolatedRegularExpressionNode) call-site cache.
 # Each AST node gets a unique idx so the emitter can produce one
 # `sp_re_dyn_<idx>` helper per source location with its own
 # function-scope cache (string key + compiled pattern). Collected
 # in scan_features so indexes are stable across compile_expr visits.
    @dyn_regex_node_ids = []
    @dyn_regex_flags = "".split(",")
 # `var = /lit/` resolution. Parallel arrays: `@local_regex_names`
 # holds the local-variable name and `@local_regex_idx` holds the
 # corresponding `@regexp_patterns` index, or -1 when the same name
 # has any other (non-regex or different-regex) write anywhere in
 # the program — in which case the dispatcher must fall through.
    @local_regex_names = "".split(",")
    @local_regex_idx = []

 # Cache for parse_id_list: AST list fields never change once loaded,
 # so the parsed IntArray can be shared across callers. The `[[0]]`
 # literal teaches Spinel that @parse_id_pool is ptr_array<int_array>;
 # slot 0 is a reserved dummy. PtrArray now scans its elements, so
 # cached IntArrays stay reachable.
    @parse_id_cache = {}
    @parse_id_pool = [[0]]

    @needs_stringio = 0
    @proc_counter = 0
    @proc_funcs = ""

 # @needs_* flags + lazy ivars that the original codebase set inside
 # compile_*/emit_* paths (which no longer live in spinel_analyze.rb).
 # Pre-initialize so dump_analysis_buf can reference them and so spinel
 # sees them as struct fields when self-compiling spinel_analyze.rb.
    @needs_file_io = 0
    @needs_poly_array = 0
    @needs_poly_poly_hash = 0
    @needs_ptr_array = 0
    @needs_str_poly_hash = 0
    @needs_sym_poly_hash = 0
    @cls_cmeth_live = ""
    @cls_meth_live = ""
    @multi_const_inits = "".split(",")

 # Lambda support
    @needs_lambda = 0
    @lambda_counter = 0
    @lambda_funcs = ""
    @lambda_params = "".split(",")
    @lambda_captures = "".split(",")
    @lambda_capture_cell_types = "".split(",")
    @lambda_var_ret_names = "".split(",")
    @lambda_var_ret_types = "".split(",")
    @last_lambda_ret_type = ""
 # `Klass.method(:cls_meth)` generates an adapter trampoline so the
 # Method object's `(void *self, mrb_int...)` ABI fits a class
 # method's no-self C signature. Tracks emitted (Klass, method)
 # pairs to avoid duplicate definitions.
    @cls_method_adapters = "".split(",")

 # Proc closure support (Phase 2)
    @in_proc_body = 0
    @proc_captures = "".split(",")
    @proc_capture_types = "".split(",")

 # Fiber support
    @needs_fiber = 0
    @needs_bigint = 0
    @fiber_counter = 0
    @fiber_funcs = ""
    @in_fiber_body = 0
    @fiber_captures = "".split(",")
    @fiber_capture_types = "".split(",")
    @heap_promoted_names = "".split(",")
    @heap_promoted_cells = "".split(",")

 # Global variables ($x)
    @gvar_names = "".split(",")
    @gvar_types = "".split(",")

 # Poly tracking: functions with params called with different types
    @poly_funcs = "".split(",")
    @poly_param_types = "".split(",")

 # Method reference tracking: var_name -> method_name
    @method_ref_vars = "".split(",")
    @method_ref_names = "".split(",")

 # Open class tracking for built-in types
    @open_class_names = "".split(",")

 # Module tracking: module_name -> body node id
    @module_names = "".split(",")
    @module_body_ids = []
 # Module-level singleton accessors :
 # `class << self; attr_accessor :foo; end` inside `module M`.
 # `@module_acc_consts[i]` is a `;`-separated list of distinct
 # constant names assigned to this slot (Stage 1: single name →
 # inline; Stage 2: multiple names → runtime sentinel switch).
 # Empty string means at least one write was non-constant — the
 # slot falls through to the un-folded path.
    @module_acc_keys = "".split(",")
    @module_acc_consts = "".split(",")

 # ---- FFI state (parallel arrays, populated by scan_ffi_decl) ----
 # Per-module registry:
    @ffi_modules = "".split(",")          # module names that declared FFI
    @ffi_module_libs = "".split(",")      # ";"-joined -l names
    @ffi_module_cflags = "".split(",")    # ";"-joined cc flag strings
 # Function registry (one entry per ffi_func decl):
    @ffi_func_modules = "".split(",")     # owning module name
    @ffi_func_names = "".split(",")       # C symbol name
    @ffi_func_arg_types = "".split(",")   # ";"-joined Spinel type tokens
    @ffi_func_ret_types = "".split(",")   # single Spinel type token
    @ffi_func_arg_specs = "".split(",")   # ";"-joined original specs (uint32, str, …)
    @ffi_func_ret_specs = "".split(",")   # original return spec
 # Buffer registry (one entry per ffi_buffer decl):
    @ffi_buf_modules = "".split(",")
    @ffi_buf_names = "".split(",")
    @ffi_buf_sizes = []                   # int sizes in bytes
 # Reader registry (one entry per ffi_read_* decl):
    @ffi_reader_modules = "".split(",")
    @ffi_reader_names = "".split(",")
    @ffi_reader_kinds = "".split(",")     # "u32", "i32", "ptr"
    @ffi_reader_offsets = []              # int byte offsets

    @pending_method_ref = ""
    @lambda_counter = 0
    @lambda_funcs = ""
    @lambda_params = "".split(",")
    @lambda_captures = "".split(",")
    @lambda_insert_pos = 0
    @cls_method_adapters = "".split(",")

 # Proc closure support (Phase 2)
    @in_proc_body = 0
    @proc_captures = "".split(",")
    @proc_capture_types = "".split(",")

 # Symbol type Phase 2 Step 1: intern table (infrastructure only; unused yet).
    @sym_names = "".split(",")

 # instance_eval block hoisting: parallel arrays indexed by synthetic
 # function id N. Each lifted block becomes a file-scope static
 # function `sp_ieval_<N>` that takes a typed `self` parameter.
    @ieval_counter = 0
    @ieval_class_idxs = []
    @ieval_body_ids = []
  end

 # Backslash-n for C string literals - bootstrap-safe (avoids escape level issues)
  def bsl_n
    92.chr + "n"
  end

 # Backslash for C char literals - bootstrap-safe
  def bsl
    92.chr
  end


 # Parse comma-sep node IDs into IntArray. Manually walks bytes to avoid
 # allocating the intermediate StrArray + substrings that `String#split`
 # would produce — this is called ~100 K times during bootstrap.
 # Results are cached by input string: AST fields are immutable once
 # parsed, so the same IntArray can be shared across callers. Callers
 # must treat the result as read-only.
  def parse_id_list(s)
    if s == ""
      return []
    end
    if @parse_id_cache.key?(s)
      return @parse_id_pool[@parse_id_cache[s]]
    end
    result = []
    bs = s.bytes
    i = 0
    n = bs.length
    num = 0
    while i < n
      b = bs[i]
      if b == 44  # ','
        result.push(num)
        num = 0
      else
        num = num * 10 + (b - 48)
      end
      i = i + 1
    end
    result.push(num)
    @parse_id_cache[s] = @parse_id_pool.length
    @parse_id_pool.push(result)
    result
  end

 # ---- AST reader ----
  def alloc_node
    nid = @nd_count
    @nd_type.push("")
    @nd_name.push("")
    @nd_value.push(0)
    @nd_content.push("")
    @nd_flags.push(0)
    @nd_operator.push("")
    @nd_binop.push("")
    @nd_callop.push("")
    @nd_unescaped.push("")
    @nd_receiver.push(-1)
    @nd_arguments.push(-1)
    @nd_body.push(-1)
    @nd_block.push(-1)
    @nd_parameters.push(-1)
    @nd_predicate.push(-1)
    @nd_subsequent.push(-1)
    @nd_else_clause.push(-1)
    @nd_left.push(-1)
    @nd_right.push(-1)
    @nd_constant_path.push(-1)
    @nd_superclass.push(-1)
    @nd_rest.push(-1)
    @nd_keyword_rest.push(-1)
    @nd_rescue_clause.push(-1)
    @nd_ensure_clause.push(-1)
    @nd_expression.push(-1)
    @nd_target.push(-1)
    @nd_pattern.push(-1)
    @nd_key.push(-1)
    @nd_reference.push(-1)
    @nd_collection.push(-1)
    @nd_stmts.push("")
    @nd_args.push("")
    @nd_requireds.push("")
    @nd_optionals.push("")
    @nd_keywords.push("")
    @nd_elements.push("")
    @nd_parts.push("")
    @nd_conditions.push("")
    @nd_exceptions.push("")
    @nd_targets.push("")
    @nd_rights.push("")
    @nd_posts.push("")
    @nd_new_name.push(-1)
    @nd_old_name.push(-1)
    @nd_names.push("")
    @nd_inferred_type.push("")
    @nd_scope_names.push("")
    @nd_scope_types.push("")
    @nd_count = @nd_count + 1
    nid
  end

  def read_text_ast(data)
    lines = data.split(10.chr)
 # Pass 1: find max node ID
    max_id = 0
    i = 0
    while i < lines.length
      line = lines[i]
      if line.length > 0
        parts = line.split(" ")
        if parts.length >= 2
          if parts.first == "ROOT"
            @root_id = parts[1].to_i
          end
          if parts.first == "N"
            nid = parts[1].to_i
            if nid > max_id
              max_id = nid
            end
          end
        end
      end
      i = i + 1
    end
 # Allocate nodes
    j = 0
    while j <= max_id
      alloc_node
      j = j + 1
    end
 # Pass 2: populate fields
    i = 0
    while i < lines.length
      line = lines[i]
      if line.length > 0
        ast_parse_line(line)
      end
      i = i + 1
    end
  end

  def ast_parse_line(line)
    parts = line.split(" ")
    if parts.length < 3
      return
    end
    tag = parts.first
    nid = parts[1].to_i
    if tag == "N"
      @nd_type[nid] = parts[2]
    end
    if tag == "S"
      field = parts[2]
      val = ""
      if parts.length >= 4
        val = unescape_str(parts[3])
      end
      set_string_field(nid, field, val)
    end
    if tag == "I"
      field = parts[2]
      ival = 0
      if parts.length >= 4
        ival = parts[3].to_i
      end
      set_int_field(nid, field, ival)
    end
    if tag == "F"
      if parts.length >= 4
        @nd_content[nid] = parts[3]
      end
    end
    if tag == "R"
      field = parts[2]
      ref_id = -1
      if parts.length >= 4
        ref_id = parts[3].to_i
      end
      set_ref_field(nid, field, ref_id)
    end
    if tag == "A"
      field = parts[2]
      ids_str = ""
      if parts.length >= 4
        ids_str = parts[3]
      end
      set_array_field(nid, field, ids_str)
    end
    0
  end

  def unescape_str(s)
    result = ""
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "%"
        if i + 2 < s.length
          hex = s[i + 1] + s[i + 2]
          if hex == "0A"
            result = result + 10.chr
            i = i + 3
          else
            if hex == "0D"
              result = result + 13.chr
              i = i + 3
            else
              if hex == "09"
                result = result + 9.chr
                i = i + 3
              else
                if hex == "20"
                  result = result + " "
                  i = i + 3
                else
                  if hex == "25"
                    result = result + "%"
                    i = i + 3
                  else
                    result = result + "%" + hex
                    i = i + 3
                  end
                end
              end
            end
          end
        else
          result = result + ch
          i = i + 1
        end
      else
        result = result + ch
        i = i + 1
      end
    end
    result
  end

  def set_string_field(nid, field, val)
    if field == "name"
      @nd_name[nid] = val
    end
    if field == "content"
      @nd_content[nid] = val
    end
    if field == "value"
      @nd_content[nid] = val
    end
    if field == "operator"
      @nd_operator[nid] = val
    end
    if field == "binary_operator"
      @nd_binop[nid] = val
    end
    if field == "call_operator"
      @nd_callop[nid] = val
    end
    if field == "unescaped"
      @nd_unescaped[nid] = val
    end
    if field == "kind"
 # UnsupportedNode carries the Prism node-type name here so
 # codegen can surface a precise compile error.
      @nd_content[nid] = val
    end
  end

  def set_int_field(nid, field, val)
    if field == "value"
      @nd_value[nid] = val
    end
    if field == "flags"
      @nd_flags[nid] = val
    end
    if field == "number"
      @nd_value[nid] = val
    end
    if field == "maximum"
      @nd_value[nid] = val
    end
    if field == "start_line"
      @nd_value[nid] = val
    end
    if field == "source_line"
 # UnsupportedNode carries the source line so codegen can cite
 # location in the compile error.
      @nd_value[nid] = val
    end
  end

  def set_ref_field(nid, field, ref_id)
    if field == "receiver"
      @nd_receiver[nid] = ref_id
    end
    if field == "arguments"
      @nd_arguments[nid] = ref_id
    end
    if field == "body"
      @nd_body[nid] = ref_id
    end
    if field == "block"
      @nd_block[nid] = ref_id
    end
    if field == "parameters"
      @nd_parameters[nid] = ref_id
    end
    if field == "predicate"
      @nd_predicate[nid] = ref_id
    end
    if field == "subsequent"
      @nd_subsequent[nid] = ref_id
    end
    if field == "else_clause"
      @nd_else_clause[nid] = ref_id
    end
    if field == "left"
      @nd_left[nid] = ref_id
    end
    if field == "right"
      @nd_right[nid] = ref_id
    end
    if field == "constant_path"
      @nd_constant_path[nid] = ref_id
    end
    if field == "superclass"
      @nd_superclass[nid] = ref_id
    end
    if field == "rest"
      @nd_rest[nid] = ref_id
    end
    if field == "keyword_rest"
      @nd_keyword_rest[nid] = ref_id
    end
    if field == "rescue_clause"
      @nd_rescue_clause[nid] = ref_id
    end
    if field == "ensure_clause"
      @nd_ensure_clause[nid] = ref_id
    end
    if field == "expression"
      @nd_expression[nid] = ref_id
    end
    if field == "target"
      @nd_target[nid] = ref_id
    end
    if field == "pattern"
      @nd_pattern[nid] = ref_id
    end
    if field == "key"
      @nd_key[nid] = ref_id
    end
    if field == "reference"
      @nd_reference[nid] = ref_id
    end
    if field == "collection"
      @nd_collection[nid] = ref_id
    end
    if field == "statements"
      @nd_body[nid] = ref_id
    end
    if field == "value"
      @nd_expression[nid] = ref_id
    end
    if field == "index"
      @nd_target[nid] = ref_id
    end
    if field == "parent"
      @nd_receiver[nid] = ref_id
    end
    if field == "rescue_expression"
      @nd_else_clause[nid] = ref_id
    end
    if field == "call"
      @nd_receiver[nid] = ref_id
    end
    if field == "new_name"
 # AliasMethodNode / AliasGlobalVariableNode -- the new-name slot
 # (a SymbolNode for methods, GlobalVariableReadNode for globals).
      @nd_new_name[nid] = ref_id
    end
    if field == "old_name"
      @nd_old_name[nid] = ref_id
    end
  end

  def set_array_field(nid, field, ids_str)
    if field == "body"
      @nd_stmts[nid] = ids_str
    end
    if field == "arguments"
      @nd_args[nid] = ids_str
    end
    if field == "requireds"
      @nd_requireds[nid] = ids_str
    end
    if field == "optionals"
      @nd_optionals[nid] = ids_str
    end
    if field == "keywords"
      @nd_keywords[nid] = ids_str
    end
    if field == "elements"
      @nd_elements[nid] = ids_str
    end
    if field == "parts"
      @nd_parts[nid] = ids_str
    end
    if field == "conditions"
      @nd_conditions[nid] = ids_str
    end
    if field == "exceptions"
      @nd_exceptions[nid] = ids_str
    end
    if field == "lefts"
      @nd_targets[nid] = ids_str
    end
    if field == "targets"
      @nd_targets[nid] = ids_str
    end
    if field == "rights"
      @nd_rights[nid] = ids_str
    end
    if field == "posts"
      @nd_posts[nid] = ids_str
    end
    if field == "names"
 # UndefNode -- list of SymbolNode names to undef.
      @nd_names[nid] = ids_str
    end
  end

 # ---- Convenience: get stmts of a body node ----
  def get_stmts(nid)
    if nid < 0
      return []
    end
 # If it's a StatementsNode, return its stmts
    if @nd_type[nid] == "StatementsNode"
      return parse_id_list(@nd_stmts[nid])
    end
 # Otherwise return single-element array
    result = []
    result.push(nid)
    result
  end

  def get_body_stmts(nid)
    if @nd_type[nid] == "StatementsNode"
      return get_stmts(nid)
    end
    body = @nd_body[nid]
    if body < 0
      return []
    end
    get_stmts(body)
  end

  def get_args(nid)
 # nid is an ArgumentsNode
    if nid < 0
      return []
    end
    if @nd_type[nid] == "ArgumentsNode"
      return parse_id_list(@nd_args[nid])
    end
    result = []
    result.push(nid)
    result
  end

 # Returns 1 if @nd_block[nid] is a literal BlockNode (do/end body),
 # 0 otherwise. Pairs with find_block_arg to dispatch correctly at
 # &block-forwarding call sites (literal block vs. `&proc_var`).

 # Returns the inner expression of a BlockArgumentNode whose payload
 # is a captured proc local (the `&block` form). Returns -1 for
 # absent block-arg, or for shapes the codegen doesn't yet forward
 # — `&:sym` (SymbolNode) and `&nil` (NilNode), which would need
 # symbol-to-proc / nil-as-no-block lowering. Call sites fall
 # through to the no-block path in those cases.

 # Resolves the call-site block-forwarding expression: returns the C
 # expression for the proc to forward at a `&block`-taking call site
 # (a literal block compiles to sp_proc_new(...); a `&proc_var` is
 # the captured `sp_Proc *` local), or "" if the call site provides
 # no block.

 # Returns the body node id for class ci's midx'th method, or -1
 # if midx is out of range or the body id is invalid. Centralises
 # the @cls_meth_bodies[ci].split(";")[midx].to_i parse so detectors
 # don't have to inline it.

 # Returns the name of the class method's single proc-typed param
 # (its `&block` slot), or "" if the signature isn't exactly one
 # proc param. Used by detectors that match the
 # `def m(&b); ...; end` shape (instance_eval trampoline today;
 # extensible to instance_exec, tap, etc.).

 # Detects the exact arity-0 instance_eval trampoline shape:
 # `def m(&b); instance_eval(&b); end`. Returns 1 when the
 # (ci, midx) method body is a single CallNode of `instance_eval`
 # forwarded the method's sole proc-typed param via &-arg, 0
 # otherwise. Spinel inlines these at the call site (yield-style)
 # with self rebound to the receiver — full Ruby instance_eval is
 # dynamic, but this AOT compromise covers the common DSL-trampoline
 # shape. Anything wider falls through to today's silent no-op.

 # Flatten a constant reference into an internal name.
 # C -> C
 # ::C -> C
 # M::C -> M_C
 # A::B::C -> A_B_C
  def const_ref_flat_name(nid)
    if nid < 0
      return ""
    end
    t = @nd_type[nid]
    if t == "ConstantReadNode"
      return @nd_name[nid]
    end
    if t == "ConstantPathNode"
      leaf = @nd_name[nid]
      parent = @nd_receiver[nid]
      if parent < 0
        return leaf
      end
      base = const_ref_flat_name(parent)
      if base == ""
        return ""
      end
      return base + "_" + leaf
    end
    ""
  end

  def const_ref_is_relative(nid)
    if nid < 0
      return 0
    end
    t = @nd_type[nid]
    if t == "ConstantReadNode"
      return 1
    end
    if t == "ConstantPathNode"
      parent = @nd_receiver[nid]
      if parent < 0
        return 0
      end
      pt = @nd_type[parent]
      if pt == "ConstantReadNode"
        return 1
      end
      if pt == "ConstantPathNode"
        return const_ref_is_relative(parent)
      end
      return 0
    end
    0
  end

  def constructor_class_name(recv_nid)
    if recv_nid < 0
      return ""
    end
    rt = @nd_type[recv_nid]
    if rt == "ConstantReadNode" || rt == "ConstantPathNode"
      return resolve_const_ref_name(recv_nid)
    end
    ""
  end

  def module_name_exists(name)
    i = 0
    while i < @module_names.length
      if @module_names[i] == name
        return 1
      end
      i = i + 1
    end
    0
  end

  def const_namespace_exists(name)
    if name == ""
      return 0
    end
    if find_const_idx(name) >= 0
      return 1
    end
    if find_class_idx(name) >= 0
      return 1
    end
    if module_name_exists(name) == 1
      return 1
    end
    0
  end

 # Constant names the codegen recognises as legitimate even when no
 # user-defined class / module / constant of the same name exists.
 # These are dispatcher-handled module-like receivers (Math, File,
 # ENV, Dir, Time, Process, IO), the global ARGV, the built-in type
 # names used in `is_a?` / `case`/`when` arms, and a handful of
 # exception classes referenced by `raise` / `rescue` patterns.

  def current_lexical_scope_name
    if @current_lexical_scope != ""
      return @current_lexical_scope
    end
    if @current_class_idx >= 0
      if @current_class_idx < @cls_names.length
        return @cls_names[@current_class_idx]
      end
      return ""
    end
    if @current_method_name != ""
      cls_idx = @current_method_name.index("_cls_")
 # CRuby returns nil when not found; spinel runtime returns -1.
      if cls_idx != nil && cls_idx >= 0
        return @current_method_name[0, cls_idx]
      end
    end
    ""
  end

  def trim_const_scope_once(name)
    if name == ""
      return ""
    end
    idx = name.rindex("_")
 # CRuby returns nil when not found; spinel runtime returns -1.
 # Treat both as "no underscore — root scope".
    if idx == nil || idx < 0
      return ""
    end
    name[0, idx]
  end

  def resolve_const_read_name(name)
    scope = current_lexical_scope_name
    while scope != ""
      cand = scope + "_" + name
      if const_namespace_exists(cand) == 1
        return cand
      end
      scope = trim_const_scope_once(scope)
    end
    name
  end

  def resolve_const_ref_name(nid)
    if nid < 0
      return ""
    end
    t = @nd_type[nid]
    if t == "ConstantReadNode"
      return resolve_const_read_name(@nd_name[nid])
    end
    if t == "ConstantPathNode"
      leaf = @nd_name[nid]
      parent = @nd_receiver[nid]
      if parent < 0
        return leaf
      end
      base = resolve_const_ref_name(parent)
      if base == ""
        return ""
      end
      return base + "_" + leaf
    end
    ""
  end

 # ---- Scope management ----
  def push_scope
    @scope_names.push("---")
    @scope_types.push("---")
    @scope_ivar_alias.push("---")
    0
  end

  def pop_scope
    while @scope_names.length > 0
      top_name = @scope_names.last
      if top_name == "---"
        @scope_names.pop
        @scope_types.pop
        @scope_ivar_alias.pop
        return
      end
      @scope_names.pop
      @scope_types.pop
      @scope_ivar_alias.pop
    end
  end

  def declare_var(name, vtype)
    @scope_names.push(name)
    @scope_types.push(vtype)
    @scope_ivar_alias.push("")
    0
  end

 # Returns the snapshot class-name C variable for an exception
 # variable currently bound by an enclosing `rescue => name`, or
 # empty string if `name` is not an active exception binding.
  def find_exc_var_cls(name)
    i = @exc_var_names.length - 1
    while i >= 0
      if @exc_var_names[i] == name
        return @exc_var_cls_vars[i]
      end
      i = i - 1
    end
    ""
  end

 # Emit a bare `raise` (no message arg). Inside a rescue body the
 # snapshotted class+message is re-raised; outside any rescue it
 # falls back to a fresh RuntimeError, matching CRuby.

 # Record / clear an ivar alias for a local variable. Walks the
 # `@scope_*` parallel stacks like `set_var_type` does. `iname` is
 # the source ivar name (e.g. `@fetch`); pass `""` to clear.

  def find_var_ivar_alias(name)
    i = @scope_names.length - 1
    while i >= 0
      if @scope_names[i] == name
        return @scope_ivar_alias[i]
      end
      i = i - 1
    end
    ""
  end

  def find_var_type(name)
 # Type-narrow override : walk top-down so the
 # innermost is_a? guard wins. Skipped for the unscoped global
 # call (e.g. ivar lookups) — narrows are about local var typing.
    i = @type_narrow_names.length - 1
    while i >= 0
      if @type_narrow_names[i] == name
        return @type_narrow_types[i]
      end
      i = i - 1
    end
    i = @scope_names.length - 1
    while i >= 0
      if @scope_names[i] == name
        return @scope_types[i]
      end
      i = i - 1
    end
    ""
  end

 # ---- is_a? type narrowing ----
 # `<expr>.is_a?(<Class>)` (or kind_of?) used as the predicate of
 # an if / ternary lets us treat <expr> as <Class>'s type inside
 # the then-arm. The four entry points (push, pop,
 # narrow_type_for_class, parse_is_a_predicate) are called from
 # both inference and codegen sides — keep the API tiny so the
 # narrow context can be plumbed through without leaking state.

  def push_type_narrow(var_name, narrow_type)
    @type_narrow_names.push(var_name)
    @type_narrow_types.push(narrow_type)
  end

  def pop_type_narrow
    @type_narrow_names.pop
    @type_narrow_types.pop
  end

 # Map a Ruby class name to spinel's static type tag for narrowing.
 # Returns "" when the class doesn't have a concrete spinel type
 # we can narrow to (and the call site should leave the var type
 # alone). The first set is concrete primitives — narrow gives a
 # real type win. The Hash / Array group widens to the catch-all
 # poly_* variant since spinel doesn't track a single "any hash"
 # type, but `poly_hash` / `poly_array` reaches every receiver-
 # method dispatch the narrowed var participates in.
  def narrow_type_for_class(cname)
    if cname == "Symbol"
      return "symbol"
    end
    if cname == "Integer" || cname == "Numeric"
      return "int"
    end
    if cname == "Float"
      return "float"
    end
    if cname == "String"
      return "string"
    end
    if cname == "TrueClass" || cname == "FalseClass"
      return "bool"
    end
    if cname == "NilClass"
      return "nil"
    end
 # Hash / Array narrow intentionally omitted: spinel has many
 # concrete hash/array variants (sym_int_hash, str_str_hash,
 # int_array, ...) and no single "any hash" type that supports
 # iteration. Narrowing to a generic "poly_hash" widens callee
 # params via unify_call_types to "poly", which makes the body's
 # `each` loop drop out (no overload for poly recv). The
 # symbolize_keys-style recursive repro in hits this: better
 # to leave the C compile error visible than emit a silently-
 # empty body. Concrete-class narrow (Symbol, Integer, ...)
 # below is unaffected.
    if cname == "Proc"
      return "proc"
    end
    if cname == "Range"
      return "range"
    end
 # User-defined class: narrow to obj_<C> when the class is
 # registered. Otherwise return "" — narrow is a no-op.
    if find_class_idx(cname) >= 0
      return "obj_" + cname
    end
    ""
  end

 # Static evaluation of `<expr>.is_a?(<Class>)` /
 # `.kind_of?(<Class>)` when expr's static type already proves the
 # answer. Returns "TRUE" / "FALSE" / "" (= dynamic). Used at
 # IfNode emit sites to skip the dead arm so the C compiler doesn't
 # type-check a recursion call whose argument types don't match.


 # When the receiver's static type is a non-poly concrete tag, we
 # can safely declare any non-matching is_a? as FALSE. Used by the
 # primitive class branches above.

 # Decode `<expr>.is_a?(<Class>)` / `.kind_of?(<Class>)` into
 # `(var_name, narrow_type)` when expr is a LocalVariableReadNode
 # and the argument is a constant naming a known class. Returns
 # `["", ""]` if the predicate isn't a narrowable shape (the empty
 # var name is the sentinel; the caller skips the push).
  def parse_is_a_predicate(pred_id)
    if pred_id < 0
      return ["", ""]
    end
    if @nd_type[pred_id] != "CallNode"
      return ["", ""]
    end
    pname = @nd_name[pred_id]
    if pname != "is_a?" && pname != "kind_of?"
      return ["", ""]
    end
    expr = @nd_receiver[pred_id]
    if expr < 0 || @nd_type[expr] != "LocalVariableReadNode"
      return ["", ""]
    end
    args = @nd_arguments[pred_id]
    if args < 0
      return ["", ""]
    end
    arg_ids = get_args(args)
    if arg_ids.length < 1
      return ["", ""]
    end
    arg0 = arg_ids[0]
    cname = ""
    if @nd_type[arg0] == "ConstantReadNode"
      cname = @nd_name[arg0]
    elsif @nd_type[arg0] == "ConstantPathNode"
      cname = resolve_const_ref_name(arg0)
    end
    if cname == ""
      return ["", ""]
    end
    nt = narrow_type_for_class(cname)
    if nt == ""
      return ["", ""]
    end
    [@nd_name[expr], nt]
  end

 # Try to evaluate a predicate expression at compile time. Returns
 # "TRUE" / "FALSE" when the result is known statically; "" when it
 # depends on runtime state. Currently handles `<typed>.is_a?(Klass)` /
 # `kind_of?(Klass)` / `instance_of?(Klass)` where the receiver's
 # static type clearly does or does not match the queried class —
 # this lets compile_if_expr / infer_type(IfNode) skip the dead arm
 # so the typed-friendly arm doesn't get widened to poly via unify.

  def set_var_type(name, vtype)
    i = @scope_names.length - 1
    while i >= 0
      if @scope_names[i] == name
        @scope_types[i] = vtype
        return
      end
      i = i - 1
    end
  end

 # ---- Class/Method lookup (all parallel arrays) ----

 # Returns a C expression evaluating to a `mrb_regexp_pattern *`, or "" if
 # the node isn't a regex source. Static literals resolve to their
 # pre-compiled `sp_re_pat_<i>` global; InterpolatedRegularExpressionNode
 # gets a runtime `sp_re_runtime_compile(...)` call. Centralizes the
 # dispatch so each =~/match?/match/gsub/sub/scan/split call site doesn't
 # have to repeat the InterpolatedRegex check.

 # Maps Prism's regex flag bits to the engine's `RE_FLAG_*` values and
 # returns a C bitwise-OR string ("0", "1", "1|6", etc.). Single source
 # of truth used by both the static-regex collector arm in scan_features
 # and the per-call-site helper emitted by emit_dyn_regex_helpers.
 # Prism: IGNORE_CASE=4, EXTENDED=8, MULTI_LINE=16.
 # Engine: IGNORECASE=1, MULTILINE=2, DOTALL=4, EXTENDED=8.
 # Ruby's /m (dot-matches-newline) maps to MULTILINE|DOTALL = 6.
  def regex_engine_flags(nid)
    if @nd_flags[nid] == 0
      return "0"
    end
    f = @nd_flags[nid]
    parts = "".split(",")
    if f & 4 != 0
      parts.push("1")
    end
    if f & 16 != 0
      parts.push("6")
    end
    if f & 8 != 0
      parts.push("8")
    end
    if parts.length == 0
      return "0"
    end
    parts.join("|")
  end

 # Index of an InterpolatedRegularExpressionNode in @dyn_regex_node_ids,
 # or -1 if scan_features hasn't registered it (defensive — should not
 # happen for any reachable node).

  def find_class_idx(name)
    i = 0
    while i < @cls_names.length
      if @cls_names[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

 # Walk @cls_parents starting from `child_idx` and return 1 if we
 # ever land on `ancestor_idx`. Used by `is_a?(<Klass>)` on poly
 # receivers to enumerate descendant cls_ids — `recv.is_a?(C)` is
 # true when recv's class is C or any subclass of C, so we OR
 # together every cls_id whose parent chain reaches C.

  def find_method_idx(name)
    i = 0
    while i < @meth_names.length
      if @meth_names[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

  def method_rest_index(mi)
    if mi >= 0 && mi < @meth_rest_index.length
      return @meth_rest_index[mi]
    end
    -1
  end

  def find_const_idx(name)
    i = 0
    while i < @const_names.length
      if @const_names[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

 # ---- Class variable helpers ----
 # Qualified name for a class-var slot: `<ClassName>_<var>` where
 # <var> drops the leading @@. Codegen emits one C global named
 # `cvar_<qname>` per registered cvar.
  def cvar_qname(class_idx, var_name)
    cls = "Toplevel"
    if class_idx >= 0
      cls = @cls_names[class_idx]
    end
    bare = var_name
    if bare.length >= 2 && bare[0, 2] == "@@"
      bare = bare[2, bare.length - 2]
    end
    cls + "_" + bare
  end

  def find_cvar_idx(qname)
    i = 0
    while i < @cvar_names.length
      if @cvar_names[i] == qname
        return i
      end
      i = i + 1
    end
    -1
  end

 # Register or update a cvar's inferred type. Called from
 # collect_cvars during the pre-pass and (defensively) again from
 # compile_stmt when the write fires.
  def register_cvar(qname, t)
    ci = find_cvar_idx(qname)
    if ci >= 0
 # Widen on type disagreement: int + string -> poly. Conservative
 # for v1 -- when the gap matters we'll revisit.
      if @cvar_types[ci] != t && @cvar_types[ci] != "poly" && t != ""
        @cvar_types[ci] = "poly"
      end
      return ci
    end
    @cvar_names.push(qname)
    @cvar_types.push(t)
    @cvar_init_values.push("")
    @cvar_names.length - 1
  end

 # Try to compile-time fold a class-body cvar initializer. If the
 # value is a simple literal (Integer/Float/String/Symbol/True/
 # False/Nil), capture it as the static decl's initializer so the
 # cvar enters the program with the source's value rather than the
 # type default. Spinel doesn't run class-body statements at
 # startup, so without this fold a `class C; @@x = 42; end` leaves
 # cvar_C_x at 0 until the first write fires.
  def try_fold_cvar_init(qname, value_id)
    if value_id < 0
      return
    end
    vt = @nd_type[value_id]
    lit = ""
    if vt == "IntegerNode"
      lit = @nd_value[value_id].to_s
    end
    if vt == "FloatNode"
      lit = @nd_content[value_id]
    end
    if vt == "StringNode"
      lit = c_string_literal(@nd_content[value_id])
    end
    if vt == "SymbolNode"
      lit = compile_symbol_literal(@nd_content[value_id])
    end
    if vt == "TrueNode"
      lit = "TRUE"
    end
    if vt == "FalseNode"
      lit = "FALSE"
    end
    if vt == "NilNode"
      lit = "0"
    end
    if lit == ""
      return
    end
    ci = find_cvar_idx(qname)
    if ci >= 0
      @cvar_init_values[ci] = lit
    end
  end

 # If the constant's initializer is a simple literal, return the
 # corresponding C expression. Otherwise return "" so callers fall
 # back to cst_<name> lookup. Enables propagation of:
 # N = 10 → 10 at use sites

 # Find method in class (search parent chain)
  def cls_find_method(ci, mname)
    names = @cls_meth_names[ci].split(";")
    j = 0
    while j < names.length
      if names[j] == mname
        return j
      end
      j = j + 1
    end
 # Check parent
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return cls_find_method(pi, mname)
      end
    end
    -1
  end

 # Walk the parent chain looking for a class method
 # (`def self.<mname>`) named `mname`. Returns the class index that
 # defines it, or -1 if not found. Lets `Leaf.all` resolve to
 # `Base.all` (and emit `sp_Base_cls_all(...)`) when Leaf inherits
 # from Base without overriding `.all`. Mirrors cls_method_return's
 # parent walk for instance methods.
 # Walks @cls_parents from `child` looking for `ancestor`.
 # Mirrors codegen's `cls_is_descendant` so the analyze-side
 # divergence check at the `<obj>.class.<m>` inference site
 # can enumerate the recv's descendants without the helper
 # being out of sync. (Codegen has its own copy with the same
 # name; the two diverge only if one side is rebuilt while
 # the other isn't -- the bootstrap fixpoint catches that.)
  def cls_is_descendant(child, ancestor)
    ck = child
    while ck >= 0
      pname = @cls_parents[ck]
      if pname == ""
        return 0
      end
      pi = find_class_idx(pname)
      if pi < 0
        return 0
      end
      if pi == ancestor
        return 1
      end
      ck = pi
    end
    0
  end

  def cls_cmethod_owner(ci, mname)
    if ci < 0
      return -1
    end
    cmnames = @cls_cmeth_names[ci].split(";")
    cj = 0
    while cj < cmnames.length
      if cmnames[cj] == mname
        return ci
      end
      cj = cj + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return cls_cmethod_owner(pi, mname)
      end
    end
    -1
  end

 # `<arr>.method(:op)` for built-in array types: which (recv_type,
 # mname) pairs we can lower into a Method-dispatch adapter. Limited
 # to int_array's bracket ops + push for now (the optcarrot CPU
 # memory-mapping shape `@ram.method(:[]=)`); extend the body to
 # cover more (recv_type, mname) pairs as workloads need them.
  def builtin_array_method_supported(recv_type, mname)
    if recv_type == "int_array"
      if mname == "[]" || mname == "[]=" || mname == "push"
        return 1
      end
    end
    0
  end


 # Emit a per-(array_type, mname) trampoline that fits the Method
 # dispatch ABI `(void *self, mrb_int...) -> mrb_int`. The body
 # forwards to the corresponding `sp_<Pfx>_<op>` runtime function;
 # write-style ops (`[]=`, `push`) return the rhs the way Ruby's
 # bracket-write does. Idempotent: re-emitting the same key is a
 # no-op. Reuses @cls_method_adapters with a `@@`-prefixed key so
 # it cannot collide with a user-class method adapter entry.

 # Emit a one-off adapter that wraps `sp_<Klass>_cls_<mname>` so the
 # Method dispatch ABI `(void *self, mrb_int...)` works on a class
 # method (which has no self param). Idempotent: re-emitting the
 # same (Klass, mname) pair is a no-op. Buffered into @lambda_funcs
 # so the function lands at the same insertion point as lambdas
 # (before main, after forward declarations of every cls method).

  def cls_cmethod_return_inherited(ci, mname)
    owner = cls_cmethod_owner(ci, mname)
    if owner < 0
      return ""
    end
    cmnames = @cls_cmeth_names[owner].split(";")
    cm_returns = @cls_cmeth_returns[owner].split(";")
    cj = 0
    while cj < cmnames.length
      if cmnames[cj] == mname
        if cj < cm_returns.length
          return cm_returns[cj]
        end
        return ""
      end
      cj = cj + 1
    end
    ""
  end

  def cls_method_return(ci, mname)
    ck = ci.to_s + ":" + mname
    if @cls_meth_return_cache.key?(ck)
      return @cls_meth_return_cache[ck]
    end
    names = @cls_meth_names[ci].split(";")
    returns = @cls_meth_returns[ci].split(";")
    j = 0
    while j < names.length
      if names[j] == mname
        ret = "int"
        if j < returns.length
          ret = returns[j]
        end
        @cls_meth_return_cache[ck] = ret
        return ret
      end
      j = j + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        ret = cls_method_return(pi, mname)
        @cls_meth_return_cache[ck] = ret
        return ret
      end
    end
    @cls_meth_return_cache[ck] = "int"
    "int"
  end

 # Get ivar type from class
  def cls_ivar_type(ci, iname)
    if @cls_ivar_type_cache_version != @cls_ivar_types_version
      @cls_ivar_type_cache = {}
      @cls_ivar_type_cache_version = @cls_ivar_types_version
    end
    ck = ci.to_s + ":" + iname
    if @cls_ivar_type_cache.key?(ck)
      return @cls_ivar_type_cache[ck]
    end
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    own_t = ""
    j = 0
    while j < names.length
      if names[j] == iname
        if j < types.length
          own_t = types[j]
        else
          own_t = "int"
        end
        break
      end
      j = j + 1
    end
    parent_t = ""
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0 && ivar_in_chain(pi, iname) == 1
        parent_t = cls_ivar_type(pi, iname)
      end
    end
    result = "int"
    if parent_t != ""
      result = parent_t
    elsif own_t != ""
      result = own_t
    end
    @cls_ivar_type_cache[ck] = result
    result
  end

 # ---- Emit helpers ----



 # ---- Type inference ----
 # `node_type` is the codegen's interface to per-AST-node types.
 # When the cache has been pre-filled (analyze→codegen split path)
 # this is an O(1) array read. Otherwise it's a transparent forward
 # to infer_type. We do NOT lazily fill the cache from emission paths
 # because emission can mutate analysis state (auto_register_attr_*,
 # @needs_* side-flags, etc.) and a cached value would freeze before
 # those mutations land.

  def infer_type(nid)
    if nid < 0
      return "void"
    end
 # During analyze fixpoint, types are still converging; reading
 # the cache would freeze callers on stale values from earlier
 # iterations. Cache lookup is gated by @analysis_frozen so it
 # only kicks in during annotate_all_node_types' bottom-up walk
 # (which runs once after fixpoint converges and benefits from
 # short-circuiting subtree-rewalks via cached children).
    if @analysis_frozen == 1
      cached = @nd_inferred_type[nid]
      if cached != ""
        return cached
      end
    end
 # NOTE: lazy caching during emission is unsafe. Several emit_*
 # methods (emit_global_constants, emit_class_structs, …) call
 # infer_type on nodes whose lexical scope isn't set up yet
 # (emit_main only push_scopes at its top). A cached miss-then-
 # compute under an empty scope freezes in the wrong type
 # (e.g. LocalVariableReadNode falls through to "int" because
 # find_var_type returns ""), and later emit_main calls then
 # see the stale cache. Until annotate_all_node_types mirrors
 # emit_main's three-pass scope refinement so we can pre-fill
 # the cache once with correct context, infer_type stays
 # uncached during emission.
    t = @nd_type[nid]
    if t == "SuperNode" || t == "ForwardingSuperNode"
 # `super` returns whatever the parent's same-named method
 # returns. Walk to the parent's `find_method_owner`-resolved
 # method and read its return type.
      if @current_class_idx >= 0 && @current_method_name != ""
        parent_name_st = @cls_parents[@current_class_idx]
        if parent_name_st != ""
          parent_ci_st = find_class_idx(parent_name_st)
          if parent_ci_st >= 0
            owner_st = find_method_owner(parent_ci_st, @current_method_name)
            if owner_st != ""
              ret_st = cls_method_return(find_class_idx(owner_st), @current_method_name)
              if ret_st != ""
                return ret_st
              end
            end
          end
        end
      end
      return "int"
    end
    if t == "IntegerNode"
      return "int"
    end
    if t == "FloatNode"
      return "float"
    end
    if t == "StringNode"
      return "string"
    end
    if t == "SourceFileNode"
      return "string"
    end
    if t == "SourceEncodingNode"
      return "string"
    end
    if t == "SymbolNode"
      return "symbol"
    end
    if t == "NumberedReferenceReadNode"
      return "string"
    end
    if t == "MatchWriteNode"
      return "int"
    end
    if t == "InterpolatedStringNode"
      return "string"
    end
    if t == "InterpolatedSymbolNode"
 # Spinel doesn't intern dynamic symbols; the runtime value is
 # the assembled string. Use sites that need symbol-typed
 # behaviour (sym_int_hash keys, ===) won't work, but puts/==/
 # string interpolation/regex match all do.
      return "string"
    end
    if t == "BackReferenceReadNode"
 # `$&`, `$`, `$'`, `$~` -- all return the matched-string
 # form. Same shape as NumberedReferenceReadNode at line 1474.
      return "string"
    end
    if t == "TrueNode"
      return "bool"
    end
    if t == "FalseNode"
      return "bool"
    end
    if t == "NilNode"
      return "nil"
    end
    if t == "XStringNode"
      return "string"
    end
    if t == "InterpolatedXStringNode"
      return "string"
    end
    if t == "ArrayNode"
      return infer_array_elem_type(nid)
    end
    if t == "HashNode"
      if is_int_array_lowered_hash(nid) == 1
        return infer_int_keyed_hash_as_array_type(nid)
      end
      return infer_hash_val_type(nid)
    end
    if t == "RangeNode"
      return "range"
    end
    if t == "RescueModifierNode"
 # `expr rescue fallback` — unify the types of the two branches.
 # The fallback always runs on error, so prefer its type when the
 # main branch is a noreturn-shaped expression like a bare `raise`
 # (whose compile_expr returns the int literal `0`).
      t1 = infer_type(@nd_expression[nid])
      t2 = infer_type(@nd_else_clause[nid])
      if t1 == t2
        return t1
      end
      if t2 != "void" && t2 != "nil"
        return t2
      end
      return t1
    end
    if t == "LocalVariableReadNode"
      vt = find_var_type(@nd_name[nid])
      if vt != ""
        return vt
      end
      return "int"
    end
    if t == "LocalVariableWriteNode"
 # `var = expr` used as an expression — the value of the
 # expression is the assigned slot's value (after any boxing
 # done by the LocalVariableWriteNode emit path). Reporting the
 # slot type lets compile_cond_expr know to wrap with
 # sp_poly_truthy when the slot is poly (e.g.
 # `if (sprite = arr[i])` where sprite is a sp_RbVal local).
      vt = find_var_type(@nd_name[nid])
      if vt != ""
        return vt
      end
      return infer_type(@nd_expression[nid])
    end
    if t == "IndexOrWriteNode" || t == "IndexAndWriteNode" || t == "IndexOperatorWriteNode"
 # `recv[k] ||= v` (etc.) as an expression value. The result type
 # is the recv's element type — same shape as Hash#[] / Array#[]
 # — so callers like LocalVariableWriteNode can pick the right
 # local slot type via the same lookup that `recv[k]` would use.
      iow_recv_t = @nd_receiver[nid]
      if iow_recv_t >= 0
        rt_iow_t = infer_type(iow_recv_t)
        leaf_iow = hash_leaf_type(rt_iow_t)
        if leaf_iow != ""
          return leaf_iow
        end
        if rt_iow_t == "int_array" || rt_iow_t == "float_array" || rt_iow_t == "str_array" || rt_iow_t == "sym_array"
          return elem_type_of_array(rt_iow_t)
        end
 # poly_array elements are sp_RbVal; chained IndexOrWriteNode
 # over a poly_array recv (or a poly-typed recv that carries a
 # poly_array at runtime) returns the element value as poly so
 # the next chain link sees an sp_RbVal, not the int default.
        if rt_iow_t == "poly_array" || rt_iow_t == "poly"
          return "poly"
        end
      end
      return "int"
    end
    if t == "GlobalVariableReadNode"
 # `alias $copy $orig` -- a $copy read must look up $orig's
 # registered type so the C codegen sees the correct format
 # specifier when interpolating or printing.
      gname = resolve_gvar_alias(@nd_name[nid])
      gi = 0
      while gi < @gvar_names.length
        if @gvar_names[gi] == gname
          return @gvar_types[gi]
        end
        gi = gi + 1
      end
      return "int"
    end
    if t == "InstanceVariableOrWriteNode" || t == "InstanceVariableAndWriteNode"
 # `(@x ||= expr)` / `(@x &&= expr)` evaluates to @x's slot type
 # (union of the prior value and the rhs, but Spinel widens those
 # via update_ivar_type already, so reading the slot type is the
 # same answer).
      if @current_class_idx >= 0
        return cls_ivar_type(@current_class_idx, @nd_name[nid])
      end
      return "int"
    end
    if t == "InstanceVariableReadNode"
      if @current_class_idx >= 0
        return cls_ivar_type(@current_class_idx, @nd_name[nid])
      end
 # Inside a module class method (`def self.foo` in `module M`,
 # compiled as the top-level `M_cls_foo`), an ivar read like
 # `@slots` resolves to `cst_M_slots` — already handled by
 # compile_expr's matching arm. Mirror that resolution here so
 # infer_type returns the slot's recorded hash/array type
 # instead of the "int" default, which would otherwise route
 # `@slots[k]` through the int-bit-extract codegen even though
 # the storage is a hash.
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != "" && @current_method_name.start_with?(mmod + "_cls_")
          iname = @nd_name[nid]
          cname3 = mmod + "_" + iname[1, iname.length - 1]
          ci3 = find_const_idx(cname3)
          if ci3 >= 0
            return @const_types[ci3]
          end
        end
        mi3 = mi3 + 1
      end
      tit = toplevel_ivar_type(@nd_name[nid])
      if tit != ""
        return tit
      end
      return "int"
    end
    if t == "ClassVariableReadNode"
      qname = cvar_qname(@current_class_idx, @nd_name[nid])
      ci = find_cvar_idx(qname)
      if ci >= 0
        return @cvar_types[ci]
      end
      return "int"
    end
    if t == "ClassVariableWriteNode"
 # `@@x = expr` as an expression returns the assigned value, so
 # the static type is the rhs's type. Without this case the
 # caller's `infer_type` falls through to the default (int) and
 # surrounding code -- e.g. a method whose body's last
 # expression is `@@x = v` -- gets `mrb_int` as the inferred
 # return, producing a const char* / mrb_int mismatch when v
 # is a string.
      return infer_type(@nd_expression[nid])
    end
    if t == "ConstantReadNode"
      if @nd_name[nid] == "ARGV"
        return "argv"
      end
      rname = resolve_const_read_name(@nd_name[nid])
      ci = find_const_idx(rname)
      if ci >= 0
        return @const_types[ci]
      end
      cx = find_class_idx(rname)
      if cx >= 0
 # class constant in value position.
 # Class constants used as method-call receivers go through
 # find_class_idx directly via constructor_class_name and
 # never call infer_type on the receiver.
        return "class"
      end
 # module constant in value
 # position. Modules share the sp_Class representation; the
 # codegen mapping to the unified cls_id space happens on the
 # emit side.
      mx = 0
      while mx < @module_names.length
        if @module_names[mx] == rname
          return "class"
        end
        mx = mx + 1
      end
 # built-in class const in value
 # position (Integer, String, Array, ...). Same sp_Class
 # representation; codegen maps to the reserved cls_id 0..20.
      if is_builtin_class_const_name(rname) == 1
        return "class"
      end
      return "int"
    end
    if t == "ConstantPathNode"
      cpname = resolve_const_ref_name(nid)
      if cpname != ""
        ci = find_const_idx(cpname)
        if ci >= 0
          return @const_types[ci]
        end
        cx = find_class_idx(cpname)
        if cx >= 0
 # class constant in value position.
          return "class"
        end
      end
      parent = @nd_receiver[nid]
      if parent >= 0
        rname = resolve_const_ref_name(parent)
        if rname == "Float"
          return "float"
        end
        if rname == "Math"
          return "float"
        end
      end
      return "int"
    end
    if t == "CallNode"
      return infer_call_type(nid)
    end
    if t == "IfNode"
      then_type = "nil"
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          then_type = infer_type(stmts.last)
        end
      end
      else_type = "nil"
      sub = @nd_subsequent[nid]
      if sub >= 0
        if @nd_type[sub] == "ElseNode"
          ebody = @nd_body[sub]
          if ebody >= 0
            es = get_stmts(ebody)
            if es.length > 0
              else_type = infer_type(es.last)
            end
          end
        else
 # elsif chain — recurse
          else_type = infer_type(sub)
        end
      end
      types = "".split(",")
      types.push(then_type)
      types.push(else_type)
      return unify_return_type(types)
    end
    if t == "UnlessNode"
      then_type = "nil"
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          then_type = infer_type(stmts.last)
        end
      end
      else_type = "nil"
      ec = @nd_else_clause[nid]
      if ec >= 0
        ebody = @nd_body[ec]
        if ebody >= 0
          es = get_stmts(ebody)
          if es.length > 0
            else_type = infer_type(es.last)
          end
        end
      end
      types = "".split(",")
      types.push(then_type)
      types.push(else_type)
      return unify_return_type(types)
    end
    if t == "CaseMatchNode"
      types = "".split(",")
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        inid = conds[k]
        if @nd_type[inid] == "InNode"
          ibody = @nd_body[inid]
          if ibody >= 0
            is = get_stmts(ibody)
            if is.length > 0
              types.push(infer_type(is.last))
            end
          end
        end
        k = k + 1
      end
      ec = @nd_else_clause[nid]
      if ec >= 0
        ebody = @nd_body[ec]
        if ebody >= 0
          es = get_stmts(ebody)
          if es.length > 0
            types.push(infer_type(es.last))
          end
        end
      end
      if types.length > 0
        return unify_return_type(types)
      end
      return "int"
    end
    if t == "CaseNode"
      types = "".split(",")
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        wid = conds[k]
        if @nd_type[wid] == "WhenNode"
          wbody = @nd_body[wid]
          if wbody >= 0
            ws = get_stmts(wbody)
            if ws.length > 0
              types.push(infer_type(ws.last))
            end
          end
        end
        k = k + 1
      end
      ec = @nd_else_clause[nid]
      if ec >= 0
        ebody = @nd_body[ec]
        if ebody >= 0
          es = get_stmts(ebody)
          if es.length > 0
            types.push(infer_type(es.last))
          end
        end
      end
      if types.length > 0
        return unify_return_type(types)
      end
      return "int"
    end
    if t == "AndNode"
      return "bool"
    end
    if t == "OrNode"
      return or_result_type(nid)
    end
    if t == "ParenthesesNode"
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          return infer_type(stmts.last)
        end
      end
      return "void"
    end
    if t == "SelfNode"
      if @current_class_idx >= 0
        return "obj_" + @cls_names[@current_class_idx]
      end
      st = find_var_type("__self_type")
      if st != ""
        return st
      end
      return "int"
    end
    if t == "LambdaNode"
 # Record return type if inside a variable assignment context
      lbody = @nd_body[nid]
      if lbody >= 0
        lbs = get_stmts(lbody)
        if lbs.length > 0
          lrt = infer_type(lbs.last)
          @last_lambda_ret_type = lrt
        end
      end
      return "lambda"
    end
    "int"
  end

  def infer_array_elem_type(nid)
    infer_array_elem_type_from_ids(parse_id_list(@nd_elements[nid]))
  end

 # Body of infer_array_elem_type, parameterised on the list of value
 # node ids. Lets a {0=>v0,1=>v1,...} HashNode lowered to an Array
 # share the same type-inference logic by feeding in [v0, v1, ...]
 # without needing an actual ArrayNode to host them.
  def infer_array_elem_type_from_ids(elems)
    if elems.length > 0
      et = infer_type(elems[0])
      if et == "symbol"
 # Check if ALL elements are symbols
        all_sym = 1
        k = 1
        while k < elems.length
          if infer_type(elems[k]) != "symbol"
            all_sym = 0
          end
          k = k + 1
        end
        if all_sym == 1
          return "sym_array"
        end
        return "poly_array"
      end
      if et == "string"
 # Check if ALL elements are strings
        all_str = 1
        k = 1
        while k < elems.length
          if infer_type(elems[k]) != "string"
            all_str = 0
          end
          k = k + 1
        end
        if all_str == 1
          return "str_array"
        end
        return "poly_array"
      end
      if et == "float"
 # Check if ALL elements are float
        all_float = 1
        k = 1
        while k < elems.length
          if infer_type(elems[k]) != "float"
            all_float = 0
          end
          k = k + 1
        end
        if all_float == 1
          return "float_array"
        end
      end
 # Check if all elements are the same obj type → ptr_array
      if is_obj_type(et) == 1
        all_same = 1
        k = 1
        while k < elems.length
          if infer_type(elems[k]) != et
            all_same = 0
          end
          k = k + 1
        end
        if all_same == 1
          @needs_gc = 1
          return et + "_ptr_array"
        end
        return "poly_array"
      end
 # Check if all elements are the same array type → array of arrays
      if et == "int_array" || et == "str_array" || et == "float_array" || et == "sym_array"
        all_same = 1
        k = 1
        while k < elems.length
          if infer_type(elems[k]) != et
            all_same = 0
          end
          k = k + 1
        end
        if all_same == 1
          @needs_gc = 1
          return et + "_ptr_array"
        end
        return "poly_array"
      end
 # Nested-deeper case: elements are themselves a typed
 # ptr_array (`int_array_ptr_array`, etc.) or already
 # poly_array. Spinel doesn't have a typed
 # `<X>_ptr_array_ptr_array` slot, so box each level via
 # poly_array — sp_box_*_array on each push, and the
 # poly-builtin dispatch on `[]` recurses into the next
 # level.
      if is_ptr_array_type(et) == 1 || et == "poly_array"
        @needs_gc = 1
        @needs_rb_value = 1
        return "poly_array"
      end
 # Hash literals as elements (`[{n: 3}, {n: 1}]`): each
 # element is a heap-allocated hash pointer. Spinel has no
 # typed `<hash>_ptr_array` slot, so box via poly_array;
 # sp_box_hash_to_poly is called on each push and the
 # poly-builtin dispatch on `arr[i]` recovers the hash.
 # Without this arm, the array's inferred type fell back to
 # `int_array` (the bottom of this function), and
 # `sp_IntArray_push` was called with a hash pointer —
 # int-from-pointer C-compile error.
      if is_hash_type(et) == 1
        @needs_gc = 1
        @needs_rb_value = 1
        return "poly_array"
      end
 # Proc / lambda literals as elements (`[proc { ... }, proc { ... }]`):
 # each element is a heap-allocated sp_Proc pointer. Spinel has no
 # typed `proc_ptr_array` slot, so box via poly_array — sp_box_proc
 # is called on each push, and the poly-builtin dispatch on `arr[i]`
 # recovers the proc. Without this arm, the array's inferred type
 # fell back to `int_array`, and `sp_IntArray_push` was called with
 # a sp_Proc pointer — int-from-pointer C-compile error.
      if et == "proc" || et == "lambda"
        @needs_gc = 1
        @needs_rb_value = 1
        return "poly_array"
      end
 # Check if elements have mixed types
      k = 1
      while k < elems.length
        et2 = infer_type(elems[k])
        if et2 != et
          return "poly_array"
        end
        k = k + 1
      end
    end
    "int_array"
  end

 # Detects a Hash literal whose keys are the consecutive non-negative
 # integers 0, 1, ..., N-1 in source order. Such a literal is
 # semantically equivalent to the Array `[v0, v1, ..., vN-1]` for the
 # common `H[k]` lookup pattern, so we lower it to an Array
 # internally — no `int_<X>_hash` runtime type needed. Detection is
 # AST-shape only (IntegerNode key with literal value `k` at index
 # `k`); any deviation (gap, duplicate, non-integer key, splat) opts
 # back into the regular hash codegen.
  def is_int_array_lowered_hash(nid)
    if @nd_type[nid] != "HashNode"
      return 0
    end
    elems = parse_id_list(@nd_elements[nid])
    if elems.length == 0
      return 0
    end
    k = 0
    while k < elems.length
      eid = elems[k]
      if @nd_type[eid] != "AssocNode"
        return 0
      end
      kid = @nd_key[eid]
      if kid < 0 || @nd_type[kid] != "IntegerNode"
        return 0
      end
      if @nd_value[kid].to_i != k
        return 0
      end
      k = k + 1
    end
    1
  end

 # Returns the array type the lowered HashNode evaluates to. Same
 # logic as infer_array_elem_type but reads each AssocNode's value.
  def infer_int_keyed_hash_as_array_type(nid)
    elems = parse_id_list(@nd_elements[nid])
    vids = []
    k = 0
    while k < elems.length
      vids.push(@nd_expression[elems[k]])
      k = k + 1
    end
    infer_array_elem_type_from_ids(vids)
  end

  def infer_hash_val_type(nid)
    rt = infer_hash_val_type_raw(nid)
 # every observed hash variant must flag its template
 # need so emit_hash_runtime instantiates the typedef + helpers.
 # Without this the `{ sym: @ivar }` shape — where the value's
 # concrete type is only resolved on a later inference iteration
 # via the value cache — emits `sp_SymStrHash *` references with
 # no matching typedef in the translation unit.
    mark_hash_needs(rt)
    rt
  end

  def infer_hash_val_type_raw(nid)
    elems = parse_id_list(@nd_elements[nid])
    if elems.length > 0
      eid = elems[0]
      if @nd_type[eid] == "AssocNode"
        first_vt = infer_type(@nd_expression[eid])
 # Check if all values have the same type
        all_same = 1
        k = 1
        while k < elems.length
          eid2 = elems[k]
          if @nd_type[eid2] == "AssocNode"
            vt2 = infer_type(@nd_expression[eid2])
            if vt2 != first_vt
              all_same = 0
            end
          end
          k = k + 1
        end
 # Detect all-symbol keys → sym_int_hash variant for int-valued
 # hashes. (sym_str_hash etc. not yet implemented; they fall
 # through to str_str_hash with sym_to_s wrapping at hash sites.)
        all_sym_keys = 1
        kk = 0
        while kk < elems.length
          ekid = elems[kk]
          if @nd_type[ekid] == "AssocNode"
            kid = @nd_key[ekid]
            if kid < 0 || @nd_type[kid] != "SymbolNode"
              all_sym_keys = 0
            end
          end
          kk = kk + 1
        end
        all_int_keys = 1
        ki = 0
        while ki < elems.length
          ekid2 = elems[ki]
          if @nd_type[ekid2] == "AssocNode"
            kid2 = @nd_key[ekid2]
            if kid2 < 0 || @nd_type[kid2] != "IntegerNode"
              all_int_keys = 0
            end
          end
          ki = ki + 1
        end
        if all_same == 1
          if first_vt == "string"
            if all_int_keys == 1
              return "int_str_hash"
            end
            if all_sym_keys == 1
              return "sym_str_hash"
            end
            return "str_str_hash"
          end
          if all_sym_keys == 1 && (first_vt == "int" || first_vt == "bool" || first_vt == "nil")
            return "sym_int_hash"
          end
 # Every value already inferred as poly (the slot was
 # widened upstream — typically an ivar that
 # finalize_ivar_heterogeneity widened on a sibling-writer
 # disagreement). Use the same poly-hash storage as the
 # mixed-types `else` branch — every value carries its
 # own tag.
          if first_vt == "poly"
            if all_sym_keys == 1
              return "sym_poly_hash"
            end
            return "str_poly_hash"
          end
 # Inner hash/array values need a poly outer so each pointer
 # can carry its own cls_id through SP_TAG_OBJ.
          if is_hash_type(first_vt) == 1 || is_array_type(first_vt) == 1
            if all_sym_keys == 1
              return "sym_poly_hash"
            end
            if all_int_keys == 0
              return "str_poly_hash"
            end
          end
        else
 # Mixed value types: use a *_poly_hash so each slot carries its
 # own tag (sp_RbVal) rather than coercing everything to one type.
          if all_sym_keys == 1
            return "sym_poly_hash"
          end
          return "str_poly_hash"
        end
      end
    end
    "str_int_hash"
  end


 # Returns the inferred C type ("int", "string", "poly", "obj_<Cname>",
 # ...) for the value a CallNode evaluates to.
 #
 # Symmetric with `compile_call_expr` (which returns the C expression
 # for the same node). The two walk identical branch structure:
 #
 # infer_call_type compile_call_expr
 # infer_operator_type ↔ compile_operator_expr
 # infer_constructor_ ↔ compile_constructor_expr
 # type
 # infer_constant_recv_ ↔ compile_constant_recv_expr
 # type
 #
 # The non-paired helpers (infer_comparison_type, infer_method_name_
 # type, infer_recv_method_type, infer_open_class_type) recognise call
 # shapes whose codegen is inlined into compile_call_expr directly
 # rather than factored out, but the dispatch order matches.
 #
 # Maintenance rule: when you add a new call shape, you almost always
 # need both. Forgetting the inference half is the failure mode in
 # — the dispatch emitted the right C function call, but the LHS
 # local was typed `mrb_int` because no inference branch claimed the
 # shape, so `lv_s = sp_M_cls_greet()` mis-typed an `const char *`
 # return. Mirror new cases in both functions, in the same order, with
 # the same recogniser logic.
  def infer_call_type(nid)
    mname = @nd_name[nid]
    recv = @nd_receiver[nid]

 # Methods on a `rescue => e` bound exception variable. The variable
 # itself is string-typed but .class / .message / .to_s / .inspect /
 # .full_message return strings; .backtrace returns nil for now.
    if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
      if find_exc_var_cls(@nd_name[recv]) != ""
        if mname == "message" || mname == "to_s" || mname == "class" || mname == "inspect" || mname == "full_message"
          return "string"
        end
        if mname == "backtrace"
          return "nil"
        end
      end
    end

 # `recv.__sp_ieval_<N>(...)`: the rewritten form of an
 # `recv.instance_eval { ... }` call. v1 only fired on top-level call
 # sites, where the call's value was always discarded — so its return
 # type didn't matter and the warn-fallback "int" was harmless. Now
 # that the rewrite can land inside a class method body whose tail
 # expression IS the instance_eval call, the enclosing method's
 # signature has to match the value the lift actually emits:
 # `compile_ieval_call_expr` returns the receiver via a comma
 # expression, so the type is recv's class. Read the synthetic id's
 # registered class directly from `@ieval_class_idxs` rather than
 # re-inferring recv — by the time this runs (compile-side type
 # iteration), recv's type may have been refined and the registered
 # class is the canonical answer.
    if is_ieval_call_name(mname) == 1
      suffix = mname[11, mname.length - 11]
      n = suffix.to_i
      if n >= 0 && n < @ieval_class_idxs.length
        return "obj_" + @cls_names[@ieval_class_idxs[n]]
      end
    end

 # Chain return type for `Module.accessor.<method>`. All resolved
 # candidates' class methods should agree on a return type; if
 # they disagree the chain becomes poly. Returning early only
 # when we have a confident answer means the existing
 # operator/comparison/etc paths still get to chime in for shapes
 # that don't match this chain.
    if recv >= 0 && @nd_type[recv] == "CallNode"
      inner_recv = @nd_receiver[recv]
      inner_mname = @nd_name[recv]
      if inner_recv >= 0 && @nd_type[inner_recv] == "ConstantReadNode"
        mod_name = @nd_name[inner_recv]
        if module_name_exists(mod_name) == 1
          rconsts = module_acc_resolved(mod_name, inner_mname)
          if rconsts != "" && rconsts != "?"
            cands = rconsts.split(";")
            common = ""
            cands.each { |cn|
              mi = find_method_idx(cn + "_cls_" + mname)
              if mi >= 0
                rt = @meth_return_types[mi]
                if common == ""
                  common = rt
                elsif common != rt
                  common = "poly"
                end
              end
            }
            if common != ""
              return common
            end
          end
        end
      end
    end

 # FFI dispatch must come before operator/comparison resolution: an
 # FFI function name can collide with a Ruby operator (e.g. a C
 # function literally named `pow`), and we want the declared FFI
 # signature to win.
    if recv >= 0
      r = infer_ffi_call_type(nid, mname, recv)
      if r != ""
        return r
      end
    end

 # Operators
    r = infer_operator_type(nid, mname, recv)
    if r != ""
      return r
    end

 # Comparison operators
    r = infer_comparison_type(mname)
    if r != ""
      return r
    end

 # Lambda call return type
    if mname == "call" || mname == "[]"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "lambda"
          if @nd_type[recv] == "LocalVariableReadNode"
            lrt = lambda_var_ret_type(@nd_name[recv])
            if lrt != ""
              return lrt
            end
          end
          return "int"
        end
 # Method#call / Method#[]: the C-level signature lowers to
 # `(void *self, mrb_int...) -> mrb_int`, so the inferred
 # return is `int`. Non-int returns (string, obj, etc.) from
 # the bound underlying method are out of scope. .
        if rt == "obj_Method"
          return "int"
        end
      end
    end

 # `method(:foo)` produces a heap-allocated Method (the synthetic
 # class registered in register_builtin_classes). Two captured
 # forms produce a real Method:
 # - no receiver, inside a class body → bound to `self`.
 # - obj-typed receiver (e.g. `@foo.method(:bar)`) → bound to
 # the inferred receiver, regardless of where the call sits.
 # Top-level `method(:foo)` with no receiver keeps the legacy
 # static-alias placeholder; LocalVariableWriteNode then records
 # the binding and `m.call(x)` rewrites to a direct `sp_<foo>(x)`
 # call. .
    if mname == "method"
      if recv < 0 && @current_class_idx >= 0
        return "obj_Method"
      end
      if recv >= 0
        rt_meth = infer_type(recv)
        if is_obj_type(rt_meth) == 1
          return "obj_Method"
        end
 # `<arr>.method(:op)` on a supported built-in array type
 # also produces a Method (lowered through a per-(type, op)
 # adapter — see emit_builtin_array_method_adapter).
        args_id_meth = @nd_arguments[nid]
        if args_id_meth >= 0
          arg_ids_meth = get_args(args_id_meth)
          if arg_ids_meth.length >= 1
            mref_meth = @nd_content[arg_ids_meth[0]]
            if mref_meth == ""
              mref_meth = @nd_name[arg_ids_meth[0]]
            end
            if builtin_array_method_supported(rt_meth, mref_meth) == 1
              return "obj_Method"
            end
          end
        end
      end
    end

 # `obj.attr = val` (attr-writer call) — Ruby semantics: the
 # assignment expression evaluates to the rhs value, so the
 # inferred type is the rhs argument's type. Without this, a
 # chain like `@a = obj.attr = val` mistypes the outer `@a`
 # as int because the inner call falls through to the int
 # default.
    if mname.length > 1 && mname[mname.length - 1] == "=" &&
       mname != "==" && mname != "!=" && mname != "<=" && mname != ">="
      if recv >= 0
        rt_w = infer_type(recv)
        if is_obj_type(rt_w) == 1
          bname_w = mname[0, mname.length - 1]
          bt_w = base_type(rt_w)
          cname_w = bt_w[4, bt_w.length - 4]
          ci_w = find_class_idx(cname_w)
          if ci_w >= 0 && cls_has_attr_writer(ci_w, bname_w) == 1
            args_id_w = @nd_arguments[nid]
            if args_id_w >= 0
              arg_ids_w = get_args(args_id_w)
              if arg_ids_w.length > 0
                return infer_type(arg_ids_w[0])
              end
            end
          end
        end
      end
    end

 # User-defined top-level method (bare call): take precedence over
 # name-based builtin inference so `def minmax(a,b); ... end; minmax(1,2)`
 # binds to the user def instead of Array#minmax's tuple return.
    if recv < 0
      mi_user = find_method_idx(mname)
      if mi_user >= 0
        return @meth_return_types[mi_user]
      end
 # bare call inside a `def self.X` body resolves to
 # a sibling cmeth on the same class/module. Two signals reach
 # here at different stages: inference sets @current_method_name
 # to "<Class>_cls_<m>" (so the marker scan succeeds); emission
 # sets only @current_method_has_self == 0 + @current_class_idx
 # for real-class cmeths (with @current_method_name = plain
 # mname, no marker).
      if @current_method_name != ""
        mark_cm = @current_method_name.index("_cls_")
        if mark_cm != nil && mark_cm >= 0
          owning_cm = @current_method_name[0, mark_cm]
          if module_name_exists(owning_cm) == 1
            synth_cm = owning_cm + "_cls_" + mname
            mi_cm = find_method_idx(synth_cm)
            if mi_cm >= 0
              return @meth_return_types[mi_cm]
            end
          end
          cci_cm = find_class_idx(owning_cm)
          if cci_cm >= 0
            cmnames_cm = @cls_cmeth_names[cci_cm].split(";")
            cmreturns_cm = @cls_cmeth_returns[cci_cm].split(";")
            cmidx_cm = 0
            while cmidx_cm < cmnames_cm.length
              if cmnames_cm[cmidx_cm] == mname && cmidx_cm < cmreturns_cm.length
                return cmreturns_cm[cmidx_cm]
              end
              cmidx_cm = cmidx_cm + 1
            end
          end
        end
      end
      if @current_class_idx >= 0 && @current_method_has_self == 0
        cmnames_cm_r = @cls_cmeth_names[@current_class_idx].split(";")
        cmreturns_cm_r = @cls_cmeth_returns[@current_class_idx].split(";")
        cmidx_cm_r = 0
        while cmidx_cm_r < cmnames_cm_r.length
          if cmnames_cm_r[cmidx_cm_r] == mname && cmidx_cm_r < cmreturns_cm_r.length
            return cmreturns_cm_r[cmidx_cm_r]
          end
          cmidx_cm_r = cmidx_cm_r + 1
        end
      end
    end

 # Method name-based type inference
    r = infer_method_name_type(nid, mname, recv)
    if r != ""
      return r
    end

 # puts/print
    if mname == "puts"
      return "void"
    end
    if mname == "print"
      return "void"
    end
    if mname == "system"
      return "bool"
    end

 # Constructor .new
    r = infer_constructor_type(nid, mname, recv)
    if r != ""
      return r
    end

 # Constant receiver (File, ENV, Dir) and StringIO
    r = infer_constant_recv_type(nid, mname, recv)
    if r != ""
      return r
    end

 # Math functions, backtick, freeze, to_a
    r = infer_math_and_misc_type(nid, mname, recv)
    if r != ""
      return r
    end

 # Method call on poly/int/obj receiver
    r = infer_recv_method_type(nid, mname, recv)
    if r != ""
      return r
    end

 # Top-level method
    mi = find_method_idx(mname)
    if mi >= 0
      return @meth_return_types[mi]
    end

 # Bare (no-receiver) method call resolved against the enclosing
 # class's method table. Only for `recv < 0` — without that guard,
 # a `Fiber.yield ...` (which has a receiver) inside a class body
 # would short-circuit here returning the int default and never
 # reach the Fiber.yield → poly branch further down.
    if recv < 0 && @current_class_idx >= 0
      mr = cls_method_return(@current_class_idx, mname)
      return mr
    end

 # proc / Proc.new
    if mname == "proc"
      return "proc"
    end
    if mname == "new"
      if recv >= 0
        rcname = constructor_class_name(recv)
        if rcname == "Proc"
          return "proc"
        end
        if rcname == "Fiber"
          return "fiber"
        end
      end
    end
 # fiber.resume returns poly
    if mname == "resume"
      if recv >= 0
        rt = base_type(infer_type(recv))
        if rt == "fiber"
          return "poly"
        end
      end
    end
 # Fiber.yield returns poly
    if mname == "yield"
      if recv >= 0
        rcname = constructor_class_name(recv)
        if rcname == "Fiber"
          return "poly"
        end
      end
    end
 # fiber.alive? returns bool
    if mname == "alive?"
      if recv >= 0
        rt = base_type(infer_type(recv))
        if rt == "fiber"
          return "bool"
        end
      end
    end
 # fiber.transfer returns poly
    if mname == "transfer"
      if recv >= 0
        rt = base_type(infer_type(recv))
        if rt == "fiber"
          return "poly"
        end
      end
    end
 # Fiber.current returns fiber
    if mname == "current"
      if recv >= 0
        rcname = constructor_class_name(recv)
        if rcname == "Fiber"
          return "fiber"
        end
      end
    end

 # Open class method dispatch
    r = infer_open_class_type(nid, mname, recv)
    if r != ""
      return r
    end

    "int"
  end

  def infer_operator_type(nid, mname, recv)
 # Receiver type is consulted by nearly every branch below; compute once.
    lt = ""
    if recv >= 0
      lt = infer_type(recv)
      if lt == "poly"
        if mname == "+" || mname == "-" || mname == "*" || mname == "/" || mname == "%" || mname == "**"
          return "poly"
        end
        if mname == "<<" || mname == ">>" || mname == "&" || mname == "|" || mname == "^" || mname == "-@"
          return "poly"
        end
      end
 # Bigint operators return bigint
      if lt == "bigint"
        if mname == "+" || mname == "-" || mname == "*" || mname == "/" || mname == "%"
          return "bigint"
        end
      end
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = parse_id_list(@nd_args[args_id])
        if aargs.length > 0 && infer_type(aargs[0]) == "bigint"
          if mname == "+" || mname == "-" || mname == "*" || mname == "/" || mname == "%"
            return "bigint"
          end
        end
      end
    end
    if mname == "+"
      if recv >= 0
        if lt == "string"
          return "string"
        end
        if lt == "mutable_str"
          return "string"
        end
        if lt == "poly"
          return "poly"
        end
        if is_array_type(lt) == 1
          return lt
        end
        if lt == "float"
          return "float"
        end
        if lt == "complex"
          return "complex"
        end
 # Check RHS for float promotion
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            rt2 = infer_type(aargs.first)
            if rt2 == "float"
              return "float"
            end
          end
        end
      end
      return "int"
    end
    if mname == "-"
      if recv >= 0
        if lt == "float"
          return "float"
        end
        if lt == "time"
          return "float"
        end
        if is_typed_array_type(lt)
          return lt
        end
 # Check RHS for float promotion
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            rt2 = infer_type(aargs.first)
            if rt2 == "float"
              return "float"
            end
          end
        end
      end
      return "int"
    end
    if mname == "*"
      if recv >= 0
        if lt == "float"
          return "float"
        end
        if lt == "string"
          return "string"
        end
        if lt == "poly"
          return "poly"
        end
        if is_array_type(lt) == 1
 # Array#* (repeat) yields another array of the same element type.
          return lt
        end
        if lt == "complex"
          return "complex"
        end
 # Check RHS for float promotion
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            rt2 = infer_type(aargs.first)
            if rt2 == "float"
              return "float"
            end
          end
        end
      end
      return "int"
    end
    if mname == "/"
      if recv >= 0
        if lt == "float"
          return "float"
        end
 # Check RHS for float promotion
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            rt2 = infer_type(aargs.first)
            if rt2 == "float"
              return "float"
            end
          end
        end
      end
      return "int"
    end
    if mname == "=~"
      return "int"
    end
    if mname == "<<"
      if recv >= 0
        if lt == "mutable_str"
          return "mutable_str"
        end
 # Array `<<` returns the recv (so `(arr << x) << y` chains).
        if is_array_type(lt) == 1
          return lt
        end
      end
      return "int"
    end
    if mname == "&" || mname == "|"
      if recv >= 0
        if is_typed_array_type(lt)
          return lt
        end
      end
      return "int"
    end
    if mname == "%"
 # String#% returns "string" when the LHS is a string (and the RHS
 # is a str_array or a single primitive value). Otherwise the
 # operator is integer modulo.
      if recv >= 0
        rt = infer_type(recv)
        if rt == "string" || rt == "mutable_str"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length > 0
              at = infer_type(aargs[0])
              if at == "str_array"
                return "string"
              end
              if rt == "string"
                return "string"
              end
            end
          end
        end
      end
      return "int"
    end
    if mname == "-@"
      if recv >= 0
        return lt
      end
      return "int"
    end
    ""
  end

  def infer_comparison_type(mname)
    if mname == "<"
      return "bool"
    end
    if mname == ">"
      return "bool"
    end
    if mname == "<="
      return "bool"
    end
    if mname == ">="
      return "bool"
    end
    if mname == "=="
      return "bool"
    end
    if mname == "!="
      return "bool"
    end
    if mname == "!"
      return "bool"
    end
    ""
  end

 # does any user class declare `mname` as an
 # instance method? Used by the hardcoded name-based inference
 # arms below to defer when the recv's type isn't pinned yet
 # but a user-class definition exists that could win at runtime.
 # Falling through to infer_recv_method_type lets the
 # cls_method_return path pick up the user's return type while
 # still terminating at "int" if no obj_<C> resolution lands.
  def any_user_class_defines_imeth(mname)
    ck = 0
    while ck < @cls_names.length
      names = @cls_meth_names[ck].split(";")
      kk = 0
      while kk < names.length
        if names[kk] == mname
          return 1
        end
        kk = kk + 1
      end
      ck = ck + 1
    end
    0
  end

 # recv is plausibly a user-class instance at runtime
 # even when its current inferred type is "int" or "" (because the
 # var-type table hasn't been populated yet during the iterative
 # inference loop). Returns 1 for LocalVariableReadNode /
 # CallNode / InstanceVariableReadNode whose actual runtime type
 # is obj_<C>. False for literal nodes (IntegerNode, StringNode,
 # etc.) where the recv is statically a primitive.
  def recv_could_be_obj(recv)
    if recv < 0
      return 0
    end
    t_rc = @nd_type[recv]
    if t_rc == "IntegerNode" || t_rc == "StringNode" || t_rc == "FloatNode" || t_rc == "SymbolNode" || t_rc == "TrueNode" || t_rc == "FalseNode" || t_rc == "NilNode" || t_rc == "ArrayNode" || t_rc == "HashNode" || t_rc == "RangeNode"
      return 0
    end
    1
  end

  def infer_method_name_type(nid, mname, recv)
 # when recv is a class/module constant ref whose
 # class/module defines a class method of the given name, defer
 # to the receiver-aware resolution that
 # infer_constant_recv_type runs later. Without this guard, the
 # name-based hardcodes below (`hash -> int`, `to_s -> string`,
 # etc.) would pre-empt user-defined `def self.hash(plain) ->
 # String` cmeths, widening downstream local types to int and
 # cascading into incompatible-pointer C errors at the next
 # use site. Return "" so the caller falls through to
 # infer_constant_recv_type, which already does the
 # cls_cmethod_return_inherited / `<Mod>_cls_<m>` lookup.
 #
 # Symmetric instance-method case: when recv is a statically
 # typed obj_<C> instance and C (or any ancestor) defines an
 # instance method named mname, defer to infer_recv_method_type
 # so its cls_method_return path returns the user's type
 # instead of the hardcoded Object#... default. Mirrors the
 # cmeth defer above; both target the imeth shape Ori called
 # out (`Item#hash` shadowed by Object#hash returning int).
    if recv >= 0
      rt_i = infer_type(recv)
      if is_obj_type(rt_i) == 1
        bt_i = base_type(rt_i)
        cname_i = bt_i[4, bt_i.length - 4]
        ci_i = find_class_idx(cname_i)
        if ci_i >= 0 && cls_find_method(ci_i, mname) >= 0
          return ""
        end
      end
    end
    if recv >= 0 && (@nd_type[recv] == "ConstantReadNode" || @nd_type[recv] == "ConstantPathNode")
      rcname = constructor_class_name(recv)
      if rcname != ""
        ci = find_class_idx(rcname)
        if ci >= 0 && cls_cmethod_owner(ci, mname) >= 0
          return ""
        end
        if module_name_exists(rcname) == 1
          if find_method_idx(rcname + "_cls_" + mname) >= 0
            return ""
          end
        end
      end
    end
    if mname == "length"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "int"
    end
    if mname == "to_s"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "string"
    end
    if mname == "inspect"
      return "string"
    end
 # Class#name -- the class's source name as
 # a string. Aliases `.to_s` at the runtime helper level
 # (sp_class_to_s), so the return type is the same.
    if mname == "name"
      if recv >= 0 && infer_type(recv) == "class"
        return "string"
      end
    end
 # hierarchy queries on a Class value.
 # .superclass -> class (the parent or sp_Class{-1})
 # .ancestors -> poly_array of boxed sp_Class
 # <, <=, >, >= -> bool
    if mname == "superclass"
      if recv >= 0 && infer_type(recv) == "class"
        return "class"
      end
    end
    if mname == "ancestors"
      if recv >= 0 && infer_type(recv) == "class"
        return "poly_array"
      end
    end
    if mname == "<" || mname == "<=" || mname == ">" || mname == ">="
      if recv >= 0 && infer_type(recv) == "class"
        return "bool"
      end
    end
 # Tier 5: `<sp_Class>.new` returns a poly value
 # (boxed user instance) only when the recv is a *dynamic*
 # sp_Class -- a local / param / ivar carrying a class value.
 # Constant-path receivers (Foo, M::Sub, etc.) still go through
 # the static constructor path so `obj = Foo.new(args)` stays
 # typed obj_Foo and the existing argful-construction emit
 # works unchanged.
    if mname == "new"
      if recv >= 0 && infer_type(recv) == "class"
        rty_n = @nd_type[recv]
        if rty_n != "ConstantReadNode" && rty_n != "ConstantPathNode"
          return "poly"
        end
      end
    end
    if mname == "to_i"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "int"
    end
    if mname == "to_f"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "float"
    end
 # Time#iso8601 and Time#strftime — both return a
 # formatted string. Gated on recv_type so unrelated user-class
 # methods sharing the names (rare in idiomatic Ruby; possible
 # if a class wraps date arithmetic) still flow through normal
 # resolution.
    if mname == "iso8601" || mname == "strftime"
      if recv >= 0 && infer_type(recv) == "time"
        return "string"
      end
    end
 # Time#utc — same instant with UTC presentation
 # flag set. Returns a Time so chained calls (`Time.now.utc.iso8601`)
 # type as the chained method's result.
    if mname == "utc"
      if recv >= 0 && infer_type(recv) == "time"
        return "time"
      end
    end
 # `obj.class` on a statically-typed instance returns
 # a sp_Class value. The codegen-side mirror in
 # compile_object_method_expr emits the matching compound literal
 # (`((sp_Class){<cls_idx>LL})`).
    if mname == "class"
      if recv >= 0
        rt = infer_type(recv)
        if is_obj_type(rt) == 1
          return "class"
        end
      end
    end
 # `<obj>.class.<cmeth>` chained dispatch. When the
 # recv is itself a `class` CallNode whose own recv is
 # statically-typed obj_<C>, the chain resolves to <C>.<cmeth>
 # and the return type is whatever the cmeth returns. Same
 # detection shape as the codegen lowering.
    if recv >= 0 && @nd_type[recv] == "CallNode" && @nd_name[recv] == "class"
      inner_recv = @nd_receiver[recv]
      if inner_recv >= 0
        inner_t = infer_type(inner_recv)
        if is_obj_type(inner_t) == 1
          inner_bt = base_type(inner_t)
          inner_cname = inner_bt[4, inner_bt.length - 4]
          inner_ci = find_class_idx(inner_cname)
          if inner_ci >= 0
            owner = cls_cmethod_owner(inner_ci, mname)
            if owner >= 0
              cmnames = @cls_cmeth_names[owner].split(";")
              cmreturns = @cls_cmeth_returns[owner].split(";")
              base_rt = ""
              cmidx = 0
              while cmidx < cmnames.length
                if cmnames[cmidx] == mname && cmidx < cmreturns.length
                  base_rt = cmreturns[cmidx]
                  cmidx = cmnames.length
                else
                  cmidx = cmidx + 1
                end
              end
              if base_rt != ""
 # when descendants of inner_ci override
 # mname and any override's return type diverges from
 # the base owner's, the codegen-side chained dispatch
 # boxes each arm to sp_RbVal so the unified result
 # temp has a single C type. Widen the inferred type
 # to "poly" so consumers see the boxed value through
 # the poly-dispatch machinery.
                diverged = 0
                ck = 0
                while ck < @cls_names.length
                  if ck != inner_ci && cls_is_descendant(ck, inner_ci) == 1
                    own = cls_cmethod_owner(ck, mname)
                    if own == ck
                      ck_cmnames = @cls_cmeth_names[ck].split(";")
                      ck_cmreturns = @cls_cmeth_returns[ck].split(";")
                      ki = 0
                      while ki < ck_cmnames.length
                        if ck_cmnames[ki] == mname && ki < ck_cmreturns.length
                          if ck_cmreturns[ki] != base_rt
                            diverged = 1
                          end
                          ki = ck_cmnames.length
                        else
                          ki = ki + 1
                        end
                      end
                    end
                  end
                  ck = ck + 1
                end
                if diverged == 1
                  return "poly"
                end
                return base_rt
              end
            end
          end
        end
      end
    end
 # Kernel coercion methods: Integer(x) / Float(x) return their class.
 # Only treat as a Kernel call when there's no explicit receiver — with
 # a receiver, "Integer" / "Float" would be ConstantReadNode lookups,
 # not method calls, and wouldn't reach this name dispatch anyway.
    if recv < 0 && mname == "Integer"
      return "int"
    end
    if recv < 0 && mname == "Float"
      return "float"
    end
 # Float#ceil(n)/floor(n)/round(n)/truncate(n) with n given return
 # Float; zero-arg / Integer#ceil etc. return Integer. (truncate's arm
 # used to live next to nan?/infinite? — folded in here for one place
 # to update.)
 #
 # Gate on a Float receiver only — `rt == "float"` exclusively,
 # not "int". A user-defined module method like
 # `ViewHelpers.truncate(s, length: 100)` reaches infer_type
 # with the module's ConstantReadNode receiver, which falls
 # back to "int", so a permissive gate would match purely by
 # name and emit `sp_box_float(...)` against a `const char *`
 # return value. Integer ceil/floor/round/truncate-with-arg are
 # rare in practice; if the codebase needs them later, a
 # future call-site-type-narrowing pass can claim them.
    if mname == "ceil" || mname == "floor" || mname == "round" || mname == "truncate"
      if recv >= 0 && infer_type(recv) == "float"
        if @nd_arguments[nid] >= 0
          return "float"
        end
        return "int"
      end
    end
    if mname == "upcase" || mname == "downcase"
      if recv >= 0 && infer_type(recv) == "symbol"
        return "symbol"
      end
      return "string"
    end
    if mname == "swapcase"
      return "string"
    end
    if mname == "delete_prefix" || mname == "delete_suffix"
      return "string"
    end
    if mname == "eql?"
      return "bool"
    end
    if mname == "partition" || mname == "rpartition"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "string"
          return "tuple:string,string,string"
        end
      end
    end
    if mname == "hash"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "int"
    end
    if mname == "strip"
      return "string"
    end
    if mname == "chomp" || mname == "chop"
      return "string"
    end
 # `force_encoding` / `encode` / `b` lower to a receiver
 # passthrough at codegen (line 13953); mirror the type
 # inference so methods like `def normalize(s); s.force_encoding("UTF-8"); end`
 # pick up the receiver's string type for the function return
 # signature. Gated on receiver type being string to avoid
 # colliding with user-defined methods of the same name (`b` is
 # the shortest collision target; struct field accessors etc.
 # commonly take that single-letter shape).
    if mname == "force_encoding" || mname == "encode" || mname == "b"
      if recv >= 0
        rt_fe = infer_type(recv)
        if rt_fe == "string"
          return "string"
        end
 # A mutable_str (sp_String *) recv goes through codegen's
 # mutable-str arm which dispatches the inner call against
 # `rc + "->data"`; force_encoding's body return is then the
 # mutable's `->data` (const char *), not the wrapping struct
 # pointer. The function-return signature should match — pick
 # "string", not "mutable_str", so the C signature reads
 # `const char *` and the body's `return lv_out->data` fits.
        if rt_fe == "mutable_str"
          return "string"
        end
      end
    end
    if mname == "include?"
      return "bool"
    end
    if mname == "cover?"
      return "bool"
    end
    if mname == "==="
      return "bool"
    end
    if mname == "match?"
      return "bool"
    end
    if mname == "start_with?"
      return "bool"
    end
    if mname == "end_with?"
      return "bool"
    end
    if mname == "even?"
      return "bool"
    end
    if mname == "odd?"
      return "bool"
    end
    if mname == "allbits?"
      return "bool"
    end
    if mname == "anybits?"
      return "bool"
    end
    if mname == "nobits?"
      return "bool"
    end
    if mname == "zero?"
      return "bool"
    end
    if mname == "frozen?"
      return "bool"
    end
    if mname == "is_a?" || mname == "kind_of?" || mname == "instance_of?"
      return "bool"
    end
    if mname == "respond_to?" || mname == "method_defined?"
      return "bool"
    end
    if mname == "chr"
      return "string"
    end
    if mname == "gcd" || mname == "lcm" || mname == "ceildiv" || mname == "div"
      return "int"
    end
    if mname == "clamp"
      return "int"
    end
    if mname == "itself" || mname == "tap"
      if recv >= 0
        return infer_type(recv)
      end
      return "int"
    end
 # String#each_byte returns the receiver per CRuby. The block-bearing
 # form is handled in compile_string_method_expr; the inference rule
 # here is what makes `ret = "hi".each_byte { ... }` typed as string.
    if mname == "each_byte"
      if recv >= 0 && @nd_block[nid] >= 0
        rt = infer_type(recv)
        if rt == "string" || rt == "mutable_str"
          return rt
        end
      end
    end
    if mname == "then" || mname == "yield_self"
 # Return type is the block's return type. Bind the block param to
 # the receiver's type so infer_type sees the inner shadow, not any
 # outer same-named local of a different type.
      if recv >= 0
        blk = @nd_block[nid]
        if blk >= 0
          bbody = @nd_body[blk]
          if bbody >= 0
            bbs = get_stmts(bbody)
            if bbs.length > 0
              bp = get_block_param(nid, 0)
              if bp == ""
                bp = "_x"
              end
              recv_t = infer_type(recv)
              push_scope
              declare_var(bp, recv_t)
              rt = infer_type(bbs.last)
              pop_scope
              return rt
            end
          end
        end
        return infer_type(recv)
      end
      return "int"
    end
    if mname == "succ" || mname == "next"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "string"
          return "string"
        end
      end
      return "int"
    end
    if mname == "pred"
      return "int"
    end
    if mname == "getbyte"
      return "int"
    end
    if mname == "bytesize"
      return "int"
    end
    if mname == "setbyte"
      return "int"
    end
    if mname == "__method__"
      return "string"
    end
    if mname == "join"
      return "string"
    end
    if mname == "uniq"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "slice!"
 # Mirrors Array#slice (with !) — returns an array of the same
 # element type as the receiver.
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "to_sym" || mname == "intern"
      return "symbol"
    end
    if mname == "lstrip"
      return "string"
    end
    if mname == "rstrip"
      return "string"
    end
    if mname == "dup"
      if recv >= 0
 # `nullable_poly_hash[k].dup` shape: the recv emits sp_RbVal
 # at runtime (codegen-side expr_emits_poly_rb_val handles the
 # unbox at call sites), but analyze's cached infer for the
 # `[]` access falls back to int because the recv carries the
 # `?` suffix. Recognize the shape here so dup surfaces as
 # poly — without this, the LV write site pins the slot at int
 # and downstream `iv_params = lv_merged` (StrIntHash * slot)
 # fails -Wint-conversion.
        if @nd_type[recv] == "CallNode" && @nd_name[recv] == "[]"
          inner_recv = @nd_receiver[recv]
          if inner_recv >= 0
            inner_rt = infer_type(inner_recv)
            inner_bt = base_type(inner_rt)
            if inner_bt == "sym_poly_hash" || inner_bt == "str_poly_hash" || inner_bt == "poly_poly_hash"
              @needs_rb_value = 1
              return "poly"
            end
          end
        end
        return infer_type(recv)
      end
      return "string"
    end
    if mname == "ord"
      return "int"
    end
    if mname == "format"
      return "string"
    end
    if mname == "sprintf"
      return "string"
    end
    if mname == "positive?"
      return "bool"
    end
    if mname == "negative?"
      return "bool"
    end
    if mname == "empty?"
      return "bool"
    end
    if mname == "any?" || mname == "all?" || mname == "none?" || mname == "one?"
      return "bool"
    end
    if mname == "between?"
      return "bool"
    end
    if mname == "nil?"
      return "bool"
    end
    if mname == "abs"
      if recv >= 0
        lt = infer_type(recv)
        if lt == "float"
          return "float"
        end
      end
      return "int"
    end
    if mname == "**" || mname == "pow"
      if recv >= 0
        lt = infer_type(recv)
        if lt == "float"
          return "float"
        end
      end
      return "int"
    end
    if mname == "fetch"
      if recv >= 0
        rt = infer_type(recv)
 # int-leaf hash with string default — Ruby's `params.fetch
 # "k", ""` idiom. The leaf int is convertible to string via
 # sp_int_to_s, so surface the result as string; codegen-side
 # emits the conversion in the get arm. Limited to int-leaf
 # variants and string-typed defaults; broader (poly leaf,
 # hash default, etc.) widening cascades through downstream
 # `is_a?(Hash)` narrowing in real-blog params and is deferred.
        if rt == "str_int_hash" || rt == "sym_int_hash"
          fargs_id_f = @nd_arguments[nid]
          if fargs_id_f >= 0
            fargs_f = get_args(fargs_id_f)
            if fargs_f.length >= 2
              def_at_f = infer_type(fargs_f[1])
              if def_at_f == "string" || def_at_f == "mutable_str"
                return "string"
              end
 # Hash-typed default (`fetch "k", {}`). The two ternary arms
 # are int (from the get) and a hash pointer (from the default
 # literal) with no shared primitive type. Box both to
 # sp_RbVal and surface the result as poly. The receiving LV
 # slot widens to sp_RbVal via the refine pass's
 # concrete-to-poly merge rule (below in this file).
              if is_hash_type(def_at_f) == 1
                @needs_rb_value = 1
                return "poly"
              end
            end
          end
        end
        if rt == "str_str_hash" || rt == "sym_str_hash" || rt == "int_str_hash"
          return "string"
        end
 # Poly recv: runtime can be any user class (with `def fetch`)
 # OR any built-in Hash variant. The codegen-side dispatch
 # (`compile_poly_method_call` + `emit_poly_builtin_dispatch`)
 # boxes each per-cls_id arm to sp_RbVal because the value
 # types diverge across arms. Surface that here so the caller's
 # slot widens accordingly — without this, `lookup` keeps a
 # `const char *` return slot and the boxed dispatch result is
 # a C type mismatch. Sibling to the `[]` poly-recv widening
 # at line 4520 above.
        if rt == "poly"
          @needs_rb_value = 1
          return "poly"
        end
      end
 # Don't claim "int" for fetch on receivers we don't recognize
 # as a built-in collection — let later dispatch resolve a
 # user-defined `def fetch` against the receiver class.
      return ""
    end
    if mname == "dig"
      if recv < 0
        return ""
      end
      rt = infer_type(recv)
      if is_hash_type(rt) != 1
        return ""
      end
      args_id = @nd_arguments[nid]
      arg_count = 0
      if args_id >= 0
        arg_count = get_args(args_id).length
      end
      if arg_count == 1
        kt = infer_type(get_args(args_id)[0])
        if hash_key_matches_recv(rt, kt) == 1
          lt = hash_leaf_type(rt)
          if lt == "int" || lt == "string"
            return lt
          end
        end
      end
      @needs_rb_value = 1
      return "poly"
    end
    if mname == "has_key?" || mname == "key?" || mname == "member?"
      return "bool"
    end
    if mname == "split"
      return "str_array"
    end
    if mname == "lines"
      return "str_array"
    end
    if mname == "scan"
      return "str_array"
    end
    if mname == "gets" || mname == "readline"
      return "string"
    end
    if mname == "readlines"
      return "str_array"
    end
    if mname == "gsub"
      return "string"
    end
    if mname == "sub"
      return "string"
    end
    if mname == "capitalize"
      return "string"
    end
    if mname == "tr"
      return "string"
    end
    if mname == "delete"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "string"
          return "string"
        end
      end
    end
    if mname == "squeeze"
      return "string"
    end
    if mname == "slice"
      return "string"
    end
    if mname == "ljust"
      return "string"
    end
    if mname == "rjust"
      return "string"
    end
    if mname == "center"
      return "string"
    end
    if mname == "chars"
      return "str_array"
    end
    if mname == "bytes"
      return "int_array"
    end
    if mname == "hex"
      return "int"
    end
    if mname == "oct"
      return "int"
    end
    if mname == "count"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "int"
    end
    if mname == "size"
      if recv_could_be_obj(recv) == 1 && any_user_class_defines_imeth(mname) == 1
        return ""
      end
      return "int"
    end
    if mname == "index" || mname == "find_index" || mname == "rindex"
      return "int"
    end
    if mname == "delete_at"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "float_array"
          return "float"
        end
 # User-class ptr_array: `arr.delete_at(i)` returns the
 # popped instance pointer, typed to the array's element
 # class. Without this branch the assignment target
 # (`v = arr.delete_at(i)`) defaults to int and the C
 # compile fails on the implicit ptr-to-int cast.
        if is_ptr_array_type(rt) == 1
          return ptr_array_elem_type(rt)
        end
      end
      return "int"
    end
    if mname == "insert"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "filter_map"
      if recv >= 0
        blk = @nd_block[nid]
        if blk >= 0
          bbody = @nd_body[blk]
          if bbody >= 0
            bbs = get_stmts(bbody)
            if bbs.length > 0
              bret = infer_type(bbs.last)
              if bret == "string"
                return "str_array"
              end
              if bret == "float"
                return "float_array"
              end
            end
          end
        end
      end
      return "int_array"
    end
    if mname == "find" || mname == "detect"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "float_array"
          return "float"
        end
        if rt == "str_int_hash" || rt == "str_str_hash"
          return "string"
        end
      end
 # Same fall-through logic as fetch above: a user-defined
 # `def find` (the canonical ActiveRecord finder shape) wins
 # over the built-in collection dispatch when the receiver
 # isn't a recognized built-in collection.
      return ""
    end
    if mname == "keys"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "int_str_hash"
          return "int_array"
        end
        if rt == "sym_int_hash" || rt == "sym_str_hash" || rt == "sym_poly_hash"
          return "sym_array"
        end
      end
      return "str_array"
    end
    if mname == "sample"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "sym_array"
          return "symbol"
        end
        if rt == "float_array"
          return "float"
        end
        if rt == "int_array"
          return "int"
        end
 # Not an array — recv has a user-defined `sample`. Defer to
 # the user-class dispatch path instead of returning the
 # Array#sample default of int.
        return ""
      end
      return "int"
    end
    if mname == "digits"
      return "int_array"
    end
    if mname == "bit_length"
      return "int"
    end
    if mname == "divmod"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "float"
          return "tuple:int,float"
        end
      end
      return "tuple:int,int"
    end
    if mname == "minmax"
      if recv >= 0
        rt = infer_type(recv)
        et = elem_type_of_array(rt)
        return "tuple:" + et + "," + et
      end
      return "tuple:int,int"
    end
    if mname == "partition"
      if recv >= 0
        rt = infer_type(recv)
        return "tuple:" + rt + "," + rt
      end
      return "tuple:int_array,int_array"
    end
    if mname == "to_a"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_int_hash"
          return "tuple:string,int_ptr_array"
        end
        if rt == "str_str_hash"
          return "tuple:string,string_ptr_array"
        end
      end
    end
 # `to_h` on a Hash variant is identity. Surface so the caller
 # gets the recv's exact type rather than an unresolved fallback.
    if mname == "to_h"
      if recv >= 0
        rt = infer_type(recv)
        if is_hash_type(rt) == 1
          return rt
        end
      end
    end
    if mname == "fdiv"
      return "float"
    end
    if mname == "nan?" || mname == "finite?"
      return "bool"
    end
    if mname == "infinite?"
      return "int"
    end
    if mname == "tally"
      if recv >= 0 && infer_type(recv) == "sym_array"
        return "sym_int_hash"
      end
      return "str_int_hash"
    end
    if mname == "values"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_str_hash" || rt == "int_str_hash" || rt == "sym_str_hash"
          return "str_array"
        end
        if rt == "sym_poly_hash" || rt == "str_poly_hash"
          return "poly_array"
        end
      end
      return "int_array"
    end
    if mname == "invert"
      return "str_str_hash"
    end
    if mname == "push"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
 # `replace(other)` returns the receiver, not a fresh array;
 # the inferred result must therefore preserve the receiver's
 # array type so that an expression-form `c = a.replace(b)`
 # still tags `c` as `int_array` (or whatever `a` is) rather
 # than falling through to `int`.
    if mname == "replace"
      if recv >= 0
        return infer_type(recv)
      end
    end
 # `clear` mutates in place and returns the now-empty receiver.
 # Same shape as `replace`: preserve the receiver's array/string
 # type so chained or `||=`-style usage doesn't fall back to int.
    if mname == "clear"
      if recv >= 0
        rt_clr = infer_type(recv)
        if rt_clr == "int_array" || rt_clr == "sym_array" ||
           rt_clr == "str_array" || rt_clr == "float_array" ||
           rt_clr == "poly_array" || is_ptr_array_type(rt_clr) == 1 ||
           rt_clr == "string" || rt_clr == "mutable_str"
          return rt_clr
        end
      end
    end
    if mname == "pop"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "sym_array"
          return "symbol"
        end
      end
      return "int"
    end
    if mname == "shift"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "sym_array"
          return "symbol"
        end
        if rt == "float_array"
          return "float"
        end
      end
      return "int"
    end
    if mname == "take" || mname == "drop" || mname == "rotate" || mname == "fill"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "sort"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "first" || mname == "last"
      if recv >= 0
        rt = infer_type(recv)
 # With arg → returns array of same type
        if @nd_arguments[nid] >= 0
          aargs = get_args(@nd_arguments[nid])
          if aargs.length > 0
            return rt
          end
        end
        if rt == "str_array"
          return "string"
        end
        if rt == "sym_array"
          return "symbol"
        end
        if rt == "float_array"
          return "float"
        end
 # `<X>_ptr_array.first` / `.last` returns an `<X>` (e.g.
 # `int_array_ptr_array.first` → `int_array`). Without this,
 # downstream typed-array consumers (notably the slice-assign
 # `arr[i, n] = banks.first` path in compile_bracket_assign,
 # which needs `infer_type(arg_ids[2]) == "int_array"` to fire)
 # see "int" and silently fall through to element-assign,
 # silently lowering `arr[i, n] = src` to `arr[i] = n`.
        if is_ptr_array_type(rt) == 1
          return ptr_array_elem_type(rt)
        end
        if rt == "poly_array"
          return "poly"
        end
      end
      return "int"
    end
    if mname == "min" || mname == "max"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
        if rt == "float_array"
          return "float"
        end
      end
      return "int"
    end
    if mname == "sum"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "float_array"
          return "float"
        end
      end
      return "int"
    end
    if mname == "reverse"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "shuffle" || mname == "shuffle!"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "compact"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "flatten"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "transpose"
 # Transposing a matrix preserves its shape type — `[[Int]]` stays
 # `[[Int]]` . Only ptr_array-of-T_array receivers are
 # currently supported by codegen; other shapes fall through to
 # the unresolved-call warning at emit time.
      if recv >= 0
        rt = infer_type(recv)
        if is_ptr_array_type(rt) == 1
          return rt
        end
      end
    end
    if mname == "flat_map"
      if recv >= 0
 # Block returns an array; result type matches block return type
        blk = @nd_block[nid]
        if blk >= 0
          bbody = @nd_body[blk]
          if bbody >= 0
            bbs = get_stmts(bbody)
            if bbs.length > 0
              bret = infer_type(bbs.last)
 # If block returns an array type, use it as result type
              if is_array_type(bret) == 1
                return bret
              end
            end
          end
        end
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "sort_by"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "min_by"
      return "int"
    end
    if mname == "max_by"
      return "int"
    end
    if mname == "unshift"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "merge"
      if recv >= 0
        return infer_type(recv)
      end
      return "str_int_hash"
    end
    if mname == "transform_values"
      if recv >= 0
        return infer_type(recv)
      end
      return "str_int_hash"
    end
    if mname == "transform_keys"
      if recv >= 0
        return infer_type(recv)
      end
      return "str_int_hash"
    end
    if mname == "zip"
      if recv >= 0
        rt = infer_type(recv)
 # Check if all zip arguments have the same element type
        heterogeneous = 0
        multi_arg = 0
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 1
            multi_arg = 1
          end
          k = 0
          while k < aargs.length
            at = infer_type(aargs[k])
            if at != rt
              heterogeneous = 1
            end
            k = k + 1
          end
        end
        if heterogeneous == 1 || multi_arg == 1
 # Build tuple type: receiver elem + each arg elem
          parts = "".split(",")
          parts.push(elem_type_of_array(rt))
          aargs2 = get_args(args_id)
          k2 = 0
          while k2 < aargs2.length
            parts.push(elem_type_of_array(infer_type(aargs2[k2])))
            k2 = k2 + 1
          end
          tt = "tuple:" + parts.join(",")
          register_tuple_type(tt)
          return tt + "_ptr_array"
        end
        if rt == "str_array"
          return "str_array_ptr_array"
        end
        if rt == "float_array"
          return "float_array_ptr_array"
        end
      end
      return "int_array_ptr_array"
    end
    if mname == "reject"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "map"
      if recv >= 0
 # Declare bp inside a scope so infer_type sees the inner element type, not a shadowed outer local.
        blk = @nd_block[nid]
        if blk >= 0
          bbody = @nd_body[blk]
          if bbody >= 0
            bbs = get_stmts(bbody)
            if bbs.length > 0
              recv_t = infer_type(recv)
              bp1 = get_block_param(nid, 0)
              push_scope
              if bp1 != ""
                declare_var(bp1, iter_elem_type(recv_t))
              end
              bret = infer_type(bbs.last)
              pop_scope
              if bret == "string"
                return "str_array"
              end
              if bret == "float"
                return "float_array"
              end
              if bret == "int"
                return "int_array"
              end
              if is_obj_type(bret) == 1
                return bret + "_ptr_array"
              end
 # Block returns a 1D array (e.g.
 # `[1, 6].map { (0..n).map { i } }` or
 # `(0..3).map { |i| (0..3).map { ... } }`) — each
 # outer element is itself a 1D typed array. Encode
 # as `<inner>_ptr_array` for the standard
 # `arr[i][j]` dispatch shape.
              if bret == "int_array" || bret == "float_array" || bret == "str_array" || bret == "sym_array"
                return bret + "_ptr_array"
              end
 # Block returns a deeper-nested array
 # (`int_array_ptr_array`, `poly_array`). Encode as
 # `poly_array` — cls_id tagging chain on the inner
 # PolyArray pushes preserves elem type for
 # `arr[i][j][k]` 3D dispatch.
              if is_ptr_array_type(bret) == 1 || bret == "poly_array"
                return "poly_array"
              end
 # Block returns a generic `poly` (e.g. `entries[key]`
 # where `entries` is `str_poly_hash`) — the resulting
 # array is heterogeneous, so encode as poly_array. The
 # @needs_rb_value bookkeeping stays in scan-pass
 # widening; this branch only types the .map result.
              if bret == "poly"
                @needs_rb_value = 1
                return "poly_array"
              end
 # poly_array bret intentionally falls through. Returning
 # poly_array_ptr_array would be more accurate, but ivars
 # holding the result (and the corresponding `[nil] *
 # n` companions) often haven't been widened to the
 # ptr_array shape by the type-inference pass yet.
 # Letting it fall through to int_array preserves the
 # pre-fix typing that those companion ivars match.
 # Block returns a non-trivial type (poly value, etc.).
 # The map's overall result is still an Array — fall
 # through to the recv-based default below only when
 # recv is already array-shaped, otherwise return
 # int_array as a generic placeholder.
            end
          end
        end
        rt_recv = infer_type(recv)
 # Range#map / Integer#step.map / non-array recv → result is
 # an Array, not a Range/IntArray. Without this, an
 # `@x = (0...n).map {...}` recorded the ivar as `range` and
 # later `@x = something_else` writes failed to type-check.
        if rt_recv == "range" || rt_recv == "int"
          return "int_array"
        end
        return rt_recv
      end
      return "int_array"
    end
    if mname == "select" || mname == "filter"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "reject"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "reduce" || mname == "inject" || mname == "each_with_object"
 # Return type is the accumulator type, inferred from initial value
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 0
          return infer_type(aargs[0])
        end
      end
      return "int"
    end
    if mname == "[]"
      if recv >= 0
 # ENV["X"] returns `const char *` (sp_str_dup_external of
 # getenv). The plain receiver-type dispatch below would
 # leave ENV at the default "int" (unknown constant) and
 # miss every branch, so claim string here directly.
 # Mirrors the codegen site's ENV check.
        if @nd_type[recv] == "ConstantReadNode" && @nd_name[recv] == "ENV"
          return "string"
        end
        rt = infer_type(recv)
        if rt == "string"
          return "string"
        end
        if rt == "mutable_str"
          return "string"
        end
        if rt == "poly"
 # Approach 2: narrow `<poly>[k]` to int when the receiver
 # came from a poly_array whose observed element kinds all
 # imply int-returning `[]`. Mirrors the codegen-side
 # narrowing in `compile_poly_method_call`.
          if poly_index_narrow_int(nid) == 1
            return "int"
          end
          return "poly"
        end
        if rt == "int_array"
 # a[range] / a[start, len] returns a slice (still int_array);
 # bare a[i] returns the element.
          args_id = @nd_arguments[nid]
          if args_id >= 0
            a = get_args(args_id)
            if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
              return "int_array"
            end
            if a.length >= 2
              return "int_array"
            end
          end
          return "int"
        end
        if rt == "sym_array"
          return "symbol"
        end
        if rt == "float_array"
 # a[range] / a[start, len] returns a slice (still float_array).
          args_id = @nd_arguments[nid]
          if args_id >= 0
            a = get_args(args_id)
            if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
              return "float_array"
            end
            if a.length >= 2
              return "float_array"
            end
          end
          return "float"
        end
        if rt == "str_array"
 # a[range] / a[start, len] returns a slice (still str_array).
          args_id = @nd_arguments[nid]
          if args_id >= 0
            a = get_args(args_id)
            if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
              return "str_array"
            end
            if a.length >= 2
              return "str_array"
            end
          end
          return "string"
        end
        if rt == "poly_array"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            a = get_args(args_id)
            if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
              return "poly_array"
            end
            if a.length >= 2
              return "poly_array"
            end
          end
          return "poly"
        end
        if is_ptr_array_type(rt) == 1
          return ptr_array_elem_type(rt)
        end
        if is_tuple_type(rt) == 1
 # Infer element type from constant index
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length > 0
              if @nd_type[aargs[0]] == "IntegerNode"
                return tuple_elem_type_at(rt, @nd_value[aargs[0]])
              end
            end
          end
          return tuple_elem_type_at(rt, 0)
        end
        if rt == "str_int_hash"
          return "int"
        end
        if rt == "str_str_hash"
          return "string"
        end
        if rt == "int_str_hash"
          return "string"
        end
        if rt == "sym_int_hash"
          return "int"
        end
        if rt == "sym_str_hash"
          return "string"
        end
        if rt == "sym_poly_hash"
          return "poly"
        end
        if rt == "str_poly_hash"
          return "poly"
        end
        if rt == "poly_poly_hash"
          return "poly"
        end
        if rt == "argv"
          return "string"
        end
        if rt == "lambda"
          return "lambda"
        end
 # User-defined `def [](k)` on an obj recv. Walk the class's
 # method table the same way infer_recv_method_type does for
 # arbitrary mname; otherwise the fallback at the tail of
 # this branch returns "int" and downstream `.to_i` /
 # `.length` etc. dispatch on the wrong recv type.
        if is_obj_type(rt) == 1
          bt = base_type(rt)
          cname = bt[4, bt.length - 4]
          ci = find_class_idx(cname)
          if ci >= 0
            mr = cls_method_return(ci, "[]")
            if mr != "" && mr != "int"
              return mr
            end
          end
        end
      end
      return "int"
    end
    if mname == "intersection" || mname == "union" || mname == "difference"
      if recv >= 0
        rt = infer_type(recv)
        return rt if is_typed_array_type(rt)
      end
      return ""
    end
    if mname == "take_while" || mname == "drop_while"
      if recv >= 0
        rt = infer_type(recv)
        return rt if rt == "int_array" || rt == "sym_array"
      end
      return ""
    end
    ""
  end

  def infer_constructor_type(nid, mname, recv)
    if mname == "new"
 # Implicit recv-less `new` in a class method body resolves
 # to `obj_<CurrentClass>` so subsequent attr_writer calls
 # and ivar widening see the right type.
      if recv < 0
        implicit = current_class_method_owning_class
        if implicit != ""
          return "obj_" + implicit
        end
      end
      if recv >= 0
        rn = constructor_class_name(recv)
        if rn != ""
          if rn == "Array"
 # Block form `Array.new(n) { ... }` — infer the container
 # from the block's tail expression, same shape as the
 # compile_constructor_expr emit. Without this branch
 # ivar widening saw `int_array` and the actual emit's
 # PtrArray / PolyArray clashed.
            blk_an = @nd_block[nid]
            if blk_an >= 0
              body_an = @nd_body[blk_an]
              if body_an >= 0
                stmts_an = get_stmts(body_an)
                if stmts_an.length > 0
 # `[]` / `[].dup` block tail: the inner element type
 # is statically ambiguous. Use poly_array (a
 # PolyArray-of-PolyArray) for the result so later
 # pushes of pointer-typed values (3-tuples,
 # IntArrays, etc.) survive — the sp_poly_shl runtime
 # dispatch handles any push kind via cls_id.
                  if is_empty_array_or_dup(stmts_an.last) == 1
                    @needs_rb_value = 1
                    @needs_gc = 1
                    return "poly_array"
                  end
                  bret = infer_type(stmts_an.last)
                  if bret == "string"
                    return "str_array"
                  end
                  if bret == "float"
                    return "float_array"
                  end
                  if bret == "symbol"
                    return "sym_array"
                  end
                  if bret == "poly"
                    @needs_rb_value = 1
                    return "poly_array"
                  end
                  if is_ptr_array_type(bret) == 1 || bret == "poly_array"
                    @needs_rb_value = 1
                    return "poly_array"
                  end
                  if bret == "int_array" || bret == "float_array" || bret == "str_array" || bret == "sym_array"
                    @needs_gc = 1
                    return bret + "_ptr_array"
                  end
                end
              end
            end
 # Check fill value type. Pointer-type fills must produce a typed
 # PtrArray; falling through to int_array would leave the
 # elements unscanned by GC.
            args_id = @nd_arguments[nid]
            if args_id >= 0
              aargs = get_args(args_id)
              if aargs.length >= 2
                vt = infer_type(aargs[1])
                if vt == "float"
                  return "float_array"
                end
                if vt == "string"
                  return "str_array"
                end
                if vt == "symbol"
                  return "sym_array"
                end
                if vt == "poly"
                  @needs_rb_value = 1
                  return "poly_array"
                end
                if type_is_pointer(vt) == 1
                  @needs_gc = 1
                  return vt + "_ptr_array"
                end
              end
            end
            return "int_array"
          end
          if rn == "Hash"
            return "str_int_hash"
          end
          if rn == "String"
 # `String.new` / `String.new("...")` returns a fresh
 # mutable string buffer (sp_String *), the same type
 # that `s = ""; s << ...` widens a string local into.
            return "mutable_str"
          end
          if rn == "Proc"
            return "proc"
          end
          if rn == "StringIO"
            return "stringio"
          end
          if rn == "Fiber"
            return "fiber"
          end
          return "obj_" + rn
        end
      end
    end
    ""
  end

  def infer_constant_recv_type(nid, mname, recv)
 # File operations
    if recv >= 0
      if @nd_type[recv] == "ConstantReadNode"
        rcname = @nd_name[recv]
        if rcname == "Process"
          if mname == "clock_gettime"
            return "float"
          end
        end
        if rcname == "Time"
          if mname == "now"
            return "time"
          end
          if mname == "at"
            return "time"
          end
        end
        if rcname == "Complex"
          if mname == "polar"
            return "complex"
          end
        end
        if rcname == "File"
          if mname == "read" || mname == "binread"
            return "string"
          end
          if mname == "exist?" || mname == "readable?"
            return "bool"
          end
          if mname == "join"
            return "string"
          end
          if mname == "basename"
            return "string"
          end
        end
        if rcname == "ENV"
          if mname == "[]"
            return "string"
          end
          if mname == "fetch"
            return "string"
          end
        end
        if rcname == "Dir"
          if mname == "home"
            return "string"
          end
        end
        if rcname == "Integer"
          if mname == "sqrt"
            return "int"
          end
        end
      end
    end
 # User-defined class methods
    if recv >= 0
      rcname = constructor_class_name(recv)
      if rcname != ""
        if rcname == "Fiber"
          if mname == "new"
            return "fiber"
          end
        end
        ci2 = find_class_idx(rcname)
        if ci2 >= 0
          if mname == "new"
            return "obj_" + rcname
          end
 # `Klass.method(:cls_meth)` — bind to a class method.
 # Returns a Method object exactly like the instance-recv
 # form. The compile path below emits an adapter trampoline
 # so the Method's `(void *, mrb_int...)` ABI absorbs the
 # missing self.
          if mname == "method"
            args_idm = @nd_arguments[nid]
            if args_idm >= 0
              ams = get_args(args_idm)
              if ams.length >= 1
                cm_ref = @nd_content[ams[0]]
                if cm_ref == ""
                  cm_ref = @nd_name[ams[0]]
                end
                if cls_cmethod_owner(ci2, cm_ref) >= 0
                  return "obj_Method"
                end
              end
            end
          end
 # Walk the parent chain so an inherited
 # `def self.<mname>` on a base class resolves correctly
 # when called on the subclass (e.g. `Leaf.all` →
 # `Base.all`'s return type).
          inherited_rt = cls_cmethod_return_inherited(ci2, mname)
          if inherited_rt != "" && inherited_rt != "int"
            return inherited_rt
          end
        end
 # Same lookup for module class methods. They live in the
 # top-level @meth_* table as `<Mod>_cls_<method>`, not in
 # @cls_cmeth_* (which is class-only) — so `Module.cls_method`
 # call sites need this branch to find the method's return
 # type and assign call-site locals correctly.
        if module_name_exists(rcname) == 1
          mfi = find_method_idx(rcname + "_cls_" + mname)
          if mfi >= 0 && mfi < @meth_return_types.length
            mrt = @meth_return_types[mfi]
            if mrt != "" && mrt != "int"
              return mrt
            end
          end
        end
      end
    end
 # StringIO methods
    if recv >= 0
      rt = infer_type(recv)
      if rt == "stringio"
        if mname == "string" || mname == "read" || mname == "gets" || mname == "getc"
          return "string"
        end
        if mname == "pos" || mname == "tell" || mname == "size" || mname == "length" || mname == "write" || mname == "putc" || mname == "getbyte" || mname == "lineno"
          return "int"
        end
        if mname == "eof?" || mname == "closed?" || mname == "sync" || mname == "isatty"
          return "bool"
        end
        if mname == "flush"
          return "stringio"
        end
      end
    end
    ""
  end

  def infer_math_and_misc_type(nid, mname, recv)
 # backtick
    if mname == "`"
      return "string"
    end
    if mname == "sqrt"
      return "float"
    end
    if mname == "cos"
      return "float"
    end
    if mname == "sin"
      return "float"
    end
    if mname == "tan"
      return "float"
    end
    if mname == "acos" || mname == "asin" || mname == "atan"
      return "float"
    end
 # Hyperbolic + inverse hyperbolic — same C99 libm wrappers as the
 # circular ones above. Issue: returning "float" here lets call sites
 # that lift `Math.tanh(x)` into a non-Float context (e.g. into an
 # IntArray slot via `arr[i] = Math.tanh(x)`) get caught at type-
 # check time instead of silently emitting `0`.
    if mname == "sinh" || mname == "cosh" || mname == "tanh"
      return "float"
    end
    if mname == "asinh" || mname == "acosh" || mname == "atanh"
      return "float"
    end
    if mname == "log"
      return "float"
    end
    if mname == "log2"
      return "float"
    end
    if mname == "log10"
      return "float"
    end
    if mname == "exp"
      return "float"
    end
    if mname == "atan2"
      return "float"
    end
    if mname == "hypot"
      return "float"
    end
    if mname == "freeze"
      if recv >= 0
        return infer_type(recv)
      end
      return "string"
    end
    if mname == "to_a"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "range"
          return "int_array"
        end
        if rt == "int_array"
          return "int_array"
        end
        if rt == "poly_array"
          return "poly_array"
        end
      end
      return "int_array"
    end
    ""
  end

  def infer_recv_method_type(nid, mname, recv)
 # Method call on poly
    if recv >= 0
      rt = infer_type(recv)
 # Complex value-type methods.
      if rt == "complex"
        if mname == "real" || mname == "imaginary" || mname == "imag"
          return "float"
        end
        if mname == "conjugate" || mname == "conj"
          return "complex"
        end
      end
      if rt == "poly"
        if mname == "nil?"
          return "bool"
        end
        if mname == "[]"
 # Narrow `<poly>[k]` to int when the receiver came from a
 # poly_array whose observed slot-type history all imply
 # int-returning element kinds (IntArray, Method). Keep this
 # in sync with `compile_poly_method_call`'s codegen-side
 # narrowing — divergence widens the consuming slot to poly
 # while emit produces int.
          if poly_index_narrow_int(nid) == 1
            return "int"
          end
          return "poly"
        end
 # Scan every user class that defines this method. If they all
 # agree on the return type, the call has that concrete type.
 # If they disagree, the call is genuinely polymorphic.
        return poly_dispatch_return_type(mname)
      end
 # Method call on int (possible IntArray element storing object pointers)
 # when recv is a LocalVariableReadNode whose
 # var-type table entry hasn't been pinned yet (find_var_type
 # returns ""), `rt` defaults to "int" via infer_type's
 # LocalVariableReadNode fallback. The cross-class widening
 # below would then pick the FIRST user class with a non-int
 # `<mname>` return, silently widening `r = c.get(...)` to the
 # wrong type when `c` is statically obj_<Other> but its type
 # hasn't propagated through the iterative loop yet. Bail out
 # of the int-recv path in that case ONLY when the candidates
 # disagree -- if every class defining mname returns the same
 # type, the widening's result is correct regardless of which
 # one the recv actually points at. A single matching class
 # (the multi_return_bare shape, where every callsite's recv
 # is the same class) keeps the widening intact too.
      recv_is_unresolved_local = 0
      if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode" && find_var_type(@nd_name[recv]) == ""
        unique_rt = ""
        diverged = 0
        ci = 0
        while ci < @cls_names.length
          if cls_find_method_direct(ci, mname) >= 0
            mr = cls_method_return(ci, mname)
            if mr != "int" && mr != ""
              if unique_rt == ""
                unique_rt = mr
              elsif unique_rt != mr
                diverged = 1
                ci = @cls_names.length
              end
            end
          end
          ci = ci + 1
        end
        if diverged == 1
          recv_is_unresolved_local = 1
        end
      end
      if rt == "int" && recv_is_unresolved_local == 0
        ci = 0
        while ci < @cls_names.length
 # Check zero-arg methods (getters)
          ci2_mnames = @cls_meth_names[ci].split(";")
          ci2_mparams = @cls_meth_params[ci].split("|")
          mi2 = 0
          while mi2 < ci2_mnames.length
            if ci2_mnames[mi2] == mname
              mp2 = ""
              if mi2 < ci2_mparams.length
                mp2 = ci2_mparams[mi2]
              end
              if mp2 == ""
 # Found zero-arg method match
                mr = cls_method_return(ci, mname)
                if mr != "int"
                  return mr
                end
              end
            end
            mi2 = mi2 + 1
          end
 # Check attr_readers
          readers2 = @cls_attr_readers[ci].split(";")
          j2 = 0
          while j2 < readers2.length
            if readers2[j2] == mname
              ivt = cls_ivar_type(ci, "@" + mname)
              if ivt != "int"
                return ivt
              end
            end
            j2 = j2 + 1
          end
 # Check methods with args
          midx = cls_find_method_direct(ci, mname)
          if midx >= 0
            mr = cls_method_return(ci, mname)
            if mr != "int"
              return mr
            end
          end
          ci = ci + 1
        end
      end
      if is_obj_type(rt) == 1
        bt_rt = base_type(rt)
        cname = bt_rt[4, bt_rt.length - 4]
        ci = find_class_idx(cname)
        if ci >= 0
 # Check attr_reader
          readers = @cls_attr_readers[ci].split(";")
          j = 0
          while j < readers.length
            if readers[j] == mname
              return cls_ivar_type(ci, "@" + mname)
            end
            j = j + 1
          end
 # Check method
          mr = cls_method_return(ci, mname)
          if mr != "int"
            return mr
          end
 # If method exists, return its return type
          mi = cls_find_method(ci, mname)
          if mi >= 0
            return cls_method_return(ci, mname)
          end
        end
      end
    end
    ""
  end

  def infer_open_class_type(nid, mname, recv)
 # Check open class methods for receiver type
    if recv >= 0
      rt = infer_type(recv)
      oc_prefix = ""
      if rt == "int"
        oc_prefix = "__oc_Integer_"
      end
      if rt == "string"
        oc_prefix = "__oc_String_"
      end
      if rt == "float"
        oc_prefix = "__oc_Float_"
      end
      if oc_prefix != ""
        oc_name = oc_prefix + mname
        oc_mi = find_method_idx(oc_name)
        if oc_mi >= 0
          return @meth_return_types[oc_mi]
        end
      end
    end
    ""
  end




  def is_obj_type(t)
    if t == nil
      return 0
    end
    if t.length > 4
      if t[0] == "o"
        if t[1] == "b"
          if t[2] == "j"
            if t[3] == "_"
              return 1
            end
          end
        end
      end
    end
    0
  end

 # Check if type is a ptr_array (e.g., "obj_Planet_ptr_array")
  def is_ptr_array_type(t)
    if t != nil && t.length > 10
      if t.end_with?("_ptr_array")
        return 1
      end
    end
    0
  end

 # Get element class type from ptr_array type (e.g., "obj_Planet_ptr_array" → "obj_Planet")
  def elem_type_of_array(t)
    if t == "int_array"
      return "int"
    end
    if t == "str_array"
      return "string"
    end
    if t == "float_array"
      return "float"
    end
    if t == "sym_array"
      return "symbol"
    end
    if t == "poly_array"
      return "poly"
    end
    if is_ptr_array_type(t) == 1
      return ptr_array_elem_type(t)
    end
    "int"
  end

  def ptr_array_elem_type(t)
    if is_ptr_array_type(t) == 1
      return t[0, t.length - 10]
    end
    ""
  end

 # ---- Tuple type helpers ----
  def is_tuple_type(t)
    if t != nil && t.length > 6
      if t[0] == "t" && t[1] == "u" && t[2] == "p" && t[3] == "l" && t[4] == "e" && t[5] == ":"
 # Exclude ptr_array of tuples
        if is_ptr_array_type(t) == 1
          return 0
        end
        return 1
      end
    end
    0
  end

  def tuple_elem_types_str(t)
 # "tuple:int,string" → "int,string"
    t[6, t.length - 6]
  end

  def tuple_elem_type_at(t, idx)
    parts = tuple_elem_types_str(t).split(",")
    if idx < parts.length
      return parts[idx]
    end
    "int"
  end



 # Whether a tuple element type must be traced by the GC scan function.
 # Scalars (int/float/bool/symbol) are pure values; pointer-to-GC-object
 # element types must be marked, otherwise the GC frees the inner object
 # while the tuple keeps a dangling pointer.

 # Returns the scan function name for the tuple, or "NULL" if no field
 # requires marking.

  def register_tuple_type(t)
    if is_tuple_type(t) == 1
      k = 0
      found = 0
      while k < @tuple_types.length
        if @tuple_types[k] == t
          found = 1
        end
        k = k + 1
      end
      if found == 0
        @tuple_types.push(t)
      end
    end
  end

 # Build "tuple:T0,T1,..." from a list of element node ids and register it.
  def tuple_type_from_elems(elems)
    parts = "".split(",")
    k = 0
    while k < elems.length
      parts.push(infer_type(elems[k]))
      k = k + 1
    end
    tt = "tuple:" + parts.join(",")
    register_tuple_type(tt)
    tt
  end

 # Inferred C type of the i-th lvalue in `a, b, c = rhs`. Tuple RHS gives
 # per-position types; everything else falls back to "int" (matching the
 # legacy default — only the homogeneous int_array case is in wide use).
  def multi_write_target_type(val_id, ti)
    if val_id < 0
      return "int"
    end
    rt = infer_type(val_id)
    if is_tuple_type(rt) == 1
      return tuple_elem_type_at(rt, ti)
    end
 # Array literal RHS: each target gets the precise element type so a
 # heterogeneous literal like [1, "x", 2.0] doesn't force everything
 # through the poly boxer.
    if @nd_type[val_id] == "ArrayNode"
      elems = parse_id_list(@nd_elements[val_id])
      if ti < elems.length
        return infer_type(elems[ti])
      end
    end
    if rt == "str_array"
      return "string"
    end
    if rt == "float_array"
      return "float"
    end
    if rt == "sym_array"
      return "symbol"
    end
    if is_ptr_array_type(rt) == 1
      return ptr_array_elem_type(rt)
    end
    if rt == "poly_array"
      return "poly"
    end
    if rt == "poly"
 # RHS evaluates to sp_RbVal (e.g. `@h[k][i]` where @h is a
 # poly_poly_hash whose values are poly_arrays of inner poly
 # elements). Mirror compile_multi_write's `val_t_local == "poly"`
 # arm: unbox to sp_PolyArray * at runtime, fetch each slot via
 # sp_PolyArray_get returning sp_RbVal, so each target slot is
 # typed `poly` and stays boxed for downstream poly dispatch.
      return "poly"
    end
    "int"
  end

 # Type for the splat target in `a, *b = rhs`. Returns the rhs's array
 # type (so `b` is a typed-array of the same element type).
  def splat_rest_type(val_id)
    if val_id < 0
      return "int_array"
    end
    rt = infer_type(val_id)
    if rt == "int_array" || rt == "str_array" || rt == "float_array" || rt == "sym_array" || rt == "poly_array"
      return rt
    end
    if is_ptr_array_type(rt) == 1
      return rt
    end
    "int_array"
  end

  def is_splat_with_target(nid)
    if nid < 0
      return 0
    end
    if @nd_type[nid] != "SplatNode"
      return 0
    end
    if @nd_expression[nid] < 0
      return 0
    end
    1
  end

  def type_is_pointer(t)
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
 # Raw C pointer (FFI). Intentionally NOT a GC pointer — foreign
 # pointers are user-managed and the GC must not trace or free them.
    if t == "ptr"
      return 0
    end
    if t == "int_array"
      return 1
    end
    if t == "float_array"
      return 1
    end
    if is_ptr_array_type(t) == 1
      return 1
    end
    if t == "str_array"
      return 1
    end
    if t == "str_int_hash"
      return 1
    end
    if t == "str_str_hash"
      return 1
    end
    if t == "int_str_hash"
      return 1
    end
    if t == "sym_int_hash"
      return 1
    end
    if t == "sym_str_hash"
      return 1
    end
    if t == "str_poly_hash"
      return 1
    end
    if t == "sym_poly_hash"
      return 1
    end
    if t == "poly_poly_hash"
      return 1
    end
    if t == "sym_array"
      return 1
    end
    if t == "poly_array"
      return 1
    end
    if t == "lambda"
      return 1
    end
    if t == "mutable_str"
      return 1
    end
    if t == "string"
      return 1
    end
    if t == "fiber" || t == "bigint"
      return 1
    end
    if t == "proc"
      return 1
    end
    if is_obj_type(t) == 1
      cname = t[4, t.length - 4]
      ci = find_class_idx(cname)
      if ci >= 0 && @cls_is_value_type[ci] == 1
        return 0
      end
      return 1
    end
    if is_tuple_type(t) == 1
      return 1
    end
    0
  end

 # Check if evaluating an expression might trigger GC allocation

  def is_nullable_type(t)
    if t.length > 1 && t[t.length - 1] == "?"
      return 1
    end
    0
  end

 # Empty `[]` / `{}` literals need deferred element-type resolution
 # — the type can only be settled by later writes. This helper
 # distinguishes `[]` from `[1, 2, 3]` so the promotion machinery
 # can recognize "writes haven't fixed the element type yet, so a
 # later push can still pick it".
  def is_empty_hash_literal(nid)
    if nid < 0
      return 0
    end
    if @nd_type[nid] != "HashNode"
      return 0
    end
    elems = parse_id_list(@nd_elements[nid])
    if elems.length == 0
      return 1
    end
    0
  end

  def is_empty_array_literal(nid)
    if nid < 0
      return 0
    end
    if @nd_type[nid] != "ArrayNode"
      return 0
    end
    elems = parse_id_list(@nd_elements[nid])
    if elems.length == 0
      return 1
    end
    0
  end

 # `[]` / `[].dup` — an empty array whose static element type is
 # ambiguous. When this is the tail of a block whose result becomes
 # the inner storage of a nested array (`Array.new(N) { [].dup }`,
 # `(0..N).map { [].dup }`), defaulting to `int_array` is unsafe:
 # later pushes of pointer-typed values (3-tuples, IntArrays, etc.)
 # silently truncate the pointer to mrb_int. Use `poly_array` for
 # the inner container instead so push goes through sp_PolyArray
 # (with sp_RbVal slots) and the runtime cls_id dispatch in
 # sp_poly_shl handles any pushed kind correctly.
  def is_empty_array_or_dup(nid)
    if is_empty_array_literal(nid) == 1
      return 1
    end
    if nid < 0
      return 0
    end
    if @nd_type[nid] != "CallNode"
      return 0
    end
    if @nd_name[nid] != "dup"
      return 0
    end
    recv = @nd_receiver[nid]
    if recv < 0
      return 0
    end
    is_empty_array_literal(recv)
  end

 # `[nil] * N` / `[0] * N` is a sized empty default — the elements
 # come from `nil`/`0`, so the array's effective element type is the
 # same as `[]`'s default (int_array). Used by writer-scan and the
 # expected-type-aware compile path so a later typed `arr[i] = obj`
 # write can promote / direct-allocate the right kind of container.

  def base_type(t)
    if t.length > 1 && t[t.length - 1] == "?"
      return t[0, t.length - 1]
    end
    t
  end

  def is_nullable_pointer_type(t)
 # Pointer types that can represent nil as NULL
    bt = base_type(t)
    if bt == "ptr"
      return 1
    end
    if bt == "string" || bt == "mutable_str"
      return 1
    end
    if bt == "int_array" || bt == "str_array" || bt == "float_array" || bt == "sym_array"
      return 1
    end
    if bt == "str_int_hash" || bt == "str_str_hash"
      return 1
    end
    if bt == "sym_int_hash" || bt == "sym_str_hash" || bt == "sym_array"
      return 1
    end
    if bt == "str_poly_hash" || bt == "sym_poly_hash"
      return 1
    end
    if bt == "stringio" || bt == "lambda" || bt == "poly_array"
      return 1
    end
    if is_ptr_array_type(bt) == 1
      return 1
    end
    if bt == "fiber" || bt == "bigint"
      return 1
    end
    if is_obj_type(bt) == 1
      return 1
    end
    if is_tuple_type(bt) == 1
      return 1
    end
    0
  end

 # True when class `ci` (or any of its parents) has registered `bname` as
 # an attr_writer / attr_accessor or a struct field — i.e. `obj.bname = v`
 # may safely become a direct field write.
  def cls_has_attr_writer(ci, bname)
    if ci < 0
      return 0
    end
    writers = @cls_attr_writers[ci].split(";")
    wi = 0
    while wi < writers.length
      if writers[wi] == bname
        return 1
      end
      wi = wi + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return cls_has_attr_writer(pi, bname)
      end
    end
    0
  end


 # ---- C type mapping ----



 # PM_RANGE_FLAGS_EXCLUDE_END = 4: bit 2 set means `...` (exclusive).



  def toplevel_ivar_type(name)
    i = 0
    while i < @toplevel_ivar_names.length
      if @toplevel_ivar_names[i] == name
        return @toplevel_ivar_types[i]
      end
      i = i + 1
    end
    ""
  end

 # `iv_<name>` at toplevel (no self), `self->iv_<name>` inside a class.

  def register_toplevel_ivar(name, type)
    if type == ""
      type = "int"
    end
    i = 0
    while i < @toplevel_ivar_names.length
      if @toplevel_ivar_names[i] == name
        cur = @toplevel_ivar_types[i]
        if (cur == "int" || cur == "nil") && type != "int" && type != "nil"
          @toplevel_ivar_types[i] = type
        end
        return
      end
      i = i + 1
    end
    @toplevel_ivar_names.push(name)
    @toplevel_ivar_types.push(type)
  end

 # Walk the program for ivar nodes at script scope. Class/Module bodies
 # are skipped — their ivars belong to the enclosing class. Top-level
 # `def` bodies ARE walked: in Ruby, `def foo; @x; end` at script scope
 # shares the same `main` ivar that bare `@x` writes.
  def scan_toplevel_ivars(nid)
    if nid < 0 || nid >= @nd_count
      return
    end
    t = @nd_type[nid]
    if t == "ClassNode" || t == "ModuleNode" || t == "SingletonClassNode"
      return
    end
    if t == "InstanceVariableWriteNode"
      register_toplevel_ivar(@nd_name[nid], infer_ivar_init_type(@nd_expression[nid]))
    elsif t == "InstanceVariableReadNode" || t == "InstanceVariableTargetNode" || t == "InstanceVariableOperatorWriteNode" || t == "InstanceVariableAndWriteNode" || t == "InstanceVariableOrWriteNode"
      register_toplevel_ivar(@nd_name[nid], "int")
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_toplevel_ivars(cs[k])
      k = k + 1
    end
  end


  def resolve_gvar_alias(name)
    i = 0
    while i < @galias_new.length
      if @galias_new[i] == name
        return @galias_old[i]
      end
      i = i + 1
    end
    name
  end

 # ---- Array type helpers ----

 # The canonical "is this an array type?" check. Use this when you need
 # to dispatch a method that's defined for every typed array — `+`,
 # `concat`, `shuffle`, `each_with_object`, `flat_map`, etc. Covers the
 # 5 typed arrays (int/str/float/sym/poly) and any *_ptr_array.
  def is_array_type(t)
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
    if t == "int_array" || t == "str_array" || t == "float_array" || t == "sym_array" || t == "poly_array"
      return 1
    end
    if is_ptr_array_type(t) == 1
      return 1
    end
    0
  end

  def is_hash_type(t)
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
    if t == "str_int_hash" || t == "str_str_hash" || t == "int_str_hash"
      return 1
    end
    if t == "sym_int_hash" || t == "sym_str_hash"
      return 1
    end
    if t == "str_poly_hash" || t == "sym_poly_hash"
      return 1
    end
    if t == "poly_poly_hash"
      return 1
    end
    0
  end


  def hash_leaf_type(recv_type)
    if recv_type == "str_int_hash" || recv_type == "sym_int_hash"
      return "int"
    end
    if recv_type == "str_str_hash" || recv_type == "sym_str_hash" || recv_type == "int_str_hash"
      return "string"
    end
    if recv_type == "str_poly_hash" || recv_type == "sym_poly_hash" || recv_type == "poly_poly_hash"
      return "poly"
    end
    ""
  end

 # CRuby returns nil for static mismatches (e.g. `{a: 1}.dig("a")`)
 # since no key compares equal — Hash#dig short-circuits to nil here.
  def hash_key_matches_recv(recv_type, key_type)
    if recv_type.start_with?("sym_") && key_type == "symbol"
      return 1
    end
    if recv_type.start_with?("str_") && key_type == "string"
      return 1
    end
    if recv_type.start_with?("int_") && key_type == "int"
      return 1
    end
    0
  end

 # The four typed arrays that have set-op runtime helpers
 # (sp_*_intersect / _union / _difference). poly_array and ptr_array
 # are deliberately excluded — element equality isn't available there.
  def is_typed_array_type(t)
    t == "int_array" || t == "sym_array" || t == "str_array" || t == "float_array"
  end

 # Returns "" to fall through to the literal C operator; otherwise the
 # typed-array helper call. `op` is one of "intersect"/"union"/"difference".
 # `lt` is the pre-computed receiver type from the caller.

 # Set the right @needs_<runtime> flag for the given array type.

 # ---- Collection pass ----
 # Returns the module-singleton-accessor index for "<Module>.<accessor>",
 # or -1 if not registered.
  def find_module_acc_idx(key)
    i = 0
    while i < @module_acc_keys.length
      if @module_acc_keys[i] == key
        return i
      end
      i = i + 1
    end
    -1
  end

 # Walk the AST for `Module.accessor = RHS` writes where
 # (Module, accessor) was registered in `collect_module` as a
 # singleton accessor. Accumulates the set of distinct
 # ConstantReadNode RHSes; the lowering paths read this list to
 # choose:
 # - 0 entries: never written, falls through (un-folded)
 # - 1 entry: Stage 1, inline `<resolved>.<method>` directly
 # - 2+ entries: Stage 2, sentinel switch over the union
 # A non-constant RHS poisons the slot with a `?` sentinel marker
 # so the lowering paths treat it as un-folded.
  def resolve_module_singleton_accessors
    if @module_acc_keys.length == 0
      return
    end
    nid = 0
    while nid < @nd_type.length
      if @nd_type[nid] == "CallNode"
        mname = @nd_name[nid]
        if mname.length > 1 && mname[mname.length - 1] == "="
          recv = @nd_receiver[nid]
          if recv >= 0 && @nd_type[recv] == "ConstantReadNode"
            mod_name = @nd_name[recv]
            if module_name_exists(mod_name) == 1
              accessor = mname[0, mname.length - 1]
              key = mod_name + "." + accessor
              idx = find_module_acc_idx(key)
              if idx >= 0 && @module_acc_consts[idx] != "?"
                args_id = @nd_arguments[nid]
                if args_id >= 0
                  arg_ids = get_args(args_id)
                  if arg_ids.length > 0 && @nd_type[arg_ids[0]] == "ConstantReadNode"
                    rhs_name = @nd_name[arg_ids[0]]
                    cur = @module_acc_consts[idx]
                    cur_list = cur.split(";")
                    if not_in(rhs_name, cur_list) == 1
                      if cur == ""
                        @module_acc_consts[idx] = rhs_name
                      else
                        @module_acc_consts[idx] = cur + ";" + rhs_name
                      end
                    end
                  else
 # Non-constant RHS poisons the slot.
                    @module_acc_consts[idx] = "?"
                  end
                end
              end
            end
          end
        end
      end
      nid = nid + 1
    end
  end

 # Returns the resolved constant list for this (module, accessor):
 # `<Name1>;<Name2>;...` for foldable, `""` if never written, `"?"`
 # if poisoned (non-constant RHS).
  def module_acc_resolved(mod_name, accessor)
    idx = find_module_acc_idx(mod_name + "." + accessor)
    if idx < 0
      return ""
    end
    @module_acc_consts[idx]
  end

 # Sentinel value for Stage 2 switch dispatch. Each module's index in
 # `@module_names` doubles as its sentinel id; reading `Module` as a
 # value lowers to this integer.

 # Look up the return type of a `<class_or_module>.<mname>`
 # singleton method, walking @meth_* (module / synthetic top-level
 # form) and @cls_cmeth_* (in-class `def self.X`). Returns "" when
 # the method isn't registered. Used by the module-dispatch ternary
 # default-tail and per-arm boxing paths.


 # Print a stderr warning the first time we see an unresolved call to
 # `mname` with the given receiver-type tag. Subsequent identical
 # warnings are suppressed so a silent-fallthrough call inside a hot
 # loop emits one line, not a torrent. The warning is informational
 # only — codegen continues and emits `0` for the call's C expression
 # (the historical silent-no-op behaviour) so existing tests/benches
 # whose outputs happen to coincide with `0` keep compiling.

 # Same dedupe pattern as warn_unresolved_call but for unknown
 # ConstantReadNode names. Reuses @unresolved_call_warnings so a
 # single program with both an undefined method and an undefined
 # constant produces two distinct warnings, not interleaved noise.

 # Walk every class's parent chain. A cycle anywhere on the chain is
 # a fatal program error: bail with a clear message instead of letting
 # the recursive helpers loop forever. Self-inheritance (`class A < A`)
 # is detected as the trivial 1-step cycle.

 # Copy each inherited class method (def self.<m> on a parent
 # class) into every subclass's @cls_cmeth_* tables so the
 # subclass gets its own synthetic copy. Each copy reuses the
 # parent's AST body id; emit_class_methods then re-compiles the
 # body under the subclass's @current_class_idx, so a bare `new`
 # inside the body resolves to the subclass's constructor.
 #
 # Run after class collection so all parents are populated, and
 # before infer_all_returns / call-site widening so the synthetic
 # entries participate in regular type inference.
  def propagate_inherited_class_methods
    ci = 0
    while ci < @cls_names.length
      seen = @cls_cmeth_names[ci].split(";")
      cur_parent = @cls_parents[ci]
      while cur_parent != ""
        pi = find_class_idx(cur_parent)
        if pi < 0
          cur_parent = ""
        else
          p_cmnames = @cls_cmeth_names[pi].split(";")
          p_cmreturns = @cls_cmeth_returns[pi].split(";")
          p_cmbodies = @cls_cmeth_bodies[pi].split(";")
          p_cmparams = @cls_cmeth_params[pi].split("|")
          p_cmptypes = @cls_cmeth_ptypes[pi].split("|")
          p_cmdefaults = @cls_cmeth_defaults[pi].split("|")
          pj = 0
          while pj < p_cmnames.length
            mname = p_cmnames[pj]
            if mname != "" && not_in(mname, seen) == 1
              pret = "int"
              if pj < p_cmreturns.length
                pret = p_cmreturns[pj]
              end
              pbid = -1
              if pj < p_cmbodies.length
                pbid = p_cmbodies[pj].to_i
              end
              pparams = ""
              if pj < p_cmparams.length
                pparams = p_cmparams[pj]
              end
              pptypes = ""
              if pj < p_cmptypes.length
                pptypes = p_cmptypes[pj]
              end
              pdefaults = ""
              if pj < p_cmdefaults.length
                pdefaults = p_cmdefaults[pj]
              end
              append_cls_cmeth(ci, mname, pparams, pptypes, pret, pbid, pdefaults)
              seen.push(mname)
            end
            pj = pj + 1
          end
          cur_parent = @cls_parents[pi]
        end
      end
      ci = ci + 1
    end
  end

  def detect_circular_inheritance
    i = 0
    while i < @cls_names.length
      visited = "".split(",")
      visited.push(@cls_names[i])
      cur = @cls_parents[i]
      while cur != ""
        if not_in(cur, visited) == 0
          $stderr.puts "Error: circular inheritance involving '" + @cls_names[i] + "' via '" + cur + "'"
          exit(1)
        end
        visited.push(cur)
        pi = find_class_idx(cur)
        if pi < 0
 # Unresolved parent — stop walking; this is a separate issue
 # (the parent lookup falls through cleanly elsewhere).
          break
        end
        cur = @cls_parents[pi]
      end
      i = i + 1
    end
  end

 # ============================================================
 # Pre-emission analysis
 # ============================================================
 #
 # Two top-level drivers turn the parsed AST into the per-class /
 # per-method / per-ivar tables that the emit phase consumes:
 #
 # collect_all — populates @cls_*, @meth_*, @const_*, @module_*
 # tables; runs structural passes (Pass 0-3).
 # infer_all_returns — refines the tables: param types from call
 # sites, ivar types from writers, return types
 # from method bodies.
 #
 # Pass-numbering convention used inside collect_all (mirrored in the
 # `Pass N` comments on each call site):
 #
 # Pass 0 collect_module modules first (used by
 # include lookup later)
 # Pass 1 collect_class class table + parents
 # Pass 1.5 detect_circular_inheritance reject cycles before any
 # parent walker recurses
 # into them
 # Pass 2 collect_toplevel_method, top-level defs, constants,
 # collect_constant, and define_method
 # collect_define_method
 # Pass 2.5 infer_lambda_param_types lambda call-site types
 # flow back into stored
 # lambda value's params
 # Pass 2.6 rewrite_instance_eval_calls hoist `recv.instance_eval`
 # blocks into file-scope
 # functions with typed
 # self
 # Pass 2.7 resolve_module_singleton_ constant-fold module-
 # accessors level singleton accessors
 # stage 1)
 # Pass 3 infer_all_returns return-type inference
 # with param/ivar refines
 #
 # Anything between this banner and `def emit_header` (the start of the
 # emission phase) is part of pre-emission analysis: the various
 # detect_*, resolve_*, rewrite_*, scan_*, infer_*, and collect_*
 # helpers that the two drivers above call into.
  def collect_all
    root = @root_id
    if @nd_type[root] != "ProgramNode"
      return
    end
    stmts = get_body_stmts(root)

 # Pass 0: modules (must come before classes for include)
    stmts.each { |sid|
      if @nd_type[sid] == "ModuleNode"
        collect_module(sid)
      end
    }

 # Pass 1: classes
    stmts.each { |sid|
      if @nd_type[sid] == "ClassNode"
        collect_class(sid)
      end
    }
 # Pass 1.3: synthetic built-in classes (Method). Appended AFTER
 # user classes so existing user-class indices don't shift; the
 # poly-dispatch BUILTIN_PTR_ARRAY branch in
 # emit_poly_builtin_dispatch assumes `cls_id 0` is the first user
 # class and would silently misroute every poly call if Method
 # took that slot. .
    register_builtin_classes
 # Pass 1.4: register class variables (@@var). Walks each class
 # body for any ClassVariable*WriteNode (Write, Operator, Or, And,
 # Target) and records the inferred type per (class, name). The
 # static C globals are emitted in pass-emit alongside constants.
    collect_cvars
 # Pass 1.5: reject circular inheritance (`class A < B; class B < A`).
 # Every parent-walking helper (cls_find_method, cls_ivar_type,
 # is_class_or_ancestor, …) recurses through @cls_parents; a cycle
 # would loop forever and hang the codegen instead of erroring out
 # like CRuby. .
    detect_circular_inheritance

 # Pass 1.6: copy inherited class methods (def self.<m>) into each
 # subclass's @cls_cmeth_* table so the subclass gets its own
 # synthetic copy of the parent's body. The copy compiles under
 # the subclass's @current_class_idx, which means a bare `new`
 # inside `def self.create; new; end` resolves to the subclass's
 # constructor . Without this, `new` statically binds
 # to the lexical class — so `Article.create` returns a `Base`
 # instance instead of an `Article`.
    propagate_inherited_class_methods

 # Pass 2: top-level methods, constants, define_method
    stmts.each { |sid|
      if @nd_type[sid] == "DefNode"
        collect_toplevel_method(sid)
      end
      if @nd_type[sid] == "ConstantWriteNode"
        collect_constant(sid)
      end
 # Top-level `A, B = expr` with constant targets.
      if @nd_type[sid] == "MultiWriteNode"
        collect_scoped_multi_const("", sid)
      end
      if @nd_type[sid] == "CallNode"
        if @nd_name[sid] == "define_method"
          collect_define_method(sid)
        end
      end
    }

 # Top-level `alias $copy $orig` and BEGIN. Aliases are
 # recorded into @galias_* (consulted by sanitize_gvar /
 # scan_features / infer_type so $copy and $orig share
 # storage). BEGIN bodies are queued for emit_main to hoist
 # to the top of main() in source-encounter order.
    stmts.each { |sid|
      if @nd_type[sid] == "AliasGlobalVariableNode"
        nn = @nd_name[@nd_new_name[sid]]
        on = @nd_name[@nd_old_name[sid]]
        if nn != "" && on != ""
          @galias_new.push(nn)
          @galias_old.push(on)
        end
      end
      if @nd_type[sid] == "PreExecutionNode"
 # Parser maps the "statements" field onto @nd_body via
 # set_ref_field at line 706.
        bid = @nd_body[sid]
        if bid >= 0
          @pre_execution_blocks.push(bid)
        end
      end
      if @nd_type[sid] == "PostExecutionNode"
        bid = @nd_body[sid]
        if bid >= 0
          @post_execution_blocks.push(bid)
        end
      end
    }

 # Pass 2.6: hoist `recv.instance_eval do ... end` blocks into
 # file-scope static functions. Receiver-class flow analysis picks the
 # receiver's class, the block body is later compiled as a function
 # with a typed `self` parameter, and the call site is rewritten to
 # invoke that function directly. v1: top-level locals previously
 # assigned `ClassName.new`; no block params; no closures; no yield.
    rewrite_instance_eval_calls

 # Pass 2.7: resolve module-level singleton accessors via constant
 # fold , Stage 1). Single assignment of a constant
 # name (typically a module/class) to `M.acc` or `@acc` inside
 # `module M` is folded; reads later substitute the resolved
 # constant.
    resolve_module_singleton_accessors

 # Pass 2.5: infer lambda parameter types from call sites
    infer_lambda_param_types

 # Pass 3: infer return types
    infer_all_returns
  end

  def rewrite_instance_eval_calls
    @ieval_counter = 0
 # Reset the registry too — codegen mode re-runs this pass after
 # loading the IR (because the AST mutations don't survive the
 # AST-file re-read), and without this reset the loaded entries
 # from analyze get appended to instead of replaced.
    @ieval_class_idxs = []
    @ieval_body_ids = []
 # Widen class-method ptypes through obj-typed receivers before the
 # walk so a method-param receiver (e.g. `def configure(app);
 # app.instance_eval { } end` invoked as `cfg.configure(routes)`)
 # has `app` typed as obj_<C>. Surgical fork — see the helper for
 # why we don't just call scan_new_calls here.
    propagate_recv_method_arg_types_for_ieval
    local_class = {}
 # Walk the AST recursively from the root, respecting scope boundaries.
 # `local_class` maps `name -> class_idx` for the current scope only.
 # Method/lambda/module/block bodies are NOT entered for local tracking:
 # their locals belong to a different scope. A reassignment to a
 # non-`Class.new` RHS poisons the mapping for that name. ClassNode
 # bodies are visited for the side effect of walking each instance
 # method's body with `@current_class_idx` set, so an `@ivar.instance_eval { }`
 # site inside a class method can resolve its receiver class via
 # `cls_ivar_type`. The local_class map is intentionally not threaded
 # into method bodies — locals there are out of scope, and the
 # ivar-only extension does not (yet) try to type method-local copies
 # of class instances.
    ieval_walk(@root_id, local_class)
    ieval_walk_class_methods
  end

 # Visit each class's instance-method bodies with `@current_class_idx`
 # set, so `@ivar.instance_eval { ... }` resolves recv's class through
 # `cls_ivar_type`. Class methods (singleton-side) are intentionally
 # excluded: they don't see the instance's @ivars, and `self` rebinding
 # against a class object would be a different (singleton-class) lift.
 #
 # Per-method scope: declare each method's params (with the ptypes
 # widened by infer_param_types_from_callsites at Pass 2.55) and the
 # body locals from scan_locals_first_type, so a LocalVariableReadNode
 # receiver inside the body can resolve its class via find_var_type.
 # That covers `def configure(app); app.instance_eval { } end` and
 # also method-local copies whose RHS is statically classifiable
 # (e.g., `routes = Routes.new`). Method returns whose call type
 # depends on infer_all_returns having run are still out of reach
 # at this Pass 2.6 timing — that's the next follow-up.
  def ieval_walk_class_methods
    ci = 0
    while ci < @cls_names.length
      @current_class_idx = ci
      bodies = @cls_meth_bodies[ci].split(";")
      bj = 0
      while bj < bodies.length
        bid = bodies[bj].to_i
        if bid >= 0
          push_scope
          pnames = cls_meth_pnames_get(ci, bj)
          ptypes = cls_meth_ptypes_get(ci, bj)
          k = 0
          while k < pnames.length
            pt = "int"
            if k < ptypes.length
              pt = ptypes[k]
            end
            declare_var(pnames[k], pt)
            k = k + 1
          end
 # Body locals: scan_locals_first_type matches what
 # infer_all_returns does in its class-methods preamble
 # (Pass 3). Pulls in `routes = Routes.new` with type
 # obj_Routes when the RHS is statically classifiable.
          lnames = "".split(",")
          ltypes = "".split(",")
          scan_locals_first_type(bid, lnames, ltypes, pnames)
          lk = 0
          while lk < lnames.length
            declare_var(lnames[lk], ltypes[lk])
            lk = lk + 1
          end
          empty_locals = {}
          ieval_walk(bid, empty_locals)
          pop_scope
        end
        bj = bj + 1
      end
      @current_class_idx = -1
      ci = ci + 1
    end
  end

  def ieval_walk(nid, local_class)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "ProgramNode"
      ieval_walk(@nd_body[nid], local_class)
      return
    end
    if t == "StatementsNode"
      stmts = parse_id_list(@nd_stmts[nid])
      k = 0
      while k < stmts.length
        ieval_walk(stmts[k], local_class)
        k = k + 1
      end
      return
    end
    if t == "LocalVariableWriteNode"
      val_nid = @nd_expression[nid]
      vname = @nd_name[nid]
      if val_nid >= 0
        ieval_walk(val_nid, local_class)
        ci = ieval_expr_class_idx(val_nid)
        if ci >= 0
          local_class[vname] = ci
        else
          if local_class.key?(vname)
            local_class.delete(vname)
          end
        end
      end
      return
    end
    if t == "CallNode"
      if @nd_name[nid] == "instance_eval"
        ieval_rewrite_call(nid, local_class)
 # Don't descend into the lifted block body.
        return
      end
      r = @nd_receiver[nid]
      if r >= 0
        ieval_walk(r, local_class)
      end
      a = @nd_arguments[nid]
      if a >= 0
        ieval_walk(a, local_class)
      end
 # Block bodies are a separate scope; don't recurse.
      return
    end
    if t == "ArgumentsNode"
      args = parse_id_list(@nd_args[nid])
      k = 0
      while k < args.length
        ieval_walk(args[k], local_class)
        k = k + 1
      end
      return
    end
    if t == "IfNode"
      ieval_walk(@nd_predicate[nid], local_class)
      ieval_walk(@nd_body[nid], local_class)
      ieval_walk(@nd_subsequent[nid], local_class)
      ieval_walk(@nd_else_clause[nid], local_class)
      return
    end
    if t == "UnlessNode"
      ieval_walk(@nd_predicate[nid], local_class)
      ieval_walk(@nd_body[nid], local_class)
      ieval_walk(@nd_else_clause[nid], local_class)
      return
    end
    if t == "ElseNode"
      ieval_walk(@nd_body[nid], local_class)
      return
    end
    if t == "WhileNode"
      ieval_walk(@nd_predicate[nid], local_class)
      ieval_walk(@nd_body[nid], local_class)
      return
    end
    if t == "UntilNode"
      ieval_walk(@nd_predicate[nid], local_class)
      ieval_walk(@nd_body[nid], local_class)
      return
    end
    if t == "CaseNode"
      ieval_walk(@nd_predicate[nid], local_class)
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        ieval_walk(conds[k], local_class)
        k = k + 1
      end
      ieval_walk(@nd_else_clause[nid], local_class)
      return
    end
    if t == "WhenNode"
      ieval_walk(@nd_body[nid], local_class)
      return
    end
    if t == "BeginNode"
      ieval_walk(@nd_body[nid], local_class)
      ieval_walk(@nd_rescue_clause[nid], local_class)
      ieval_walk(@nd_ensure_clause[nid], local_class)
      return
    end
 # DefNode, LambdaNode, ClassNode, ModuleNode, BlockNode: not entered.
 # Their bodies introduce new scopes; the top-level map must not leak
 # in. Anything else: stop. Conservative — we won't rewrite.
  end

  def ieval_expr_class_idx(nid)
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "new"
        recv = @nd_receiver[nid]
        if recv >= 0
          if @nd_type[recv] == "ConstantReadNode"
            return find_class_idx(@nd_name[recv])
          end
 # `Foo::Bar.new`: Spinel's class registry is keyed by the leaf
 # name, matching how `collect_class` records nested classes.
          if @nd_type[recv] == "ConstantPathNode"
            return find_class_idx(@nd_name[recv])
          end
        end
      end
    end
    -1
  end

  def ieval_rewrite_call(nid, local_class)
    if @nd_name[nid] != "instance_eval"
      return
    end
    recv = @nd_receiver[nid]
    blk = @nd_block[nid]
    if recv < 0
      return
    end
    if blk < 0
      return
    end
 # Skip blocks with parameters: lifted function takes only `self`.
    if @nd_parameters[blk] >= 0
      return
    end
    ci = -1
    if @nd_type[recv] == "LocalVariableReadNode"
      vname = @nd_name[recv]
      if local_class.key?(vname)
        ci = local_class[vname]
      else
 # Inside a class instance method, the v1 top-level local_class
 # map is intentionally empty. Fall back to find_var_type so a
 # method param (or scan_locals-typed local) resolves through
 # the scope chain that ieval_walk_class_methods sets up. The
 # is_obj_type / base_type strip is the same shape used in the
 # ivar branch and at every other obj_-prefix site in this file.
        vt = find_var_type(vname)
        bt = base_type(vt)
        if is_obj_type(bt) == 1
          ci = find_class_idx(bt[4, bt.length - 4])
        end
      end
    elsif @nd_type[recv] == "InstanceVariableReadNode"
 # `@ivar.instance_eval { }` inside a class method. ieval_walk_class_methods
 # sets @current_class_idx so cls_ivar_type returns the ivar's stored
 # type — "obj_<Class>" when the ivar was bound to `Class.new` (and
 # not since widened to poly). Strip the "obj_" prefix to look up
 # the class index, the same shape `is_obj_type` / `base_type`
 # gates use elsewhere in the codegen for object-typed values.
      if @current_class_idx >= 0
        it = cls_ivar_type(@current_class_idx, @nd_name[recv])
        bt = base_type(it)
        if is_obj_type(bt) == 1
          ci = find_class_idx(bt[4, bt.length - 4])
        end
      end
    end
    if ci < 0
      return
    end
    body_id = @nd_body[blk]
 # v1: bail if the block uses yield/block_given?. Lifting it as a
 # plain function would lose the enclosing method's block plumbing.
 # Spinel rejected such code before — leaving it rejected is no
 # regression, and the support belongs in a follow-up.
    if body_id >= 0 && body_has_yield(body_id) == 1
      return
    end
    n = @ieval_counter
    @ieval_counter = @ieval_counter + 1
    @ieval_class_idxs.push(ci)
    @ieval_body_ids.push(body_id)
 # Mark the call site: the function name doubles as the synthetic id.
 # compile_call_expr / compile_call_stmt recognise the prefix and
 # emit a direct C call to `sp_ieval_<N>`.
    @nd_name[nid] = "__sp_ieval_" + n.to_s
    @nd_block[nid] = -1
  end


 # Type inference: walk each lifted block body with `@current_class_idx`
 # set to the receiver's class so bare self-calls inside the block
 # propagate arg types to the class's methods. Without this pass, a
 # block like `app.instance_eval { get("/") }` would fail to teach
 # `Routes#get(path)` that `path` is a string. Sibling pass to
 # `infer_class_body_call_types` for hoisted blocks.
  def infer_ieval_body_call_types
    n = 0
    while n < @ieval_class_idxs.length
      ci = @ieval_class_idxs[n]
      bid = @ieval_body_ids[n]
      if bid >= 0
        @current_class_idx = ci
        push_scope
        scan_cls_method_calls(ci, bid)
        scan_new_calls(bid)
        pop_scope
        @current_class_idx = -1
      end
      n = n + 1
    end
  end

  def is_ieval_call_name(mname)
    if mname.length <= 11
      return 0
    end
    if mname[0, 11] == "__sp_ieval_"
      return 1
    end
    0
  end


 # v1 lifts blocks into void-returning functions (Ruby's
 # instance_eval-as-expression value isn't supported yet). When a
 # call appears in expression position, return the recv pointer as a
 # truthy default via a comma expression so callers like
 # `if obj.instance_eval { ... }` still type-check. Real expression
 # support — return the block's last expression — is a v2 follow-up.


  def is_builtin_type_name(name)
    if name == "Integer"
      return 1
    end
    if name == "String"
      return 1
    end
    if name == "Float"
      return 1
    end
    0
  end

 # built-in class / module names that
 # get a reserved cls_id (0..20). Kept in sync with
 # spinel_codegen.rb's @builtin_class_names array.
  def is_builtin_class_const_name(name)
    if name == "BasicObject" || name == "Object" || name == "Kernel" || name == "Comparable" || name == "Enumerable"
      return 1
    end
    if name == "NilClass" || name == "TrueClass" || name == "FalseClass"
      return 1
    end
    if name == "Numeric" || name == "Integer" || name == "Float"
      return 1
    end
    if name == "String" || name == "Symbol"
      return 1
    end
    if name == "Array" || name == "Hash" || name == "Range" || name == "Time"
      return 1
    end
    if name == "Module" || name == "Class" || name == "Complex" || name == "Proc"
      return 1
    end
    0
  end

  def collect_class(nid)
    collect_class_with_prefix(nid, "")
  end

 # Walk class bodies, module bodies, and the top-level statement
 # list for class-var writes; register each (class-or-Toplevel,
 # name) pair so the static-declaration pass can emit
 # `static <type> cvar_<qname> = <default>;` ahead of the functions
 # that touch it.
 #
 # Module-scope and top-level `@@x = ...` writes belong to the
 # `Toplevel` namespace -- spinel models cvars per-class, modules
 # don't have a cls_id of their own, and tying them to "Toplevel"
 # matches what the read/write codegen already emits when
 # `@current_class_idx == -1`.
  def collect_cvars
    root = @root_id
    if @nd_type[root] != "ProgramNode"
      return
    end
    collect_cvars_in(root, -1)
  end

 # Recursively scan `nid`'s subtree for ClassVariable*WriteNode and
 # register each. Only the WriteNode form contributes a static type
 # at this stage; the compound forms (Operator/Or/And) are
 # registered when their parent ClassVariableWriteNode is seen, OR
 # lazily during compile_stmt if no plain Write precedes them in
 # the same class.
 #
 # When the walk crosses into a nested ClassNode or ModuleNode the
 # context flips: nested ClassNode switches to that class's
 # @cls_names index, ModuleNode keeps the Toplevel namespace.
  def collect_cvars_in(nid, class_idx)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "ClassNode"
      cp = @nd_constant_path[nid]
      if cp >= 0
        cname = const_ref_flat_name(cp)
        ci = find_class_idx(cname)
        if ci >= 0
          collect_cvars_in(@nd_body[nid], ci)
        end
      end
      return
    end
    if t == "ModuleNode"
      collect_cvars_in(@nd_body[nid], -1)
      return
    end
    if t == "DefNode"
 # Method bodies aren't walked at collect-time. A `@@x = v`
 # inside a method writes during the call, but `v`'s type isn't
 # yet resolved (LocalVariableReadNode falls back to "int" when
 # the var-type table hasn't been populated), which would
 # spuriously widen a class-body literal's `string`/`float`
 # initialization to poly. The method-body write is registered
 # defensively at compile_stmt time anyway, when v's call-site-
 # resolved type is known.
      return
    end
    if t == "ClassVariableWriteNode"
      qname = cvar_qname(class_idx, @nd_name[nid])
      val_t = infer_type(@nd_expression[nid])
      register_cvar(qname, val_t)
      try_fold_cvar_init(qname, @nd_expression[nid])
    end
 # `@@x op= val` / `@@x ||= val` / `@@x &&= val` — same
 # storage as plain `@@x = ...`. Register the cvar so the
 # static decl pass emits a slot, and seed its type from the
 # rhs (or default int for `||=`/`&&=` reading nil).
    if t == "ClassVariableOperatorWriteNode" || t == "ClassVariableOrWriteNode" || t == "ClassVariableAndWriteNode"
      qname = cvar_qname(class_idx, @nd_name[nid])
      val_t = infer_type(@nd_expression[nid])
      register_cvar(qname, val_t)
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_cvars_in(cs[k], class_idx)
      k = k + 1
    end
  end

  def collect_scoped_constant(scope_name, nid)
    cname = @nd_name[nid]
    if scope_name != ""
      cname = scope_name + "_" + cname
    end
    expr_id = @nd_expression[nid]
    if expr_id >= 0
      if @nd_type[expr_id] == "CallNode"
        if @nd_name[expr_id] == "new"
          sr = @nd_receiver[expr_id]
          if sr >= 0
            if @nd_type[sr] == "ConstantReadNode"
              if @nd_name[sr] == "Struct"
                collect_struct_class(cname, expr_id)
                return
              end
            end
          end
        end
      end
    end
    ct = "int"
    if expr_id >= 0
      old_scope = @current_lexical_scope
      @current_lexical_scope = scope_name
      ct = infer_type(expr_id)
      @current_lexical_scope = old_scope
    end
    ci = find_const_idx(cname)
    if ci >= 0
      @const_types[ci] = ct
      @const_expr_ids[ci] = expr_id
      @const_scope_names[ci] = scope_name
      return
    end
    @const_names.push(cname)
    @const_types.push(ct)
    @const_expr_ids.push(expr_id)
    @const_scope_names.push(scope_name)
  end

  def collect_class_with_prefix(nid, module_prefix)
    ci = @cls_names.length
    cname = ""
    cp = @nd_constant_path[nid]
    if cp >= 0
      cname = const_ref_flat_name(cp)
 # For `module M; class C; ... end; end`, Prism gives class name as
 # ConstantReadNode("C"), so attach lexical module prefix.
      if module_prefix != "" && const_ref_is_relative(cp) == 1
        cname = module_prefix + "_" + cname
      end
    end

 # Check for open class on built-in type
    if is_builtin_type_name(cname) == 1
      @open_class_names.push(cname)
 # Collect methods as top-level functions with special naming
      body = @nd_body[nid]
      if body >= 0
        body_stmts = get_stmts(body)
        body_stmts.each { |sid|
          if @nd_type[sid] == "DefNode"
 # Add as top-level method with prefix
            mname = @nd_name[sid]
 # Store with special naming for lookup
            @meth_names.push("__oc_" + cname + "_" + mname)
            params = collect_params_str(sid)
            @meth_param_names.push(params)
            @meth_param_types.push(collect_ptypes_str(sid, -1))
            @meth_param_empty.push("")
            @meth_return_types.push("int")
            @meth_body_ids.push(@nd_body[sid])
            @meth_has_yield.push(0)
            @meth_has_defaults.push("0")
            @meth_rest_index.push(collect_rest_index(sid))
          end
        }
      end
      return
    end

 # Class reopening: if the class was already registered (in an
 # earlier `class Foo ... end` block), reuse the existing entry
 # so methods and attrs from this body get appended rather than
 # producing a duplicate C struct/constructor.
    existing_ci = find_class_idx(cname)
    if existing_ci >= 0
      ci = existing_ci
      body = @nd_body[nid]
      if body < 0
        return
      end
      body_stmts = get_stmts(body)
      body_stmts.each { |sid|
        if @nd_type[sid] == "DefNode"
          collect_class_method(ci, sid)
        end
        if @nd_type[sid] == "ConstantWriteNode"
          collect_scoped_constant(cname, sid)
        end
        if @nd_type[sid] == "CallNode"
          cn = @nd_name[sid]
          if cn != "include"
            if cn != "private"
              collect_attr_call(ci, sid)
            end
          end
        end
        if @nd_type[sid] == "ClassNode"
          collect_class_with_prefix(sid, cname)
        end
        if @nd_type[sid] == "ModuleNode"
          collect_module_with_prefix(sid, cname)
        end
      }
      body_stmts.each { |sid|
        if @nd_type[sid] == "CallNode"
          if @nd_name[sid] == "include"
            inc_args = @nd_arguments[sid]
            if inc_args >= 0
              inc_ids = get_args(inc_args)
              ik = 0
              while ik < inc_ids.length
                if @nd_type[inc_ids[ik]] == "ConstantReadNode"
                  mod_name = @nd_name[inc_ids[ik]]
 # when this class is nested inside a
 # module (module_prefix != ""), the include arg is
 # a bare ConstantReadNode but the registered module
 # name is `<prefix>_<name>`. Try the qualified form
 # first; fall back to the bare name for top-level
 # modules.
                  resolved_mod_name = resolve_include_module_name(mod_name, module_prefix)
                  collect_module_methods_into_class(ci, resolved_mod_name)
                  record_class_include(ci, resolved_mod_name)
                end
                ik = ik + 1
              end
            end
          end
        end
      }
 # Pin lexical scope while collecting ivars. See longer comment
 # at the other collect_ivars call site below for rationale.
      saved_idx = @current_class_idx
      @current_class_idx = ci
      collect_ivars(ci)
      @current_class_idx = saved_idx
      return
    end

    parent = ""
    sp = @nd_superclass[nid]
    struct_fields = "".split(",")
    if sp >= 0
      if @nd_type[sp] == "CallNode"
        if @nd_name[sp] == "new"
          sr = @nd_receiver[sp]
          if sr >= 0
            if @nd_type[sr] == "ConstantReadNode"
              if @nd_name[sr] == "Struct"
 # Struct.new(:x, :y, keyword_init: true)
                sargs_id = @nd_arguments[sp]
                if sargs_id >= 0
                  sarg_ids = get_args(sargs_id)
                  sk = 0
                  while sk < sarg_ids.length
                    if @nd_type[sarg_ids[sk]] == "SymbolNode"
                      fname = @nd_content[sarg_ids[sk]]
                      if fname == ""
                        fname = @nd_name[sarg_ids[sk]]
                      end
                      struct_fields.push(fname)
                    end
                    if @nd_type[sarg_ids[sk]] == "KeywordHashNode"
 # keyword_init detected
                    end
                    sk = sk + 1
                  end
                end
              end
            end
          end
        end
      else
        parent = const_ref_flat_name(sp)
        if parent == ""
          parent = @nd_name[sp]
        end
 # Resolve the parent class name against the same module-prefix
 # chain that was used to register the child. `class Sub < Base`
 # inside `module M` should land on `M_Base` (the registered
 # name), not bare "Base", or `emit_class_fields` later fails
 # `find_class_idx` and silently drops every inherited field.
        if module_prefix != "" && const_ref_is_relative(sp) == 1
          if find_class_idx(parent) < 0
            mp = module_prefix
            while mp != ""
              cand = mp + "_" + parent
              if find_class_idx(cand) >= 0
                parent = cand
                mp = ""
              else
                idx = mp.rindex("_")
 # CRuby returns nil when not found; spinel runtime returns -1.
                if idx == nil || idx < 0
                  mp = ""
                else
                  mp = mp[0, idx]
                end
              end
            end
          end
        end
      end
    end

    ci = @cls_names.length
    @cls_names.push(cname)
    @cls_is_value_type.push(0)
    @cls_is_sra.push(0)
    @cls_parents.push(parent)
    @cls_includes.push("")
 # Initialize struct fields as ivars
    ivar_names = ""
    ivar_types = ""
    attr_readers = ""
    attr_writers = ""
    sk = 0
    while sk < struct_fields.length
      if sk > 0
        ivar_names = ivar_names + ";"
        ivar_types = ivar_types + ";"
        attr_readers = attr_readers + ";"
        attr_writers = attr_writers + ";"
      end
      ivar_names = ivar_names + "@" + struct_fields[sk]
      ivar_types = ivar_types + "int"
      attr_readers = attr_readers + struct_fields[sk]
      attr_writers = attr_writers + struct_fields[sk]
      sk = sk + 1
    end
    @cls_ivar_names.push(ivar_names)
    @cls_ivar_types.push(ivar_types)
 # Struct fields are added via attr_*-style fallback (no scanned literal
 # write yet). Mark each as non-definite.
    struct_definite = ""
    sk2 = 0
    while sk2 < struct_fields.length
      if sk2 > 0
        struct_definite = struct_definite + ";"
      end
      struct_definite = struct_definite + "0"
      sk2 = sk2 + 1
    end
    @cls_ivar_init_definite.push(struct_definite)
 # Initialize observed_types parallel to ivar_names: one empty
 # comma-list per struct field, joined by semicolons.
    obs_init = ""
    sk3 = 0
    while sk3 < struct_fields.length
      if sk3 > 0
        obs_init = obs_init + ";"
      end
      sk3 = sk3 + 1
    end
    @cls_ivar_observed_types.push(obs_init)
 # Auto-generate initialize method for struct-derived classes
    if struct_fields.length > 0
      init_params = ""
      init_ptypes = ""
      init_defaults = ""
      sk = 0
      while sk < struct_fields.length
        if sk > 0
          init_params = init_params + ","
          init_ptypes = init_ptypes + ","
          init_defaults = init_defaults + ","
        end
        init_params = init_params + struct_fields[sk]
        init_ptypes = init_ptypes + "int"
        init_defaults = init_defaults + "-1"
        sk = sk + 1
      end
      @cls_meth_names.push("initialize")
      @cls_meth_params.push(init_params)
      @cls_meth_ptypes.push(init_ptypes)
      @cls_meth_returns.push("void")
      @cls_meth_bodies.push("-2")
      @cls_meth_defaults.push(init_defaults)
      @cls_meth_ptypes_empty.push("")
    else
      @cls_meth_names.push("")
      @cls_meth_params.push("")
      @cls_meth_ptypes.push("")
      @cls_meth_returns.push("")
      @cls_meth_bodies.push("")
      @cls_meth_defaults.push("")
      @cls_meth_ptypes_empty.push("")
    end
    @cls_attr_readers.push(attr_readers)
    @cls_attr_writers.push(attr_writers)
    @cls_cmeth_names.push("")
    @cls_cmeth_params.push("")
    @cls_cmeth_ptypes.push("")
    @cls_cmeth_returns.push("")
    @cls_cmeth_bodies.push("")
    @cls_cmeth_defaults.push("")
    @cls_cmeth_scope_names.push("")
    @cls_cmeth_scope_types.push("")
    @cls_meth_has_yield.push("")

 # Collect class body
    body = @nd_body[nid]
    if body < 0
      return
    end
    body_stmts = get_stmts(body)
 # First pass: collect all class methods and attrs
    body_stmts.each { |sid|
      if @nd_type[sid] == "DefNode"
        collect_class_method(ci, sid)
      end
      if @nd_type[sid] == "ConstantWriteNode"
        collect_scoped_constant(cname, sid)
      end
 # Class-body `A, B = ...` multi-write to constants.
      if @nd_type[sid] == "MultiWriteNode"
        collect_scoped_multi_const(cname, sid)
      end
      if @nd_type[sid] == "CallNode"
        cn = @nd_name[sid]
        if cn != "include"
          if cn != "private"
            collect_attr_call(ci, sid)
          end
        end
      end
 # Nested class / module inside class. Mirroring the
 # nested-in-module path, the inner type is registered at top
 # level under its outer-class–prefixed name (e.g. `A::B` →
 # `A_B`) so a `A::B.new` call resolves via the same flat lookup.
      if @nd_type[sid] == "ClassNode"
        collect_class_with_prefix(sid, cname)
      end
      if @nd_type[sid] == "ModuleNode"
        collect_module_with_prefix(sid, cname)
      end
    }
 # Second pass: handle includes (after all own methods are known)
    body_stmts.each { |sid|
      if @nd_type[sid] == "CallNode"
        if @nd_name[sid] == "include"
          inc_args = @nd_arguments[sid]
          if inc_args >= 0
            inc_ids = get_args(inc_args)
            ik = 0
            while ik < inc_ids.length
              if @nd_type[inc_ids[ik]] == "ConstantReadNode"
                mod_name = @nd_name[inc_ids[ik]]
 # same prefix resolution as the
 # class-reopening branch -- the include arg is a
 # bare name but the registered module's name is
 # `<prefix>_<name>` when nested in a module.
                resolved_mod_name = resolve_include_module_name(mod_name, module_prefix)
                collect_module_methods_into_class(ci, resolved_mod_name)
 # record the include
 # relationship for ancestors-table emission. The
 # methods are already merged into the class above;
 # this pass keeps the module link alive so the
 # codegen can weave it into the MRO.
                record_class_include(ci, resolved_mod_name)
              end
              ik = ik + 1
            end
          end
        end
      end
    }

 # Third pass: handle alias / undef inside the class body. Must
 # run AFTER all own methods + included module methods are
 # registered, so the alias source can be located.
    body_stmts.each { |sid|
      if @nd_type[sid] == "AliasMethodNode"
        nn = symbol_node_literal(@nd_new_name[sid])
        on = symbol_node_literal(@nd_old_name[sid])
        if nn != "" && on != ""
          collect_class_method_alias(ci, nn, on)
        end
      end
      if @nd_type[sid] == "UndefNode"
 # `undef foo, bar` -- record the removal in @undef_*. Spinel
 # doesn't currently enforce "calling an undef'd method
 # fails" at the compile-time dispatch path; the recording
 # is the foundation for that future check.
        unames = parse_id_list(@nd_names[sid])
        uk = 0
        while uk < unames.length
          uname = symbol_node_literal(unames[uk])
          if uname != ""
            collect_class_method_undef(ci, uname)
          end
          uk = uk + 1
        end
      end
    }


 # Collect ivars. Pin the lexical scope to this class so any
 # `@x = Foo.new(...)` inside its methods resolves `Foo` against
 # the same scope chain the call site sees — without this,
 # current_lexical_scope_name returns "" and a `Config` reference
 # inside `Optcarrot::NES` resolves to bare "Config" (no
 # Optcarrot_ prefix), poisoning the ivar's recorded type and the
 # eventual `sp_Config *` field declaration that fails to compile.
    saved_idx = @current_class_idx
    @current_class_idx = ci
    collect_ivars(ci)
    @current_class_idx = saved_idx
  end

 # Extract the literal name from a SymbolNode argument. Returns ""
 # for InterpolatedSymbolNode and other shapes Spinel doesn't
 # support as alias source/target or undef target at compile time.
  def symbol_node_literal(nid)
    if nid < 0
      return ""
    end
    if @nd_type[nid] == "SymbolNode"
      v = @nd_content[nid]
      if v == ""
        v = @nd_name[nid]
      end
      return v
    end
    ""
  end

 # `alias new old` -- copy the existing class method's slot to a
 # new name in @cls_meth_*. CRuby snapshots the method body at
 # alias time; copying the body_id (a number, not a reference)
 # gives the same snapshot semantics: a later `def old` redefinition
 # would assign a new body_id to the old slot but the alias slot
 # keeps the original.
  def collect_class_method_alias(ci, new_name, old_name)
    src = cls_find_method_direct(ci, old_name)
    if src < 0
      $stderr.puts "Spinel: alias `" + new_name + "` -> `" + old_name + "`: source method not found in class " + @cls_names[ci]
      exit(1)
    end
    pnames_all = @cls_meth_params[ci].split("|")
    ptypes_all = @cls_meth_ptypes[ci].split("|")
    rets_all   = @cls_meth_returns[ci].split(";")
    bodies_all = @cls_meth_bodies[ci].split(";")
    defs_all   = @cls_meth_defaults[ci].split("|")
    params  = pnames_all[src] || ""
    ptypes  = ptypes_all[src] || ""
    ret     = rets_all[src] || "int"
    body_id = bodies_all[src].to_i
    defs    = defs_all[src] || ""
    append_cls_meth(ci, new_name, params, ptypes, ret, body_id, defs)
    if @cls_meth_has_yield[ci] != ""
      @cls_meth_has_yield[ci] = @cls_meth_has_yield[ci] + ";0"
    end
  end

 # `undef foo` -- mark the method as removed in the @undef_*
 # tracker. Spinel currently doesn't enforce "call after undef
 # fails to compile" -- the recording is the foundation for that
 # future check. Documented out of scope in test/undef.rb.
  def collect_class_method_undef(ci, name)
    @undef_class_idx.push(ci)
    @undef_method.push(name)
  end


 # resolve a bare include arg against the enclosing
 # module's lexical scope. For `module Ns; class Base; include
 # Helper; end; end`, the include's ConstantReadNode is the
 # unqualified `Helper`, but the registered module's name (from
 # collect_module_with_prefix) is `Ns_Helper`. Try the qualified
 # form first; fall back to the bare name for top-level modules.
 # append a module name to the per-class
 # includes list. Skipped when the module name doesn't resolve to
 # a registered module (collect_module_methods_into_class already
 # warns / no-ops in that case; the ancestors table only carries
 # modules known to the program).
  def record_class_include(ci, mod_name)
    mi = 0
    found = 0
    while mi < @module_names.length
      if @module_names[mi] == mod_name
        found = 1
        mi = @module_names.length
      else
        mi = mi + 1
      end
    end
    if found == 0
      return
    end
    cur = @cls_includes[ci]
 # Dedup: a re-included module shouldn't duplicate in ancestors.
    parts = cur.split(";")
    pk = 0
    while pk < parts.length
      if parts[pk] == mod_name
        return
      end
      pk = pk + 1
    end
    if cur == ""
      @cls_includes[ci] = mod_name
    else
      @cls_includes[ci] = cur + ";" + mod_name
    end
  end

  def resolve_include_module_name(mod_name, module_prefix)
    if module_prefix != ""
      qualified = module_prefix + "_" + mod_name
      i = 0
      while i < @module_names.length
        if @module_names[i] == qualified
          return qualified
        end
        i = i + 1
      end
    end
    mod_name
  end

  def collect_module_methods_into_class(ci, mod_name)
 # Find the module and add its methods to the class
    mi = 0
    while mi < @module_names.length
      if @module_names[mi] == mod_name
        mbody = @module_body_ids[mi]
        if mbody >= 0
          mstmts = get_stmts(mbody)
          mk = 0
          while mk < mstmts.length
            sid = mstmts[mk]
            if @nd_type[sid] == "DefNode"
              mname = @nd_name[sid]
 # Only add if class doesn't already have this method
              existing = cls_find_method_direct(ci, mname)
              if existing < 0
                collect_class_method(ci, sid)
              end
            end
            mk = mk + 1
          end
        end
      end
      mi = mi + 1
    end
  end

  def collect_class_method(ci, nid)
    mname = @nd_name[nid]
    body_id = @nd_body[nid]

 # Check for class method (def self.xxx)
    if @nd_receiver[nid] >= 0
      if @nd_type[@nd_receiver[nid]] == "SelfNode"
 # Class method
        params_str = collect_params_str(nid)
        ptypes_str = collect_ptypes_str(nid, ci)
        defaults_str = collect_defaults_str(nid)
        append_cls_cmeth(ci, mname, params_str, ptypes_str, "int", body_id, defaults_str)
        return
      end
    end

    params_str = collect_params_str(nid)
    ptypes_str = collect_ptypes_str(nid, ci)
    defaults_str = collect_defaults_str(nid)
    has_y = body_has_yield(body_id)
    append_cls_meth(ci, mname, params_str, ptypes_str, "int", body_id, defaults_str)
 # Track yield info
    if @cls_meth_has_yield[ci] != ""
      @cls_meth_has_yield[ci] = @cls_meth_has_yield[ci] + ";" + has_y.to_s
    else
      @cls_meth_has_yield[ci] = has_y.to_s
    end
    return
  end

  def collect_params_str(nid)
    params = @nd_parameters[nid]
    if params < 0
      return ""
    end
    reqs = parse_id_list(@nd_requireds[params])
    opts = parse_id_list(@nd_optionals[params])
    kws = parse_id_list(@nd_keywords[params])
    result = ""
    k = 0
    while k < reqs.length
      if result != ""
        result = result + ","
      end
      result = result + @nd_name[reqs[k]]
      k = k + 1
    end
    k = 0
    while k < opts.length
      if result != ""
        result = result + ","
      end
      result = result + @nd_name[opts[k]]
      k = k + 1
    end
    k = 0
    while k < kws.length
      if result != ""
        result = result + ","
      end
      result = result + @nd_name[kws[k]]
      k = k + 1
    end
 # Rest param (splat)
    rest = @nd_rest[params]
    if rest >= 0
      if @nd_type[rest] == "RestParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + @nd_name[rest]
      end
    end
 # Post-rest required params: `def f(*r, x, y)` -> `x, y` come AFTER
 # rest in the AST's `posts` slot. Same shape as `requireds` (each
 # entry is a RequiredParameterNode); flow them straight through.
    posts = parse_id_list(@nd_posts[params])
    k = 0
    while k < posts.length
      if @nd_type[posts[k]] == "RequiredParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + @nd_name[posts[k]]
      end
      k = k + 1
    end
 # Keyword rest (`**kw`). Anonymous `**` synthesizes `__anon_kwrest`.
 # NoKeywordsParameterNode (`**nil`) is skipped here -- it doesn't
 # carry a slot.
    kwrest = @nd_keyword_rest[params]
    if kwrest >= 0
      if @nd_type[kwrest] == "KeywordRestParameterNode"
        if result != ""
          result = result + ","
        end
        kn = @nd_name[kwrest]
        if kn == ""
          kn = "__anon_kwrest"
        end
        result = result + kn
      end
    end
 # Block parameter (&block)
    blk = @nd_block[params]
    if blk >= 0
      if @nd_type[blk] == "BlockParameterNode"
        if result != ""
          result = result + ","
        end
 # Anonymous `&` (Ruby 3.1+) — `def m(&); inner(&); end` —
 # produces a BlockParameterNode with no name. Synthesize a
 # stable internal name so the param gets a proper `lv_` slot
 # and downstream lookups (find_block_param_name,
 # @current_method_block_param) work the same as for `&block`.
        bn = @nd_name[blk]
        if bn == ""
          bn = "__anon_block"
        end
        result = result + bn
      end
    end
    result
  end

  def collect_rest_index(nid)
    params = @nd_parameters[nid]
    if params < 0
      return -1
    end
    rest = @nd_rest[params]
    if rest < 0 || @nd_type[rest] != "RestParameterNode"
      return -1
    end
    idx = 0
    idx = idx + parse_id_list(@nd_requireds[params]).length
    idx = idx + parse_id_list(@nd_optionals[params]).length
    idx = idx + parse_id_list(@nd_keywords[params]).length
    idx
  end

  def collect_ptypes_str(nid, ci)
    params = @nd_parameters[nid]
    if params < 0
      return ""
    end
    reqs = parse_id_list(@nd_requireds[params])
    opts = parse_id_list(@nd_optionals[params])
    kws = parse_id_list(@nd_keywords[params])
    result = ""
    k = 0
    while k < reqs.length
      if result != ""
        result = result + ","
      end
      result = result + "int"
      k = k + 1
    end
    k = 0
    while k < opts.length
      if result != ""
        result = result + ","
      end
 # Infer from default value
      def_id = @nd_expression[opts[k]]
      if def_id >= 0
        result = result + infer_type(def_id)
      else
        result = result + "int"
      end
      k = k + 1
    end
    k = 0
    while k < kws.length
      if result != ""
        result = result + ","
      end
 # Infer from default value
      def_id = @nd_expression[kws[k]]
      if def_id >= 0
        result = result + infer_type(def_id)
      else
        result = result + "int"
      end
      k = k + 1
    end
 # Rest param (splat)
    rest = @nd_rest[params]
    if rest >= 0
      if @nd_type[rest] == "RestParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "int_array"
      end
    end
 # Post-rest required params (`def f(*r, x, y)`).
    posts = parse_id_list(@nd_posts[params])
    k = 0
    while k < posts.length
      if @nd_type[posts[k]] == "RequiredParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "int"
      end
      k = k + 1
    end
 # Keyword rest (**kw). Spinel kwargs use symbol keys (matches
 # `f(a: 1)` keyword hash construction), so the slot is sym_poly_hash.
    kwrest = @nd_keyword_rest[params]
    if kwrest >= 0
      if @nd_type[kwrest] == "KeywordRestParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "sym_poly_hash"
      end
    end
 # Block parameter (&block)
    blk = @nd_block[params]
    if blk >= 0
      if @nd_type[blk] == "BlockParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "proc"
      end
    end
    result
  end

  def collect_defaults_str(nid)
    params = @nd_parameters[nid]
    if params < 0
      return ""
    end
    reqs = parse_id_list(@nd_requireds[params])
    opts = parse_id_list(@nd_optionals[params])
    kws = parse_id_list(@nd_keywords[params])
    result = ""
    k = 0
    while k < reqs.length
      if result != ""
        result = result + ","
      end
      result = result + "-1"
      k = k + 1
    end
    k = 0
    while k < opts.length
      if result != ""
        result = result + ","
      end
      def_id = @nd_expression[opts[k]]
      if def_id >= 0
        result = result + def_id.to_s
      else
        result = result + "-1"
      end
      k = k + 1
    end
    k = 0
    while k < kws.length
      if result != ""
        result = result + ","
      end
      def_id = @nd_expression[kws[k]]
      if def_id >= 0
        result = result + def_id.to_s
      else
        result = result + "-1"
      end
      k = k + 1
    end
 # Rest param
    rest = @nd_rest[params]
    if rest >= 0
      if @nd_type[rest] == "RestParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "-1"
      end
    end
 # Post-rest required params (`def f(*r, x, y)`) — no defaults.
    posts = parse_id_list(@nd_posts[params])
    k = 0
    while k < posts.length
      if @nd_type[posts[k]] == "RequiredParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "-1"
      end
      k = k + 1
    end
 # Keyword rest (**kw): no compile-time default, slot stays NULL
 # until the caller provides a hash.
    kwrest = @nd_keyword_rest[params]
    if kwrest >= 0
      if @nd_type[kwrest] == "KeywordRestParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "-1"
      end
    end
 # Block param
    blk = @nd_block[params]
    if blk >= 0
      if @nd_type[blk] == "BlockParameterNode"
        if result != ""
          result = result + ","
        end
        result = result + "-1"
      end
    end
    result
  end

  def append_cls_meth(ci, name, params, ptypes, ret, body_id, defaults)
    if @cls_meth_names[ci] != ""
      @cls_meth_names[ci] = @cls_meth_names[ci] + ";" + name
      @cls_meth_params[ci] = @cls_meth_params[ci] + "|" + params
      @cls_meth_params_version = @cls_meth_params_version + 1
      @cls_meth_ptypes[ci] = @cls_meth_ptypes[ci] + "|" + ptypes
      @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      @cls_meth_returns[ci] = @cls_meth_returns[ci] + ";" + ret
      @cls_meth_bodies[ci] = @cls_meth_bodies[ci] + ";" + body_id.to_s
      @cls_meth_defaults[ci] = @cls_meth_defaults[ci] + "|" + defaults
      @cls_meth_ptypes_empty[ci] = @cls_meth_ptypes_empty[ci] + "|"
    else
      @cls_meth_names[ci] = name
      @cls_meth_params[ci] = params
      @cls_meth_params_version = @cls_meth_params_version + 1
      @cls_meth_ptypes[ci] = ptypes
      @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      @cls_meth_returns[ci] = ret
      @cls_meth_bodies[ci] = body_id.to_s
      @cls_meth_defaults[ci] = defaults
      @cls_meth_ptypes_empty[ci] = ""
    end
 # Invalidate split caches keyed on @cls_meth_names / @cls_meth_returns.
    @cls_meth_idx_cache = {}
    @cls_meth_return_cache = {}
  end

  def append_cls_cmeth(ci, name, params, ptypes, ret, body_id, defaults = "")
    if @cls_cmeth_names[ci] != ""
      @cls_cmeth_names[ci] = @cls_cmeth_names[ci] + ";" + name
      @cls_cmeth_params[ci] = @cls_cmeth_params[ci] + "|" + params
      @cls_cmeth_params_version = @cls_cmeth_params_version + 1
      @cls_cmeth_ptypes[ci] = @cls_cmeth_ptypes[ci] + "|" + ptypes
      @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
      @cls_cmeth_returns[ci] = @cls_cmeth_returns[ci] + ";" + ret
      @cls_cmeth_bodies[ci] = @cls_cmeth_bodies[ci] + ";" + body_id.to_s
      @cls_cmeth_defaults[ci] = @cls_cmeth_defaults[ci] + "|" + defaults
    else
      @cls_cmeth_names[ci] = name
      @cls_cmeth_params[ci] = params
      @cls_cmeth_params_version = @cls_cmeth_params_version + 1
      @cls_cmeth_ptypes[ci] = ptypes
      @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
      @cls_cmeth_returns[ci] = ret
      @cls_cmeth_bodies[ci] = body_id.to_s
      @cls_cmeth_defaults[ci] = defaults
    end
  end

  def collect_attr_call(ci, nid)
    mname = @nd_name[nid]
    args_id = @nd_arguments[nid]
    if args_id < 0
      return
    end
    arg_ids = get_args(args_id)
    if mname == "attr_accessor"
      k = 0
      while k < arg_ids.length
        aname = @nd_content[arg_ids[k]]
        append_attr_reader(ci, aname)
        append_attr_writer(ci, aname)
        k = k + 1
      end
    end
    if mname == "attr_reader"
      k = 0
      while k < arg_ids.length
        aname = @nd_content[arg_ids[k]]
        append_attr_reader(ci, aname)
        k = k + 1
      end
    end
    if mname == "attr_writer"
      k = 0
      while k < arg_ids.length
        aname = @nd_content[arg_ids[k]]
        append_attr_writer(ci, aname)
        k = k + 1
      end
    end
  end

  def append_attr_reader(ci, name)
    if @cls_attr_readers[ci] != ""
      @cls_attr_readers[ci] = @cls_attr_readers[ci] + ";" + name
    else
      @cls_attr_readers[ci] = name
    end
  end

  def append_attr_writer(ci, name)
    if @cls_attr_writers[ci] != ""
      @cls_attr_writers[ci] = @cls_attr_writers[ci] + ";" + name
    else
      @cls_attr_writers[ci] = name
    end
  end

  def collect_ivars(ci)
 # Scan all methods for ivar writes
    meths = @cls_meth_bodies[ci].split(";")
    j = 0
    while j < meths.length
      bid = meths[j].to_i
      if bid >= 0
        scan_ivars(ci, bid)
      end
      j = j + 1
    end
 # Add ivars from attr_readers/writers that might not have explicit writes
    readers = @cls_attr_readers[ci].split(";")
    j = 0
    while j < readers.length
      iname = "@" + readers[j]
      if ivar_exists(ci, iname) == 0
        add_ivar(ci, iname, "int")
      end
      j = j + 1
    end
    writers = @cls_attr_writers[ci].split(";")
    j = 0
    while j < writers.length
      iname = "@" + writers[j]
      if ivar_exists(ci, iname) == 0
        add_ivar(ci, iname, "int")
      end
      j = j + 1
    end
  end

 # Direct, unconditional ivar type replacement. Bypasses the
 # widening logic in update_ivar_type — used when the caller has
 # already determined the new type is correct (e.g. promoting an
 # empty-hash default to a concrete hash type from a `[]=` write).
  def replace_ivar_type(ci, iname, new_type)
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    k = 0
    while k < names.length
      if names[k] == iname
        if k < types.length
          types[k] = new_type
          @cls_ivar_types[ci] = types.join(";")
          @cls_ivar_types_version = @cls_ivar_types_version + 1
        end
      end
      k = k + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0 && pi != ci
        replace_ivar_type(pi, iname, new_type)
      end
    end
  end

  def update_ivar_type(ci, iname, new_type)
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    k = 0
    while k < names.length
      if names[k] == iname
        if k < types.length
          old = types[k]
 # Stale unqualified obj-name normalization. collect_ivars
 # registers ivar types during Pass 1 (class enumeration), in
 # source order; at that point sibling classes that come later
 # in the file haven't been registered yet. A
 # `@cpu = CPU.new(@conf)` write inside the lexically-first
 # class then resolves "CPU" against an incomplete class
 # table, lands at unqualified "obj_CPU", and pins the slot.
 # When a subsequent pass — with all classes registered —
 # records the qualified "obj_<scope>_CPU" via scan_writer_calls
 # (whose lexical resolution does succeed), the stale and
 # qualified forms compare unequal and the heterogeneity
 # branch below widens to "poly". Detect the relationship
 # (old's bare name matches the trailing segment of new's
 # name AND old's bare name doesn't refer to any registered
 # class) and accept the qualified form as a refinement
 # rather than a disagreement.
          if is_obj_type(old) == 1 && is_obj_type(new_type) == 1 && old != new_type
            old_bare_uit = old[4, old.length - 4]
            new_bare_uit = new_type[4, new_type.length - 4]
            if find_class_idx(old_bare_uit) < 0 && find_class_idx(new_bare_uit) >= 0
              suffix_uit = "_" + old_bare_uit
              if new_bare_uit.length > suffix_uit.length && new_bare_uit[(new_bare_uit.length - suffix_uit.length), suffix_uit.length] == suffix_uit
                types[k] = new_type
                @cls_ivar_types[ci] = types.join(";")
                @cls_ivar_types_version = @cls_ivar_types_version + 1
                old = new_type
              end
            end
          end
 # Heterogeneous int/nil + obj → poly when the prior write
 # was a *definite* int/nil literal. The previous "int wins"
 # / "nil wins" overwrite silently cast the int payload to a
 # struct pointer, miscompiling `@x = 10; @x = Box.new` and
 # any subsequent obj method dispatch. Widen to poly so the
 # slot can carry either case at runtime; the dispatch path
 # then decides per cls_id at the call site.
 #
 # The definiteness gate avoids false widening when "int"
 # was just `infer_ivar_init_type`'s placeholder fallback for
 # a CallNode rhs that's later refined to an obj type by
 # the writer-scan / inference passes (e.g.
 # `@m = method(:foo)` initially scans as int and a
 # refinement promotes it to `obj_Method` — no heterogeneity
 # to widen for).
          if (old == "int" || old == "nil") && is_obj_type(new_type) == 1 && cls_ivar_definite_flag(ci, iname) == 1
            types[k] = "poly"
            @needs_rb_value = 1
            @cls_ivar_types[ci] = types.join(";")
            @cls_ivar_types_version = @cls_ivar_types_version + 1
          elsif old == "int" || old == "nil"
            types[k] = new_type
            @cls_ivar_types[ci] = types.join(";")
            @cls_ivar_types_version = @cls_ivar_types_version + 1
          elsif old != new_type && old != "poly"
            if is_array_type(old) == 1 && is_array_type(new_type) == 1
 # Don't widen a typed `<obj>_ptr_array` slot back to
 # poly when the disagreement is just `int_array`. That
 # disagreement comes from the `[nil] * N` empty default
 # — the writer-scan `[]=` / `<<` widening is more
 # specific and should win.
              if is_ptr_array_type(old) == 1 && new_type == "int_array"
                k = k + 1
                next
              end
              types[k] = "poly_array"
              @needs_rb_value = 1
              @cls_ivar_types[ci] = types.join(";")
              @cls_ivar_types_version = @cls_ivar_types_version + 1
              k = k + 1
              next
            end
 # Nullable pattern: nil + T → T?, T + nil → T?
            if new_type == "nil" && is_nullable_pointer_type(old) == 1
              if old[old.length - 1] != "?"
                types[k] = old + "?"
                @cls_ivar_types[ci] = types.join(";")
                @cls_ivar_types_version = @cls_ivar_types_version + 1
              end
            elsif old == "nil" && is_nullable_pointer_type(new_type) == 1
              types[k] = new_type + "?"
              @cls_ivar_types[ci] = types.join(";")
              @cls_ivar_types_version = @cls_ivar_types_version + 1
 # Same base, one nullable: keep / adopt the nullable form
 # rather than collapsing to poly. `@x = "hi"` (string) +
 # `@x = some_string_or_nil` (string?) is one storage shape
 # — `const char *` with NULL as the nil sentinel.
            elsif base_type(old) == base_type(new_type) && is_nullable_pointer_type(old) == 1
              if is_nullable_type(new_type) == 1 && is_nullable_type(old) == 0
                types[k] = new_type
                @cls_ivar_types[ci] = types.join(";")
                @cls_ivar_types_version = @cls_ivar_types_version + 1
              end
            else
              types[k] = "poly"
              @cls_ivar_types[ci] = types.join(";")
              @cls_ivar_types_version = @cls_ivar_types_version + 1
            end
          end
        end
      end
      k = k + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0 && pi != ci
        update_ivar_type(pi, iname, new_type)
      end
    end
  end

  def ivar_exists(ci, iname)
    names = @cls_ivar_names[ci].split(";")
    k = 0
    while k < names.length
      if names[k] == iname
        return 1
      end
      k = k + 1
    end
    0
  end

  def ivar_exists_in_ancestor(ci, iname)
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        if ivar_exists(pi, iname) == 1
          return 1
        end
        return ivar_exists_in_ancestor(pi, iname)
      end
    end
    0
  end

  def add_ivar(ci, iname, itype, definite = 0)
    if @cls_ivar_names[ci] != ""
      @cls_ivar_names[ci] = @cls_ivar_names[ci] + ";" + iname
      @cls_ivar_types[ci] = @cls_ivar_types[ci] + ";" + itype
      @cls_ivar_types_version = @cls_ivar_types_version + 1
      @cls_ivar_init_definite[ci] = @cls_ivar_init_definite[ci] + ";" + definite.to_s
      @cls_ivar_observed_types[ci] = @cls_ivar_observed_types[ci] + ";"
    else
      @cls_ivar_names[ci] = iname
      @cls_ivar_types[ci] = itype
      @cls_ivar_types_version = @cls_ivar_types_version + 1
      @cls_ivar_init_definite[ci] = definite.to_s
      @cls_ivar_observed_types[ci] = ""
    end
  end

 # Record `at` as a distinct observation for the (class, ivar)
 # slot if it's a concrete (non-fallback) type. "Concrete" means
 # either `at != "int"` and `at != "nil"` (those are infer_type's
 # catch-all placeholders), or the rhs is a definite-literal AST.
 # The dedup keeps the list short — a slot written with int from
 # twenty different `obj.length` call sites still records "int"
 # once.
  def record_ivar_observation(ci, iname, at, expr_id)
    is_concrete = 0
    if at != "int" && at != "nil"
      is_concrete = 1
    elsif expr_id >= 0 && is_definite_ivar_init(expr_id) == 1
      is_concrete = 1
    end
    if is_concrete == 0
      return
    end
    names = @cls_ivar_names[ci].split(";")
 # Pad obs to names.length: split(";", -1) preserves trailing "" but
 # an entirely-empty storage gives a 0-length array. Walk by index
 # and treat missing entries as "".
    obs_str = @cls_ivar_observed_types[ci]
    k = 0
    while k < names.length
      if names[k] == iname
 # Build an array of slots, one per ivar, defaulting empty.
        slots = "".split(",")
        ix = 0
        while ix < names.length
          slots.push("")
          ix = ix + 1
        end
        if obs_str != ""
          parts = obs_str.split(";", -1)
          px = 0
          while px < parts.length && px < slots.length
            slots[px] = parts[px]
            px = px + 1
          end
        end
        existing = slots[k].split(",")
        ex = 0
        while ex < existing.length
          if existing[ex] == at
            return
          end
          ex = ex + 1
        end
        if slots[k] == ""
          slots[k] = at
        else
          slots[k] = slots[k] + "," + at
        end
        @cls_ivar_observed_types[ci] = slots.join(";")
        return
      end
      k = k + 1
    end
  end

 # Was the AST expression a definite-literal that
 # `infer_ivar_init_type` types unambiguously? Used by scan_ivars
 # to decide when to widen a multi-write ivar slot to poly.
 #
 # Also accepts typed-hash[key] reads — when the receiver is a
 # hash whose value type is statically known (str_int_hash /
 # sym_int_hash / *_str_hash / int_str_hash), the call always
 # produces that value type. Recording this writer's "int" (or
 # "string") as a *concrete* observation in
 # finalize_ivar_heterogeneity lets a sibling-method writer of a
 # different concrete type trigger the poly widen, instead of
 # silently narrowing the slot to whichever sibling won the type
 # race.
  def is_definite_ivar_init(nid)
    if nid < 0
      return 0
    end
    t = @nd_type[nid]
    if t == "IntegerNode" || t == "FloatNode" || t == "StringNode"
      return 1
    end
    if t == "SymbolNode" || t == "TrueNode" || t == "FalseNode"
      return 1
    end
    if t == "CallNode" && @nd_name[nid] == "[]"
      recv = @nd_receiver[nid]
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_int_hash" || rt == "sym_int_hash" || rt == "str_str_hash" || rt == "sym_str_hash" || rt == "int_str_hash"
          return 1
        end
      end
    end
 # A ternary whose branches are themselves definite is itself
 # definite. Lets the multi-write poly-widening rule still fire
 # when a later concrete write disagrees with an IfNode-typed
 # slot.
    if t == "IfNode"
      then_d = 0
      body = @nd_body[nid]
      if body >= 0
        ts = get_stmts(body)
        if ts.length > 0
          then_d = is_definite_ivar_init(ts.last)
        end
      end
      else_d = 0
      sub = @nd_subsequent[nid]
      if sub >= 0
        if @nd_type[sub] == "ElseNode"
          eb = @nd_body[sub]
          if eb >= 0
            es = get_stmts(eb)
            if es.length > 0
              else_d = is_definite_ivar_init(es.last)
            end
          end
        else
          else_d = is_definite_ivar_init(sub)
        end
      end
      if then_d == 1 && else_d == 1
        return 1
      end
    end
    0
  end

  def cls_ivar_definite_flag(ci, iname)
    names = @cls_ivar_names[ci].split(";")
    flags = @cls_ivar_init_definite[ci].split(";")
    k = 0
    while k < names.length
      if names[k] == iname
        if k < flags.length
          return flags[k].to_i
        end
        return 0
      end
      k = k + 1
    end
    0
  end

  def scan_ivars(ci, nid)
    if nid < 0
      return
    end
    if @nd_type[nid] == "InstanceVariableWriteNode"
      iname = @nd_name[nid]
      expr_first = @nd_expression[nid]
      if ivar_exists(ci, iname) == 0 && ivar_exists_in_ancestor(ci, iname) == 1
 # Slot is on a parent class — route the write through
 # update_ivar_type so the parent's type widens consistently.
 # Without this, the child re-adds the ivar to its own table
 # with the new write's type, while the parent widens to poly
 # via update_ivar_type's recurse, leaving the two tables
 # disagreeing — and downstream cls_ivar_type lookups on the
 # child see only its (narrower) entry, missing the widening.
        vtype = infer_ivar_init_type(expr_first)
        if vtype != "int" && vtype != "nil"
          update_ivar_type(ci, iname, vtype)
        end
      elsif ivar_exists(ci, iname) == 0
        vtype = infer_ivar_init_type(expr_first)
        add_ivar(ci, iname, vtype, is_definite_ivar_init(expr_first))
      else
 # When the new write is a definite-literal AND the ivar's
 # first scanned write was also a definite-literal AND the
 # types disagree, widen to poly. The dual definite-literal
 # gate avoids false widening on `infer_ivar_init_type`'s
 # "int" fallback for non-recognized expressions (CallNodes,
 # LocalVariableReadNodes) — spinel_codegen's own ivars
 # (e.g. `@current_method_name = "x" + n.to_s`) would
 # otherwise widen and break the bootstrap.
        expr = @nd_expression[nid]
        if expr >= 0
          if @nd_type[expr] != "NilNode"
            vtype = infer_ivar_init_type(expr)
            cur = cls_ivar_type(ci, iname)
            new_def = is_definite_ivar_init(expr)
            cur_def = cls_ivar_definite_flag(ci, iname)
            if new_def == 1 && cur_def == 1 && cur != vtype && cur != "poly"
              replace_ivar_type(ci, iname, "poly")
              @needs_rb_value = 1
            elsif vtype != "int"
              update_ivar_type(ci, iname, vtype)
            end
          end
        end
      end
    end
    if @nd_type[nid] == "InstanceVariableOperatorWriteNode"
      iname = @nd_name[nid]
      if ivar_exists(ci, iname) == 0
        add_ivar(ci, iname, "int")
      end
    end
 # `@x ||= expr` / `@x &&= expr`: register the slot when first
 # encountered. The rhs type seeds the ivar; without registration
 # the struct comes out without the slot and any subsequent read
 # ground out at the int default.
    if @nd_type[nid] == "InstanceVariableOrWriteNode" || @nd_type[nid] == "InstanceVariableAndWriteNode"
      iname = @nd_name[nid]
      expr_first = @nd_expression[nid]
      if ivar_exists(ci, iname) == 0 && ivar_exists_in_ancestor(ci, iname) == 1
        vtype = infer_ivar_init_type(expr_first)
        if vtype != "int" && vtype != "nil"
          update_ivar_type(ci, iname, vtype)
        end
      elsif ivar_exists(ci, iname) == 0
        vtype = infer_ivar_init_type(expr_first)
 # ||= reads @x first; pre-register a nil observation so the
 # `nil + T → T?` rule fires when a later writer-scan widens.
        add_ivar(ci, iname, "nil", 0)
        if vtype != "int" && vtype != "nil"
          update_ivar_type(ci, iname, vtype)
        end
      else
        expr = @nd_expression[nid]
        if expr >= 0 && @nd_type[expr] != "NilNode"
          vtype = infer_ivar_init_type(expr)
          if vtype != "int" && vtype != "nil"
            update_ivar_type(ci, iname, vtype)
          end
        end
      end
    end
 # Multi-write to ivars: `@a, @b = expr1, expr2` (or `[expr1, expr2]`).
 # Without this branch, ivars assigned only via destructuring never get
 # registered and the struct comes out missing them.
    if @nd_type[nid] == "MultiWriteNode"
      targets = parse_id_list(@nd_targets[nid])
      val_id = @nd_expression[nid]
      ti = 0
      while ti < targets.length
        tid = targets[ti]
        if @nd_type[tid] == "InstanceVariableTargetNode"
          iname = @nd_name[tid]
          vtype = scan_ivars_multi_target_type(val_id, ti)
          if ivar_exists(ci, iname) == 0
            add_ivar(ci, iname, vtype)
          else
            if vtype != "int" && vtype != "nil"
              update_ivar_type(ci, iname, vtype)
            end
          end
        end
        ti = ti + 1
      end
    end
 # Recurse into children
    scan_ivars_children(ci, nid)
  end

 # i-th element type when scanning ivar destructuring writes. Mirrors
 # multi_write_target_type but also handles array literals on the RHS
 # (ivar collection runs before tuple inference, so `@a, @b = 1, 2`
 # gets its types from positional ArrayNode elements rather than a
 # tuple return).
  def scan_ivars_multi_target_type(val_id, ti)
    if val_id < 0
      return "int"
    end
    if @nd_type[val_id] == "ArrayNode"
      elems = parse_id_list(@nd_elements[val_id])
      if ti < elems.length
        return infer_ivar_init_type(elems[ti])
      end
      return "int"
    end
    rt = infer_type(val_id)
    if is_tuple_type(rt) == 1
      return tuple_elem_type_at(rt, ti)
    end
 # `A, B = arr.map { block }` — each target is one element of the
 # mapped array, so its type is the block's return type. Spinel
 # collapses array-of-array to a placeholder (int_array) at the
 # outer infer_type level, so we have to peek through the call
 # node directly to recover the element type. `collect` is the
 # standard alias for `map`; treat both. Trust the block return
 # even when it's `int` — `rt` (the outer call's inferred type)
 # is unreliable for nested-array shapes per the comment above,
 # so the block return is more authoritative.
    if @nd_type[val_id] == "CallNode" && (@nd_name[val_id] == "map" || @nd_name[val_id] == "collect")
      blk = @nd_block[val_id]
      if blk >= 0
        bbody = @nd_body[blk]
        if bbody >= 0
          bbs = get_stmts(bbody)
          if bbs.length > 0
            bret = infer_type(bbs.last)
            if bret != "" && bret != "void"
              return bret
            end
          end
        end
      end
    end
 # Array-typed RHS (e.g. `A, B = [1, 6].map { ... }`): each target
 # gets one element of the recv array. Use the recv's element type.
    if rt == "int_array" || rt == "sym_array"
      return "int"
    end
    if is_ptr_array_type(rt) == 1
      return ptr_array_elem_type(rt)
    end
    if rt == "str_array"
      return "string"
    end
    if rt == "float_array"
      return "float"
    end
    "int"
  end

  def scan_ivars_children(ci, nid)
    if @nd_body[nid] >= 0
      scan_ivars(ci, @nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_ivars(ci, stmts[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_ivars(ci, @nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_ivars(ci, @nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_ivars(ci, @nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_ivars(ci, @nd_else_clause[nid])
    end
    if @nd_receiver[nid] >= 0
      scan_ivars(ci, @nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      scan_ivars(ci, @nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_ivars(ci, args[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_ivars(ci, conds[k])
      k = k + 1
    end
    if @nd_left[nid] >= 0
      scan_ivars(ci, @nd_left[nid])
    end
    if @nd_right[nid] >= 0
      scan_ivars(ci, @nd_right[nid])
    end
    if @nd_block[nid] >= 0
      scan_ivars(ci, @nd_block[nid])
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_ivars(ci, elems[k])
      k = k + 1
    end
    if @nd_rescue_clause[nid] >= 0
      scan_ivars(ci, @nd_rescue_clause[nid])
    end
    if @nd_ensure_clause[nid] >= 0
      scan_ivars(ci, @nd_ensure_clause[nid])
    end
  end

  def infer_ivar_init_type(nid)
    if nid < 0
      return "int"
    end
    t = @nd_type[nid]
    if t == "NilNode"
      return "nil"
    end
    if t == "IntegerNode"
      return "int"
    end
    if t == "FloatNode"
      return "float"
    end
    if t == "StringNode"
      return "string"
    end
    if t == "SymbolNode"
      return "symbol"
    end
    if t == "TrueNode"
      return "bool"
    end
    if t == "FalseNode"
      return "bool"
    end
    if t == "ArrayNode"
      return infer_array_elem_type(nid)
    end
    if t == "HashNode"
      if is_int_array_lowered_hash(nid) == 1
        return infer_int_keyed_hash_as_array_type(nid)
      end
      return infer_hash_val_type(nid)
    end
    if t == "CallNode"
      mname = @nd_name[nid]
      if mname == "to_a"
        return "int_array"
      end
      if mname == "split"
        return "str_array"
      end
      if mname == "new"
        r = @nd_receiver[nid]
        if r >= 0
          rname = constructor_class_name(r)
          if rname != ""
            if rname == "Array"
 # Block form `Array.new(n) { ... }` — infer container
 # from the block tail (matches compile_constructor_expr).
 # When the block tail's type can't be resolved at this
 # pre-compile pass (a LocalVariableReadNode whose write
 # is in the same block, e.g. `_a = [0]; _a.clear; _a`,
 # hasn't been declare_var'd yet — find_var_type returns
 # ""), fall through to the placeholder "int" path so
 # the later compile pass's update_ivar_type can widen
 # int → concrete-container without going through the
 # array+array → poly_array widening.
              blk_an2 = @nd_block[nid]
              if blk_an2 >= 0
                body_an2 = @nd_body[blk_an2]
                if body_an2 >= 0
                  stmts_an2 = get_stmts(body_an2)
                  if stmts_an2.length > 0
 # `[]` / `[].dup` block tail → poly_array (see
 # infer_constructor_type for rationale).
                    if is_empty_array_or_dup(stmts_an2.last) == 1
                      @needs_rb_value = 1
                      @needs_gc = 1
                      return "poly_array"
                    end
                    bret2 = infer_type(stmts_an2.last)
                    if bret2 == "string"
                      return "str_array"
                    end
                    if bret2 == "float"
                      return "float_array"
                    end
                    if bret2 == "symbol"
                      return "sym_array"
                    end
                    if bret2 == "poly"
                      @needs_rb_value = 1
                      return "poly_array"
                    end
                    if is_ptr_array_type(bret2) == 1 || bret2 == "poly_array"
                      @needs_rb_value = 1
                      return "poly_array"
                    end
                    if bret2 == "int_array" || bret2 == "float_array" || bret2 == "str_array" || bret2 == "sym_array"
                      @needs_gc = 1
                      return bret2 + "_ptr_array"
                    end
 # bret2 == "int" or "" — leave the slot untyped so
 # the later compile pass can refine without
 # triggering update_ivar_type's array-widening.
                    return "int"
                  end
                end
              end
 # Check fill value type for Array.new(n, val).
 # Pointer-type fills must produce a typed PtrArray; falling
 # through to int_array would leave the elements unscanned by GC.
              args_id = @nd_arguments[nid]
              if args_id >= 0
                aargs = get_args(args_id)
                if aargs.length >= 2
                  vt = infer_type(aargs[1])
                  if vt == "float"
                    return "float_array"
                  end
                  if vt == "string"
                    return "str_array"
                  end
                  if vt == "symbol"
                    return "sym_array"
                  end
                  if vt == "poly"
                    @needs_rb_value = 1
                    return "poly_array"
                  end
                  if type_is_pointer(vt) == 1
                    @needs_gc = 1
                    return vt + "_ptr_array"
                  end
                end
              end
              return "int_array"
            end
            if rname == "Hash"
              return "str_int_hash"
            end
            if rname == "StringIO"
              return "stringio"
            end
            if rname == "Fiber"
              return "fiber"
            end
            return "obj_" + rname
          end
        end
      end
    end
    if t == "LocalVariableReadNode"
      vt = find_var_type(@nd_name[nid])
      if vt != ""
        return vt
      end
    end
 # Ternary / if-as-expression RHS: recurse into both branches'
 # last statements and unify with strict comparison. Cannot
 # delegate to unify_return_type — that helper has an "int is
 # default/unresolved" escape hatch (`int + T → T`) which is
 # correct for method-return inference but the wrong rule for
 # ivar-write inference: mixing concrete int and concrete
 # non-int in a ternary needs to widen to poly here, not
 # silently pick the non-int side. nil branches still defer to
 # the other type so existing nullable widening
 # (string + nil → string?) flows through update_ivar_type.
    if t == "IfNode"
      then_t = "nil"
      body = @nd_body[nid]
      if body >= 0
        ts = get_stmts(body)
        if ts.length > 0
          then_t = infer_ivar_init_type(ts.last)
        end
      end
      else_t = "nil"
      sub = @nd_subsequent[nid]
      if sub >= 0
        if @nd_type[sub] == "ElseNode"
          eb = @nd_body[sub]
          if eb >= 0
            es = get_stmts(eb)
            if es.length > 0
              else_t = infer_ivar_init_type(es.last)
            end
          end
        else
          else_t = infer_ivar_init_type(sub)
        end
      end
      if then_t == else_t
        return then_t
      end
 # Nullable widening (`T + nil → T?`, `nil + T → T?`) — match
 # unify_return_type's behavior locally so a later `infer_type`-
 # based pass (spinel_codegen.rb:7045) computing the same "T?"
 # doesn't widen us to poly via update_ivar_type's missing
 # T + T? → T? handler.
      if then_t == "nil"
        if is_nullable_pointer_type(else_t) == 1 && is_nullable_type(else_t) == 0
          return else_t + "?"
        end
        return else_t
      end
      if else_t == "nil"
        if is_nullable_pointer_type(then_t) == 1 && is_nullable_type(then_t) == 0
          return then_t + "?"
        end
        return then_t
      end
      return "poly"
    end
    "int"
  end

  def collect_toplevel_method(nid)
    mname = @nd_name[nid]
    body_id = @nd_body[nid]
    params_str = collect_params_str(nid)
    ptypes_str = ""
    defaults_str = collect_defaults_str(nid)

 # Infer param types from defaults
    params = @nd_parameters[nid]
    if params >= 0
      reqs = parse_id_list(@nd_requireds[params])
      opts = parse_id_list(@nd_optionals[params])
      kws = parse_id_list(@nd_keywords[params])
      k = 0
      while k < reqs.length
        if ptypes_str != ""
          ptypes_str = ptypes_str + ","
        end
        ptypes_str = ptypes_str + "int"
        k = k + 1
      end
      k = 0
      while k < opts.length
        if ptypes_str != ""
          ptypes_str = ptypes_str + ","
        end
        def_id = @nd_expression[opts[k]]
        if def_id >= 0
          ptypes_str = ptypes_str + infer_type(def_id)
        else
          ptypes_str = ptypes_str + "int"
        end
        k = k + 1
      end
      k = 0
      while k < kws.length
        if ptypes_str != ""
          ptypes_str = ptypes_str + ","
        end
        def_id = @nd_expression[kws[k]]
        if def_id >= 0
          ptypes_str = ptypes_str + infer_type(def_id)
        else
          ptypes_str = ptypes_str + "int"
        end
        k = k + 1
      end
 # Rest param (splat)
      rest = @nd_rest[params]
      if rest >= 0
        if @nd_type[rest] == "RestParameterNode"
          if ptypes_str != ""
            ptypes_str = ptypes_str + ","
          end
          ptypes_str = ptypes_str + "int_array"
        end
      end
 # Block param (&block)
      blk = @nd_block[params]
      if blk >= 0
        if @nd_type[blk] == "BlockParameterNode"
          if ptypes_str != ""
            ptypes_str = ptypes_str + ","
          end
          ptypes_str = ptypes_str + "proc"
        end
      end
    end

    @meth_names.push(mname)
    @meth_param_names.push(params_str)
    @meth_param_types.push(ptypes_str)
    @meth_param_empty.push("")
    @meth_return_types.push("int")
    @meth_body_ids.push(body_id)
    @meth_has_defaults.push(defaults_str)
    @meth_has_yield.push(body_has_yield(body_id))
    @meth_rest_index.push(collect_rest_index(nid))
    0
  end

  def collect_define_method(nid)
 # define_method(:name) { |args| body }
    args_id = @nd_arguments[nid]
    if args_id < 0
      return
    end
    arg_ids = get_args(args_id)
    if arg_ids.length < 1
      return
    end
    mname = @nd_content[arg_ids[0]]
    if mname == ""
      mname = @nd_name[arg_ids[0]]
    end
    blk = @nd_block[nid]
    if blk < 0
      return
    end
    body_id = @nd_body[blk]
 # Collect block params
    params_str = ""
    ptypes_str = ""
    bp = @nd_parameters[blk]
    if bp >= 0
      inner = @nd_parameters[bp]
      if inner >= 0
        reqs = parse_id_list(@nd_requireds[inner])
        k = 0
        while k < reqs.length
          if params_str != ""
            params_str = params_str + ","
            ptypes_str = ptypes_str + ","
          end
          params_str = params_str + @nd_name[reqs[k]]
          ptypes_str = ptypes_str + "int"
          k = k + 1
        end
      end
    end
    @meth_names.push(mname)
    @meth_param_names.push(params_str)
    @meth_param_types.push(ptypes_str)
    @meth_param_empty.push("")
    @meth_return_types.push("int")
    @meth_body_ids.push(body_id)
    @meth_has_defaults.push("")
    @meth_has_yield.push(0)
    @meth_rest_index.push(-1)
  end

  def collect_module(nid)
    collect_module_with_prefix(nid, "")
  end

  def collect_module_with_prefix(nid, module_prefix)
    mname = ""
    cp = @nd_constant_path[nid]
    if cp >= 0
      mname = const_ref_flat_name(cp)
      if module_prefix != "" && const_ref_is_relative(cp) == 1
        mname = module_prefix + "_" + mname
      end
    end
    body = @nd_body[nid]
 # Store module info for include
    @module_names.push(mname)
    @module_body_ids.push(body)
    if body < 0
      return
    end
    body_stmts = get_stmts(body)

 # Match top-level collection order: modules first, then classes.
    body_stmts.each { |sid|
      if @nd_type[sid] == "ModuleNode"
        collect_module_with_prefix(sid, mname)
      end
    }
    body_stmts.each { |sid|
      if @nd_type[sid] == "ClassNode"
        collect_class_with_prefix(sid, mname)
      end
    }

    in_module_function = 0
    body_stmts.each { |sid|
      if @nd_type[sid] == "ConstantWriteNode"
        collect_scoped_constant(mname, sid)
      end
 # Module-body `A, B = ...` multi-write to constants.
      if @nd_type[sid] == "MultiWriteNode"
        collect_scoped_multi_const(mname, sid)
      end
 # `module_function` (no args) flips subsequent `def name`
 # into class-method dispatch, parallel to `def self.name`.
 # Spinel only needs the class-method shape; the full Ruby
 # semantics also installs the methods as private instance
 # methods (for include-mixin), unmodeled here.
      if @nd_type[sid] == "CallNode" && @nd_receiver[sid] < 0 && @nd_name[sid] == "module_function"
        args_id_mf = @nd_arguments[sid]
        if args_id_mf < 0 || get_args(args_id_mf).length == 0
          in_module_function = 1
        end
      end
 # Collect module class methods (def self.xxx) as top-level functions
      if @nd_type[sid] == "DefNode"
        is_self_def = 0
        if @nd_receiver[sid] >= 0 && @nd_type[@nd_receiver[sid]] == "SelfNode"
          is_self_def = 1
        end
        if is_self_def == 1 || (in_module_function == 1 && @nd_receiver[sid] < 0)
          dmname = @nd_name[sid]
 # Create as top-level method with module prefix for dispatch
          @meth_names.push(mname + "_cls_" + dmname)
          @meth_param_names.push(collect_params_str(sid))
          @meth_param_types.push(collect_ptypes_str(sid, -1))
          @meth_param_empty.push("")
          @meth_return_types.push("int")
          @meth_body_ids.push(@nd_body[sid])
          @meth_has_yield.push(0)
 # Capture default-arg expressions so call sites that
 # omit trailing args get them filled in by
 # compile_call_args_with_defaults — the actual default
 # value is required (not just literal 0) for string
 # default args etc.
          @meth_has_defaults.push(collect_defaults_str(sid))
          @meth_rest_index.push(collect_rest_index(sid))
        end
      end
 # Collect module-level ivar writes as global statics
      if @nd_type[sid] == "InstanceVariableWriteNode"
        iname = @nd_name[sid]
        cname2 = mname + "_" + iname[1, iname.length - 1]
        expr_id = @nd_expression[sid]
        ct = "int"
        if expr_id >= 0
          old_scope = @current_lexical_scope
          @current_lexical_scope = mname
          ct = infer_type(expr_id)
          @current_lexical_scope = old_scope
        end
        @const_names.push(cname2)
        @const_types.push(ct)
        @const_expr_ids.push(expr_id)
        @const_scope_names.push(mname)
      end
 # FFI DSL: ffi_lib, ffi_cflags, ffi_func, ffi_const, ffi_buffer,
 # ffi_read_u32, ffi_read_i32, ffi_read_ptr. Bare CallNode with no
 # explicit receiver whose name starts with "ffi_".
      if @nd_type[sid] == "CallNode" && @nd_receiver[sid] < 0
        cname_ffi = @nd_name[sid]
        if cname_ffi.length >= 4 && cname_ffi[0, 4] == "ffi_"
          scan_ffi_decl(mname, sid)
        end
      end
 # `class << self; attr_accessor :foo; end` — register `foo` as a
 # module-level singleton accessor. Stage 1 of the
 # accessor's value is resolved later via the constant-fold pass
 # (rewrite_module_singleton_accessors) once we've seen all writes.
      if @nd_type[sid] == "SingletonClassNode"
        sbody = @nd_body[sid]
        if sbody >= 0
          sbody_stmts = get_stmts(sbody)
          sbody_stmts.each { |sst|
            if @nd_type[sst] == "CallNode" && @nd_name[sst] == "attr_accessor"
              args_id = @nd_arguments[sst]
              if args_id >= 0
                arg_ids = get_args(args_id)
                arg_ids.each { |aid|
                  if @nd_type[aid] == "SymbolNode"
                    accessor = @nd_content[aid]
                    @module_acc_keys.push(mname + "." + accessor)
                    @module_acc_consts.push("")
                  end
                }
              end
            end
          }
        end
      end
    }
  end

 # Refine module-ivar types for empty-container literals
 # (`@h = {}` / `@arr = []`) by walking the module's class-method
 # bodies for `@h[k] = v` / `@h << v` style writes and picking the
 # most specific hash / array shape from the observed key + value
 # types. Otherwise the empty-hash default `str_int_hash` stays
 # frozen and a sym-key / string-value write site emits a typed-
 # mismatch sp_StrIntHash_set call.
 #
 # Called from generate_code after the param-type inference loop so
 # `infer_type(args[i])` resolves params to their call-site-widened
 # types — refining at module-collect time would see every param as
 # the placeholder "int".
  def refine_all_module_ivar_types
    mi = 0
    while mi < @module_names.length
      mname_m = @module_names[mi]
      body_m = @module_body_ids[mi]
      if mname_m != "" && body_m >= 0
        refine_module_ivar_types(mname_m, get_stmts(body_m))
      end
      mi = mi + 1
    end
  end

  def refine_module_ivar_types(mname, body_stmts)
    body_stmts.each { |sid|
 # Two shapes both refine the same `@const_types` slot:
 # @slots = {} (InstanceVariableWriteNode in a module body —
 # spinel hoists module ivars to file-scope
 # constants named `<Mod>_<iname>`)
 # LOG = [] (ConstantWriteNode — registered directly as
 # `<Mod>_<cname>`)
 # The bare-constant shape needs the same refinement so
 # `LOG << some_hash` writes flip the constant off the
 # empty-array `int_array` default.
      iv_write = @nd_type[sid] == "InstanceVariableWriteNode"
      const_write = @nd_type[sid] == "ConstantWriteNode"
      next unless iv_write || const_write
      if iv_write
        iname = @nd_name[sid]
        cname2 = mname + "_" + iname[1, iname.length - 1]
      else
        iname = @nd_name[sid]
        cname2 = mname + "_" + iname
      end
      ci = find_const_idx(cname2)
      next if ci < 0
      cur = @const_types[ci]
 # Refine when the recorded type is the empty-hash default
 # (str_int_hash), empty-array default (int_array), or nil
 # (from `@ivar = nil` lazy-init).
      next unless cur == "str_int_hash" || cur == "int_array" || cur == "nil"
      expr_id = @nd_expression[sid]
      next unless expr_id >= 0
      next unless (cur == "str_int_hash" && is_empty_hash_literal(expr_id) == 1) ||
                  (cur == "int_array" && is_empty_array_literal(expr_id) == 1) ||
                  (cur == "nil" && @nd_type[expr_id] == "NilNode")
 # Walk all class methods in the module looking for writes to
 # this ivar.
      key_t_set = "".split(",")
      val_t_set = "".split(",")
      direct_t_set = "".split(",")
      body_stmts.each { |sid2|
        next unless @nd_type[sid2] == "DefNode"
        bid = @nd_body[sid2]
        next unless bid >= 0
 # The def's params live in the top-level method table under the
 # synthetic `<mname>_cls_<dmname>` name. Pull the
 # call-site-widened types from there and declare them in a
 # temporary scope so `infer_type(LocalVariableReadNode)` for
 # those params resolves correctly during the walk.
        synth_name = mname + "_cls_" + @nd_name[sid2]
        mi3 = find_method_idx(synth_name)
        push_scope
        if mi3 >= 0
          pnames3 = @meth_param_names[mi3].split(",")
          ptypes3 = @meth_param_types[mi3].split(",")
          k3 = 0
          while k3 < pnames3.length
            pt3 = "int"
            if k3 < ptypes3.length
              pt3 = ptypes3[k3]
            end
            declare_var(pnames3[k3], pt3)
            k3 = k3 + 1
          end
 # Declare body locals too: a `LOG << entry` write where
 # `entry = { ... }` was assigned earlier in the body needs
 # the LocalVariableReadNode read on `entry` to resolve
 # against the local's actual type, not infer_type's "int"
 # default. Without this val_t_set ends up `["int"]` and
 # pick_array_class returns the same int_array we started
 # with. The ivar shape (`@slots[k] = v` where k/v are
 # params) only needs the params declared above.
          local_names = "".split(",")
          local_types = "".split(",")
          scan_locals(bid, local_names, local_types, pnames3)
          lk = 0
          while lk < local_names.length
            declare_var(local_names[lk], local_types[lk])
            lk = lk + 1
          end
        end
        saved_meth = @current_method_name
        @current_method_name = synth_name
        if iv_write
          scan_module_ivar_writes(bid, iname, key_t_set, val_t_set)
          if cur == "nil"
            scan_module_ivar_direct_writes(bid, iname, direct_t_set)
          end
        else
          scan_module_const_writes(bid, iname, key_t_set, val_t_set)
        end
        @current_method_name = saved_meth
        pop_scope
      }
      if cur == "str_int_hash"
 # Pick the hash class that fits the observed (key, value) types.
        new_t = pick_hash_class(key_t_set, val_t_set)
        if new_t != "" && new_t != cur
          @const_types[ci] = new_t
 # Set the @needs_* flag so emit_sym_runtime / future hash
 # runtime emitters declare the matching struct + helpers
 # before this const's `cst_<name>` declaration is rendered.
          mark_hash_needs(new_t)
        end
      elsif cur == "int_array"
        new_t = pick_array_class(val_t_set)
        if new_t != "" && new_t != cur
          @const_types[ci] = new_t
          mark_array_needs(new_t)
        end
      elsif cur == "nil"
 # `@ivar = nil` at module body → slot defaults to "nil" (mrb_int).
 # Pick the widest non-nil write type observed in any cmeth body so
 # `@ivar = <ptr>` later in `def self.configure` widens the slot.
        new_t = pick_module_ivar_nil_widen(direct_t_set)
        if new_t != "" && new_t != cur
          @const_types[ci] = new_t
        end
      end
    }
  end

 # Walk `nid`'s subtree collecting types of every InstanceVariableWriteNode
 # whose name == iname. Used to widen module-level `@ivar = nil` slots
 # when a later cmeth assigns a non-nil value (e.g. FFI pointer read).
  def scan_module_ivar_direct_writes(nid, iname, type_set)
    if nid < 0
      return
    end
    if @nd_type[nid] == "InstanceVariableWriteNode" && @nd_name[nid] == iname
      expr_id = @nd_expression[nid]
      if expr_id >= 0
        wt = infer_type(expr_id)
        if wt != "" && wt != "nil"
          uniq_push(type_set, wt)
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_module_ivar_direct_writes(cs[k], iname, type_set)
      k = k + 1
    end
  end

  def pick_module_ivar_nil_widen(type_set)
    return "" if type_set.length == 0
    if type_set.length == 1
      return type_set[0]
    end
    "poly"
  end

  def mark_hash_needs(t)
    if t == "sym_int_hash"
      @needs_sym_int_hash = 1
    elsif t == "sym_str_hash"
      @needs_sym_str_hash = 1
    elsif t == "str_str_hash"
      @needs_str_str_hash = 1
    elsif t == "int_str_hash"
      @needs_int_str_hash = 1
    elsif t == "sym_poly_hash"
      @needs_sym_poly_hash = 1
      @needs_rb_value = 1
    elsif t == "str_poly_hash"
      @needs_str_poly_hash = 1
      @needs_rb_value = 1
    elsif t == "poly_poly_hash"
      @needs_poly_poly_hash = 1
      @needs_rb_value = 1
    end
  end

  def mark_array_needs(t)
    if t == "float_array"
      @needs_float_array = 1
    elsif t == "str_array"
      @needs_str_array = 1
    elsif t == "poly_array"
      @needs_rb_value = 1
    end
  end

 # Walk `nid` accumulating distinct key + value types observed at
 # `LOG << v` / `LOG[k] = v` / `LOG.push(v)` writes against a
 # module-level constant `LOG`. Parallel to
 # scan_module_ivar_writes for the ivar / hoisted shape; this
 # variant tracks ConstantReadNode recv (resolving via the
 # enclosing module's lexical scope) and feeds the same set the
 # refinement uses to pick the typed-array / typed-hash shape.
  def scan_module_const_writes(nid, cname, key_t_set, val_t_set)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      mname_call = @nd_name[nid]
      recv = @nd_receiver[nid]
      if recv >= 0 && @nd_type[recv] == "ConstantReadNode" && @nd_name[recv] == cname
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if mname_call == "[]=" && arg_ids.length >= 2
            uniq_push(key_t_set, infer_type(arg_ids[0]))
            uniq_push(val_t_set, infer_type(arg_ids[1]))
          elsif (mname_call == "<<" || mname_call == "push") && arg_ids.length >= 1
            uniq_push(val_t_set, infer_type(arg_ids[0]))
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_module_const_writes(cs[k], cname, key_t_set, val_t_set)
      k = k + 1
    end
  end

  def scan_module_ivar_writes(nid, iname, key_t_set, val_t_set)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "CallNode"
      mname_call = @nd_name[nid]
      recv = @nd_receiver[nid]
      if recv >= 0 && @nd_type[recv] == "InstanceVariableReadNode" && @nd_name[recv] == iname
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if mname_call == "[]=" && arg_ids.length >= 2
            kt = infer_type(arg_ids[0])
            vt = infer_type(arg_ids[1])
            uniq_push(key_t_set, kt)
            uniq_push(val_t_set, vt)
          elsif (mname_call == "<<" || mname_call == "push") && arg_ids.length >= 1
            vt = infer_type(arg_ids[0])
            uniq_push(val_t_set, vt)
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_module_ivar_writes(cs[k], iname, key_t_set, val_t_set)
      k = k + 1
    end
  end

  def uniq_push(set, val)
    found = 0
    k = 0
    while k < set.length
      if set[k] == val
        found = 1
        break
      end
      k = k + 1
    end
    if found == 0
      set.push(val)
    end
  end

  def pick_hash_class(key_t_set, val_t_set)
    return "" if key_t_set.length == 0 || val_t_set.length == 0
 # Heterogeneous keys → poly hash; mixed values → poly hash too.
    sym_keys = key_t_set.length == 1 && key_t_set[0] == "symbol"
    str_keys = key_t_set.length == 1 && key_t_set[0] == "string"
    int_keys = key_t_set.length == 1 && key_t_set[0] == "int"
    int_vals = val_t_set.length == 1 && val_t_set[0] == "int"
    str_vals = val_t_set.length == 1 && val_t_set[0] == "string"
    if sym_keys && int_vals
      return "sym_int_hash"
    end
    if sym_keys && str_vals
      return "sym_str_hash"
    end
    if str_keys && int_vals
      return "str_int_hash"
    end
    if str_keys && str_vals
      return "str_str_hash"
    end
    if int_keys && str_vals
      return "int_str_hash"
    end
 # Fall back to poly when the observed pair doesn't fit a typed hash.
    if sym_keys
      return "sym_poly_hash"
    end
    if str_keys
      return "str_poly_hash"
    end
 # Heterogeneous key types — fall back to poly_poly_hash so each
 # entry carries its own tag and OBJ-tag eql? dispatches via the
 # codegen-emitted class hooks (e.g. Method#eql? for the optcarrot
 # `@peeks[peek] ||= peek` cache).
    "poly_poly_hash"
  end

  def pick_array_class(val_t_set)
    return "" if val_t_set.length == 0
    if val_t_set.length == 1
      vt = val_t_set[0]
      return "int_array" if vt == "int"
      return "float_array" if vt == "float"
      return "str_array" if vt == "string"
      return "sym_array" if vt == "symbol"
    end
    "poly_array"
  end

 # ---------- FFI declaration scanning ----------
 #
 # Each ffi_* DSL form inside a `module M ... end` body is recognized by
 # collect_module_with_prefix and dispatched here. Declarations are
 # recorded into the @ffi_* parallel arrays; emission happens later from
 # emit_ffi_externs and the compile/infer hooks.

 # Return the index of `mname` in @ffi_modules, creating an entry if missing.
  def ffi_module_idx(mname)
    i = 0
    while i < @ffi_modules.length
      if @ffi_modules[i] == mname
        return i
      end
      i = i + 1
    end
    @ffi_modules.push(mname)
    @ffi_module_libs.push("")
    @ffi_module_cflags.push("")
    @ffi_modules.length - 1
  end

 # Map an FFI type-spec symbol (e.g. "uint32", "str", "ptr") to a Spinel
 # type token ("int", "string", "ptr"). Returns "" on unknown input.
  def ffi_type_of(spec)
    if spec == "int" || spec == "uint32" || spec == "int32" || spec == "uint16" || spec == "int16" || spec == "uint8" || spec == "int8" || spec == "size_t" || spec == "long"
      return "int"
    end
    if spec == "float" || spec == "double"
      return "float"
    end
    if spec == "bool"
      return "bool"
    end
    if spec == "str"
      return "string"
    end
    if spec == "ptr"
      return "ptr"
    end
    if spec == "void"
      return "void"
    end
    ""
  end

 # Map an FFI type-spec symbol to the C type used in extern prototypes
 # and call-site casts. Unlike ffi_type_of (which collapses to Spinel
 # tokens), this preserves C-level detail (uint32_t vs size_t etc.).

 # Extract a string literal from a SymbolNode or StringNode arg. Returns
 # "" if the arg is not a literal we recognize.
  def ffi_arg_str(nid)
    if nid < 0
      return ""
    end
    t = @nd_type[nid]
    if t == "SymbolNode" || t == "StringNode"
      return @nd_content[nid]
    end
    ""
  end

 # Extract an integer from an IntegerNode arg. Returns -1 on non-int.
  def ffi_arg_int(nid)
    if nid < 0
      return -1
    end
    if @nd_type[nid] == "IntegerNode"
      return @nd_value[nid]
    end
    -1
  end

 # Emit an FFI decl error and abort with a pointed message.
  def ffi_error(mname, dname, msg)
    $stderr.puts "FFI error in module " + mname + ": " + dname + ": " + msg
    exit(1)
  end

 # Mangle a buffer's C symbol with module prefix to keep two modules
 # from colliding when both declare e.g. `:scratch`.

 # Lookup helpers — return the registry index, or -1 if not declared.
  def ffi_find_func(mod_name, fn_name)
    k = 0
    while k < @ffi_func_names.length
      if @ffi_func_modules[k] == mod_name && @ffi_func_names[k] == fn_name
        return k
      end
      k = k + 1
    end
    -1
  end

  def ffi_find_buffer(mod_name, b_name)
    k = 0
    while k < @ffi_buf_names.length
      if @ffi_buf_modules[k] == mod_name && @ffi_buf_names[k] == b_name
        return k
      end
      k = k + 1
    end
    -1
  end

  def ffi_find_reader(mod_name, r_name)
    k = 0
    while k < @ffi_reader_names.length
      if @ffi_reader_modules[k] == mod_name && @ffi_reader_names[k] == r_name
        return k
      end
      k = k + 1
    end
    -1
  end

 # Dispatch on the specific ffi_* declaration name. Called once per
 # recognized CallNode in a module body.
  def scan_ffi_decl(mname, nid)
    dname = @nd_name[nid]
    args_id = @nd_arguments[nid]
    args = []
    if args_id >= 0
      args = get_args(args_id)
    end
    mi = ffi_module_idx(mname)

    if dname == "ffi_lib"
      if args.length != 1
        ffi_error(mname, dname, "expected 1 arg (library name)")
      end
      libname = ffi_arg_str(args[0])
      if libname == ""
        ffi_error(mname, dname, "argument must be a string or symbol literal")
      end
      if @ffi_module_libs[mi] == ""
        @ffi_module_libs[mi] = libname
      else
        @ffi_module_libs[mi] = @ffi_module_libs[mi] + ";" + libname
      end
      return
    end

    if dname == "ffi_cflags"
      if args.length != 1
        ffi_error(mname, dname, "expected 1 arg (cflags string)")
      end
      flags = ffi_arg_str(args[0])
      if flags == ""
        ffi_error(mname, dname, "argument must be a string literal")
      end
      if @ffi_module_cflags[mi] == ""
        @ffi_module_cflags[mi] = flags
      else
        @ffi_module_cflags[mi] = @ffi_module_cflags[mi] + ";" + flags
      end
      return
    end

    if dname == "ffi_func"
 # ffi_func :name, [:arg1, :arg2], :ret
      if args.length != 3
        ffi_error(mname, dname, "expected 3 args (name, [arg types], ret type)")
      end
      fname = ffi_arg_str(args[0])
      if fname == ""
        ffi_error(mname, dname, "first arg must be a symbol (function name)")
      end
      if @nd_type[args[1]] != "ArrayNode"
        ffi_error(mname, dname, "second arg must be an array literal of type symbols")
      end
      arg_elems = parse_id_list(@nd_elements[args[1]])
      arg_toks = ""
      arg_spec_joined = ""
      k = 0
      while k < arg_elems.length
        spec = ffi_arg_str(arg_elems[k])
        tok = ffi_type_of(spec)
        if tok == "" || tok == "void"
          ffi_error(mname, dname, "unknown or invalid arg type spec '" + spec + "' in " + fname)
        end
        if k > 0
          arg_toks = arg_toks + ";"
          arg_spec_joined = arg_spec_joined + ";"
        end
        arg_toks = arg_toks + tok
        arg_spec_joined = arg_spec_joined + spec
        k = k + 1
      end
      ret_spec = ffi_arg_str(args[2])
      ret_tok = ffi_type_of(ret_spec)
      if ret_tok == ""
        ffi_error(mname, dname, "unknown return type spec '" + ret_spec + "' in " + fname)
      end
      @ffi_func_modules.push(mname)
      @ffi_func_names.push(fname)
      @ffi_func_arg_types.push(arg_toks)
      @ffi_func_ret_types.push(ret_tok)
      @ffi_func_arg_specs.push(arg_spec_joined)
      @ffi_func_ret_specs.push(ret_spec)
      return
    end

    if dname == "ffi_const"
 # ffi_const :NAME, <int>. Reuse the existing module-constant
 # storage so ConstantPathNode (Module::NAME) finds it via the
 # standard lookup. Names are mangled "<Mod>_<NAME>" to match the
 # convention set by collect_scoped_constant.
      if args.length != 2
        ffi_error(mname, dname, "expected 2 args (name, integer value)")
      end
      kname = ffi_arg_str(args[0])
      if kname == ""
        ffi_error(mname, dname, "first arg must be a symbol (constant name)")
      end
      cname_full = mname + "_" + kname
      @const_names.push(cname_full)
      @const_types.push("int")
      @const_expr_ids.push(args[1])
      @const_scope_names.push(mname)
      return
    end

    if dname == "ffi_buffer"
 # ffi_buffer :name, <size>
      if args.length != 2
        ffi_error(mname, dname, "expected 2 args (name, size)")
      end
      bname = ffi_arg_str(args[0])
      if bname == ""
        ffi_error(mname, dname, "first arg must be a symbol (buffer name)")
      end
      bsize = ffi_arg_int(args[1])
      if bsize <= 0
        ffi_error(mname, dname, "second arg must be a positive integer size")
      end
      @ffi_buf_modules.push(mname)
      @ffi_buf_names.push(bname)
      @ffi_buf_sizes.push(bsize)
      return
    end

    if dname == "ffi_read_u32" || dname == "ffi_read_i32" || dname == "ffi_read_ptr"
      if args.length != 2
        ffi_error(mname, dname, "expected 2 args (name, byte offset)")
      end
      rname = ffi_arg_str(args[0])
      if rname == ""
        ffi_error(mname, dname, "first arg must be a symbol (reader name)")
      end
      roff = ffi_arg_int(args[1])
      if roff < 0
        ffi_error(mname, dname, "second arg must be a non-negative integer offset")
      end
      kind = dname[9, dname.length - 9]  # strip "ffi_read_"
      @ffi_reader_modules.push(mname)
      @ffi_reader_names.push(rname)
      @ffi_reader_kinds.push(kind)
      @ffi_reader_offsets.push(roff)
      return
    end

    ffi_error(mname, dname, "unknown FFI declaration")
  end

 # ---------- FFI inference and call emission ----------

 # Type inference for FFI method calls. Returns the declared Spinel
 # return type for a ConstantReadNode-receiver call matching a
 # registered ffi_func / ffi_buffer / ffi_read_*, or "" otherwise.
  def infer_ffi_call_type(nid, mname, recv)
    if recv < 0
      return ""
    end
    if @nd_type[recv] != "ConstantReadNode"
      return ""
    end
    rcname = @nd_name[recv]
    fi = ffi_find_func(rcname, mname)
    if fi >= 0
      return @ffi_func_ret_types[fi]
    end
    bi = ffi_find_buffer(rcname, mname)
    if bi >= 0
      return "ptr"
    end
    ri = ffi_find_reader(rcname, mname)
    if ri >= 0
      kind = @ffi_reader_kinds[ri]
      if kind == "ptr"
        return "ptr"
      end
      return "int"
    end
    ""
  end

 # Compile a call to an FFI function/buffer/reader. Returns "" if this
 # is not an FFI call so the caller can fall through.

 # Emit a direct call to the FFI function indexed by `fi`. Each
 # argument is cast to its declared C type so type mismatches fail
 # loudly at compile time and we sidestep -Wconversion noise.

 # Emit a field-read from a buffer: Module.<reader_name>(buf).

 # Emit FFI prologue: link/cflag markers, extern prototypes, buffer
 # storage. Called from generate_code right after the symbol runtime.
 # No-op when no FFI module was declared.

  def collect_constant(nid)
    collect_scoped_constant("", nid)
  end

 # Multi-write to constants: `A, B, C = expr`. Each ConstantTargetNode
 # in the targets list gets registered as a separate constant whose
 # value is the i-th element of the RHS at emit time. Used both at
 # top level and inside class/module bodies (`scope_name`).
  def collect_scoped_multi_const(scope_name, nid)
    targets = parse_id_list(@nd_targets[nid])
    val_id = @nd_expression[nid]
    ti = 0
    while ti < targets.length
      tid = targets[ti]
      if @nd_type[tid] == "ConstantTargetNode"
        cname = @nd_name[tid]
        if scope_name != ""
          cname = scope_name + "_" + cname
        end
 # Use the existing scope chain when inferring element types so
 # nested-array RHS (`A, B = (1..2).map {...}`) gets the elem
 # type of the array rather than int.
        old_scope = @current_lexical_scope
        @current_lexical_scope = scope_name
        ct = scan_ivars_multi_target_type(val_id, ti)
        @current_lexical_scope = old_scope
        ci = find_const_idx(cname)
        if ci >= 0
          @const_types[ci] = ct
          @const_expr_ids[ci] = -1
          @const_scope_names[ci] = scope_name
        else
          @const_names.push(cname)
          @const_types.push(ct)
 # Element-of-multi: emit_global_constants can't reduce these
 # at module-init time since the source expression is the
 # whole RHS, not a per-target one. Mark expr_id = -1 and
 # rely on @multi_const_inits below to drive the assignment.
          @const_expr_ids.push(-1)
          @const_scope_names.push(scope_name)
        end
        ti = ti + 1
      else
        ti = ti + 1
      end
    end
 # Record the multi-write as a single deferred init: the RHS is
 # evaluated once and each constant takes one element.
    if @multi_const_inits == nil
      @multi_const_inits = "".split(",")
    end
    @multi_const_inits.push(scope_name + "|" + nid.to_s)
  end

 # Synthetic built-in user class for `method(:foo)` capture: every
 # `m = method(:foo)` / `obj.method(:foo)` produces an instance of
 # this class, allocated by sp_Method_new and walked by the auto-
 # generated sp_Method_gc_scan. .
 #
 # The two ivars:
 # - `@self_obj` typed `obj_Method`. Load-bearing hack: this type
 # makes ivar_is_gc_ptr true, which makes the auto-generated gc
 # scanner emit `sp_gc_mark(self->iv_self_obj)`. The actual
 # captured pointer is the bound receiver (any user class), cast
 # to `sp_Method *`. sp_gc_mark dispatches via the GC header's
 # `scan` field — set at the *bound receiver's* allocation time —
 # not via the static pointer type, so the cast is safe at the
 # GC level even though it's a type lie at the C level.
 # detect_method_taken_classes excludes the bound receiver's
 # class from value-type optimization so the captured pointer
 # never dangles (value-type instances live on the caller's
 # stack and would be reclaimed when the binding method returns).
 # - `@fn_ptr` typed `int`. Holds `(uintptr_t)&sp_<DefCls>_<mname>`
 # reinterpreted as mrb_int (int64_t — wide enough on every
 # target). The Method.call/[] codegen in compile_call_expr casts
 # it back to a function pointer of the right shape.
  def register_builtin_classes
    @cls_names.push("Method")
    @cls_is_value_type.push(0)
    @cls_is_sra.push(0)
    @cls_parents.push("")
    @cls_includes.push("")
    @cls_ivar_names.push("@self_obj;@fn_ptr")
    @cls_ivar_types.push("obj_Method;int")
    @cls_ivar_init_definite.push("1;1")
    @cls_ivar_observed_types.push("obj_Method;int")
    @cls_meth_names.push("initialize")
    @cls_meth_params.push("self_obj,fn_ptr")
    @cls_meth_ptypes.push("obj_Method,int")
    @cls_meth_returns.push("void")
    @cls_meth_bodies.push("-2")
    @cls_meth_defaults.push("0,0")
    @cls_meth_ptypes_empty.push("")
    @cls_attr_readers.push("")
    @cls_attr_writers.push("")
    @cls_cmeth_names.push("")
    @cls_cmeth_params.push("")
    @cls_cmeth_ptypes.push("")
    @cls_cmeth_returns.push("")
    @cls_cmeth_bodies.push("")
    @cls_cmeth_defaults.push("")
    @cls_cmeth_scope_names.push("")
    @cls_cmeth_scope_types.push("")
    @cls_meth_has_yield.push("0")
  end

  def collect_struct_class(cname, call_nid)
 # Generate a synthetic class from Struct.new(:field1, :field2, ...)
    ci = @cls_names.length
    @cls_names.push(cname)
    @cls_is_value_type.push(0)
    @cls_is_sra.push(0)
    @cls_parents.push("")
    @cls_includes.push("")
    @cls_ivar_names.push("")
    @cls_ivar_types.push("")
    @cls_ivar_init_definite.push("")
    @cls_ivar_observed_types.push("")
    @cls_meth_names.push("")
    @cls_meth_params.push("")
    @cls_meth_ptypes.push("")
    @cls_meth_returns.push("")
    @cls_meth_bodies.push("")
    @cls_meth_defaults.push("")
    @cls_meth_ptypes_empty.push("")
    @cls_attr_readers.push("")
    @cls_attr_writers.push("")
    @cls_cmeth_names.push("")
    @cls_cmeth_params.push("")
    @cls_cmeth_ptypes.push("")
    @cls_cmeth_returns.push("")
    @cls_cmeth_bodies.push("")
    @cls_cmeth_defaults.push("")
    @cls_cmeth_scope_names.push("")
    @cls_cmeth_scope_types.push("")
    @cls_meth_has_yield.push("")

 # Get field names from symbol args (skip keyword_init hash)
    args_id = @nd_arguments[call_nid]
    field_names = "".split(",")
    if args_id >= 0
      aids = get_args(args_id)
      k = 0
      while k < aids.length
 # Skip KeywordHashNode (keyword_init: true)
        if @nd_type[aids[k]] == "KeywordHashNode"
          k = k + 1
          next
        end
        fname = @nd_content[aids[k]]
        if fname != ""
          field_names.push(fname)
 # Add ivar
          iname = "@" + fname
          add_ivar(ci, iname, "int")
 # Add reader/writer
          append_attr_reader(ci, fname)
          append_attr_writer(ci, fname)
        end
        k = k + 1
      end
    end

 # Generate initialize method with params matching fields
    init_params = field_names.join(",")
    init_ptypes = ""
    k = 0
    while k < field_names.length
      if k > 0
        init_ptypes = init_ptypes + ","
      end
      init_ptypes = init_ptypes + "int"
      k = k + 1
    end
 # For struct, we don't have a body node - the constructor is synthetic
 # We'll handle this specially in emit_constructor
    append_cls_meth(ci, "initialize", init_params, init_ptypes, "void", -1, "")
 # Mark yield info
    @cls_meth_has_yield[ci] = "0"

 # Store struct info for synthetic constructor generation
 # We'll use a special marker in the body id (-2 = synthetic struct)
    bodies = @cls_meth_bodies[ci].split(";")
    if bodies.length > 0
      bodies[0] = "-2"
      @cls_meth_bodies[ci] = bodies.join(";")
    end
  end

 # ---- Yield detection ----
  def body_has_yield(nid)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "YieldNode"
      return 1
    end
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "block_given?"
        return 1
      end
    end
 # Don't recurse into nested DefNode (that's a different method)
    if @nd_type[nid] == "DefNode"
      return 0
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      if body_has_yield(cs[k]) == 1
        return 1
      end
      k = k + 1
    end
    0
  end

 # Walks `nid` for YieldNodes and returns max(`current`, max_args_of_yields).
 # Mirrors body_has_yield's recursion shape. `current` carries the running
 # max so callers can seed a floor (1, since every yield-using method needs
 # at least one mrb_int slot in `_block`'s signature).

 # ---- Return type inference ----
  def infer_constructor_types
 # Scan AST for ClassName.new(args) calls and infer param types
    scan_new_calls(@root_id)
  end

 # Narrow pre-pass for `rewrite_instance_eval_calls`: walk top-level
 # CallNodes shaped `recv.method(args)` where recv resolves to an
 # obj_<C> via top-level scope, and let scan_new_calls' receiver-method
 # branch widen the class method's ptypes. Without this, a method-param
 # receiver inside `def configure(app); app.instance_eval { } end`
 # has `app` ptype stuck at "int" at Pass 2.6 time — the existing
 # `infer_main_call_types` does the same scope-wrap but only runs in
 # `compile()` after `collect_all` returns.
 #
 # Why not reuse `infer_main_call_types`: that pass also runs the
 # top-level-method branch and the constructor branch, both of which
 # detect_poly_params later refines via different rules. Running them
 # twice (once at Pass 2.55, once in compile()) re-orders the inputs
 # detect_poly_params sees and demonstrably regresses
 # `test/poly_dispatch_builtin_all.rb` (lenof's poly param drops to
 # int_array when the early run primes ptypes ahead of the iterative
 # loop). This pre-pass scans the same AST but skips both other
 # branches; it only widens class-method ptypes through obj-typed
 # receivers — the exact piece rewrite_instance_eval_calls needs.
  def propagate_recv_method_arg_types_for_ieval
    push_scope
    if @nd_type[@root_id] == "ProgramNode"
      tl_body = @nd_body[@root_id]
      if tl_body >= 0
        empty_params = "".split(",")
        tl_lnames = "".split(",")
        tl_ltypes = "".split(",")
        scan_locals_first_type(tl_body, tl_lnames, tl_ltypes, empty_params)
        ti = 0
        while ti < tl_lnames.length
          declare_var(tl_lnames[ti], tl_ltypes[ti])
          ti = ti + 1
        end
      end
    end
    walk_recv_method_calls(@root_id)
    pop_scope
  end

 # Surgical fork of scan_new_calls: only the `obj.method(args)` branch,
 # only when `obj`'s static type is obj_<C>. Mirrors the cls_meth_ptypes
 # widening at lines ~6603-6615 of scan_new_calls (the same int->concrete
 # promotion gate) and falls through to recursion. Other branches of
 # scan_new_calls (constructor and top-level method) are deliberately
 # absent — running them earlier than master's compile() pipeline
 # interacts badly with detect_poly_params (see commentary above).
  def walk_recv_method_calls(nid)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
      if recv >= 0
        rt = infer_type(recv)
        if is_obj_type(rt) == 1
          cname = rt[4, rt.length - 4]
          ci = find_class_idx(cname)
          if ci >= 0
            owner_ci = ci
            midx = cls_find_method_direct(ci, mname)
            if midx < 0
              owner = find_method_owner(ci, mname)
              if owner != "" && owner != cname
                owner_ci = find_class_idx(owner)
                if owner_ci >= 0
                  midx = cls_find_method_direct(owner_ci, mname)
                end
              end
            end
            if midx >= 0
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
                ptypes = cls_meth_ptypes_get(owner_ci, midx)
                if ptypes.length > 0
                  kk = 0
                  while kk < arg_ids.length
                    at = infer_type(arg_ids[kk])
                    if kk < ptypes.length
                      if ptypes[kk] == "int"
                        if at != "int"
                          ptypes[kk] = at
                        end
                      end
                    end
                    kk = kk + 1
                  end
                  cls_meth_ptypes_put(owner_ci, midx, ptypes)
                end
              end
            end
          end
        end
      end
    end
    walk_recv_method_calls(@nd_body[nid])
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      walk_recv_method_calls(stmts[k])
      k = k + 1
    end
    walk_recv_method_calls(@nd_expression[nid])
    walk_recv_method_calls(@nd_arguments[nid])
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      walk_recv_method_calls(args[k])
      k = k + 1
    end
    walk_recv_method_calls(@nd_predicate[nid])
    walk_recv_method_calls(@nd_subsequent[nid])
    walk_recv_method_calls(@nd_else_clause[nid])
    walk_recv_method_calls(@nd_rescue_clause[nid])
    walk_recv_method_calls(@nd_ensure_clause[nid])
    walk_recv_method_calls(@nd_receiver[nid])
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      walk_recv_method_calls(conds[k])
      k = k + 1
    end
  end

 # Merge `at` (inferred from a new call-site argument) into the
 # accumulated ctor param type `old_pt`. "int" is normally treated as
 # a fallback/placeholder (many unresolved reads default to it), but a
 # literal IntegerNode is concrete — if old_pt is already a different
 # concrete pointer type, int becomes genuine polymorphism. `arg_id`
 # lets us distinguish literal from inferred.
  def unify_call_types(old_pt, at, arg_id)
    if old_pt == at
      return old_pt
    end
 # Stale unqualified obj-name normalization. Same shape as the
 # update_ivar_type fix: a Pass 1 ivar / param scan that ran
 # before all sibling classes were registered may have stamped
 # an unqualified `obj_<bare>` on a slot, which a later pass
 # then sees alongside the qualified `obj_<scope>_<bare>`.
 # Without this normalization the two strings compare unequal
 # and the trailing `incompatible → poly` tail collapses the
 # ptype, then propagates poly across every Class.new call site
 # that reads the slot. Detect the relationship and accept the
 # qualified form.
    if is_obj_type(old_pt) == 1 && is_obj_type(at) == 1
      old_bare_uct = old_pt[4, old_pt.length - 4]
      new_bare_uct = at[4, at.length - 4]
      if find_class_idx(old_bare_uct) < 0 && find_class_idx(new_bare_uct) >= 0
        suffix_uct = "_" + old_bare_uct
        if new_bare_uct.length > suffix_uct.length && new_bare_uct[(new_bare_uct.length - suffix_uct.length), suffix_uct.length] == suffix_uct
          return at
        end
      end
      if find_class_idx(new_bare_uct) < 0 && find_class_idx(old_bare_uct) >= 0
        suffix_uct = "_" + new_bare_uct
        if old_bare_uct.length > suffix_uct.length && old_bare_uct[(old_bare_uct.length - suffix_uct.length), suffix_uct.length] == suffix_uct
          return old_pt
        end
      end
    end
    arg_is_literal = 0
    if arg_id >= 0 && is_literal_value_expr(arg_id) == 1
      arg_is_literal = 1
    end
    if old_pt == "nil"
      if at == "nil"
        return "nil"
      end
      if is_nullable_pointer_type(at) == 1
        if is_nullable_type(at) == 1
          return at
        end
        return at + "?"
      end
      return at
    end
    if at == "nil"
      if is_nullable_pointer_type(old_pt) == 1
        if is_nullable_type(old_pt) == 1
          return old_pt
        end
        return old_pt + "?"
      end
      return old_pt
    end
    if old_pt == "int"
      if at == "int"
        return "int"
      end
      return at
    end
    if is_array_type(old_pt) == 1 && is_array_type(at) == 1
      if old_pt == "poly_array" || at == "poly_array"
        @needs_rb_value = 1
        return "poly_array"
      end
      if old_pt == "int_array"
        return at
      end
      if at == "int_array"
        return old_pt
      end
      @needs_rb_value = 1
      return "poly_array"
    end
 # Hash variants: empty-default str_int_hash (from `attrs = {}`)
 # is type-flexible — let the call-site type win, same as the
 # int_array → typed-array escalation above. . The
 # poly variants subsume any other hash; mismatched concrete
 # hash types (str_int_hash vs sym_str_hash) fall through to
 # the "incompatible → poly" tail.
    if is_hash_type(old_pt) == 1 && is_hash_type(at) == 1
      if old_pt == "str_poly_hash" || at == "str_poly_hash"
        @needs_str_poly_hash = 1
        @needs_rb_value = 1
        return "str_poly_hash"
      end
      if old_pt == "sym_poly_hash" || at == "sym_poly_hash"
        @needs_sym_poly_hash = 1
        @needs_rb_value = 1
        return "sym_poly_hash"
      end
      if old_pt == "str_int_hash"
        return at
      end
      if at == "str_int_hash"
        return old_pt
      end
    end
    if at == "int"
 # Numeric compat: int + float is safe in both directions.
      if old_pt == "float"
        return "float"
      end
 # Literal `0` against a pointer-typed slot: C's null pointer
 # constant. The literal narrows nothing; preserve the existing
 # pointer type as nullable instead of collapsing to poly.
 # Mirrors the `at == "nil"` branch above for the integer-zero
 # spelling Ruby callers reach for when passing "no value" to
 # an FFI / pointer slot.
      if arg_is_literal == 1 && arg_id >= 0 && @nd_type[arg_id] == "IntegerNode" && @nd_value[arg_id].to_i == 0 && is_nullable_pointer_type(old_pt) == 1
        if is_nullable_type(old_pt) == 1
          return old_pt
        end
        return old_pt + "?"
      end
 # Literal int into a non-numeric concrete type: genuine poly.
      if arg_is_literal == 1
        @needs_rb_value = 1
        return "poly"
      end
 # Inferred int (likely fallback): keep existing type.
      return old_pt
    end
    if base_type(old_pt) == base_type(at)
 # Nullable-compatible variants of the same base.
      if is_nullable_type(at) == 1
        return at
      end
      if is_nullable_type(old_pt) == 1
        return old_pt
      end
      return old_pt
    end
    if (old_pt == "float" && at == "int") || (old_pt == "int" && at == "float")
      return "float"
    end
 # `def f(conf = ARGV)`: the default's type lands as `argv`
 # (spinel's specialised `**argv`-like scalar), but bootstrapping
 # callers in real programs almost always invoke `f("path.nes")`
 # — a single string. Without this narrow, unification falls to
 # poly and the body's `Config.new(conf)` (string-expecting) gets
 # a poly arg. Bias toward the call-site shape so single-string
 # entry points don't drag the whole signature into poly.
    if (old_pt == "argv" && at == "string") || (old_pt == "string" && at == "argv")
      return "string"
    end
 # Genuinely incompatible types: fall back to polymorphic value.
    @needs_rb_value = 1
    "poly"
  end



 # Resolve a ClassNode AST id to its registered index in
 # @cls_names, walking the same module-prefix chain that
 # `resolve_const_read_name` / `find_class_idx` use at emit
 # time. Returns -1 when the class isn't in @cls_names (e.g. a
 # nested `class << self` body that hasn't been registered as a
 # regular class).
  def class_node_to_idx(nid)
    cp = @nd_constant_path[nid]
    if cp < 0
      return -1
    end
    flat = const_ref_flat_name(cp)
    if flat == ""
      return -1
    end
    resolved = resolve_const_read_name(flat)
    ci = find_class_idx(resolved)
    if ci >= 0
      return ci
    end
    find_class_idx(flat)
  end

 # Set @current_class_idx / @current_lexical_scope to the class
 # introduced by `nid` (a ClassNode). No-op when the class isn't
 # registered. Caller is responsible for save/restore.
  def enter_class_scope_from_node(nid)
    ci = class_node_to_idx(nid)
    if ci >= 0
      @current_class_idx = ci
      @current_lexical_scope = @cls_names[ci]
    end
  end

 # Append the module name introduced by `nid` (a ModuleNode) to
 # `@current_lexical_scope`. Caller is responsible for save/
 # restore. Mirrors the prefix pattern that
 # `collect_module_with_prefix` uses for nested module names.
  def enter_module_scope_from_node(nid)
    cp = @nd_constant_path[nid]
    if cp < 0
      return
    end
    mname_s = const_ref_flat_name(cp)
    if mname_s == ""
      return
    end
    if @current_lexical_scope != ""
      @current_lexical_scope = @current_lexical_scope + "_" + mname_s
    else
      @current_lexical_scope = mname_s
    end
  end

 # Widen a callee's `ptypes` array from a single call site's
 # argument list, correctly handling keyword args. Positional args
 # unify by index; a `KeywordHashNode` (kwargs) unifies each
 # `key: value` pair into the slot whose param-name matches the
 # key. Mutates `ptypes` in place; the caller joins the result
 # back into the storage table (`@meth_param_types[mi]`,
 # `@cls_cmeth_ptypes[ci]`, etc.).
 #
 # The kwargs branch is essential: a positional-only loop would
 # unify a `KeywordHashNode` into `ptypes[0]` (the AST presents
 # kwargs as a single trailing hash arg), leaving the callee's
 # actual kwarg slots un-widened.
  def widen_ptypes_from_args(arg_ids, pnames, ptypes)
    pos_idx = 0
    ai = 0
    while ai < arg_ids.length
      aid = arg_ids[ai]
      if @nd_type[aid] == "KeywordHashNode"
        elems = parse_id_list(@nd_elements[aid])
        ei = 0
        while ei < elems.length
          if @nd_type[elems[ei]] == "AssocNode"
            key_id = @nd_key[elems[ei]]
            val_id = @nd_expression[elems[ei]]
            if key_id >= 0 && val_id >= 0 && @nd_type[key_id] == "SymbolNode"
              kname = @nd_content[key_id]
              if kname == ""
                kname = @nd_name[key_id]
              end
              pi = 0
              while pi < pnames.length
                if pnames[pi] == kname && pi < ptypes.length
                  at = infer_type(val_id)
                  ptypes[pi] = unify_call_types(ptypes[pi], at, val_id)
                end
                pi = pi + 1
              end
            end
          end
          ei = ei + 1
        end
      else
        if pos_idx < ptypes.length
          at = infer_type(aid)
          ptypes[pos_idx] = unify_call_types(ptypes[pos_idx], at, aid)
        end
        pos_idx = pos_idx + 1
      end
      ai = ai + 1
    end
  end

  def scan_new_calls(nid)
    if nid < 0
      return
    end
 # When we descend into a class body, pin @current_class_idx so
 # any `infer_type(@ivar)` in the args of a nested .new call
 # resolves against this class's ivar table. Without this scope
 # set-up, arguments like `@cpu` inside `Foo.initialize`'s
 # body that contains `Bar.new(@cpu, ...)` infer as "int" (the
 # default for an InstanceVariableReadNode with no scope), which
 # then wedges Bar.initialize's first param at int even after
 # multiple iterations of the fixpoint loop.
    if @nd_type[nid] == "ClassNode"
      saved_ci = @current_class_idx
      saved_scope = @current_lexical_scope
      enter_class_scope_from_node(nid)
      body = @nd_body[nid]
      if body >= 0
        scan_new_calls(body)
      end
      @current_class_idx = saved_ci
      @current_lexical_scope = saved_scope
      return
    end
    if @nd_type[nid] == "ModuleNode"
      saved_scope2 = @current_lexical_scope
      enter_module_scope_from_node(nid)
      body = @nd_body[nid]
      if body >= 0
 # Module-body top-level statements form a load-time scope just
 # like main. Seed calls like `Foo::Wrapper.new(_seed)` where
 # `_seed = "hello"` lives at module body need their argument
 # types pinned, but the cascade-1 guard in scan_locals stops
 # `infer_main_call_types` from descending here. Mirror main's
 # locals-then-calls shape on a fresh scope so the seed call's
 # arg infers off the local's literal type. The same guard keeps
 # nested def/class/module bodies from leaking back out.
        push_scope
        lnames = "".split(",")
        ltypes = "".split(",")
        empty_p = "".split(",")
        scan_locals(body, lnames, ltypes, empty_p)
        k = 0
        while k < lnames.length
          declare_var(lnames[k], ltypes[k])
          k = k + 1
        end
        scan_new_calls(body)
        pop_scope
      end
      @current_lexical_scope = saved_scope2
      return
    end
 # IfNode (incl. ternary). When the predicate is
 # `var.is_a?(C)` / `kind_of?(C)`, push a narrow on `var` while
 # walking the then-arm so a recursive call inside the arm sees
 # the narrowed type and unify_call_types widens the callee's
 # param accordingly. The else-arm walks unchanged — we don't
 # currently model "type minus C" (see POLY-AS-SET.md).
    if @nd_type[nid] == "IfNode"
      pred = @nd_predicate[nid]
      if pred >= 0
        scan_new_calls(pred)
      end
      parsed = parse_is_a_predicate(pred)
      narrow_var = parsed[0]
      narrow_t = parsed[1]
      if narrow_var != ""
        push_type_narrow(narrow_var, narrow_t)
      end
      then_body = @nd_body[nid]
      if then_body >= 0
        scan_new_calls(then_body)
      end
      if narrow_var != ""
        pop_type_narrow
      end
      sub = @nd_subsequent[nid]
      if sub >= 0
        scan_new_calls(sub)
      end
      else_body = @nd_else_clause[nid]
      if else_body >= 0
        scan_new_calls(else_body)
      end
      return
    end
    if @nd_type[nid] == "CallNode"
 # Also infer top-level method param types from call sites
      mname = @nd_name[nid]
      if @nd_receiver[nid] < 0
        mi = find_method_idx(mname)
        if mi >= 0
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
            ptypes = @meth_param_types[mi].split(",")
            pnames = @meth_param_names[mi].split(",")
            rest_param_idx = method_rest_index(mi)
            if rest_param_idx >= ptypes.length
              rest_param_idx = -1
            end
 # Handle keyword hash args
            ak = 0
            while ak < arg_ids.length
              if @nd_type[arg_ids[ak]] == "KeywordHashNode"
                elems = parse_id_list(@nd_elements[arg_ids[ak]])
                ek = 0
                while ek < elems.length
                  if @nd_type[elems[ek]] == "AssocNode"
                    key_id = @nd_key[elems[ek]]
                    if key_id >= 0
                      kname = ""
                      if @nd_type[key_id] == "SymbolNode"
                        kname = @nd_content[key_id]
                      end
                      at = infer_type(@nd_expression[elems[ek]])
 # Find matching param name
                      pi = 0
                      while pi < pnames.length
                        if pnames[pi] == kname
                          if pi < ptypes.length
                            ptypes[pi] = unify_call_types(ptypes[pi], at, @nd_expression[elems[ek]])
                          end
                        end
                        pi = pi + 1
                      end
                    end
                  end
                  ek = ek + 1
                end
              else
 # SplatNode: treat the splat source's element type as
 # contributing to *every* fixed param from `ak` up to the
 # last non-rest one. So `foo(*strs)` correctly infers a
 # str-typed first param even though the call site has
 # only a single SplatNode arg.
                if @nd_type[arg_ids[ak]] == "SplatNode"
                  splat_src_for_inf = @nd_expression[arg_ids[ak]]
                  if splat_src_for_inf >= 0
                    splat_t_for_inf = infer_type(splat_src_for_inf)
                    elem_t_for_inf = elem_type_of_array(splat_t_for_inf)
                    if elem_t_for_inf != "int" && elem_t_for_inf != ""
                      pi3 = ak
                      while pi3 < ptypes.length
                        if pi3 == rest_param_idx
                          pi3 = pi3 + 1
                          next
                        end
                        if ptypes[pi3] == "int"
                          ptypes[pi3] = elem_t_for_inf
                        end
                        pi3 = pi3 + 1
                      end
                    end
                  end
                else
                  at = infer_type(arg_ids[ak])
                  if ak < ptypes.length
                    if rest_param_idx < 0 || ak < rest_param_idx
                      ptypes[ak] = unify_call_types(ptypes[ak], at, arg_ids[ak])
                    end
                  end
                end
              end
              ak = ak + 1
            end
            @meth_param_types[mi] = ptypes.join(",")
          end
        end
 # Bare call inside a class method body that resolves to an
 # inherited instance method. find_method_idx above only
 # finds *top-level* methods; an `assert_not_nil(x)` inside
 # `T2 < T`'s body needs to widen T's `assert_not_nil`
 # ptypes from this call's args. Mirror the obj.method()
 # walk in the recv >= 0 branch below — find_method_owner
 # finds the ancestor that actually defines the method, then
 # we update *that* class's @cls_meth_ptypes.
        if @current_class_idx >= 0
          inh_ci = @current_class_idx
          inh_owner_ci = inh_ci
          inh_midx = cls_find_method_direct(inh_ci, mname)
          if inh_midx < 0
            inh_owner_name = find_method_owner(inh_ci, mname)
            if inh_owner_name != "" && inh_owner_name != @cls_names[inh_ci]
              inh_owner_ci = find_class_idx(inh_owner_name)
              if inh_owner_ci >= 0
                inh_midx = cls_find_method_direct(inh_owner_ci, mname)
              end
            end
          end
          if inh_midx >= 0
            inh_args_id = @nd_arguments[nid]
            if inh_args_id >= 0
              inh_arg_ids = get_args(inh_args_id)
              inh_ptypes = cls_meth_ptypes_get(inh_owner_ci, inh_midx)
              if inh_ptypes.length > 0
 # Route through widen_ptypes_from_args so a trailing
 # KeywordHashNode lands on the matching named kwarg slot
 # rather than the next positional. Without this, `head(204,
 # content_type: "x")` from a child class widened
 # `content_type`'s slot with the *whole keyword hash*'s
 # type (sym_str_hash) and left it un-pinned to "string".
                inh_pnames = cls_meth_pnames_get(inh_owner_ci, inh_midx)
                widen_ptypes_from_args(inh_arg_ids, inh_pnames, inh_ptypes)
                cls_meth_ptypes_put(inh_owner_ci, inh_midx, inh_ptypes)
              end
            end
          end
        end
 # bare call inside a `def self.X` body resolves
 # to a sibling cmeth on the same class/module. Widen the
 # sibling's ptypes from this call site's args. Mirrors the
 # explicit-`M.X(args)` widening branch below but keyed off
 # @current_method_name's `<Class>_cls_<m>` marker (set by
 # infer_function_body_call_types / infer_class_body_call_types
 # before walking each cmeth body) since recv is absent here.
        if @current_method_name != ""
          marker = @current_method_name.index("_cls_")
          if marker != nil && marker >= 0
            owning = @current_method_name[0, marker]
            cci = find_class_idx(owning)
            if cci >= 0
              cmnames = @cls_cmeth_names[cci].split(";")
              cmidx = 0
              while cmidx < cmnames.length
                if cmnames[cmidx] == mname
                  args_id = @nd_arguments[nid]
                  if args_id >= 0
                    arg_ids = get_args(args_id)
                    cmpt = cls_cmeth_ptypes_get(cci, cmidx)
                    if cmpt.length > 0
                      cmpn = cls_cmeth_pnames_get(cci, cmidx)
                      widen_ptypes_from_args(arg_ids, cmpn, cmpt)
                      cls_cmeth_ptypes_put(cci, cmidx, cmpt)
                    end
                  end
                end
                cmidx = cmidx + 1
              end
            end
            if module_name_exists(owning) == 1
              synth = owning + "_cls_" + mname
              sib_mi_m = find_method_idx(synth)
              if sib_mi_m >= 0
                args_id_m = @nd_arguments[nid]
                if args_id_m >= 0
                  arg_ids_m = get_args(args_id_m)
                  ptypes_m = @meth_param_types[sib_mi_m].split(",")
                  pnames_m = @meth_param_names[sib_mi_m].split(",")
                  widen_ptypes_from_args(arg_ids_m, pnames_m, ptypes_m)
                  @meth_param_types[sib_mi_m] = ptypes_m.join(",")
                end
              end
            end
          end
        end
      end
      if @nd_name[nid] == "new"
        recv = @nd_receiver[nid]
        if recv >= 0
          cname = constructor_class_name(recv)
          if cname != ""
            ci = find_class_idx(cname)
            if ci >= 0
              init_ci = find_init_class(ci)
              if init_ci >= 0
                init_idx = cls_find_method_direct(init_ci, "initialize")
                if init_idx >= 0
                  args_id = @nd_arguments[nid]
                  if args_id >= 0
                    arg_ids = get_args(args_id)
                    ptypes = cls_meth_ptypes_get(init_ci, init_idx)
                    if ptypes.length > 0
                      pnames = cls_meth_pnames_get(init_ci, init_idx)
                      widen_ptypes_from_args(arg_ids, pnames, ptypes)
                      cls_meth_ptypes_put(init_ci, init_idx, ptypes)
                    end
                  end
                end
              end
            end
          end
        end
      end
 # Also infer method param types from method/operator calls on objects
      if @nd_receiver[nid] >= 0
        rt = infer_type(@nd_receiver[nid])
        if is_obj_type(rt) == 1
          cname = rt[4, rt.length - 4]
          ci = find_class_idx(cname)
          if ci >= 0
 # Walk inheritance: when the method isn't on `ci` directly,
 # find the parent that actually defines it and update
 # *that* class's @cls_meth_ptypes so the body-side
 # promotion (infer_param_array_type_from_body) sees the
 # caller's arg types. .
            owner_ci = ci
            midx = cls_find_method_direct(ci, mname)
            if midx < 0
              owner = find_method_owner(ci, mname)
              if owner != "" && owner != cname
                owner_ci = find_class_idx(owner)
                if owner_ci >= 0
                  midx = cls_find_method_direct(owner_ci, mname)
                end
              end
            end
            if midx >= 0
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
 # Unify against the existing param type rather
 # than only widening from "int". A first-non-int-
 # wins rule would let one call site freeze the
 # param type and a later disagreeing site (e.g.
 # `addr` seen as Range from one and Integer from
 # another) lands as a signature mismatch.
 # unify_call_types collapses incompatibles to
 # "poly".
                ptypes = cls_meth_ptypes_get(owner_ci, midx)
                if ptypes.length > 0
                  pnames = cls_meth_pnames_get(owner_ci, midx)
                  widen_ptypes_from_args(arg_ids, pnames, ptypes)
                  cls_meth_ptypes_put(owner_ci, midx, ptypes)
                end
              end
            end
          end
        end
 # Forward-ref dispatch: when the receiver is statically `int`
 # (a yet-untyped ivar / param read whose true class hasn't
 # propagated through the inference fixpoint yet) but `mname`
 # belongs to exactly one user class — and isn't shared with a
 # primitive type's method of the same name — the int→class
 # fallback at the bottom of compile_no_recv_call_expr will
 # dispatch the call to that class's C function. Widen the
 # callee's param types from this site's args so the C
 # signatures match the values the fallback passes. Without
 # this, file orderings that put the caller (e.g. `rom.rb`'s
 # `@ppu.set_chr_mem(@chr_ref, @chr_ram)`) before the callee
 # (PPU) leave the callee's params at the default `mrb_int`,
 # producing Wint-conversion / incompatible-pointer errors at
 # the int→class call site. unify_call_types collapses to
 # poly when later sites disagree.
 #
 # Gate on the receiver being an ivar / local read — those are
 # the only shapes the fallback realistically widens through.
 # Skipping CallNode / IntegerNode / etc. avoids accidentally
 # treating `(self <=> other) > 0` (where the recv is the int
 # result of `<=>` and `>` is genuinely an int operator) as a
 # forward-ref to `Temperature#>`, which would unify the
 # already-correct `obj_Temperature` param against the literal
 # `0` arg and collapse it to poly.
        recv_iow_fwd = @nd_receiver[nid]
        recv_is_ivar_or_local = recv_iow_fwd >= 0 && (@nd_type[recv_iow_fwd] == "InstanceVariableReadNode" || @nd_type[recv_iow_fwd] == "LocalVariableReadNode")
        if rt == "int" && recv_is_ivar_or_local && primitive_method_shared_with_user_class(mname) == 0
 # Walk every user class that defines mname AND whose
 # param count matches the call's arg count. Pre-fix
 # this branch bailed on multi-match cases
 # (matched_ci_fwd = -2), leaving every candidate's
 # params at the int default and producing
 # incompatible-pointer C errors when poly-typed
 # iteration over heterogeneous receivers reaches
 # them. case 2: `[IndexHandler.new,
 # UsersHandler.new].each { |h| h.handle(req, res) }`
 # -- both Index/UsersHandler#handle(req,res) get
 # widened from "/" and "" args; sibling classes that
 # happen to define `handle` with a different arity
 # (SQLite's attr_accessor, 0-arg getter) are filtered
 # out by the arity check.
          args_id_fwd = @nd_arguments[nid]
          if args_id_fwd >= 0
            arg_ids_fwd = get_args(args_id_fwd)
            arg_count_fwd = arg_ids_fwd.length
            if arg_count_fwd > 0
              ci_fwd = 0
              while ci_fwd < @cls_names.length
                midx_fwd_c = cls_find_method_direct(ci_fwd, mname)
                if midx_fwd_c >= 0
                  ptypes_fwd_c = cls_meth_ptypes_get(ci_fwd, midx_fwd_c)
                  if ptypes_fwd_c.length == arg_count_fwd
                    kk_fwd_c = 0
                    while kk_fwd_c < arg_count_fwd
                      at_fwd_c = infer_type(arg_ids_fwd[kk_fwd_c])
                      if kk_fwd_c < ptypes_fwd_c.length
                        ptypes_fwd_c[kk_fwd_c] = unify_call_types(ptypes_fwd_c[kk_fwd_c], at_fwd_c, arg_ids_fwd[kk_fwd_c])
                      end
                      kk_fwd_c = kk_fwd_c + 1
                    end
                    cls_meth_ptypes_put(ci_fwd, midx_fwd_c, ptypes_fwd_c)
                  end
                end
                ci_fwd = ci_fwd + 1
              end
            end
          end
        end
 # Poly-receiver widening: when the receiver is poly (e.g. a
 # method param widened to accept multiple user classes),
 # the per-class arms emitted by compile_poly_method_call
 # call each class's `<mname>` C function with the same arg
 # expressions. Walk every user class that defines mname and
 # unify its ptypes with the call site's arg types so the
 # arm signatures match — otherwise a String key passed to a
 # default-`int`-typed param produces a Wint-conversion or
 # incompatible-pointer error. unify_call_types collapses to
 # `poly` if another call site disagrees.
        if rt == "poly"
          args_id_p = @nd_arguments[nid]
          if args_id_p >= 0
            arg_ids_p = get_args(args_id_p)
            ci_p = 0
            while ci_p < @cls_names.length
              midx_p = cls_find_method_direct(ci_p, mname)
              if midx_p >= 0
                ptypes_p = cls_meth_ptypes_get(ci_p, midx_p)
                if ptypes_p.length > 0
                  kk_p = 0
                  while kk_p < arg_ids_p.length
                    if kk_p < ptypes_p.length
                      at_p = infer_type(arg_ids_p[kk_p])
                      ptypes_p[kk_p] = unify_call_types(ptypes_p[kk_p], at_p, arg_ids_p[kk_p])
                    end
                    kk_p = kk_p + 1
                  end
                  cls_meth_ptypes_put(ci_p, midx_p, ptypes_p)
                end
              end
              ci_p = ci_p + 1
            end
          end
        end
      end
 # `<Class>.cls_method(args)` — widen class method parameter
 # types from call-site argument types. Same shape as the
 # receiver-method unify above but operating on
 # @cls_cmeth_ptypes for class-constant recvs.
      if @nd_type[nid] == "CallNode" && @nd_receiver[nid] >= 0
        crecv = @nd_receiver[nid]
        if @nd_type[crecv] == "ConstantReadNode" || @nd_type[crecv] == "ConstantPathNode"
          rcname = constructor_class_name(crecv)
          if rcname != ""
            cci = find_class_idx(rcname)
            if cci >= 0
              cmname = @nd_name[nid]
              cmnames = @cls_cmeth_names[cci].split(";")
              cmidx = 0
              while cmidx < cmnames.length
                if cmnames[cmidx] == cmname
                  args_id = @nd_arguments[nid]
                  if args_id >= 0
                    arg_ids = get_args(args_id)
                    cmptypes = cls_cmeth_ptypes_get(cci, cmidx)
                    if cmptypes.length > 0
                      cmpnames = cls_cmeth_pnames_get(cci, cmidx)
                      widen_ptypes_from_args(arg_ids, cmpnames, cmptypes)
                      cls_cmeth_ptypes_put(cci, cmidx, cmptypes)
                    end
                  end
                end
                cmidx = cmidx + 1
              end
            end
 # Module class methods (`module M; def self.greet(...);
 # end; end`) live in the top-level `@meth_*` tables
 # under the synthetic name `<Mod>_cls_<m>`, not in
 # `@cls_cmeth_*` — `find_class_idx` returns -1 for
 # module names so the widening branch above misses
 # them. Parallel branch widens `@meth_param_types` for
 # the prefixed name so a `M.greet("a")` call site
 # teaches the synthetic function to accept the actual
 # arg type instead of the default `mrb_int`.
            if module_name_exists(rcname) == 1
              mod_mfn = rcname + "_cls_" + @nd_name[nid]
              mod_mi = find_method_idx(mod_mfn)
              if mod_mi >= 0
                mod_args_id = @nd_arguments[nid]
                if mod_args_id >= 0
                  mod_arg_ids = get_args(mod_args_id)
                  mod_pnames = @meth_param_names[mod_mi].split(",")
                  mod_ptypes = @meth_param_types[mod_mi].split(",")
                  widen_ptypes_from_args(mod_arg_ids, mod_pnames, mod_ptypes)
                  @meth_param_types[mod_mi] = mod_ptypes.join(",")
                end
              end
            end
          end
        end
      end
 # Module-dispatch ternary call sites
 # (`Disp.adapter.method(args)` where Disp.adapter resolves to
 # N candidate classes via the module-singleton-accessor
 # table). The dispatch ternary calls every candidate's class
 # method, so all of them need the args' types unified into
 # their per-class @cls_cmeth_ptypes entry — otherwise the
 # ternary arms type-mismatch when the caller passes a poly
 # value but the targets' params are still mrb_int.
      if @nd_type[nid] == "CallNode" && @nd_receiver[nid] >= 0
        mua_outer_recv = @nd_receiver[nid]
        if @nd_type[mua_outer_recv] == "CallNode"
          mua_inner_recv = @nd_receiver[mua_outer_recv]
          mua_inner_mname = @nd_name[mua_outer_recv]
          if mua_inner_recv >= 0 && @nd_type[mua_inner_recv] == "ConstantReadNode"
            mua_mod_name = @nd_name[mua_inner_recv]
            if module_name_exists(mua_mod_name) == 1
              mua_rconsts = module_acc_resolved(mua_mod_name, mua_inner_mname)
              if mua_rconsts != "" && mua_rconsts != "?"
                mua_cands = mua_rconsts.split(";")
                mua_outer_mname = @nd_name[nid]
                mua_args_id = @nd_arguments[nid]
                if mua_args_id >= 0
                  mua_arg_ids = get_args(mua_args_id)
                  mua_cands.each { |mua_cn|
                    mua_cci = find_class_idx(mua_cn)
                    if mua_cci >= 0
                      mua_cmnames = @cls_cmeth_names[mua_cci].split(";")
                      mua_cmpall = @cls_cmeth_ptypes[mua_cci].split("|")
                      mua_cmidx = 0
                      while mua_cmidx < mua_cmnames.length
                        if mua_cmnames[mua_cmidx] == mua_outer_mname && mua_cmidx < mua_cmpall.length
                          mua_cmpt = mua_cmpall[mua_cmidx].split(",")
                          mua_k = 0
                          while mua_k < mua_arg_ids.length
                            mua_at = infer_type(mua_arg_ids[mua_k])
                            if mua_k < mua_cmpt.length
                              mua_cmpt[mua_k] = unify_call_types(mua_cmpt[mua_k], mua_at, mua_arg_ids[mua_k])
                            end
                            mua_k = mua_k + 1
                          end
                          mua_cmpall[mua_cmidx] = mua_cmpt.join(",")
                          @cls_cmeth_ptypes[mua_cci] = mua_cmpall.join("|")
                          @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
                        end
                        mua_cmidx = mua_cmidx + 1
                      end
                    end
 # When the candidate is a module (not a class),
 # its `def self.method` is stored in `@meth_*`
 # as `<Mod>_cls_<m>` rather than in
 # `@cls_cmeth_*`. Mirrors the module-class-
 # method widening branch above so a
 # `Mod.accessor.method(args)` call site widens
 # the synthetic top-level function's params.
                    if module_name_exists(mua_cn) == 1
                      mua_mod_mfn = mua_cn + "_cls_" + mua_outer_mname
                      mua_mod_mi = find_method_idx(mua_mod_mfn)
                      if mua_mod_mi >= 0
                        mua_mod_pnames = @meth_param_names[mua_mod_mi].split(",")
                        mua_mod_ptypes = @meth_param_types[mua_mod_mi].split(",")
                        widen_ptypes_from_args(mua_arg_ids, mua_mod_pnames, mua_mod_ptypes)
                        @meth_param_types[mua_mod_mi] = mua_mod_ptypes.join(",")
                      end
                    end
                  }
                end
              end
            end
          end
        end
      end
    end
 # Recurse into children
    if @nd_body[nid] >= 0
      scan_new_calls(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_new_calls(stmts[k])
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      scan_new_calls(@nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      scan_new_calls(@nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_new_calls(args[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_new_calls(@nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_new_calls(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_new_calls(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_new_calls(@nd_else_clause[nid])
    end
    if @nd_left[nid] >= 0
      scan_new_calls(@nd_left[nid])
    end
    if @nd_right[nid] >= 0
      scan_new_calls(@nd_right[nid])
    end
    if @nd_block[nid] >= 0
      scan_new_calls(@nd_block[nid])
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_new_calls(elems[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_new_calls(conds[k])
      k = k + 1
    end
 # InterpolatedStringNode and friends carry their components in @nd_parts.
 # Without this, an EmbeddedStatementsNode inside `"#{...}"` is the only
 # call site for a method whose param type would otherwise widen, and
 # the param keeps its default `int` => C error at the call site.
    parts = parse_id_list(@nd_parts[nid])
    k = 0
    while k < parts.length
      scan_new_calls(parts[k])
      k = k + 1
    end
  end

 # Bare `new(args)` inside a class method body widens the
 # subclass's `initialize` ptypes from this call's args.
 # scan_new_calls' generic `new` handler only fires when the
 # receiver is an explicit class constant (`Article.new(attrs)`);
 # the bare form has no receiver and would otherwise leave
 # `sp_Article_new(mrb_int)` called with `sp_SymStrHash *`.
 #
 # By the time this runs, propagate_inherited_class_methods has
 # already given each subclass its own copy of the inherited
 # cls method, with @cls_cmeth_ptypes widened from explicit
 # call sites. Walking each cls method body for bare `new(args)`
 # and unifying the matching `find_init_class(ci).initialize`
 # ptypes fills the gap. Args that read a cls-method local
 # resolve via the cls method's pnames/ptypes; other args fall
 # through to infer_type with @current_class_idx pinned so
 # @ivar refs in the args resolve against the right class.
 # Propagate ptypes from child#initialize to parent#initialize via
 # super calls. Without this, a `Sub.new(owner_obj)` site widens
 # `Sub#initialize`'s `_owner` param to `obj_<C>` correctly (via
 # the existing constructor branch in scan_new_calls), but the
 # body's bare `super` lowers to a C call against the parent's
 # `Base#initialize`, whose param ptype stays at the default
 # `mrb_int` because no call-site path widens the *parent's*
 # initialize through super. The C output then casts the typed
 # pointer to `mrb_int` at the super-call site, the parent's body's
 # `@owner = owner` records the int into the slot, and every
 # subsequent `@owner.<method>` dispatch on a child instance lands
 # on whatever class shares the int recv's cls_id — commonly the
 # wrong one.
 #
 # ForwardingSuperNode (bare `super`): forwards every param of the
 # current method by name. Unify the parent ptypes element-wise
 # against the child ptypes (capped at min length, so a wider
 # parent signature stays unwidened past the forwarded prefix).
 #
 # SuperNode (`super(arg1, arg2, …)`): explicit arg list. Each arg
 # might be a LocalVariableReadNode that points back at one of the
 # current method's params; if so, use the param's ptype to unify
 # at the super arg's position. Other shapes (literals, calls)
 # fall through to infer_type.
  def propagate_super_init_to_parent
    ci = 0
    while ci < @cls_names.length
      if @cls_parents[ci] != ""
        parent_ci = find_class_idx(@cls_parents[ci])
 # The parent chain may insert classes without their own
 # #initialize (e.g. an empty marker subclass between the
 # super caller and its semantic parent). Walk up until we
 # find one that does.
        walk_ci = parent_ci
        parent_init_ci = -1
        while walk_ci >= 0
          if cls_find_method_direct(walk_ci, "initialize") >= 0
            parent_init_ci = walk_ci
            break
          end
          parent_str = @cls_parents[walk_ci]
          if parent_str == ""
            break
          end
          walk_ci = find_class_idx(parent_str)
        end
        if parent_init_ci >= 0
          propagate_super_init_one(ci, parent_init_ci)
        end
      end
      ci = ci + 1
    end
  end

  def propagate_super_init_one(child_ci, parent_init_ci)
    child_init_idx = cls_find_method_direct(child_ci, "initialize")
    if child_init_idx < 0
      return
    end
    bodies = @cls_meth_bodies[child_ci].split(";")
    if child_init_idx >= bodies.length
      return
    end
    bid = bodies[child_init_idx].to_i
    if bid < 0
      return
    end
    child_pnames = cls_meth_pnames_get(child_ci, child_init_idx)
    child_ptypes = cls_meth_ptypes_get(child_ci, child_init_idx)
    parent_init_idx = cls_find_method_direct(parent_init_ci, "initialize")
    if parent_init_idx < 0
      return
    end
    parent_ptypes = cls_meth_ptypes_get(parent_init_ci, parent_init_idx)
    if parent_ptypes.length == 0
      return
    end
    propagate_super_walk(bid, child_pnames, child_ptypes, parent_ptypes)
    cls_meth_ptypes_put(parent_init_ci, parent_init_idx, parent_ptypes)
  end

  def propagate_super_walk(nid, child_pnames, child_ptypes, parent_ptypes)
    if nid < 0
      return
    end
    nt = @nd_type[nid]
    if nt == "DefNode"
      return
    end
    if nt == "ForwardingSuperNode"
 # Bare `super`: forward every child param into the parent's slot
 # at the same position, capped at min length.
      kk = 0
      lim = child_ptypes.length
      if parent_ptypes.length < lim
        lim = parent_ptypes.length
      end
      while kk < lim
        ct = child_ptypes[kk]
        if ct != "" && ct != "int"
          parent_ptypes[kk] = unify_call_types(parent_ptypes[kk], ct, -1)
        end
        kk = kk + 1
      end
      return
    end
    if nt == "SuperNode"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a_ids = get_args(args_id)
        kk = 0
        while kk < a_ids.length && kk < parent_ptypes.length
          aid = a_ids[kk]
          at = ""
          if @nd_type[aid] == "LocalVariableReadNode"
            vname = @nd_name[aid]
            pi = 0
            while pi < child_pnames.length
              if child_pnames[pi] == vname && pi < child_ptypes.length
                at = child_ptypes[pi]
              end
              pi = pi + 1
            end
          end
          if at == ""
            at = infer_type(aid)
          end
          if at != "" && at != "int"
            parent_ptypes[kk] = unify_call_types(parent_ptypes[kk], at, aid)
          end
          kk = kk + 1
        end
      end
      return
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      propagate_super_walk(cs[k], child_pnames, child_ptypes, parent_ptypes)
      k = k + 1
    end
  end

  def propagate_bare_new_to_subclass_initialize
    ci = 0
    while ci < @cls_names.length
      cmnames = @cls_cmeth_names[ci].split(";")
      cm_bodies = @cls_cmeth_bodies[ci].split(";")
      j = 0
      while j < cmnames.length
        bid = -1
        if j < cm_bodies.length
          bs = cm_bodies[j]
          if bs != ""
            bid = bs.to_i
          end
        end
        if bid > 0
          pnames = cls_cmeth_pnames_get(ci, j)
          ptypes = cls_cmeth_ptypes_get(ci, j)
          saved_ci = @current_class_idx
          @current_class_idx = ci
          walk_bare_new_in_cmeth_body(bid, ci, pnames, ptypes)
          @current_class_idx = saved_ci
        end
        j = j + 1
      end
      ci = ci + 1
    end
  end

  def walk_bare_new_in_cmeth_body(nid, cls_ci, cm_pnames, cm_ptypes)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "new" && @nd_receiver[nid] < 0
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        init_ci = find_init_class(cls_ci)
        if init_ci >= 0
          init_idx = cls_find_method_direct(init_ci, "initialize")
          if init_idx >= 0
            init_ptypes = cls_meth_ptypes_get(init_ci, init_idx)
            if init_ptypes.length > 0
              kk = 0
              while kk < arg_ids.length
                at = ""
                if @nd_type[arg_ids[kk]] == "LocalVariableReadNode"
                  vname = @nd_name[arg_ids[kk]]
                  pi = 0
                  while pi < cm_pnames.length
                    if cm_pnames[pi] == vname && pi < cm_ptypes.length
                      at = cm_ptypes[pi]
                    end
                    pi = pi + 1
                  end
                end
                if at == ""
                  at = infer_type(arg_ids[kk])
                end
                if kk < init_ptypes.length && at != "" && at != "int"
                  init_ptypes[kk] = unify_call_types(init_ptypes[kk], at, arg_ids[kk])
                end
                kk = kk + 1
              end
              cls_meth_ptypes_put(init_ci, init_idx, init_ptypes)
            end
          end
        end
      end
    end
 # Mirror scan_new_calls' recursion shape so a `new(...)` at any
 # depth in the body is reachable: stmts, body, conditionals,
 # call args, expressions, etc.
    if @nd_body[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_body[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    bn_stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < bn_stmts.length
      walk_bare_new_in_cmeth_body(bn_stmts[k], cls_ci, cm_pnames, cm_ptypes)
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_receiver[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_arguments[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_arguments[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    bn_args = parse_id_list(@nd_args[nid])
    k = 0
    while k < bn_args.length
      walk_bare_new_in_cmeth_body(bn_args[k], cls_ci, cm_pnames, cm_ptypes)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_expression[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_predicate[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_predicate[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_subsequent[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_subsequent[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_else_clause[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_else_clause[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_left[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_left[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_right[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_right[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    if @nd_block[nid] >= 0
      walk_bare_new_in_cmeth_body(@nd_block[nid], cls_ci, cm_pnames, cm_ptypes)
    end
    bn_elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < bn_elems.length
      walk_bare_new_in_cmeth_body(bn_elems[k], cls_ci, cm_pnames, cm_ptypes)
      k = k + 1
    end
  end

  def update_ivar_types_from_params
 # Special case: synthetic struct constructors - ivars match params directly
    i = 0
    while i < @cls_names.length
      init_idx2 = cls_find_method_direct(i, "initialize")
      if init_idx2 >= 0
        bodies = @cls_meth_bodies[i].split(";")
        if init_idx2 < bodies.length
          if bodies[init_idx2].to_i == -2
 # Synthetic struct - update ivar types from init param types
            pnames = cls_meth_pnames_get(i, init_idx2)
            ptypes = cls_meth_ptypes_get(i, init_idx2)
            pk = 0
            while pk < pnames.length
              iname = "@" + pnames[pk]
              if pk < ptypes.length
                if ptypes[pk] != "int"
                  update_ivar_type(i, iname, ptypes[pk])
                end
              end
              pk = pk + 1
            end
          end
        end
      end
      i = i + 1
    end
 # For each class method, if it assigns @ivar = param, update ivar type from param type
    i = 0
    while i < @cls_names.length
      mnames = @cls_meth_names[i].split(";")
      mi = 0
      while mi < mnames.length
        init_idx = mi
        pnames = cls_meth_pnames_get(i, init_idx)
        ptypes = cls_meth_ptypes_get(i, init_idx)
        bodies = @cls_meth_bodies[i].split(";")
        bid = -1
        if init_idx < bodies.length
          bid = bodies[init_idx].to_i
        end
        if bid >= 0
          stmts = get_stmts(bid)
          stmts.each { |sid|
            if @nd_type[sid] == "InstanceVariableWriteNode"
              expr = @nd_expression[sid]
              if expr >= 0
                if @nd_type[expr] == "LocalVariableReadNode"
                  pname = @nd_name[expr]
 # Find param index
                  pi = 0
                  while pi < pnames.length
                    if pnames[pi] == pname
                      if pi < ptypes.length
 # Update ivar type
                        iname = @nd_name[sid]
                        ivar_names = @cls_ivar_names[i].split(";")
                        ivar_types = @cls_ivar_types[i].split(";")
                        ij = 0
                        while ij < ivar_names.length
                          if ij < ivar_types.length
                            if ivar_names[ij] == iname
                              if ptypes[pi] != ""
                                ivar_types[ij] = unify_call_types(ivar_types[ij], ptypes[pi], -1)
                              end
                            end
                          end
                          ij = ij + 1
                        end
                        @cls_ivar_types[i] = ivar_types.join(";")
                        @cls_ivar_types_version = @cls_ivar_types_version + 1
                      end
                    end
                    pi = pi + 1
                  end
                end
              end
            end
          }
        end
        mi = mi + 1
      end
      i = i + 1
    end
  end

  def infer_cls_meth_param_from_body
 # For each class method, if a param is used as param.attr_reader where attr_reader
 # belongs to ANY class, infer param type as that class.
 # Check all classes' methods (not just the class owning the readers).
    oci = 0
    while oci < @cls_names.length
      mnames = @cls_meth_names[oci].split(";")
      bodies = @cls_meth_bodies[oci].split(";")
      j = 0
      while j < mnames.length
        if mnames[j] != "initialize"
          pnames = cls_meth_pnames_get(oci, j)
          ptypes = cls_meth_ptypes_get(oci, j)
          bid = -1
          if j < bodies.length
            bid = bodies[j].to_i
          end
          if bid >= 0
            pk = 0
            while pk < pnames.length
              if pk < ptypes.length
                if ptypes[pk] == "int"
 # Pick the class whose surface (readers + writers +
 # methods, walked through parents) contains every
 # method actually called on this param. Matching
 # only one reader and ignoring later accesses
 # would pick a class that fails to satisfy the
 # full method set.
                  called = "".split(",")
                  collect_param_methods(bid, pnames[pk], called)
                  if called.length > 0 && called_methods_only_on_container_builtins(called) == 0
                    ci2 = 0
                    best = -1
                    while ci2 < @cls_names.length
                      if best < 0 && class_has_all_methods(ci2, called) == 1
                        best = ci2
                      end
                      ci2 = ci2 + 1
                    end
                    if best >= 0
                      ptypes[pk] = "obj_" + @cls_names[best]
                      cls_meth_ptypes_put(oci, j, ptypes)
                    end
                  end
                end
              end
              pk = pk + 1
            end
          end
        end
        j = j + 1
      end
      oci = oci + 1
    end
 # Also infer top-level method param types from body usage. Same
 # all-methods-must-match rule as the cls_meth_param branch above.
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length
            if ptypes[pk] == "int"
              called = "".split(",")
              collect_param_methods(bid, pnames[pk], called)
              if called.length > 0 && called_methods_only_on_container_builtins(called) == 0
                ci2 = 0
                best = -1
                while ci2 < @cls_names.length
                  if best < 0 && class_has_all_methods(ci2, called) == 1
                    best = ci2
                  end
                  ci2 = ci2 + 1
                end
                if best >= 0
                  ptypes[pk] = "obj_" + @cls_names[best]
                  @meth_param_types[mi] = ptypes.join(",")
                end
              end
            end
          end
          pk = pk + 1
        end
      end
      mi = mi + 1
    end
  end

 # Methods that exist on String but not on any user-overrideable
 # receiver, and not on the container builtins. A body that calls
 # one of these on a param is strong evidence the param is a
 # String. Used by infer_string_param_from_body to lift "int"
 # default ptypes to "string" when the body's only user signal is
 # a String-specific receiver method (#450 cascade 2:
 # `def match(method, path, table); ...; path.split("/"); end`
 # left `path` typed mrb_int because called_methods_only_on_
 # container_builtins filtered `split` out and no user class was
 # a perfect fit).
  def is_string_only_method(mname)
    if mname == "split" || mname == "start_with?" || mname == "end_with?"
      return 1
    end
    if mname == "chomp" || mname == "chop" || mname == "strip" || mname == "lstrip" || mname == "rstrip"
      return 1
    end
    if mname == "upcase" || mname == "downcase" || mname == "swapcase" || mname == "capitalize"
      return 1
    end
    if mname == "tr" || mname == "gsub" || mname == "sub" || mname == "scan" || mname == "match" || mname == "match?"
      return 1
    end
    if mname == "ljust" || mname == "rjust" || mname == "center"
      return 1
    end
    if mname == "bytes" || mname == "bytesize" || mname == "chars" || mname == "lines" || mname == "codepoints"
      return 1
    end
    if mname == "encode" || mname == "force_encoding" || mname == "encoding"
      return 1
    end
    if mname == "concat" || mname == "ascii_only?"
      return 1
    end
    0
  end

 # Walk every method body. If a param is still typed "int" (the
 # placeholder default) and the body calls at least one
 # String-specific method on it, promote to "string". Mirrors
 # infer_cls_meth_param_from_body's class-promotion shape, but
 # specialised for the String case where the canonical receiver
 # is the primitive rather than a user class.
 # Walk `nid`'s subtree collecting `(callee_mname, arg_pos)` pairs
 # for every CallNode whose `arg_pos`-th positional arg is a
 # LocalVariableReadNode named `pname`. Used by
 # infer_param_type_from_callee_slot to back-propagate a callee's
 # concrete slot type to the caller's still-`int` param.
  def collect_param_callee_slots(nid, pname, acc)
    if nid < 0
      return
    end
 # Don't cross nested DefNode / ClassNode / ModuleNode bodies.
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
    if @nd_type[nid] == "CallNode"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        ai = 0
        while ai < aargs.length
          if @nd_type[aargs[ai]] == "LocalVariableReadNode" && @nd_name[aargs[ai]] == pname
            acc.push(@nd_name[nid] + "\t" + ai.to_s)
          end
          ai = ai + 1
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_param_callee_slots(cs[k], pname, acc)
      k = k + 1
    end
  end

 # Walk every method body. For each param still typed "int" (the
 # placeholder default), look at where it's passed as an arg to
 # another method. If every observed callee's matching slot is the
 # same concrete pointer-y type (`ptr` / `obj_<C>` / specific
 # array/hash), widen the param to that type. Mirrors the shape of
 # `infer_string_param_from_body` (#450 cascade 2) but driven by
 # callee-slot evidence rather than receiver-method evidence.
 #
 # Originally added for the Db.column_bool shape:
 #   def self.column_bool(stmt, idx); column_int(stmt, idx) != 0; end
 # where column_int's slot is `void *` (ptr) via FFI inference
 # but column_bool's `stmt` stayed mrb_int, breaking C compile at
 # `sp_Db_cls_column_int(lv_stmt, ...)`.
  def infer_param_type_from_callee_slot
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        changed_cs = 0
 # Derive the lexical scope from the method name itself —
 # `Db_cls_column_bool` → "Db" — so a bare sibling call like
 # `column_int(stmt, idx)` inside the body resolves to the
 # `Db_cls_column_int` synthetic.
        saved_scope_cs = @current_lexical_scope
        mname_cs = @meth_names[mi]
        cls_marker_cs = mname_cs.index("_cls_")
        if cls_marker_cs != nil
          @current_lexical_scope = mname_cs[0, cls_marker_cs]
        end
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length && ptypes[pk] == "int"
            obs = "".split(",")
            collect_param_callee_slots(bid, pnames[pk], obs)
            agreed = ""
            disagree = 0
            kk = 0
            while kk < obs.length
              tab = obs[kk].index("\t")
              if tab >= 0
                callee_name = obs[kk][0, tab]
                pos_s = obs[kk][tab + 1, obs[kk].length - tab - 1]
                pos = pos_s.to_i
                callee_t = callee_slot_type(callee_name, pos)
                if callee_t == "ptr" || is_obj_type(callee_t) == 1
                  if agreed == ""
                    agreed = callee_t
                  elsif agreed != callee_t
                    disagree = 1
                  end
                end
              end
              kk = kk + 1
            end
            if agreed != "" && disagree == 0
              ptypes[pk] = agreed
              changed_cs = 1
            end
          end
 # Hash-typed param widening: a body that forwards `pname` to
 # a callee whose matching slot is a poly variant of the same
 # hash family (str_int_hash forwarded to str_poly_hash, etc.)
 # should widen the caller's param so both sides agree on the
 # C struct. Mirrors the ptr-back-prop above for the hash case.
 # parse_form_into(into) -> assign_form_pair(into, ...) where
 # assign_form_pair's `into` widened to str_poly_hash via body
 # heterogeneous `into[k] = val` + `into[k] = {}` writes.
          if pk < ptypes.length && is_hash_type(ptypes[pk]) == 1 && ptypes[pk].include?("poly") == false
            obs_h = "".split(",")
            collect_param_callee_slots(bid, pnames[pk], obs_h)
            cur_kt_h = hash_key_part(ptypes[pk])
            agreed_h = ""
            disagree_h = 0
            kh = 0
            while kh < obs_h.length
              tab_h = obs_h[kh].index("\t")
              if tab_h >= 0
                cn_h = obs_h[kh][0, tab_h]
                pos_h = obs_h[kh][tab_h + 1, obs_h[kh].length - tab_h - 1].to_i
                ct_h = callee_slot_type(cn_h, pos_h)
                if is_hash_type(ct_h) == 1 && ct_h.include?("poly") && hash_key_part(ct_h) == cur_kt_h
                  if agreed_h == ""
                    agreed_h = ct_h
                  elsif agreed_h != ct_h
                    disagree_h = 1
                  end
                end
              end
              kh = kh + 1
            end
            if agreed_h != "" && disagree_h == 0
              ptypes[pk] = agreed_h
              changed_cs = 1
            end
          end
          pk = pk + 1
        end
        @current_lexical_scope = saved_scope_cs
        if changed_cs == 1
          @meth_param_types[mi] = ptypes.join(",")
        end
      end
      mi = mi + 1
    end
 # Real-class cmeths live in @cls_cmeth_*, not @meth_*. A
 # `def self.column_bool(stmt, idx)` inside `class Db` whose
 # body forwards `stmt` to a `void *`-slotted callee needs the
 # same back-propagation as the module-class case. Mirror the
 # @meth_* walk over each registered class's cmeth table.
    cci_cs = 0
    while cci_cs < @cls_names.length
      cmnames_cs = @cls_cmeth_names[cci_cs].split(";")
      cmbodies_cs = @cls_cmeth_bodies[cci_cs].split(";")
      cls_changed_cs = 0
      cmj_cs = 0
      while cmj_cs < cmnames_cs.length
        bid_cm_cs = -1
        if cmj_cs < cmbodies_cs.length
          bid_cm_cs = cmbodies_cs[cmj_cs].to_i
        end
        if bid_cm_cs >= 0
          pnames_cm_cs = cls_cmeth_pnames_get(cci_cs, cmj_cs)
          ptypes_cm_cs = cls_cmeth_ptypes_get(cci_cs, cmj_cs)
          m_changed_cs = 0
          saved_scope_cm_cs = @current_lexical_scope
          @current_lexical_scope = @cls_names[cci_cs]
          pk_cs = 0
          while pk_cs < pnames_cm_cs.length
            if pk_cs < ptypes_cm_cs.length && ptypes_cm_cs[pk_cs] == "int"
              obs_cs = "".split(",")
              collect_param_callee_slots(bid_cm_cs, pnames_cm_cs[pk_cs], obs_cs)
              agreed_cs = ""
              disagree_cs = 0
              kk_cs = 0
              while kk_cs < obs_cs.length
                tab_cs = obs_cs[kk_cs].index("\t")
                if tab_cs >= 0
                  cn_cs = obs_cs[kk_cs][0, tab_cs]
                  pos_s_cs = obs_cs[kk_cs][tab_cs + 1, obs_cs[kk_cs].length - tab_cs - 1]
                  pos_cs = pos_s_cs.to_i
                  ct_cs = callee_slot_type(cn_cs, pos_cs)
                  if ct_cs == "ptr" || is_obj_type(ct_cs) == 1
                    if agreed_cs == ""
                      agreed_cs = ct_cs
                    elsif agreed_cs != ct_cs
                      disagree_cs = 1
                    end
                  end
                end
                kk_cs = kk_cs + 1
              end
              if agreed_cs != "" && disagree_cs == 0
                ptypes_cm_cs[pk_cs] = agreed_cs
                m_changed_cs = 1
              end
            end
 # Hash-typed param widening: mirror the @meth_* branch above
 # — when the body forwards `pname` to a callee whose matching
 # slot is a poly variant of the same hash family, widen the
 # caller's param accordingly.
            if pk_cs < ptypes_cm_cs.length && is_hash_type(ptypes_cm_cs[pk_cs]) == 1 && ptypes_cm_cs[pk_cs].include?("poly") == false
              obs_ch = "".split(",")
              collect_param_callee_slots(bid_cm_cs, pnames_cm_cs[pk_cs], obs_ch)
              cur_kt_ch = hash_key_part(ptypes_cm_cs[pk_cs])
              agreed_ch = ""
              disagree_ch = 0
              kch = 0
              while kch < obs_ch.length
                tab_ch = obs_ch[kch].index("\t")
                if tab_ch >= 0
                  cn_ch = obs_ch[kch][0, tab_ch]
                  pos_ch = obs_ch[kch][tab_ch + 1, obs_ch[kch].length - tab_ch - 1].to_i
                  ct_ch = callee_slot_type(cn_ch, pos_ch)
                  if is_hash_type(ct_ch) == 1 && ct_ch.include?("poly") && hash_key_part(ct_ch) == cur_kt_ch
                    if agreed_ch == ""
                      agreed_ch = ct_ch
                    elsif agreed_ch != ct_ch
                      disagree_ch = 1
                    end
                  end
                end
                kch = kch + 1
              end
              if agreed_ch != "" && disagree_ch == 0
                ptypes_cm_cs[pk_cs] = agreed_ch
                m_changed_cs = 1
              end
            end
            pk_cs = pk_cs + 1
          end
          @current_lexical_scope = saved_scope_cm_cs
          if m_changed_cs == 1
            cls_cmeth_ptypes_put(cci_cs, cmj_cs, ptypes_cm_cs)
            cls_changed_cs = 1
          end
        end
        cmj_cs = cmj_cs + 1
      end
      if cls_changed_cs == 1
        @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
      end
      cci_cs = cci_cs + 1
    end
  end

 # LV counterpart of infer_param_type_from_callee_slot. Walks
 # each method body's LV table and, for each hash-typed LV
 # forwarded to a callee whose matching slot has widened to a
 # poly variant of the same key family, widens the LV.
 #
 # Real-blog shape:
 #
 #   def parse_request(env, stdin)
 #     params = {}                         # initial str_int_hash
 #     parse_form_into(query, params)      # callee widened to str_poly_hash
 #                                         # via narrow_param_hash_types_from_body_writes
 #     ...
 #   end
 #
 # Pre-fix `params` stayed str_int_hash and the call passed
 # sp_StrIntHash * into a sp_StrPolyHash * slot, warning. Plus
 # 4 downstream `iv_X = lv_params` cascade warnings. With this
 # pass, params widens up the chain.
  def infer_lv_types_from_callee_arg_slots
 # Top-level + module class methods (@meth_*).
    mi_lv = 0
    while mi_lv < @meth_names.length
      bid_lv = @meth_body_ids[mi_lv]
      if bid_lv >= 0
        sn_lv = @nd_scope_names[bid_lv].split("|", -1)
        st_lv = @nd_scope_types[bid_lv].split("|", -1)
        changed_lv = 0
        saved_scope_lv = @current_lexical_scope
        mname_lv = @meth_names[mi_lv]
        cls_marker_lv = mname_lv.index("_cls_")
        if cls_marker_lv != nil && cls_marker_lv >= 0
          @current_lexical_scope = mname_lv[0, cls_marker_lv]
        end
        lvi = 0
        while lvi < sn_lv.length
          if lvi < st_lv.length && is_hash_type(st_lv[lvi]) == 1 && st_lv[lvi].include?("poly") == false
            obs_lv = "".split(",")
            collect_param_callee_slots(bid_lv, sn_lv[lvi], obs_lv)
            cur_kt_lv = hash_key_part(st_lv[lvi])
            agreed_lv = ""
            disagree_lv = 0
            kk_lv = 0
            while kk_lv < obs_lv.length
              tab_lv = obs_lv[kk_lv].index("\t")
              if tab_lv >= 0
                cn_lv = obs_lv[kk_lv][0, tab_lv]
                pos_lv = obs_lv[kk_lv][tab_lv + 1, obs_lv[kk_lv].length - tab_lv - 1].to_i
                ct_lv = callee_slot_type(cn_lv, pos_lv)
                if is_hash_type(ct_lv) == 1 && ct_lv.include?("poly") && hash_key_part(ct_lv) == cur_kt_lv
                  if agreed_lv == ""
                    agreed_lv = ct_lv
                  elsif agreed_lv != ct_lv
                    disagree_lv = 1
                  end
                end
              end
              kk_lv = kk_lv + 1
            end
            if agreed_lv != "" && disagree_lv == 0
              st_lv[lvi] = agreed_lv
              changed_lv = 1
            end
          end
          lvi = lvi + 1
        end
        @current_lexical_scope = saved_scope_lv
        if changed_lv == 1
          @nd_scope_types[bid_lv] = st_lv.join("|")
        end
      end
      mi_lv = mi_lv + 1
    end
 # Class instance methods (@cls_meth_*) and real-class cmeths
 # (@cls_cmeth_*) share the @nd_scope_names indirection; the
 # per-(class, cmeth) tables from Task b also need updating.
 # For brevity we update only the per-bid table here; the
 # per-(ci, cmj) Task b tables are populated by a separate
 # pass earlier in the inference loop and use these same scope
 # entries, so the widening propagates on the next iteration.
    ci_lv = 0
    while ci_lv < @cls_names.length
      bodies_lv = @cls_meth_bodies[ci_lv].split(";")
      bj_lv = 0
      while bj_lv < bodies_lv.length
        bid_im = bodies_lv[bj_lv].to_i
        if bid_im >= 0
          sn_im = @nd_scope_names[bid_im].split("|", -1)
          st_im = @nd_scope_types[bid_im].split("|", -1)
          changed_im = 0
          lvii = 0
          while lvii < sn_im.length
            if lvii < st_im.length && is_hash_type(st_im[lvii]) == 1 && st_im[lvii].include?("poly") == false
              obs_im = "".split(",")
              collect_param_callee_slots(bid_im, sn_im[lvii], obs_im)
              cur_kt_im = hash_key_part(st_im[lvii])
              agreed_im = ""
              disagree_im = 0
              kki = 0
              while kki < obs_im.length
                tab_im = obs_im[kki].index("\t")
                if tab_im >= 0
                  cn_im = obs_im[kki][0, tab_im]
                  pos_im = obs_im[kki][tab_im + 1, obs_im[kki].length - tab_im - 1].to_i
                  ct_im = callee_slot_type(cn_im, pos_im)
                  if is_hash_type(ct_im) == 1 && ct_im.include?("poly") && hash_key_part(ct_im) == cur_kt_im
                    if agreed_im == ""
                      agreed_im = ct_im
                    elsif agreed_im != ct_im
                      disagree_im = 1
                    end
                  end
                end
                kki = kki + 1
              end
              if agreed_im != "" && disagree_im == 0
                st_im[lvii] = agreed_im
                changed_im = 1
              end
            end
            lvii = lvii + 1
          end
          if changed_im == 1
            @nd_scope_types[bid_im] = st_im.join("|")
          end
        end
        bj_lv = bj_lv + 1
      end
      ci_lv = ci_lv + 1
    end
  end

 # Resolve `callee_name` (a bare-call mname or a `<Mod>_cls_<m>`
 # synthetic) to its param-types array and return the slot at
 # `pos`, or "" if unresolvable. Used by
 # infer_param_type_from_callee_slot to look up evidence from a
 # callee body without committing to receiver-resolution
 # (`Db.column_int` vs sibling-call `column_int` lookups).
  def callee_slot_type(callee_name, pos)
    cmi = find_method_idx(callee_name)
    if cmi >= 0
      cpts = @meth_param_types[cmi].split(",")
      if pos < cpts.length
        return cpts[pos]
      end
    end
 # Try sibling-class-method synth: `<CurrentScope>_cls_<callee_name>`.
    if @current_lexical_scope != ""
      synth = @current_lexical_scope + "_cls_" + callee_name
      smi = find_method_idx(synth)
      if smi >= 0
        spts = @meth_param_types[smi].split(",")
        if pos < spts.length
          return spts[pos]
        end
      end
 # Real-class sibling cmeth: stored in @cls_cmeth_* (not the
 # synthetic top-level @meth_*). A bare call from one
 # `def self.foo` to a peer `def self.bar` inside `class X`
 # resolves here.
      sci = find_class_idx(@current_lexical_scope)
      if sci >= 0
        sc_cmnames = @cls_cmeth_names[sci].split(";")
        smj = 0
        while smj < sc_cmnames.length
          if sc_cmnames[smj] == callee_name
            sc_pts = cls_cmeth_ptypes_get(sci, smj)
            if pos < sc_pts.length
              return sc_pts[pos]
            end
          end
          smj = smj + 1
        end
      end
    end
    ""
  end

  def infer_string_param_from_body
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        changed_top = 0
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length && ptypes[pk] == "int"
            called = "".split(",")
            collect_param_methods(bid, pnames[pk], called)
            sawk = 0
            kk = 0
            while kk < called.length
              if is_string_only_method(called[kk]) == 1
                sawk = 1
              end
              kk = kk + 1
            end
            if sawk == 1
              ptypes[pk] = "string"
              changed_top = 1
            end
          end
          pk = pk + 1
        end
        if changed_top == 1
          @meth_param_types[mi] = ptypes.join(",")
        end
      end
      mi = mi + 1
    end
    ci = 0
    while ci < @cls_names.length
      mnames = @cls_meth_names[ci].split(";")
      bodies = @cls_meth_bodies[ci].split(";")
      cls_changed = 0
      mj = 0
      while mj < mnames.length
        if mnames[mj] != "initialize"
          pnames_j = cls_meth_pnames_get(ci, mj)
          ptypes_j = cls_meth_ptypes_get(ci, mj)
          bid_j = -1
          if mj < bodies.length
            bid_j = bodies[mj].to_i
          end
          if bid_j >= 0
            m_changed = 0
            pk = 0
            while pk < pnames_j.length
              if pk < ptypes_j.length && ptypes_j[pk] == "int"
                called_c = "".split(",")
                collect_param_methods(bid_j, pnames_j[pk], called_c)
                sawc = 0
                kk = 0
                while kk < called_c.length
                  if is_string_only_method(called_c[kk]) == 1
                    sawc = 1
                  end
                  kk = kk + 1
                end
                if sawc == 1
                  ptypes_j[pk] = "string"
                  m_changed = 1
                end
              end
              pk = pk + 1
            end
            if m_changed == 1
              cls_meth_ptypes_put(ci, mj, ptypes_j)
              cls_changed = 1
            end
          end
        end
        mj = mj + 1
      end
      if cls_changed == 1
        @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      end
      ci = ci + 1
    end
  end

 # Widen each method's stored parameter types to encompass any
 # in-body reassignments to the parameter name. Without this,
 # a body shape like
 #
 # def f(hclk, ...)
 # hclk = "forever" if hclk == FOREVER_CLOCK
 # ...
 # end
 #
 # leaves `hclk`'s slot at the call-site-inferred `int`, and the
 # `lv_hclk = (const char *)…` C statement fails -Wint-conversion.
 # Widening to "poly" lets the slot hold both shapes via sp_RbVal.
 #
 # `int` is treated as a default/fallback (matches unify_call_types):
 # int + concrete-non-int → concrete. Only genuinely incompatible
 # writes (e.g. ptype already concrete and a different concrete
 # write) escalate to poly.
  def widen_param_types_from_body_writes
 # Top-level methods.
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        ptypes_changed = 0
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length
            new_t = scan_param_body_write_unify(bid, pnames[pk], ptypes[pk])
            if new_t != ptypes[pk]
              ptypes[pk] = new_t
              ptypes_changed = 1
            end
          end
          pk = pk + 1
        end
        if ptypes_changed == 1
          @meth_param_types[mi] = ptypes.join(",")
        end
      end
      mi = mi + 1
    end

 # Per-class instance and class methods.
    ci = 0
    while ci < @cls_names.length
      mnames = @cls_meth_names[ci].split(";")
      all_params = @cls_meth_params[ci].split("|")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      bodies = @cls_meth_bodies[ci].split(";")
      cls_changed = 0
      mj = 0
      while mj < mnames.length
        if mj < all_params.length && mj < all_ptypes.length && mj < bodies.length
          bid_j = bodies[mj].to_i
          if bid_j >= 0
            pnames_j = all_params[mj].split(",")
            ptypes_j = all_ptypes[mj].split(",")
            inner_changed = 0
            pk = 0
            while pk < pnames_j.length
              if pk < ptypes_j.length
                new_t = scan_param_body_write_unify(bid_j, pnames_j[pk], ptypes_j[pk])
 # `def []=(name, value)` style: the body dispatches on
 # `name` and writes `value` into ivars of differing
 # types (e.g. `@id = value` AND `@name = value`, with
 # @id mrb_int and @name const char*). Without widening
 # the param to poly, the C emitter pins lv_value at the
 # first-observed type and rejects the other ivar's
 # assignment. Sniff for >1 distinct ivar slot type
 # observed as the RHS-is-pname target, and promote to
 # "poly" when found.
                if new_t != "poly" && mnames[mj] == "[]="
                  ivt_set = "".split(",")
                  collect_param_ivar_write_slot_types(bid_j, pnames_j[pk], ci, ivt_set)
                  if ivt_set.length >= 2
                    new_t = "poly"
                  end
                end
                if new_t != ptypes_j[pk]
                  ptypes_j[pk] = new_t
                  inner_changed = 1
                end
              end
              pk = pk + 1
            end
            if inner_changed == 1
              all_ptypes[mj] = ptypes_j.join(",")
              cls_changed = 1
            end
          end
        end
        mj = mj + 1
      end
      if cls_changed == 1
        @cls_meth_ptypes[ci] = all_ptypes.join("|")
        @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      end
      ci = ci + 1
    end
  end

 # Walk `nid` looking for `InstanceVariableWriteNode` whose RHS is
 # a `LocalVariableReadNode` named `pname`, and accumulate each
 # written-to ivar's declared slot type into `ivt_set` (unique).
 # Used by widen_param_types_from_body_writes to detect
 # `def []=(_, value); case ... when :a then @a = value;
 # when :b then @b = value; end` shapes where the param flows
 # into ivars of differing types — the param then needs to widen
  # to poly so each ivar-write arm can unbox/cast to its slot type.

  # Find the RHS type of the most recent write to `vname` in body
  # rooted at `nid`. Returns "" if not found.
  def find_var_write_rhs_type(nid, vname)
    if nid < 0
      return ""
    end
    if @nd_type[nid] == "LocalVariableWriteNode" && @nd_name[nid] == vname
      rhs = @nd_expression[nid]
      if rhs >= 0
        rt = infer_type(rhs)
        if rt != "" && rt != "int" && rt != "void"
          return rt
        end
      end
    end
    stmts = get_body_stmts(nid)
    k = 0
    while k < stmts.length
      rt = find_var_write_rhs_type(stmts[k], vname)
      if rt != ""
        return rt
      end
      k = k + 1
    end
    return ""
  end

  def infer_proc_blk_param_types
    @meth_blk_param_types = "".split(",")
    mi = 0
    while mi < @meth_param_names.length
      types = @meth_param_types[mi].split(",")
      if types.length > 0 && types.last == "proc"
        bpname = @meth_param_names[mi].split(",").last
        acc = "".split(",")
        collect_blk_call_arg_types(@meth_body_ids[mi], bpname, acc, @meth_body_ids[mi])
        @meth_blk_param_types.push(acc.join("|"))
      else
        @meth_blk_param_types.push("")
      end
      mi = mi + 1
    end
    @cls_cmeth_blk_param_types = "".split(",")
    ci = 0
    while ci < @cls_names.length
      if ci < @cls_cmeth_names.length
        cmnames = @cls_cmeth_names[ci].split(";")
        cm_bodies = @cls_cmeth_bodies[ci].split(";")
        cm_ptypes = @cls_cmeth_ptypes[ci].split("|")
        cmidx = 0
        while cmidx < cmnames.length
          if cmidx < cm_ptypes.length
            cpts = cm_ptypes[cmidx].split(",")
            if cpts.length > 0 && cpts.last == "proc"
              cpnames = cls_cmeth_pnames_get(ci, cmidx)
              bpname = cpnames[cpnames.length - 1]
              acc = "".split(",")
              body_id = cm_bodies[cmidx].to_i
              collect_blk_call_arg_types(body_id, bpname, acc, body_id)
              @cls_cmeth_blk_param_types.push(acc.join("|"))
            else
              @cls_cmeth_blk_param_types.push("")
            end
          else
            @cls_cmeth_blk_param_types.push("")
          end
          cmidx = cmidx + 1
        end
      end
      ci = ci + 1
    end
  end

  def collect_blk_call_arg_types(nid, bpname, acc, body_id = -1)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "call"
      recv = @nd_receiver[nid]
      if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode" && @nd_name[recv] == bpname
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          pi = 0
          while pi < arg_ids.length
            t = infer_type(arg_ids[pi])
            if t == "" || t == "int"
              arg_node = arg_ids[pi]
              if arg_node >= 0 && @nd_type[arg_node] == "LocalVariableReadNode"
                lvname = @nd_name[arg_node]
                lvt = find_var_type(lvname)
                if lvt != "" && lvt != "int"
                  t = lvt
                end
                if t == "int" && body_id >= 0
                  wtt = find_var_write_rhs_type(body_id, lvname)
                  if wtt != ""
                    t = wtt
                  end
                end
              end
            end
            while acc.length <= pi
              acc.push("")
            end
            if acc[pi] == ""
              acc[pi] = t
            elsif acc[pi] != t
              acc[pi] = "poly"
              @needs_rb_value = 1 if acc[pi] == "poly"
            end
            pi = pi + 1
          end
        end
        return
      end
    end
    stmts = get_body_stmts(nid)
    k = 0
    while k < stmts.length
      collect_blk_call_arg_types(stmts[k], bpname, acc, body_id)
      k = k + 1
    end
  end

  # Used by widen_param_types_from_body_writes to detect
  def collect_param_ivar_write_slot_types(nid, pname, ci, ivt_set)
    if nid < 0
      return
    end
    if @nd_type[nid] == "InstanceVariableWriteNode"
      rhs = @nd_expression[nid]
      if rhs >= 0 && @nd_type[rhs] == "LocalVariableReadNode" && @nd_name[rhs] == pname
        iname = @nd_name[nid]
        slot_t = cls_ivar_type(ci, iname)
        if slot_t != "" && not_in(slot_t, ivt_set) == 1
          ivt_set.push(slot_t)
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_param_ivar_write_slot_types(cs[k], pname, ci, ivt_set)
      k = k + 1
    end
  end

 # Walk under `nid` looking for `LocalVariableWriteNode`s targeting
 # `pname` and unify the literal-typed RHS with `cur_t`. Method-call
 # RHSes are skipped: this pass runs before return-type inference,
 # so `infer_type(<call>)` for an unanalyzed method falls through to
 # the `int` default and would falsely widen a string param to poly.
 # Literal RHSes (StringNode, IntegerNode, FloatNode, SymbolNode,
 # NilNode, true/false) are reliable enough to widen on.
  def scan_param_body_write_unify(nid, pname, cur_t)
    if nid < 0
      return cur_t
    end
 # Nested DefNode / ClassNode / ModuleNode bodies introduce
 # new bindings that may shadow `pname`. Don't cross.
    if @nd_type[nid] == "DefNode"
      return cur_t
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return cur_t
    end
    nt = @nd_type[nid]
    if nt == "LocalVariableWriteNode"
      if @nd_name[nid] == pname
        rhs = @nd_expression[nid]
        if rhs >= 0 && is_reliable_literal_for_widen(rhs) == 1
          at = infer_type(rhs)
          cur_t = unify_param_for_body_write(cur_t, at)
        end
      end
    end
    if nt == "MultiWriteNode"
      mw_targets = parse_id_list(@nd_targets[nid])
      mw_val = @nd_expression[nid]
      ti = 0
      mw_targets.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode" && @nd_name[tid] == pname
          if mw_val >= 0 && @nd_type[mw_val] == "ArrayNode"
            elems = parse_id_list(@nd_elements[mw_val])
            if ti < elems.length && is_reliable_literal_for_widen(elems[ti]) == 1
              slot_t = infer_type(elems[ti])
              cur_t = unify_param_for_body_write(cur_t, slot_t)
            end
          end
        end
        ti = ti + 1
      }
    end
 # Recurse into all child fields. Mirrors collect_param_methods'
 # walk shape so we don't miss conditional/loop branches.
    if @nd_body[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_body[nid], pname, cur_t)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      cur_t = scan_param_body_write_unify(stmts[k], pname, cur_t)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_expression[nid], pname, cur_t)
    end
    if @nd_predicate[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_predicate[nid], pname, cur_t)
    end
    if @nd_subsequent[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_subsequent[nid], pname, cur_t)
    end
    if @nd_else_clause[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_else_clause[nid], pname, cur_t)
    end
    if @nd_receiver[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_receiver[nid], pname, cur_t)
    end
    if @nd_arguments[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_arguments[nid], pname, cur_t)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      cur_t = scan_param_body_write_unify(args[k], pname, cur_t)
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      cur_t = scan_param_body_write_unify(conds[k], pname, cur_t)
      k = k + 1
    end
    if @nd_left[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_left[nid], pname, cur_t)
    end
    if @nd_right[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_right[nid], pname, cur_t)
    end
    if @nd_block[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_block[nid], pname, cur_t)
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      cur_t = scan_param_body_write_unify(elems[k], pname, cur_t)
      k = k + 1
    end
    if @nd_rescue_clause[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_rescue_clause[nid], pname, cur_t)
    end
    if @nd_ensure_clause[nid] >= 0
      cur_t = scan_param_body_write_unify(@nd_ensure_clause[nid], pname, cur_t)
    end
    cur_t
  end

 # `nid` is a literal whose type is reliable even before
 # return-type inference has run. Limits the body-write widening
 # to writes whose RHS is unambiguous, avoiding false widenings
 # driven by `infer_type` of unanalyzed method calls (which falls
 # through to the `int` default).
  def is_reliable_literal_for_widen(nid)
    if nid < 0
      return 0
    end
    nt = @nd_type[nid]
    if nt == "StringNode" || nt == "IntegerNode" || nt == "FloatNode" || nt == "SymbolNode" || nt == "NilNode" || nt == "TrueNode" || nt == "FalseNode"
      return 1
    end
    0
  end

 # Body-write unification rule. Distinct from `unify_call_types`
 # because the call-site rule treats `int` as a placeholder that
 # gets overwritten by any concrete `at` — that produces
 # `int + string → string`, which is fine for call-site widening
 # (`int` was a default) but wrong for body-write widening (the
 # call site really did pass int, so the slot must hold both shapes).
  def unify_param_for_body_write(pt_a, pt_b)
    if pt_a == pt_b
      return pt_a
    end
 # `nil` writes don't widen; `if x.nil?` body-side `x = sentinel`
 # is a common shape that mustn't push the slot to poly.
    if pt_a == "nil"
      return pt_b
    end
    if pt_b == "nil"
      return pt_a
    end
 # int + float coerce to float (numeric-compatible).
    if pt_a == "int" && pt_b == "float"
      return "float"
    end
    if pt_a == "float" && pt_b == "int"
      return "float"
    end
 # Genuinely incompatible: must hold both at runtime.
    @needs_rb_value = 1
    return "poly"
  end

 # Collect every method name called on `pname` anywhere under nid.
 # Used by parameter type inference to find the class that satisfies
 # ALL accesses, avoiding a single-reader match that ignores later
 # method calls on the same parameter.
  def collect_param_methods(nid, pname, acc)
    if nid < 0
      return
    end
 # Don't cross nested DefNode / ClassNode / ModuleNode bodies —
 # their locals introduce new bindings that may shadow `pname`.
 # An inner method's `pname` local would falsely contribute its
 # called methods to the outer param's observation set
 # (#450 cascade 1's family of boundary bugs).
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
    if @nd_type[nid] == "CallNode"
      recv = @nd_receiver[nid]
      if recv >= 0
        if @nd_type[recv] == "LocalVariableReadNode"
          if @nd_name[recv] == pname
            mname = @nd_name[nid]
            if not_in(mname, acc) == 1
              acc.push(mname)
            end
          end
        end
      end
    end
    if @nd_body[nid] >= 0
      collect_param_methods(@nd_body[nid], pname, acc)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      collect_param_methods(stmts[k], pname, acc)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      collect_param_methods(@nd_expression[nid], pname, acc)
    end
    if @nd_left[nid] >= 0
      collect_param_methods(@nd_left[nid], pname, acc)
    end
    if @nd_right[nid] >= 0
      collect_param_methods(@nd_right[nid], pname, acc)
    end
    if @nd_arguments[nid] >= 0
      collect_param_methods(@nd_arguments[nid], pname, acc)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      collect_param_methods(args[k], pname, acc)
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      collect_param_methods(@nd_receiver[nid], pname, acc)
    end
 # Walk into IfNode's predicate / else-branch and CaseNode's
 # predicate / when-conditions. Without these arms, `def update(p);
 # if p.title.nil?; ...; else self.title = p.title; end; end`
 # has `p.title` only inside the predicate + else — collect
 # would return [] and body-side type inference would leave `p`
 # at "int" while the int-class fallback silently picked an
 # arbitrary user class.
    if @nd_predicate[nid] >= 0
      collect_param_methods(@nd_predicate[nid], pname, acc)
    end
    if @nd_subsequent[nid] >= 0
      collect_param_methods(@nd_subsequent[nid], pname, acc)
    end
    if @nd_else_clause[nid] >= 0
      collect_param_methods(@nd_else_clause[nid], pname, acc)
    end
    if @nd_collection[nid] >= 0
      collect_param_methods(@nd_collection[nid], pname, acc)
    end
    if @nd_block[nid] >= 0
      collect_param_methods(@nd_block[nid], pname, acc)
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      collect_param_methods(elems[k], pname, acc)
      k = k + 1
    end
    parts = parse_id_list(@nd_parts[nid])
    k = 0
    while k < parts.length
      collect_param_methods(parts[k], pname, acc)
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      collect_param_methods(conds[k], pname, acc)
      k = k + 1
    end
  end

 # Collect every element type seen in `pname.push(elem)` or
 # `pname << elem` patterns under nid. The deferred-element-type
 # promotion pass uses this to decide what concrete typed-array a
 # parameter should be promoted to when callers all passed empty
 # `[]` literals.
  def collect_param_push_elem_types(nid, pname, acc)
    if nid < 0
      return
    end
 # Nested DefNode / ClassNode / ModuleNode bodies introduce
 # new bindings that may shadow `pname`. Don't cross.
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "push" || @nd_name[nid] == "<<"
        recv = @nd_receiver[nid]
        if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
          if @nd_name[recv] == pname
            args_id = @nd_arguments[nid]
            if args_id >= 0
              aargs = get_args(args_id)
              if aargs.length > 0
                at = infer_type(aargs[0])
                if not_in(at, acc) == 1
                  acc.push(at)
                end
              end
            end
          end
        end
      end
    end
    if @nd_body[nid] >= 0
      collect_param_push_elem_types(@nd_body[nid], pname, acc)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      collect_param_push_elem_types(stmts[k], pname, acc)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      collect_param_push_elem_types(@nd_expression[nid], pname, acc)
    end
    if @nd_left[nid] >= 0
      collect_param_push_elem_types(@nd_left[nid], pname, acc)
    end
    if @nd_right[nid] >= 0
      collect_param_push_elem_types(@nd_right[nid], pname, acc)
    end
    if @nd_arguments[nid] >= 0
      collect_param_push_elem_types(@nd_arguments[nid], pname, acc)
    end
    args2 = parse_id_list(@nd_args[nid])
    k = 0
    while k < args2.length
      collect_param_push_elem_types(args2[k], pname, acc)
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      collect_param_push_elem_types(@nd_receiver[nid], pname, acc)
    end
    if @nd_predicate[nid] >= 0
      collect_param_push_elem_types(@nd_predicate[nid], pname, acc)
    end
    if @nd_subsequent[nid] >= 0
      collect_param_push_elem_types(@nd_subsequent[nid], pname, acc)
    end
    if @nd_else_clause[nid] >= 0
      collect_param_push_elem_types(@nd_else_clause[nid], pname, acc)
    end
    if @nd_block[nid] >= 0
      collect_param_push_elem_types(@nd_block[nid], pname, acc)
    end
  end

 # Promote each top-level method parameter from int_array to a
 # concrete typed-array (str_array, float_array, sym_array) when
 # (a) every caller passed an empty `[]` literal (guarded by
 # @meth_param_empty[mi][k] == "1") and (b) the body's pushes on
 # that parameter all agree on a single concrete element type.
 # Both gates are required: a real-int_array caller without (a)
 # would be silently miscompiled, and a mixed-element body
 # without (b) should surface as a type error rather than pick
 # one arbitrarily.
  def infer_param_array_type_from_body
    iter = 0
    changed = 1
    while changed == 1 && iter < 4
      changed = 0
      iter = iter + 1
 # Top-level methods. Set up the method's scope so that
 # collect_param_push_elem_types' infer_type calls can resolve
 # other parameters (e.g. `buf.push(name)` where `name` is a
 # string-typed parameter on the same method).
      mi = 0
      while mi < @meth_names.length
        bid = @meth_body_ids[mi]
        if bid >= 0
          pnames = @meth_param_names[mi].split(",")
          ptypes = @meth_param_types[mi].split(",")
          empty_str = ""
          if mi < @meth_param_empty.length
            empty_str = @meth_param_empty[mi]
          end
          empties = empty_str.split(",")
          push_scope
          dj = 0
          while dj < pnames.length
            pt = "int"
            if dj < ptypes.length
              pt = ptypes[dj]
            end
            declare_var(pnames[dj], pt)
            dj = dj + 1
          end
          ml = "".split(",")
          mt = "".split(",")
          scan_locals(bid, ml, mt, pnames)
          lk = 0
          while lk < ml.length
            declare_var(ml[lk], mt[lk])
            lk = lk + 1
          end
          promoted = 0
          pk = 0
          while pk < pnames.length
            if pk < ptypes.length && pk < empties.length
              if empties[pk] == "1" && ptypes[pk] == "int_array"
                elem_acc = "".split(",")
                collect_param_push_elem_types(bid, pnames[pk], elem_acc)
                promoted_type = empty_array_promotion_for(elem_acc)
                if promoted_type != ""
                  ptypes[pk] = promoted_type
                  if promoted_type == "str_array"
                    @needs_str_array = 1
                  end
                  if promoted_type == "float_array"
                    @needs_float_array = 1
                  end
                  promoted = 1
                  changed = 1
                end
              end
            end
            pk = pk + 1
          end
          pop_scope
          if promoted == 1
            @meth_param_types[mi] = ptypes.join(",")
          end
        end
        mi = mi + 1
      end
 # Class methods (instance methods on user classes). Same
 # scope-setup so `buf.push(name)` resolves the param type.
      ci = 0
      while ci < @cls_names.length
        @current_class_idx = ci
        all_params = @cls_meth_params[ci].split("|")
        all_ptypes = @cls_meth_ptypes[ci].split("|")
        all_empty = @cls_meth_ptypes_empty[ci].split("|")
        bodies = @cls_meth_bodies[ci].split(";")
        cls_changed = 0
        mj = 0
        while mj < all_params.length
          bid = -1
          if mj < bodies.length
            bid = bodies[mj].to_i
          end
          if bid >= 0
            cm_pnames = all_params[mj].split(",")
            cm_ptypes = "".split(",")
            cm_empties = "".split(",")
            if mj < all_ptypes.length
              cm_ptypes = all_ptypes[mj].split(",")
            end
            if mj < all_empty.length
              cm_empties = all_empty[mj].split(",")
            end
            push_scope
            cdj = 0
            while cdj < cm_pnames.length
              cpt = "int"
              if cdj < cm_ptypes.length
                cpt = cm_ptypes[cdj]
              end
              declare_var(cm_pnames[cdj], cpt)
              cdj = cdj + 1
            end
            cml = "".split(",")
            cmt = "".split(",")
            scan_locals(bid, cml, cmt, cm_pnames)
            cmlk = 0
            while cmlk < cml.length
              declare_var(cml[cmlk], cmt[cmlk])
              cmlk = cmlk + 1
            end
            pk = 0
            cm_promoted = 0
            while pk < cm_pnames.length
              if pk < cm_ptypes.length && pk < cm_empties.length
                if cm_empties[pk] == "1" && cm_ptypes[pk] == "int_array"
                  elem_acc = "".split(",")
                  collect_param_push_elem_types(bid, cm_pnames[pk], elem_acc)
                  promoted_type = empty_array_promotion_for(elem_acc)
                  if promoted_type != ""
                    cm_ptypes[pk] = promoted_type
                    if promoted_type == "str_array"
                      @needs_str_array = 1
                    end
                    if promoted_type == "float_array"
                      @needs_float_array = 1
                    end
                    cm_promoted = 1
                    changed = 1
                  end
                end
              end
              pk = pk + 1
            end
            pop_scope
            if cm_promoted == 1
              all_ptypes[mj] = cm_ptypes.join(",")
              cls_changed = 1
            end
          end
          mj = mj + 1
        end
        if cls_changed == 1
          @cls_meth_ptypes[ci] = all_ptypes.join("|")
          @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
        end
        ci = ci + 1
      end
      @current_class_idx = -1
    end
  end

 # Body-side param narrowing (Stage 1 of the callee→caller direction
 # of type inference, complementing scan_new_calls' caller→callee
 # widening). For each method's params still typed at the default
 # `int`, walk the body for `param.<m>` calls. If <m> is defined
 # on exactly one user class (and isn't a common operator / built-
 # in-overlap method), the param's static type narrows to that
 # class. Conflicting strong signals leave the param at int.
 #
 # The narrow direction (int → obj_<C>) only fires when the caller-
 # side widening hasn't already pinned the param to a non-int type,
 # so it never overrides observation from a real call site. If a
 # later iteration's caller-side scan finds an actual int arg flowing
 # into this slot, unify_call_types will widen back to poly — net
 # effect no worse than skipping the narrow.
 #
 # Excluded method names cover operators (which user classes
 # routinely overload but built-in primitives also implement) and
 # the common Object/Enumerable surface (`length`, `each`, `to_s`,
 # ...) — when these match a user class they don't actually
 # discriminate from a built-in.
  def narrow_param_types_from_body_method_calls
 # Top-level methods.
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        observations = "".split(",")
        collect_param_method_calls(bid, pnames, observations)
        changed = 0
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length && ptypes[pk] == "int"
            target_cls = unify_param_class_from_observations(pnames[pk], observations)
            if target_cls >= 0
              ptypes[pk] = "obj_" + @cls_names[target_cls]
              changed = 1
            end
          end
          pk = pk + 1
        end
        if changed == 1
          @meth_param_types[mi] = ptypes.join(",")
        end
      end
      mi = mi + 1
    end
 # Class instance methods.
    ci = 0
    while ci < @cls_names.length
      all_params = @cls_meth_params[ci].split("|")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      bodies = @cls_meth_bodies[ci].split(";")
      cls_changed = 0
      mj = 0
      while mj < all_params.length
        bid = -1
        if mj < bodies.length
          bid = bodies[mj].to_i
        end
        if bid >= 0
          cm_pnames = all_params[mj].split(",")
          cm_ptypes = "".split(",")
          if mj < all_ptypes.length
            cm_ptypes = all_ptypes[mj].split(",")
          end
          observations = "".split(",")
          collect_param_method_calls(bid, cm_pnames, observations)
          m_changed = 0
          pk = 0
          while pk < cm_pnames.length
            if pk < cm_ptypes.length && cm_ptypes[pk] == "int"
              target_cls = unify_param_class_from_observations(cm_pnames[pk], observations)
              if target_cls >= 0
                cm_ptypes[pk] = "obj_" + @cls_names[target_cls]
                m_changed = 1
              end
            end
            pk = pk + 1
          end
          if m_changed == 1
            all_ptypes[mj] = cm_ptypes.join(",")
            cls_changed = 1
          end
        end
        mj = mj + 1
      end
      if cls_changed == 1
        @cls_meth_ptypes[ci] = all_ptypes.join("|")
        @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      end
      ci = ci + 1
    end
  end

 # Walk `nid` for CallNode whose receiver is a LocalVariableReadNode
 # naming one of the params. Each such site contributes one entry
 # `<pname>\t<mname>` to `observations`. Stops at nested DefNode
 # (those introduce their own scope with different params).
  def collect_param_method_calls(nid, pnames, observations)
    if nid < 0
      return
    end
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "CallNode"
      recv = @nd_receiver[nid]
      if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
        vname = @nd_name[recv]
        if not_in(vname, pnames) == 0
          mname = @nd_name[nid]
          observations.push(vname + "\t" + mname)
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_param_method_calls(cs[k], pnames, observations)
      k = k + 1
    end
  end

 # For all `<pname>\t<mname>` observations, find the unique user
 # class that defines `<mname>`. If every strong observation
 # (single-class match, not on the deny list) points to the same
 # class, return its index. Conflicting classes or no signal at
 # all return -1.
  def unify_param_class_from_observations(pname, observations)
    unique_class = -1
    has_signal = 0
    k = 0
    while k < observations.length
      tab = observations[k].index("\t")
      if tab >= 0
        obs_p = observations[k][0, tab]
        if obs_p == pname
          mname = observations[k][tab + 1, observations[k].length - tab - 1]
          if is_common_method_name(mname) == 0
            cls_count = 0
            cls_idx = -1
            ci = 0
            while ci < @cls_names.length
              if cls_find_method_direct(ci, mname) >= 0
                cls_count = cls_count + 1
                cls_idx = ci
              end
              ci = ci + 1
            end
            if cls_count == 1
              if unique_class == -1
                unique_class = cls_idx
                has_signal = 1
              elsif unique_class != cls_idx
                return -1
              end
            end
          end
        end
      end
      k = k + 1
    end
    if has_signal == 1
      return unique_class
    end
    -1
  end

 # Methods whose name doesn't reliably discriminate a user class
 # from a built-in receiver. Reuses `is_primitive_shared_method`
 # (the same list used to gate compile_int_class_fallback_expr)
 # so the body-side narrow stays consistent with the emit-time
 # int-recv fallback's safety net. Plus a small deny list of
 # numeric / boolean operators that the primitive-shared list
 # doesn't currently cover.
  def is_common_method_name(mname)
    if is_primitive_shared_method(mname) == 1
      return 1
    end
    if mname == "+" || mname == "-" || mname == "*" || mname == "/"
      return 1
    end
    if mname == "%" || mname == "**" || mname == "&" || mname == "|" || mname == "^" || mname == "~"
      return 1
    end
    if mname == "<" || mname == ">" || mname == "<=" || mname == ">=" || mname == "<=>"
      return 1
    end
    if mname == ">>"
      return 1
    end
    0
  end

 # Helper: given the set of element types observed in pname.push(...)
 # patterns, return the typed-array tag to promote to, or "" if the
 # observations don't agree on a single concrete type.
  def empty_array_promotion_for(elem_acc)
    if elem_acc.length != 1
      return ""
    end
    if elem_acc[0] == "string"
      return "str_array"
    end
    if elem_acc[0] == "float"
      return "float_array"
    end
    if elem_acc[0] == "symbol"
      return "sym_array"
    end
    if elem_acc[0] == "poly"
      @needs_rb_value = 1
      return "poly_array"
    end
    if elem_acc[0] == "proc"
      @needs_rb_value = 1
      return "poly_array"
    end
    if is_obj_type(elem_acc[0]) == 1
 # Homogeneous obj array — use a typed `<obj>_ptr_array` so reads
 # return a typed pointer (no sp_RbVal unbox needed at the call
 # site). Falls back to poly_array via normal widening when a
 # later mismatched-type write happens.
      return base_type(elem_acc[0]) + "_ptr_array"
    end
    ""
  end

 # Pick the concrete hash type for an ivar that was initialized as
 # the empty-hash default (`str_int_hash`) and is later written via
 # `@h[k] = v`. Returns "" when the (key, value) pair has no
 # matching concrete container — the caller leaves the ivar type
 # alone in that case.
  def promote_empty_hash_for(kt, vt)
    if kt == "string"
      if vt == "string"
        return "str_str_hash"
      end
      if vt == "int" || vt == "bool" || vt == "nil"
        return "str_int_hash"
      end
      return "str_poly_hash"
    end
    if kt == "symbol"
      if vt == "string"
        return "sym_str_hash"
      end
      if vt == "int" || vt == "bool" || vt == "nil"
        return "sym_int_hash"
      end
      return "sym_poly_hash"
    end
    if kt == "int"
      if vt == "string"
        return "int_str_hash"
      end
 # `kt == "int"` with `vt == "int"` is ambiguous: it could be a
 # real int-keyed-int-valued hash (no native variant, would go
 # to poly_poly_hash), OR it could be the analyzer's fallback
 # when the key expression's type couldn't be resolved on the
 # first pass (e.g. `parts[0]` where `parts` isn't declared
 # yet). Promoting to poly_poly_hash here is destructive: a
 # later pass that correctly resolves the key as "string" can't
 # downgrade poly_poly_hash back to str_int_hash. Defer to the
 # next pass instead. Non-int vt (array, obj, etc.) still
 # routes through the catch-all below since those are
 # unambiguous "real concrete write" signals.
      if vt == "int" || vt == "bool" || vt == "nil"
        return ""
      end
    end
 # Non-string / non-symbol / non-int key types: Method, IntArray,
 # generic obj_X, or already-poly. Use poly_poly_hash so the runtime
 # uses sp_RbVal-keyed eql? dispatch (Method instances dedup via the
 # codegen-emitted hash hook, identity for everything else).
    if kt != ""
      return "poly_poly_hash"
    end
    ""
  end

 # Block-param-aware lookup used by scan_locals's empty-hash
 # promotion. When `nid` reads a local variable that scan_locals
 # itself has just collected (e.g. a block param like `|k, v|`),
 # prefer the type recorded in `types[]` over `infer_type` —
 # the block param hasn't been `declare_var`'d in scope yet, so
 # a bare infer_type would fall back to "int" and break the
 # promotion of the surrounding `out[k] = v` write.
  def scan_locals_arg_type(nid, names, types, params)
    if nid >= 0 && @nd_type[nid] == "LocalVariableReadNode"
      lname = @nd_name[nid]
      k = 0
      while k < names.length
        if names[k] == lname
          if k < types.length
            return types[k]
          end
        end
        k = k + 1
      end
    end
    infer_type(nid)
  end

 # Does class `ci` provide `mname` as a reader, writer, or method?
 # Walks parent classes for inherited members.
 # Does class `ci` define `mname` directly — instance method, attr
 # reader, or attr writer — without walking the parent chain?
  def class_has_method_local(ci, mname)
    readers = @cls_attr_readers[ci].split(";")
    if not_in(mname, readers) == 0
      return 1
    end
    if mname.length > 1 && mname[mname.length - 1] == "="
      bname = mname[0, mname.length - 1]
      writers = @cls_attr_writers[ci].split(";")
      if not_in(bname, writers) == 0
        return 1
      end
    end
    mnames = @cls_meth_names[ci].split(";")
    if not_in(mname, mnames) == 0
      return 1
    end
    return 0
  end

  def class_has_method(ci, mname)
    if class_has_method_local(ci, mname) == 1
      return 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return class_has_method(pi, mname)
      end
    end
    return 0
  end

  def class_has_all_methods(ci, called)
    k = 0
    while k < called.length
      if class_has_method(ci, called[k]) == 0
        return 0
      end
      k = k + 1
    end
    return 1
  end

 # When picking a user class for a parameter from "what methods
 # does the body call on it", reject sets that consist only of
 # methods also available on built-in container types
 # (int_array / float_array / str_array / sym_array / ptr_array).
 # A user class that happens to define `def length` (and nothing
 # else) would otherwise get picked for any param the body calls
 # `.length` on — even when the actual call site passes an
 # IntArray. The body signal is too weak to commit; leave the
 # param at "int" so call-site unification (scan_new_calls)
 # decides.
 #
 # Specifically: is `mname` a method that's also defined on a
 # primitive type (String / Array / Hash / Integer) AND on at
 # least one user class? When both are true, an int-typed
 # receiver shouldn't pick the user class on the auto-cast
 # fallback path — that's almost certainly the wrong dispatch
 # (a param that flowed in as mrb_int because no upstream call
 # site pinned it, with the user's actual intent being the
 # primitive method).
  def primitive_method_shared_with_user_class(mname)
    return 0 unless is_primitive_shared_method(mname) == 1
 # Cheap guard: the user must define a class with this method.
    ci2 = 0
    while ci2 < @cls_names.length
      mns = @cls_meth_names[ci2].split(";")
      jj = 0
      while jj < mns.length
        if mns[jj] == mname
          return 1
        end
        jj = jj + 1
      end
      ci2 = ci2 + 1
    end
    0
  end

 # Whitelist of methods defined on built-in primitive types that
 # commonly collide with user-class method names. Mirrors (and
 # delegates the surface to) the predicate
 # `called_methods_only_on_container_builtins` introduced for
 # — same intent, applied at a different decision site.
  def is_primitive_shared_method(m)
    if m == "length" || m == "size" || m == "[]" || m == "[]=" ||
       m == "<<" || m == "push" || m == "pop" ||
       m == "shift" || m == "unshift" ||
       m == "first" || m == "last" ||
       m == "each" || m == "each_with_index" || m == "each_index" ||
       m == "map" || m == "map!" || m == "collect" ||
       m == "select" || m == "reject" || m == "filter" ||
       m == "count" || m == "include?" ||
       m == "empty?" || m == "any?" || m == "all?" || m == "none?" ||
       m == "sort" || m == "sort!" || m == "reverse" || m == "reverse!" ||
       m == "join" || m == "to_a" || m == "to_s" || m == "inspect" ||
       m == "find" || m == "find_index" ||
       m == "sum" || m == "min" || m == "max" || m == "minmax" ||
       m == "concat" || m == "flatten" || m == "uniq" || m == "compact" ||
       m == "slice" || m == "fetch" || m == "dig" ||
       m == "freeze" || m == "frozen?" || m == "dup" || m == "clone" ||
       m == "hash" || m == "class" || m == "tap" ||
       m == "==" || m == "!=" || m == "eql?" || m == "equal?" ||
       m == "nil?" || m == "is_a?" || m == "kind_of?" || m == "respond_to?" ||
 # String / Hash / Integer-shared (kept in sync with the
 # extension to called_methods_only_on_container_builtins).
       m == "index" || m == "rindex" || m == "match" || m == "match?" ||
       m == "scan" || m == "sub" || m == "gsub" || m == "tr" ||
       m == "split" || m == "chars" || m == "bytes" || m == "lines" ||
       m == "chomp" || m == "chop" || m == "strip" || m == "lstrip" || m == "rstrip" ||
       m == "upcase" || m == "downcase" || m == "capitalize" || m == "swapcase" ||
       m == "start_with?" || m == "end_with?" ||
       m == "to_i" || m == "to_f" || m == "to_sym" || m == "to_str" ||
       m == "chr" || m == "ord" || m == "bytesize" || m == "=~" ||
       m == "ljust" || m == "rjust" || m == "center" || m == "replace" || m == "clear" ||
       m == "keys" || m == "values" || m == "each_pair" || m == "each_key" || m == "each_value" ||
       m == "has_key?" || m == "has_value?" || m == "key?" || m == "value?" ||
       m == "merge" || m == "merge!" || m == "invert" ||
       m == "transform_keys" || m == "transform_values" || m == "delete" ||
       m == "succ" || m == "next" || m == "pred" || m == "digits" || m == "bit_length" ||
       m == "times" || m == "upto" || m == "downto" || m == "step" ||
       m == "abs" || m == "divmod" || m == "gcd" || m == "lcm" ||
       m == "even?" || m == "odd?" || m == "zero?" || m == "positive?" || m == "negative?"
      return 1
    end
    0
  end

  def called_methods_only_on_container_builtins(called)
    k = 0
    while k < called.length
      m = called[k]
      if m != "length" && m != "size" && m != "[]" && m != "[]=" &&
         m != "<<" && m != "push" && m != "pop" &&
         m != "shift" && m != "unshift" &&
         m != "first" && m != "last" &&
         m != "each" && m != "each_with_index" && m != "each_index" &&
         m != "map" && m != "map!" && m != "collect" &&
         m != "select" && m != "reject" && m != "filter" &&
         m != "count" && m != "include?" &&
         m != "empty?" && m != "any?" && m != "all?" && m != "none?" &&
         m != "sort" && m != "sort!" && m != "reverse" && m != "reverse!" &&
         m != "join" && m != "to_a" && m != "to_s" && m != "inspect" &&
         m != "find" && m != "find_index" &&
         m != "sum" && m != "min" && m != "max" && m != "minmax" &&
         m != "concat" && m != "flatten" && m != "uniq" && m != "compact" &&
         m != "slice" && m != "fetch" && m != "dig" &&
 # Universal Object methods
         m != "freeze" && m != "frozen?" && m != "dup" && m != "clone" &&
         m != "hash" && m != "class" && m != "tap" &&
         m != "==" && m != "!=" && m != "eql?" && m != "equal?" &&
         m != "nil?" && m != "is_a?" && m != "kind_of?" && m != "respond_to?" &&
 # Generic operators that exist on builtin numeric/container types
 # too — `+` is concat on Array/String, arithmetic on Numeric;
 # a param using just `+` is no more "user-classy" than `length`.
         m != "+" && m != "-" && m != "*" && m != "/" && m != "%" &&
         m != "&" && m != "|" && m != "^" && m != "~" &&
         m != "<" && m != ">" && m != "<=" && m != ">=" && m != "<=>" &&
         m != "===" && m != "!" &&
 # Methods shared across String / Hash / Integer primitive
 # types that the body-side inference must NOT use as
 # evidence of a user-class receiver. The Rails pattern of
 # `def index` / `def show` / `def create` on every
 # controller collides with `String#index` etc.; the
 # canonical caller `s.index("[")` on a string would
 # otherwise be routed to whichever user-class `index` the
 # inference picked first.
         m != "index" && m != "rindex" && m != "match" && m != "match?" &&
         m != "scan" && m != "sub" && m != "gsub" && m != "tr" &&
         m != "split" && m != "chars" && m != "bytes" && m != "lines" &&
         m != "chomp" && m != "chop" && m != "strip" && m != "lstrip" && m != "rstrip" &&
         m != "upcase" && m != "downcase" && m != "capitalize" && m != "swapcase" &&
         m != "start_with?" && m != "end_with?" &&
         m != "to_i" && m != "to_f" && m != "to_sym" && m != "to_str" &&
         m != "chr" && m != "ord" && m != "bytesize" && m != "=~" &&
         m != "ljust" && m != "rjust" && m != "center" && m != "replace" && m != "clear" &&
 # Hash-shared
         m != "keys" && m != "values" && m != "each_pair" && m != "each_key" && m != "each_value" &&
         m != "has_key?" && m != "has_value?" && m != "key?" && m != "value?" &&
         m != "merge" && m != "merge!" && m != "invert" &&
         m != "transform_keys" && m != "transform_values" && m != "delete" &&
 # Integer-shared (`succ` is also Range/String etc.)
         m != "succ" && m != "next" && m != "pred" && m != "digits" && m != "bit_length" &&
         m != "times" && m != "upto" && m != "downto" && m != "step" &&
         m != "abs" && m != "divmod" && m != "gcd" && m != "lcm" &&
         m != "even?" && m != "odd?" && m != "zero?" && m != "positive?" && m != "negative?"
        return 0
      end
      k = k + 1
    end
    return 1
  end

  def infer_ivar_types_from_writers
 # Set up main scope for type inference
    push_scope
    stmts = get_body_stmts(@root_id)
    lnames = "".split(",")
    ltypes = "".split(",")
    empty_p = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            if @nd_type[sid] != "ModuleNode"
              scan_locals(sid, lnames, ltypes, empty_p)
            end
          end
        end
      end
    }
    k = 0
    while k < lnames.length
      declare_var(lnames[k], ltypes[k])
      k = k + 1
    end
 # Also scan inside method bodies
    i = 0
    while i < @meth_names.length
      push_scope
 # Pin @current_method_name so current_lexical_scope_name can pull
 # the module prefix out of `<Mod>_cls_<m>` style names. Without
 # this, a `Foo.new` inside e.g. `Optcarrot::Driver.load` resolves
 # `Foo` against the empty scope and lands on bare `Foo` instead
 # of `Optcarrot_Foo`, which then poisons the local variable's
 # recorded type.
      saved_meth = @current_method_name
      @current_method_name = @meth_names[i]
      pnames = @meth_param_names[i].split(",")
      ptypes = @meth_param_types[i].split(",")
      j = 0
      while j < pnames.length
        pt = "int"
        if j < ptypes.length
          pt = ptypes[j]
        end
        declare_var(pnames[j], pt)
        j = j + 1
      end
      if @meth_body_ids[i] >= 0
        ml = "".split(",")
        mt = "".split(",")

        scan_locals(@meth_body_ids[i], ml, mt, pnames)
        lk = 0
        while lk < ml.length
          declare_var(ml[lk], mt[lk])
          lk = lk + 1
        end
        @cur_writer_body = @meth_body_ids[i]
        scan_writer_calls(@meth_body_ids[i])
        @cur_writer_body = -1
      end
      @current_method_name = saved_meth
      pop_scope
      i = i + 1
    end
 # Scan class instance method bodies
    ci = 0
    while ci < @cls_names.length
      @current_class_idx = ci
      bodies = @cls_meth_bodies[ci].split(";")
      mnames = @cls_meth_names[ci].split(";")
      bj = 0
      while bj < bodies.length
        bid = bodies[bj].to_i
        if bid >= 0
          push_scope
          pnames2 = cls_meth_pnames_get(ci, bj)
          ptypes2 = cls_meth_ptypes_get(ci, bj)
          pk = 0
          while pk < pnames2.length
            pt = "int"
            if pk < ptypes2.length
              pt = ptypes2[pk]
            end
            declare_var(pnames2[pk], pt)
            pk = pk + 1
          end
          ml2 = "".split(",")
          mt2 = "".split(",")
          scan_locals(bid, ml2, mt2, pnames2)
          lk2 = 0
          while lk2 < ml2.length
            declare_var(ml2[lk2], mt2[lk2])
            lk2 = lk2 + 1
          end
          @cur_writer_body = bid
          scan_writer_calls(bid)
          @cur_writer_body = -1
          pop_scope
        end
        bj = bj + 1
      end
 # Also scan class method bodies (def self.<m>) so an
 # attr_writer call on a freshly-`new`'d instance inside a
 # `def self.from_raw(...)` factory widens the ivar's type.
      cm_bodies = @cls_cmeth_bodies[ci].split(";")
      cm_names = @cls_cmeth_names[ci].split(";")
      saved_meth = @current_method_name
      cbj = 0
      while cbj < cm_bodies.length
        cbid = cm_bodies[cbj].to_i
        if cbid >= 0
 # Pin @current_method_name to the "<Class>_cls_<m>" form
 # used by current_class_method_owning_class so implicit
 # bare `new` inside the body resolves to obj_<Class>.
          if cbj < cm_names.length
            @current_method_name = @cls_names[ci] + "_cls_" + cm_names[cbj]
          end
          push_scope
          cpnames = cls_cmeth_pnames_get(ci, cbj)
          cptypes = cls_cmeth_ptypes_get(ci, cbj)
          cpk = 0
          while cpk < cpnames.length
            cpt = "int"
            if cpk < cptypes.length
              cpt = cptypes[cpk]
            end
            declare_var(cpnames[cpk], cpt)
            cpk = cpk + 1
          end
          cml = "".split(",")
          cmt = "".split(",")
          scan_locals(cbid, cml, cmt, cpnames)
          clk = 0
          while clk < cml.length
            declare_var(cml[clk], cmt[clk])
            clk = clk + 1
          end
          @cur_writer_body = cbid
          scan_writer_calls(cbid)
          @cur_writer_body = -1
          pop_scope
        end
        cbj = cbj + 1
      end
      @current_method_name = saved_meth
      ci = ci + 1
    end
    @current_class_idx = -1
 # Scan main-level code
    scan_writer_calls(@root_id)
    pop_scope
 # Every writer's concrete type observation has been recorded
 # into @cls_ivar_observed_types[ci] (deduped per slot); widen
 # any slot with 2+ distinct concrete types to poly. The
 # narrow-then-overwrite path in update_ivar_type can otherwise
 # leave the slot pinned to whichever writer's update_ivar_type
 # call ran last, causing the loser's emit site to type-mismatch.
    finalize_ivar_heterogeneity
  end

 # Drop "obj_<bare>" observations from `obs` when (a) <bare> doesn't
 # resolve to any registered class, AND (b) some other observation in
 # the list is "obj_<scope>_<bare>" for the same trailing <bare> and
 # <scope>_<bare> IS registered. Caller passes the raw split list of
 # observed types; we return a filtered list with stale unqualified
 # obj-names removed (their qualified peer carries the same slot type
 # information). Used by finalize_ivar_heterogeneity so the
 # distinct-count poly-widen decision doesn't flip a single-class
 # ivar to poly just because Pass 1 recorded the bare name and a
 # later pass recorded the qualified one.
  def drop_stale_unqualified_obj_obs(obs)
    out = "".split(",")
 # Type-prime locals as strings so spinel-self-compile picks the
 # right C type (sp_str_sub_range / sp_str_concat returns
 # `const char *`; without the empty-string seed scan_locals
 # falls back to `mrb_int`, which then fails the `-Wint-conversion`
 # bootstrap when this method is invoked from finalize_ivar_heterogeneity).
    o = ""
    p = ""
    ob = ""
    pb = ""
    sfx = ""
    tail = ""
    oi = 0
    while oi < obs.length
      o = obs[oi]
      keep = 1
      if is_obj_type(o) == 1
        ob = o[4, o.length - 4]
        if find_class_idx(ob) < 0
          oj = 0
          while oj < obs.length
            if oj != oi
              p = obs[oj]
              if is_obj_type(p) == 1
                pb = p[4, p.length - 4]
                if find_class_idx(pb) >= 0
                  sfx = "_" + ob
                  if pb.length > sfx.length
                    tail = pb[(pb.length - sfx.length), sfx.length]
                    if tail == sfx
                      keep = 0
                    end
                  end
                end
              end
            end
            oj = oj + 1
          end
        end
      end
      if keep == 1
        out.push(o)
      end
      oi = oi + 1
    end
    out
  end

  def finalize_ivar_heterogeneity
    ci = 0
    while ci < @cls_names.length
      names = @cls_ivar_names[ci].split(";")
      types = @cls_ivar_types[ci].split(";")
      obs = @cls_ivar_observed_types[ci].split(";", -1)
      changed = 0
      ivk = 0
      while ivk < names.length
        if ivk < types.length && ivk < obs.length && types[ivk] != "poly"
          distinct = obs[ivk].split(",")
 # Collapse stale unqualified obj-name observations against
 # their qualified form before the distinct-count widening
 # decision. A first-pass `@x = Foo.new(...)` recorded inside
 # a class whose sibling `Foo` hadn't been registered yet
 # stamped "obj_Foo" into the observation list; a later pass
 # (with all classes registered) records the proper
 # "obj_<scope>_Foo". They refer to the same class, but the
 # raw distinct-count below would treat them as 2 types and
 # widen the slot to poly. Mirror the update_ivar_type /
 # unify_call_types normalization.
          distinct = drop_stale_unqualified_obj_obs(distinct)
 # Collapse nullable-and-non-nullable variants of the same
 # base type to a single (nullable) entry. `@x = "hi"` (string)
 # in initialize + `@x = some_nullable_string_param` in a
 # setter are semantically the same shape — both store a
 # string, the second one optionally nil. Counting them as 2
 # distinct observations widens the ivar to poly even though
 # the slot can stay `const char *` with a NULL sentinel.
 # Without this, callers that read the slot through an attr_
 # reader then forward into another method's typed param see a
 # poly value and degrade every downstream slot.
          collapsed = "".split(",")
          ck = 0
          while ck < distinct.length
            cur = distinct[ck]
            cur_base = base_type(cur)
            cm = 0
            seen = -1
            while cm < collapsed.length
              if base_type(collapsed[cm]) == cur_base
                seen = cm
                cm = collapsed.length
              else
                cm = cm + 1
              end
            end
            if seen < 0
              collapsed.push(cur)
            else
 # Prefer the nullable form so subsequent reads keep the
 # null-check semantics callers may have relied on.
              if is_nullable_type(cur) == 1 && is_nullable_type(collapsed[seen]) == 0
                collapsed[seen] = cur
              end
            end
            ck = ck + 1
          end
          distinct = collapsed
          if distinct.length == 1 && types[ivk] != distinct[0]
            if base_type(types[ivk]) == base_type(distinct[0])
              types[ivk] = distinct[0]
              changed = 1
            end
          end
          if distinct.length >= 2
 # When every observed type is itself an array AND the
 # current type is already `poly_array` (set by an earlier
 # `[]=` widening — see scan_writer_calls), keep it as
 # `poly_array` rather than collapsing to `poly`. The
 # latter discards element-array semantics: reads on a
 # `poly` slot dont dispatch through cls_id_to_storage,
 # they box-and-unbox the entire ivar value. Optcarrot's
 # `@fetch[a][a]` (heterogeneous IntArray + Method) only
 # works when @fetch stays `poly_array`. For other
 # combinations (mix of array and non-array, or arrays
 # without prior poly_array marker), keep the existing
 # widen-to-poly behavior.
            all_arrays = 1
            di = 0
            while di < distinct.length
              if is_array_type(distinct[di]) == 0
                all_arrays = 0
              end
              di = di + 1
            end
            if all_arrays == 1 && types[ivk] == "poly_array"
 # Already poly_array; do nothing — keep it.
            elsif all_arrays == 1
 # Compatible array shapes but not yet poly_array; widen.
              types[ivk] = "poly_array"
              @needs_rb_value = 1
              @needs_gc = 1
              changed = 1
            else
              types[ivk] = "poly"
              @needs_rb_value = 1
              changed = 1
            end
          end
        end
        ivk = ivk + 1
      end
      if changed == 1
        @cls_ivar_types[ci] = types.join(";")
        @cls_ivar_types_version = @cls_ivar_types_version + 1
      end
      ci = ci + 1
    end
  end

 # when `mname` is an instance method on class `ci`
 # whose body returns a bare `@<iname>` as its last expression,
 # return that ivar name. Lets scan_writer_calls treat
 # `<getter>.push(v)` and `<getter> << v` as if the push were
 # against the ivar directly. Returns "" when the method
 # doesn't match the simple getter shape or isn't defined on
 # the class.
  def method_returns_ivar_in_class(ci, mname)
    if ci < 0
      return ""
    end
    midx = cls_find_method_direct(ci, mname)
    if midx < 0
      return ""
    end
    bodies = @cls_meth_bodies[ci].split(";")
    if midx >= bodies.length
      return ""
    end
    bid = bodies[midx].to_i
    if bid < 0
      return ""
    end
    stmts = get_stmts(bid)
    if stmts.length == 0
      return ""
    end
    last = stmts[stmts.length - 1]
    if @nd_type[last] == "InstanceVariableReadNode"
      return @nd_name[last]
    end
    ""
  end

  def scan_writer_calls(nid)
    bname = ""
    if nid < 0
      return
    end
 # Direct ivar write: @left = expr (inside class methods)
    if @nd_type[nid] == "InstanceVariableWriteNode"
      if @current_class_idx >= 0
        iname = @nd_name[nid]
        expr_id = @nd_expression[nid]
 # Drill through chained `@a = @b = ... = expr` so every chain
 # participant sees the bottom rhs type — infer_type returns
 # the default "int" for an InstanceVariableWriteNode expr,
 # which would leave participants un-widened against a real
 # concrete bottom type and force compile_stmt to emit
 # type-mismatched stores. collecting *every*
 # participant (not just the head) is required because a
 # CallNode rhs (`@a = @b = make_int`) bypasses scan_ivars's
 # dual-definite-literal widening and tail slots stay at
 # their pre-existing concrete type.
        chain_inames = "".split(",")
        chain_inames.push(iname)
        bottom = expr_id
        while bottom >= 0 && @nd_type[bottom] == "InstanceVariableWriteNode"
          chain_inames.push(@nd_name[bottom])
          bottom = @nd_expression[bottom]
        end
 # Empty `{}` / `[]` literal: don't reset the ivar's tracked
 # type to the default (`str_int_hash` / `int_array`), since a
 # later `[]=` write may have already promoted the slot to a
 # more specific type. Reseeding from the empty-default would
 # widen the promoted type to poly on the next iteration.
        if is_empty_hash_literal(bottom) == 0 && is_empty_array_literal(bottom) == 0
          at = infer_type(bottom)
 # `@ivar = <local>` where <local>'s slot type is "int_array"
 # (the empty-array default that scan_locals records for `x =
 # []`) often promotes later via push observations within the
 # same body — `results = []; results << Article.new;
 # @articles = results`. infer_type at scan time only sees the
 # current scope-recorded type (int_array), so the ivar pins
 # at int_array even though the assignment is meant to carry a
 # ptr_array of obj. Look at the body's push elements to derive
 # the promoted type before recording.
          if at == "int_array" && @cur_writer_body >= 0 && @nd_type[bottom] == "LocalVariableReadNode"
            lname_a = @nd_name[bottom]
            elem_acc_a = "".split(",")
            collect_param_push_elem_types(@cur_writer_body, lname_a, elem_acc_a)
            promoted_a = empty_array_promotion_for(elem_acc_a)
            if promoted_a != ""
              at = promoted_a
            end
          end
 # Record this writer's concrete type observation while
 # the local-scope context is set up (params declared at
 # their iteratively-widened ptypes). After all
 # writer-scans finish, finalize_ivar_heterogeneity reads
 # the accumulated per-slot list and widens to poly when
 # 2+ distinct concrete types appear.
          record_ivar_observation(@current_class_idx, iname, at, bottom)
 # Chain participants bypass the `int` guard so that
 # `@string_slot = @int_slot = expr_returning_int` widens
 # the head to poly instead of leaving it stuck at
 # `string`. The `nil` guard stays in place even for
 # chains: `@a = @b = ... = nil` is handled at emit time
 # by `compile_chained_ivar_writes`'s per-slot recurse
 # path (NilNode lowers to a literal `0`, a null pointer
 # constant valid for any slot type), and forcing nil
 # into the slot type interacts badly with parent-cascade
 # update_ivar_type — a subclass write that pins the same
 # slot to a concrete obj type ping-pongs between obj_X
 # and obj_X? across iter rounds and lands on poly,
 # breaking the typed-pointer store at emit time.
          if chain_inames.length > 1 && at == "int"
            ci_idx = 0
            while ci_idx < chain_inames.length
              update_ivar_type(@current_class_idx, chain_inames[ci_idx], at)
              ci_idx = ci_idx + 1
            end
          elsif at != "int" && at != "nil"
            update_ivar_type(@current_class_idx, iname, at)
          end
        end
      end
    end
 # `@x ||= expr` / `@x &&= expr`: writer-scan the rhs the same as a
 # plain `@x = expr` so the slot widens from its `nil` seed to the
 # rhs's actual type (`infer_type` resolves CallNode return types,
 # unlike `infer_ivar_init_type` used at registration time).
    if @nd_type[nid] == "InstanceVariableOrWriteNode" || @nd_type[nid] == "InstanceVariableAndWriteNode"
      if @current_class_idx >= 0
        iname = @nd_name[nid]
        expr_id = @nd_expression[nid]
        if expr_id >= 0
          at = infer_type(expr_id)
          record_ivar_observation(@current_class_idx, iname, at, expr_id)
          if at != "int" && at != "nil"
            update_ivar_type(@current_class_idx, iname, at)
          end
        end
      end
    end
 # Multi-write to ivars: `@a, @b = pulse_0, pulse_1`. Mirrors the
 # single-write branch above so each ivar's type is widened from
 # the corresponding RHS slot. Without this the multi-write left
 # the ivars at their initial "int" guess and the struct came out
 # with mrb_int fields that the assigning method then tried to
 # overwrite with pointer values.
    if @nd_type[nid] == "MultiWriteNode"
      if @current_class_idx >= 0
        targets_mw = parse_id_list(@nd_targets[nid])
        val_mw = @nd_expression[nid]
        ti_mw = 0
        while ti_mw < targets_mw.length
          tid_mw = targets_mw[ti_mw]
          if @nd_type[tid_mw] == "InstanceVariableTargetNode"
            iname_mw = @nd_name[tid_mw]
            at_mw = scan_ivars_multi_target_type(val_mw, ti_mw)
            if at_mw != "int" && at_mw != "nil"
              update_ivar_type(@current_class_idx, iname_mw, at_mw)
            end
          end
          ti_mw = ti_mw + 1
        end
      end
    end
 # IndexOrWriteNode (`recv[idx] ||= val`) — scan widens the hash type
 # the same way the regular `[]=` CallNode path does. Without this,
 # optcarrots `entries = {}; entries[k] ||= [...]` leaves `entries`
 # at the str_int_hash default, so reads return int (lost cls_id)
 # and downstream `(0..N).map { entries[k] }` collapses to IntArray.
    if @nd_type[nid] == "IndexOrWriteNode" || @nd_type[nid] == "IndexAndWriteNode" || @nd_type[nid] == "IndexOperatorWriteNode"
      iow_recv = @nd_receiver[nid]
      iow_args_id = @nd_arguments[nid]
      iow_val = @nd_expression[nid]
      if iow_recv >= 0 && iow_args_id >= 0 && iow_val >= 0
        iow_args = get_args(iow_args_id)
        if iow_args.length >= 1
          iow_kt = infer_type(iow_args[0])
          iow_vt = infer_type(iow_val)
 # Widen the recv's hash type if it's still the empty-default
          if @nd_type[iow_recv] == "InstanceVariableReadNode" && @current_class_idx >= 0
            iow_iname = @nd_name[iow_recv]
            iow_cur = cls_ivar_type(@current_class_idx, iow_iname)
            if iow_cur == "str_int_hash"
              iow_promoted = promote_empty_hash_for(iow_kt, iow_vt)
              if iow_promoted != "" && iow_promoted != iow_cur
                replace_ivar_type(@current_class_idx, iow_iname, iow_promoted)
                if iow_promoted == "str_poly_hash" || iow_promoted == "sym_poly_hash"
                  @needs_rb_value = 1
                end
                if iow_promoted == "poly_poly_hash"
                  @needs_poly_poly_hash = 1
                  @needs_rb_value = 1
                end
              end
            end
          elsif @nd_type[iow_recv] == "LocalVariableReadNode"
            iow_lname = @nd_name[iow_recv]
            iow_cur = find_var_type(iow_lname)
            if iow_cur == "str_int_hash"
              iow_promoted = promote_empty_hash_for(iow_kt, iow_vt)
              if iow_promoted != "" && iow_promoted != iow_cur
                set_var_type(iow_lname, iow_promoted)
                if iow_promoted == "str_poly_hash" || iow_promoted == "sym_poly_hash"
                  @needs_rb_value = 1
                end
                if iow_promoted == "poly_poly_hash"
                  @needs_poly_poly_hash = 1
                  @needs_rb_value = 1
                end
              end
            end
          end
        end
      end
    end
    if @nd_type[nid] == "CallNode"
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
      if recv >= 0
        if mname.length > 1
          if mname[mname.length - 1] == "="
            bname = mname[0, mname.length - 1]
            rt = infer_type(recv)
            if is_obj_type(rt) == 1
              cname = rt[4, rt.length - 4]
              ci = find_class_idx(cname)
              if ci >= 0
                writers = @cls_attr_writers[ci].split(";")
                wk = 0
                while wk < writers.length
                  if writers[wk] == bname
                    iname = "@" + bname
                    args_id = @nd_arguments[nid]
                    if args_id >= 0
                      arg_ids = get_args(args_id)
                      if arg_ids.length > 0
                        at = infer_type(arg_ids[0])
                        if at != "int" && at != "nil"
                          update_ivar_type(ci, iname, at)
                        end
                      end
                    end
                  end
                  wk = wk + 1
                end
              end
            end
          end
        end
      end
 # `@h[k] = v` against an ivar still typed as the empty-hash
 # default (str_int_hash) — promote based on the actual key/value
 # types so the codegen picks the matching `sp_*Hash_set` (issue
 # #64). Only the empty-default → another concrete hash type
 # transition; richer mismatches stay where they are.
      if mname == "[]=" && @current_class_idx >= 0 && recv >= 0 && @nd_type[recv] == "InstanceVariableReadNode"
        iname = @nd_name[recv]
        cur_t = cls_ivar_type(@current_class_idx, iname)
        if cur_t == "str_int_hash"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            ai = get_args(args_id)
            if ai.length >= 2
              kt = infer_type(ai[0])
              vt = infer_type(ai[ai.length - 1])
              promoted = promote_empty_hash_for(kt, vt)
              if promoted != "" && promoted != cur_t
 # Direct assign: update_ivar_type would widen the
 # existing-vs-new mismatch to `poly`, but we know this
 # transition is just refining the empty-hash default.
                replace_ivar_type(@current_class_idx, iname, promoted)
 # Mark the runtime feature as needed before emit_features
 # runs, so the corresponding `sp_*Hash_*` helpers are
 # emitted into the generated C.
                if promoted == "str_str_hash"
                  @needs_str_str_hash = 1
                elsif promoted == "int_str_hash"
                  @needs_int_str_hash = 1
                elsif promoted == "sym_int_hash"
                  @needs_sym_int_hash = 1
                elsif promoted == "sym_str_hash"
                  @needs_sym_str_hash = 1
                elsif promoted == "str_poly_hash"
                  @needs_rb_value = 1
                elsif promoted == "sym_poly_hash"
                  @needs_rb_value = 1
                elsif promoted == "poly_poly_hash"
                  @needs_poly_poly_hash = 1
                  @needs_rb_value = 1
                end
              end
            end
          end
        end
      end
 # push through a getter method that returns the
 # ivar. Recognise the canonical "lazy init + return" shape:
 #
 # def errors
 # @errors = [] if @errors.nil?
 # @errors
 # end
 # ...
 # errors << "bad"
 #
 # The recv of `<<` is a bare call to `errors`, not the ivar
 # directly. Resolve through that to the underlying ivar so
 # the IntArray default (from `[]`) promotes to the correct
 # element-typed array when the push args are non-int.
      push_alias_iname = ""
      push_alias_owner_ci = -1
      if (mname == "push" || mname == "<<") && @current_class_idx >= 0 && recv >= 0
        getter_mname_x = ""
        if @nd_type[recv] == "CallNode" && @nd_receiver[recv] < 0
          getter_mname_x = @nd_name[recv]
        end
        if @nd_type[recv] == "CallNode" && @nd_receiver[recv] >= 0 && @nd_type[@nd_receiver[recv]] == "SelfNode"
          getter_mname_x = @nd_name[recv]
        end
        if getter_mname_x != ""
 # Walk the parent chain so a getter defined on a parent
 # class (Rails-style `def errors; @errors = [] if
 # @errors.nil?; @errors; end` on Base) is matched when the
 # push happens from a subclass method. Without the walk
 # (#430 only inspected @current_class_idx) the parent's
 # ivar stays IntArray and the subclass's String push fails
 # C compile (#451).
          search_ci = @current_class_idx
          while search_ci >= 0
            iname_try = method_returns_ivar_in_class(search_ci, getter_mname_x)
            if iname_try != ""
              push_alias_iname = iname_try
              push_alias_owner_ci = search_ci
              break
            end
            parent_name = @cls_parents[search_ci]
            if parent_name == ""
              search_ci = -1
            else
              search_ci = find_class_idx(parent_name)
            end
          end
        end
      end
      if (mname == "push" || mname == "<<") && push_alias_owner_ci >= 0 && push_alias_iname != ""
        iname = push_alias_iname
        cur_t = cls_ivar_type(push_alias_owner_ci, iname)
        if cur_t == "int_array"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            ai = get_args(args_id)
            if ai.length > 0
              et = infer_type(ai[0])
              promoted = empty_array_promotion_for([et])
              if promoted != "" && promoted != cur_t
                replace_ivar_type(push_alias_owner_ci, iname, promoted)
                if promoted == "str_array"
                  @needs_str_array = 1
                elsif promoted == "float_array"
                  @needs_float_array = 1
                elsif promoted == "sym_array"
                  @needs_sym_intern = 1
                elsif promoted == "poly_array"
                  @needs_poly_array = 1
                  @needs_rb_value = 1
                end
              end
            end
          end
        end
      end
      if (mname == "push" || mname == "<<") && @current_class_idx >= 0 && recv >= 0 && @nd_type[recv] == "InstanceVariableReadNode"
        iname = @nd_name[recv]
        cur_t = cls_ivar_type(@current_class_idx, iname)
        if cur_t == "int_array"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            ai = get_args(args_id)
            if ai.length > 0
              et = infer_type(ai[0])
              promoted = empty_array_promotion_for([et])
              if promoted != "" && promoted != cur_t
                replace_ivar_type(@current_class_idx, iname, promoted)
                if promoted == "str_array"
                  @needs_str_array = 1
                elsif promoted == "float_array"
                  @needs_float_array = 1
                elsif promoted == "sym_array"
                  @needs_int_array = 1
                elsif promoted == "poly_array"
                  @needs_rb_value = 1
                  @needs_gc = 1
                elsif is_ptr_array_type(promoted) == 1
 # ptr_array slots hold object pointers, so the
 # owning class needs gc_scan emitted; without
 # this flag the captured pointers leak and the
 # collector misses them. Gemini review.
                  @needs_gc = 1
                end
              end
            end
          end
        end
      end
 # `@arr[i] = v` against an ivar typed `int_array` (the
 # `[nil] * N` empty default) should widen to match the value
 # type the same way `<<` / `push` does. Without this, optcarrot's
 # `@fetch[addr] = method(:peek_X)` writes a Method into an
 # int_array slot — the read side then can't recover and `[].call`
 # dispatches against int.
      if mname == "[]=" && @current_class_idx >= 0 && recv >= 0 && @nd_type[recv] == "InstanceVariableReadNode"
        iname = @nd_name[recv]
        cur_t = cls_ivar_type(@current_class_idx, iname)
        promoted = ""
        if cur_t == "int_array"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            ai = get_args(args_id)
            if ai.length >= 2
              vt = infer_type(ai[ai.length - 1])
              promoted = empty_array_promotion_for([vt])
            end
          end
        elsif cur_t == "obj_Method_ptr_array"
 # Specific case: `<X>_ptr_array<Method>` widened from int_array
 # by the prior empty_array_promotion_for. A subsequent write
 # of a different element type (e.g. optcarrots `@fetch[i] =
 # @ram` where @ram is IntArray) would silently void*-cast
 # into the typed Method slot. Widen to poly_array so the
 # cls_id dispatch catches both Method and the other type at
 # runtime. Restricted to Method here because the other
 # `<X>_ptr_array` shapes (e.g. obj_IntArray_ptr_array for
 # `@sp_map = [@sp_map_buffer[0], ...]`) dont need this and
 # widening them too aggressively breaks downstream
 # operations (`clear` etc.) that have separate poly_array
 # codepaths only in stmt context.
          args_id = @nd_arguments[nid]
          if args_id >= 0
            ai = get_args(args_id)
            if ai.length >= 2
              vt = infer_type(ai[ai.length - 1])
              if vt != "nil" && vt != "obj_Method"
                promoted = "poly_array"
              end
            end
          end
        end
        if promoted != "" && promoted != cur_t
          replace_ivar_type(@current_class_idx, iname, promoted)
          if promoted == "str_array"
            @needs_str_array = 1
          elsif promoted == "float_array"
            @needs_float_array = 1
          elsif promoted == "sym_array"
            @needs_int_array = 1
          elsif promoted == "poly_array"
            @needs_rb_value = 1
            @needs_gc = 1
          elsif is_ptr_array_type(promoted) == 1
 # ptr_array slots hold object pointers, so the
 # owning class needs gc_scan emitted; without
 # this flag the captured pointers leak and the
 # collector misses them. Gemini review.
            @needs_gc = 1
          end
        end
      end
    end
 # Recurse via the centralized child walker (push_child_ids covers
 # the full set of AST slots — visiting a few extra slots is a
 # no-op for nodes scan_writer_calls doesn't recognise).
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_writer_calls(cs[k])
      k = k + 1
    end
  end

  def infer_writer_param_types
 # For setter methods (def x=(v); @x = v; end), infer param type from ivar type
    ci = 0
    while ci < @cls_names.length
      mnames = @cls_meth_names[ci].split(";")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      ivar_names = @cls_ivar_names[ci].split(";")
      ivar_types = @cls_ivar_types[ci].split(";")
      changed = 0
      j = 0
      bname = ""
      iname = ""
      while j < mnames.length
        mn = mnames[j]
        if mn.length > 1
          if mn[mn.length - 1] == "="
            bname = mn[0, mn.length - 1]
            iname = "@" + bname
 # Find ivar type
            ik = 0
            while ik < ivar_names.length
              if ivar_names[ik] == iname
                ivt = ivar_types[ik]
                if ivt != "int"
                  if ivt != "nil"
                    if j < all_ptypes.length
                      if all_ptypes[j] == "int"
                        all_ptypes[j] = ivt
                        changed = 1
                      end
                    end
                  end
                end
              end
              ik = ik + 1
            end
          end
        end
        j = j + 1
      end
      if changed == 1
        @cls_meth_ptypes[ci] = all_ptypes.join("|")
        @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
      end
      ci = ci + 1
    end
  end

  def infer_lambda_param_types
 # Scan all call sites in the program AST for calls to top-level methods
 # where lambda arguments are passed. Update param types accordingly.
    scan_lambda_call_sites(@root_id)
 # Second pass: scan method bodies for parameters used as lambda receivers
 # or passed to functions that expect lambda args (transitive closure)
    changed = 1
    while changed == 1
      changed = 0
      mi = 0
      while mi < @meth_names.length
        bid = @meth_body_ids[mi]
        if bid >= 0
          pnames = @meth_param_names[mi].split(",")
          ptypes = @meth_param_types[mi].split(",")
          pk = 0
          while pk < pnames.length
            if pk < ptypes.length
              if ptypes[pk] != "lambda"
 # Check if param is used as lambda receiver (e.g., param[...])
 # or passed to a function that expects lambda
                if param_used_as_lambda(pnames[pk], bid) == 1
                  ptypes[pk] = "lambda"
                  changed = 1
                end
              end
            end
            pk = pk + 1
          end
          @meth_param_types[mi] = ptypes.join(",")
        end
        mi = mi + 1
      end
    end
  end

  def param_used_as_lambda(pname, nid)
    if nid < 0
      return 0
    end
    t = @nd_type[nid]
 # Handle StatementsNode by iterating its statements
    if t == "StatementsNode"
      stmts2 = parse_id_list(@nd_stmts[nid])
      k = 0
      while k < stmts2.length
        if param_used_as_lambda(pname, stmts2[k]) == 1
          return 1
        end
        k = k + 1
      end
      return 0
    end
    if t == "CallNode"
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
 # Check if param is used as receiver of [] with a lambda argument
 # (distinguishes lambda call from array indexing)
      if mname == "[]"
        if recv >= 0
          if @nd_type[recv] == "LocalVariableReadNode"
            if @nd_name[recv] == pname
 # Only flag as lambda if the argument is a lambda
              args_id5 = @nd_arguments[nid]
              if args_id5 >= 0
                aargs5 = get_args(args_id5)
                if aargs5.length > 0
                  if infer_type(aargs5[0]) == "lambda"
                    return 1
                  end
                end
              end
            end
          end
 # Check if param is passed as argument to [] on a lambda receiver
          rt = infer_type(recv)
          if rt == "lambda"
            args_id3 = @nd_arguments[nid]
            if args_id3 >= 0
              aargs3 = get_args(args_id3)
              k3 = 0
              while k3 < aargs3.length
                if @nd_type[aargs3[k3]] == "LocalVariableReadNode"
                  if @nd_name[aargs3[k3]] == pname
                    return 1
                  end
                end
                k3 = k3 + 1
              end
            end
          end
        end
      end
 # Check if param is passed to a function that expects lambda
      if recv < 0
        fmi = find_method_idx(mname)
        if fmi >= 0
          fptypes = @meth_param_types[fmi].split(",")
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            k = 0
            while k < aargs.length
              if k < fptypes.length
                if fptypes[k] == "lambda"
                  if @nd_type[aargs[k]] == "LocalVariableReadNode"
                    if @nd_name[aargs[k]] == pname
                      return 1
                    end
                  end
                end
              end
              k = k + 1
            end
          end
        end
      end
    end
 # Recurse into children
    if @nd_body[nid] >= 0
      bstmts = get_stmts(@nd_body[nid])
      if bstmts.length > 0
        k = 0
        while k < bstmts.length
          if param_used_as_lambda(pname, bstmts[k]) == 1
            return 1
          end
          k = k + 1
        end
      else
        if param_used_as_lambda(pname, @nd_body[nid]) == 1
          return 1
        end
      end
    end
    if @nd_receiver[nid] >= 0
      if param_used_as_lambda(pname, @nd_receiver[nid]) == 1
        return 1
      end
    end
    if @nd_arguments[nid] >= 0
      aargs2 = get_args(@nd_arguments[nid])
      k = 0
      while k < aargs2.length
        if param_used_as_lambda(pname, aargs2[k]) == 1
          return 1
        end
        k = k + 1
      end
    end
    if @nd_expression[nid] >= 0
      if param_used_as_lambda(pname, @nd_expression[nid]) == 1
        return 1
      end
    end
    if @nd_predicate[nid] >= 0
      if param_used_as_lambda(pname, @nd_predicate[nid]) == 1
        return 1
      end
    end
    if @nd_subsequent[nid] >= 0
      if param_used_as_lambda(pname, @nd_subsequent[nid]) == 1
        return 1
      end
    end
    if @nd_else_clause[nid] >= 0
      if param_used_as_lambda(pname, @nd_else_clause[nid]) == 1
        return 1
      end
    end
    if @nd_left[nid] >= 0
      if param_used_as_lambda(pname, @nd_left[nid]) == 1
        return 1
      end
    end
    if @nd_right[nid] >= 0
      if param_used_as_lambda(pname, @nd_right[nid]) == 1
        return 1
      end
    end
    if @nd_block[nid] >= 0
      if param_used_as_lambda(pname, @nd_block[nid]) == 1
        return 1
      end
    end
 # Check StatementsNode stmts
    stmts3 = parse_id_list(@nd_stmts[nid])
    k3 = 0
    while k3 < stmts3.length
      if param_used_as_lambda(pname, stmts3[k3]) == 1
        return 1
      end
      k3 = k3 + 1
    end
    0
  end

  def scan_lambda_call_sites(nid)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "CallNode"
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
 # Only bare function calls (no receiver) can be top-level methods
      if recv < 0
        mi = find_method_idx(mname)
        if mi >= 0
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            ptypes = @meth_param_types[mi].split(",")
            changed = 0
            k = 0
            while k < aargs.length
              if k < ptypes.length
                at = infer_type(aargs[k])
                if at == "lambda"
                  if ptypes[k] != "lambda"
                    ptypes[k] = "lambda"
                    changed = 1
                  end
                end
              end
              k = k + 1
            end
            if changed == 1
              @meth_param_types[mi] = ptypes.join(",")
            end
          end
        end
      end
    end
 # Recurse into children
    if @nd_body[nid] >= 0
      bstmts = get_stmts(@nd_body[nid])
      if bstmts.length > 0
        k = 0
        while k < bstmts.length
          scan_lambda_call_sites(bstmts[k])
          k = k + 1
        end
      else
        scan_lambda_call_sites(@nd_body[nid])
      end
    end
    if @nd_receiver[nid] >= 0
      scan_lambda_call_sites(@nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      aargs2 = get_args(@nd_arguments[nid])
      k = 0
      while k < aargs2.length
        scan_lambda_call_sites(aargs2[k])
        k = k + 1
      end
    end
    if @nd_expression[nid] >= 0
      scan_lambda_call_sites(@nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_lambda_call_sites(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_lambda_call_sites(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_lambda_call_sites(@nd_else_clause[nid])
    end
    if @nd_left[nid] >= 0
      scan_lambda_call_sites(@nd_left[nid])
    end
    if @nd_right[nid] >= 0
      scan_lambda_call_sites(@nd_right[nid])
    end
    if @nd_block[nid] >= 0
      scan_lambda_call_sites(@nd_block[nid])
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_lambda_call_sites(elems[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_lambda_call_sites(conds[k])
      k = k + 1
    end
    stmts2 = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts2.length
      scan_lambda_call_sites(stmts2[k])
      k = k + 1
    end
  end

  def infer_all_returns
 # Pre-pass: infer class method param types from body usage
    infer_cls_meth_param_from_body
 # Pre-pass: scan for .new calls to infer constructor param types
    infer_constructor_types
 # Bare `new(args)` in an inherited class method body widens
 # the subclass's `initialize` ptypes via the cls method's
 # already-widened ptypes. Runs after infer_constructor_types
 # so the cls method ptypes (set by scan_new_calls' caller-side
 # widening) are current; runs inside the iterative loop so
 # subsequent rounds pick up cls method widening from new call
 # sites.
    propagate_bare_new_to_subclass_initialize
 # `super` inside a child's `#initialize` calls the parent's
 # initialize with the child's params. Propagate the child's
 # ptypes (already widened by scan_new_calls' constructor branch
 # for `Child.new(args)` call sites) into the parent's ptypes so
 # the parent's body sees the right param types for `@apu = apu`
 # ivar widening — without this the parent's slot stays at the
 # default `mrb_int` even when every `Child.new` site passes a
 # typed pointer.
    propagate_super_init_to_parent
 # Update ivar types from constructor params
    update_ivar_types_from_params
 # Infer setter param types from ivar types
    infer_writer_param_types
 # Widen param slot types when a body reassigns the param to an
 # incompatible value (e.g. `def f(hclk); hclk = "forever" if
 # hclk.nil?; end` — call sites pass int, body assigns string,
 # so the slot becomes poly).
    widen_param_types_from_body_writes

  # Top-level methods
    i = 0
    while i < @meth_names.length
      push_scope
 # Open class self type
      mfn = @meth_names[i]
 # Pin @current_method_name so current_lexical_scope_name can
 # peel `<Mod>_cls_<m>` and resolve bare class refs in the body
 # (e.g. `Video.new` inside `Top::Drv.load` resolves Video to
 # Top_Video instead of bare Video).
      saved_meth_ar = @current_method_name
      @current_method_name = mfn
      if mfn.start_with?("__oc_Integer_")
        declare_var("__self_type", "int")
      end
      if mfn.start_with?("__oc_String_")
        declare_var("__self_type", "string")
      end
      if mfn.start_with?("__oc_Float_")
        declare_var("__self_type", "float")
      end
      pnames = @meth_param_names[i].split(",")
      ptypes = @meth_param_types[i].split(",")
      j = 0
      while j < pnames.length
        pt = "int"
        if j < ptypes.length
          pt = ptypes[j]
        end
        declare_var(pnames[j], pt)
        j = j + 1
      end
 # Also declare locals for better return type inference.
 # Route through refine_method_body_locals so the same
 # multi-pass refinement precompute_all_scope_decls runs at
 # end-of-analyze applies here too — without it's
 # int_array -> ptr_array upgrade (and's nil ->
 # nullable_pointer promotion) only land at scope-decl emit
 # time, after `infer_body_return` has already pinned the
 # function's return type at the unrefined value.
      if @meth_body_ids[i] >= 0
        lnames = "".split(",")
        ltypes = "".split(",")
        refine_method_body_locals(@meth_body_ids[i], lnames, ltypes, pnames)
      end
      rt = infer_body_return(@meth_body_ids[i])
      @meth_return_types[i] = rt
 # Back-propagate: a `default = nil` param leaves its slot typed
 # "nil" (= mrb_int at C codegen). If the function's overall return
 # type is a nullable pointer (e.g. "string?"), an implicit-return
 # of the param mismatches the function signature (`return mrb_int`
 # vs declared `const char *`). Widen those params to the return
 # type's nullable form so the implicit-return and any callers'
 # default-value sites lower as NULL of the right pointer type.
      if is_nullable_pointer_type(rt) == 1
        widen_nil_defaults_to(@meth_param_types, i, rt)
      end
      @current_method_name = saved_meth_ar
      pop_scope
      i = i + 1
    end

 # Class methods
    i = 0
    while i < @cls_names.length
      @current_class_idx = i
      mnames = @cls_meth_names[i].split(";")
      all_params = @cls_meth_params[i].split("|")
      all_ptypes = @cls_meth_ptypes[i].split("|")
      bodies = @cls_meth_bodies[i].split(";")
      returns = @cls_meth_returns[i].split(";")

      j = 0
      while j < mnames.length
        push_scope
        pnames = "".split(",")
        ptypes = "".split(",")

        if j < all_params.length
          pnames = all_params[j].split(",")
        end
        if j < all_ptypes.length
          ptypes = all_ptypes[j].split(",")
        end

 # Infer param types for initialize
        if mnames[j] == "initialize"
          k = 0
          while k < pnames.length
 # Two sources of param types feed this slot:
 # existing_pt: from infer_constructor_types scanning Foo.new(...)
 # call sites (already widened to "poly" via unify_call_types
 # when call sites disagree).
 # body_pt: from scanning the initialize body for `@x = param`
 # ivar writes; "int" means "no info" (the fallback).
 # Body inference must not silently clobber call-site evidence.
            existing_pt = "int"
            if k < ptypes.length
              existing_pt = ptypes[k]
            end
            body_pt = infer_init_param_type(i, pnames[k])
            pt = body_pt
            if existing_pt != "int" && existing_pt != "nil"
              if body_pt == "int" || body_pt == "nil"
 # Body has no info; keep call-site type.
                pt = existing_pt
              elsif existing_pt == "poly"
 # Call sites already widened to poly; do not narrow.
                pt = "poly"
              elsif body_pt != existing_pt && body_pt != "poly"
 # Two concrete types disagree; demote to poly.
                @needs_rb_value = 1
                pt = "poly"
              end
            end
            if k < ptypes.length
              ptypes[k] = pt
            end
            declare_var(pnames[k], pt)
            k = k + 1
          end
 # Update ptypes in class storage
          new_ptypes = ptypes.join(",")
          if j < all_ptypes.length
            all_ptypes[j] = new_ptypes
          end
          @cls_meth_ptypes[i] = all_ptypes.join("|")
          @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
        else
          k = 0
          while k < pnames.length
            pt = "int"
            if k < ptypes.length
              pt = ptypes[k]
            end
            declare_var(pnames[k], pt)
            k = k + 1
          end
        end

        bid = -1
        if j < bodies.length
          bid = bodies[j].to_i
        end
 # Declare locals for better return type inference
        if bid >= 0
          rlnames = "".split(",")
          rltypes = "".split(",")
          scan_locals(bid, rlnames, rltypes, pnames)
          rlk = 0
          while rlk < rlnames.length
            declare_var(rlnames[rlk], rltypes[rlk])
            rlk = rlk + 1
          end
 # Second pass: full scan_locals so the empty-hash promotion
 # path sees the in-scope param/local types and
 # promotes `out = {}` correctly when the iterated value is
 # poly. The full scanner also marks polymorphic locals if it
 # encounters disagreeing concrete-literal writes; that's a
 # superset of scan_locals_first_type's "never-poly" rule but
 # produces strictly more accurate types for return inference.
          rlnames2 = "".split(",")
          rltypes2 = "".split(",")
          scan_locals(bid, rlnames2, rltypes2, pnames)
          rlk2 = 0
          while rlk2 < rlnames2.length
            cur = ""
            mk2 = 0
            while mk2 < rlnames.length
              if rlnames[mk2] == rlnames2[rlk2]
                cur = rltypes[mk2]
              end
              mk2 = mk2 + 1
            end
            if rltypes2[rlk2] != "int" && rltypes2[rlk2] != cur
 # Same merge widening as the top-level path: hash variants
 # escalate to poly_hash, int/nil → concrete, etc.
              if cur == "int" || cur == "nil" ||
                 ((rltypes2[rlk2] == "str_poly_hash" || rltypes2[rlk2] == "sym_poly_hash") && cur != rltypes2[rlk2])
                set_var_type(rlnames2[rlk2], rltypes2[rlk2])
              end
            end
            rlk2 = rlk2 + 1
          end
        end
        rt = "int"
        if mnames[j] == "initialize"
          rt = "void"
        else
          if mnames[j] == "to_s"
            rt = "string"
          else
            rt = infer_body_return(bid)
          end
        end
        if j < returns.length
          returns[j] = rt
        end
 # Same back-propagation as the top-level case: widen any "nil"-
 # typed param (from `default = nil`) to the function's nullable
 # pointer return type, so `return param` doesn't emit a mrb_int →
 # const-char* mismatch.
        if is_nullable_pointer_type(rt) == 1
          widened = widen_nil_ptypes(ptypes, rt)
          if widened != 0
            if j < all_ptypes.length
              all_ptypes[j] = ptypes.join(",")
            end
            @cls_meth_ptypes[i] = all_ptypes.join("|")
            @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
          end
        end
 # Save incrementally so later methods can see updated return types
        @cls_meth_returns[i] = returns.join(";")
        @cls_meth_return_cache = {}
        pop_scope
        j = j + 1
      end

 # Class methods. mirror the param/local scope
 # setup the instance-method side does so a `def self.<m>`
 # body's return type sees its locals (instance = new etc.)
 # and bare `new` resolves via current_class_method_owning_class.
      cmnames = @cls_cmeth_names[i].split(";")
      cm_bodies = @cls_cmeth_bodies[i].split(";")
      cm_returns = @cls_cmeth_returns[i].split(";")
      cm_params = @cls_cmeth_params[i].split("|")
      cm_ptypes = @cls_cmeth_ptypes[i].split("|")
      saved_meth = @current_method_name
      j = 0
      while j < cmnames.length
        push_scope
        bid = -1
        if j < cm_bodies.length
          bid = cm_bodies[j].to_i
        end
        @current_method_name = @cls_names[i] + "_cls_" + cmnames[j]
        cpnames = "".split(",")
        cptypes = "".split(",")
        if j < cm_params.length
          cpnames = cm_params[j].split(",")
        end
        if j < cm_ptypes.length
          cptypes = cm_ptypes[j].split(",")
        end
        cpk = 0
        while cpk < cpnames.length
          cpt = "int"
          if cpk < cptypes.length
            cpt = cptypes[cpk]
          end
          declare_var(cpnames[cpk], cpt)
          cpk = cpk + 1
        end
        if bid >= 0
 # route through refine_method_body_locals so a
 # `result = nil; ...; result = obj` shape upgrades the
 # local from "nil" to `obj_<C>?` before infer_body_return
 # reads the tail expression. Without this, the cmeth's
 # return type stays "nil" -> `mrb_int` even though the
 # actually-emitted local is correctly typed `sp_<C> *`,
 # so the function signature and the body's `return`
 # statement disagree at the C boundary.
          ml = "".split(",")
          mt = "".split(",")
          refine_method_body_locals(bid, ml, mt, cpnames)
        end
        rt = infer_body_return(bid)
        if j < cm_returns.length
          cm_returns[j] = rt
        end
        if is_nullable_pointer_type(rt) == 1
          widened = widen_nil_ptypes(cptypes, rt)
          if widened != 0
            if j < cm_ptypes.length
              cm_ptypes[j] = cptypes.join(",")
            end
            @cls_cmeth_ptypes[i] = cm_ptypes.join("|")
          end
        end
        pop_scope
        j = j + 1
      end
      @current_method_name = saved_meth
      @cls_cmeth_returns[i] = cm_returns.join(";")
      @current_class_idx = -1
      i = i + 1
    end
  end

 # Widen "nil"-typed entries in `ptypes` to the nullable form of `rt`.
 # Used by the back-propagation pass in infer_all_returns: if a method
 # returns a nullable pointer (e.g. "string?"), any param defaulted to
 # nil should be typed as that nullable pointer too so `return param`
 # doesn't generate a mrb_int → pointer cast in C.
 # Returns 1 if any entry was widened, 0 otherwise.
  def widen_nil_ptypes(ptypes, rt)
    target = rt
    if is_nullable_type(target) == 0
      target = base_type(target) + "?"
    end
    changed = 0
    pk = 0
    while pk < ptypes.length
      if ptypes[pk] == "nil"
        ptypes[pk] = target
        changed = 1
      end
      pk = pk + 1
    end
    changed
  end

 # Top-level method variant of widen_nil_ptypes: reads/writes
 # @meth_param_types[i] directly.
  def widen_nil_defaults_to(table, i, rt)
    ptypes = table[i].split(",")
    if widen_nil_ptypes(ptypes, rt) == 1
      table[i] = ptypes.join(",")
    end
  end

  def infer_init_param_type(ci, pname)
 # Synthetic Struct.new(...) constructors (body id -2) have no AST
 # body to scan — the implicit rule is "param pname → @pname = pname",
 # so the param type must match the ivar type. update_ivar_types_from_params
 # has already propagated call-site-inferred types into ivars by this point.
    init_idx0 = cls_find_method_direct(ci, "initialize")
    if init_idx0 >= 0
      bodies0 = @cls_meth_bodies[ci].split(";")
      if init_idx0 < bodies0.length && bodies0[init_idx0].to_i == -2
        return cls_ivar_type(ci, "@" + pname)
      end
    end
 # Check if param is assigned to an ivar in initialize
    mnames = @cls_meth_names[ci].split(";")
    bodies = @cls_meth_bodies[ci].split(";")
    j = 0
    while j < mnames.length
      if mnames[j] == "initialize"
        bid = -1
        if j < bodies.length
          bid = bodies[j].to_i
        end
        if bid >= 0
          stmts = get_stmts(bid)
          stmts.each { |sid|
            if @nd_type[sid] == "InstanceVariableWriteNode"
              expr = @nd_expression[sid]
              if expr >= 0
                if @nd_type[expr] == "LocalVariableReadNode"
                  if @nd_name[expr] == pname
                    return cls_ivar_type(ci, @nd_name[sid])
                  end
                end
              end
            end
 # Also check super calls
            if @nd_type[sid] == "SuperNode"
              super_args = @nd_arguments[sid]
              if super_args >= 0
                sa_ids = get_args(super_args)
                sk = 0
                while sk < sa_ids.length
                  if @nd_type[sa_ids[sk]] == "LocalVariableReadNode"
                    if @nd_name[sa_ids[sk]] == pname
 # This param is passed to parent's initialize at position sk
                      if @cls_parents[ci] != ""
                        parent_ci = find_class_idx(@cls_parents[ci])
                        if parent_ci >= 0
                          parent_init = cls_find_method_direct(parent_ci, "initialize")
                          if parent_init >= 0
                            parent_ptypes = @cls_meth_ptypes[parent_ci].split("|")
                            if parent_init < parent_ptypes.length
                              ppt = parent_ptypes[parent_init].split(",")
                              if sk < ppt.length
                                return ppt[sk]
                              end
                            end
                          end
                        end
                      end
                    end
                  end
                  sk = sk + 1
                end
              end
            end
          }
        end
      end
      j = j + 1
    end
    "int"
  end

  def infer_body_return(body_id)
    if body_id < 0
      return "void"
    end
    stmts = get_stmts(body_id)
    if stmts.length == 0
      return "void"
    end
 # Collect all explicit return types
    types = "".split(",")
    collect_return_types_nid(body_id, types)
 # Add implicit return (last expression). When the implicit return
 # is a bare LocalVariableReadNode of an "int_array"-typed local
 # (the empty-`[]` default), look at the body's push observations
 # on the same local to derive the promoted array type, so a
 # `def f; results = []; results << Comment.new; results; end`
 # body's return is sp_PtrArray<Comment> rather than sp_IntArray.
    last_id = stmts.last
    last_type = infer_type(last_id)
    if last_type == "int_array" && @nd_type[last_id] == "LocalVariableReadNode"
      lname_lr = @nd_name[last_id]
      elem_acc_lr = "".split(",")
      collect_param_push_elem_types(body_id, lname_lr, elem_acc_lr)
      promoted_lr = empty_array_promotion_for(elem_acc_lr)
      if promoted_lr != ""
        last_type = promoted_lr
      end
    end
    types.push(last_type)
 # Unify all return path types
    unify_return_type(types)
  end

  def collect_return_types_nid(nid, types)
    stmts = get_stmts(nid)
    k = 0
    while k < stmts.length
      collect_return_types(stmts[k], types)
      k = k + 1
    end
  end

  def collect_return_types(nid, types)
    if nid < 0
      return
    end
    if @nd_type[nid] == "ReturnNode"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length > 1
 # `return a, b` materializes as a fixed-arity tuple. Heterogeneous
 # element types are preserved unboxed (no poly_array fallback).
          types.push(tuple_type_from_elems(arg_ids))
          return
        end
        if arg_ids.length > 0
          types.push(infer_type(arg_ids[0]))
          return
        end
      end
      types.push("nil")
      return
    end
 # Don't recurse into nested method definitions
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "IfNode"
      body = @nd_body[nid]
      if body >= 0
        collect_return_types_nid(body, types)
      end
      sub = @nd_subsequent[nid]
      if sub >= 0
        collect_return_types(sub, types)
      end
      return
    end
    if @nd_type[nid] == "UnlessNode"
 # `unless cond; ...; end` — same shape as IfNode but the
 # then-branch fires when the predicate is false. body holds
 # the then-statements, else_clause holds the else block.
 # Without this arm, `unless cond; return {hash}; end; nil`
 # only registered the trailing nil as the function's return
 # path, mistyping the declared return as mrb_int and failing
 # C compile on the hash-returning path (#449).
      body = @nd_body[nid]
      if body >= 0
        collect_return_types_nid(body, types)
      end
      ec = @nd_else_clause[nid]
      if ec >= 0
        collect_return_types(ec, types)
      end
      return
    end
    if @nd_type[nid] == "ElseNode"
      body = @nd_body[nid]
      if body >= 0
        collect_return_types_nid(body, types)
      end
      return
    end
    if @nd_type[nid] == "WhileNode"
      body = @nd_body[nid]
      if body >= 0
        collect_return_types_nid(body, types)
      end
      return
    end
    if @nd_type[nid] == "CaseMatchNode"
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        inid = conds[k]
        if @nd_type[inid] == "InNode"
          ibody = @nd_body[inid]
          if ibody >= 0
            collect_return_types_nid(ibody, types)
          end
        end
        k = k + 1
      end
      return
    end
  end

  def unify_return_type(types)
    result = ""
    has_nil = 0
    k = 0
    while k < types.length
      t = types[k]
      if t == "nil" || t == "void"
        has_nil = 1
      else
        if result == ""
          result = t
        elsif base_type(result) == base_type(t)
 # Same base type — prefer nullable version
          if is_nullable_type(t) == 1
            result = t
          end
        elsif result == "int"
 # int is default/unresolved — real type takes priority
          result = t
        elsif t == "int"
 # int is default/unresolved — keep existing result
        else
 # Genuinely different types
          return "poly"
        end
      end
      k = k + 1
    end
    if result == ""
      if has_nil == 1
        return "nil"
      end
      return "void"
    end
    if has_nil == 1
      if is_nullable_pointer_type(result) == 1
        if is_nullable_type(result) == 0
          result = result + "?"
        end
      end
    end
    result
  end

 # Two-pass scan_locals merge: given the older type stored in
 # scope (cur_t) and a freshly-observed type from a re-scan with
 # more locals declared (new_t), return the type that should
 # replace cur_t, or "" to keep cur_t unchanged. Centralises the
 # rule table that previously lived inline at every refine_* /
 # multi-pass merge site. Each clause encodes a known transition
 # where the later pass is strictly better informed; everything
 # else (incl. regressions like T -> int) is rejected.
  def merge_refined_local_type(cur_t, new_t)
    if new_t == cur_t || new_t == ""
      return ""
    end
 # Never regress to default placeholders. A pass that produced
 # "int" / "void" usually means it walked into a scope where the
 # local wasn't visible — keep the earlier pass's verdict.
    if new_t == "int" || new_t == "void"
      return ""
    end
 # int default → anything specific (incl. nil for nullable vars).
    if cur_t == "int"
      if new_t == "nil"
        return ""
      end
      return new_t
    end
 # nil starting type → nullable pointer. Add "?" so the C slot
 # gets NULL-init and tag handling. is_nullable_type already
 # checks for trailing "?".
    if cur_t == "nil"
      if is_nullable_pointer_type(new_t) == 1
        if is_nullable_type(new_t) == 1
          return new_t
        end
        return new_t + "?"
      end
      return ""
    end
 # Concrete → poly: genuine polymorphism detected after an
 # upstream local resolved. Fixes the #463 cascade where
 # `sub = if raw_sub.is_a?(Hash) then raw_sub else {} end`
 # got stuck at str_int_hash from pass 1.
    if new_t == "poly"
      return "poly"
    end
 # str_poly_hash / sym_poly_hash widen any hash variant (poly
 # value side covers the disagreement).
    if new_t == "str_poly_hash" || new_t == "sym_poly_hash"
      return new_t
    end
 # int_array pass-1 default → typed / poly array once the push
 # rhs resolves.
    if cur_t == "int_array"
      if new_t == "str_array" || new_t == "float_array" || new_t == "sym_array" || is_ptr_array_type(new_t) == 1 || new_t == "poly_array"
        return new_t
      end
    end
 # ptr_array lateral move (sp_FooPtrArray <-> sp_BarPtrArray) —
 # pass 2 typically picks the right element class.
    if is_ptr_array_type(cur_t) == 1 && is_ptr_array_type(new_t) == 1
      return new_t
    end
 # int_str_hash pass-1 default → resolved key/value combination.
    if cur_t == "int_str_hash"
      if new_t == "str_str_hash" || new_t == "sym_str_hash" || new_t == "str_int_hash" || new_t == "sym_int_hash"
        return new_t
      end
    end
 # str_int_hash → str_str_hash (value refinement once rhs local
 # is declared as string).
    if cur_t == "str_int_hash" && new_t == "str_str_hash"
      return new_t
    end
 # Tuple refinements: non-tuple → tuple, and tuple X → tuple Y.
    if is_tuple_type(new_t) == 1
      if is_tuple_type(cur_t) == 0 || cur_t != new_t
        return new_t
      end
    end
    ""
  end

  def fix_lambda_return_types
 # For methods that return "lambda", check if they are called from
 # contexts that expect primitive types. If so, downgrade the return type.
    i = 0
    while i < @meth_names.length
      if @meth_return_types[i] == "lambda"
 # Scan call sites to see what type the return value is used as
        usage = scan_method_return_usage(@meth_names[i], @root_id)
        if usage == "int"
          @meth_return_types[i] = "int"
        end
        if usage == "bool"
          @meth_return_types[i] = "bool"
        end
        if usage == "string"
          @meth_return_types[i] = "string"
        end
      end
      i = i + 1
    end
  end

  def scan_method_return_usage(mname, nid)
    if nid < 0
      return ""
    end
    t = @nd_type[nid]
    if t == "CallNode"
      cn = @nd_name[nid]
      recv = @nd_receiver[nid]
 # Check if this call is our method and its result is used somewhere
      if recv < 0
        if cn == mname
 # This is a call to our method - check parent context
 # We can't easily check parent here, so check all call sites
          return ""
        end
      end
 # Check if our method is called as an argument to another call
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        aargs.each { |aid|
          if @nd_type[aid] == "CallNode"
            if @nd_name[aid] == mname
              if @nd_receiver[aid] < 0
 # Our method is called as argument - check what the parent expects
                if cn == "slice"
                  return "int"
                end
 # For until/if/while conditions, need bool
              end
            end
          end
        }
      end
    end
 # Check if method is called in a negation context (boolean)
    if t == "CallNode"
      cn = @nd_name[nid]
      if cn == "!"
        recv = @nd_receiver[nid]
        if recv >= 0
          if @nd_type[recv] == "CallNode"
            if @nd_name[recv] == mname
              if @nd_receiver[recv] < 0
                return "bool"
              end
            end
          end
        end
      end
    end
 # Check UntilNode predicate
    if t == "UntilNode"
      pred = @nd_predicate[nid]
      if pred >= 0
        if @nd_type[pred] == "CallNode"
          if @nd_name[pred] == mname
            if @nd_receiver[pred] < 0
              return "bool"
            end
          end
        end
      end
    end
 # Recurse
    result = ""
    if @nd_body[nid] >= 0
      bstmts = get_stmts(@nd_body[nid])
      if bstmts.length > 0
        k = 0
        while k < bstmts.length
          r = scan_method_return_usage(mname, bstmts[k])
          if r != ""
            result = r
          end
          k = k + 1
        end
      else
        r = scan_method_return_usage(mname, @nd_body[nid])
        if r != ""
          result = r
        end
      end
    end
    if @nd_receiver[nid] >= 0
      r = scan_method_return_usage(mname, @nd_receiver[nid])
      if r != ""
        result = r
      end
    end
    if @nd_arguments[nid] >= 0
      aargs2 = get_args(@nd_arguments[nid])
      k = 0
      while k < aargs2.length
        r = scan_method_return_usage(mname, aargs2[k])
        if r != ""
          result = r
        end
        k = k + 1
      end
    end
    if @nd_expression[nid] >= 0
      r = scan_method_return_usage(mname, @nd_expression[nid])
      if r != ""
        result = r
      end
    end
    if @nd_predicate[nid] >= 0
      r = scan_method_return_usage(mname, @nd_predicate[nid])
      if r != ""
        result = r
      end
    end
    if @nd_else_clause[nid] >= 0
      r = scan_method_return_usage(mname, @nd_else_clause[nid])
      if r != ""
        result = r
      end
    end
    if @nd_left[nid] >= 0
      r = scan_method_return_usage(mname, @nd_left[nid])
      if r != ""
        result = r
      end
    end
    if @nd_right[nid] >= 0
      r = scan_method_return_usage(mname, @nd_right[nid])
      if r != ""
        result = r
      end
    end
    if @nd_block[nid] >= 0
      r = scan_method_return_usage(mname, @nd_block[nid])
      if r != ""
        result = r
      end
    end
    result
  end

 # Map a simple-literal AST node to its canonical type name. Returns ""
 # for anything that isn't a leaf-literal (hashes, arrays, calls, etc.).
 # Used by pre_scan_simple_local_writes to seed @scope_names before
 # scan_locals's first pass runs.
  def simple_literal_type(nid)
    if nid < 0
      return ""
    end
    t = @nd_type[nid]
    if t == "StringNode"
      return "string"
    end
    if t == "IntegerNode"
      return "int"
    end
    if t == "FloatNode"
      return "float"
    end
    if t == "SymbolNode"
      return "symbol"
    end
    if t == "TrueNode"
      return "bool"
    end
    if t == "FalseNode"
      return "bool"
    end
    if t == "NilNode"
      return "nil"
    end
    ""
  end

 # Pre-populate @scope_names with simple-literal local writes so that
 # scan_locals's pass-1 inference can resolve LocalVariableReadNode
 # references during type inference. Without this, hash shorthand
 # `{first:}` whose key resolves to a previously-written string-valued
 # local mis-types because find_var_type runs against an empty scope and
 # falls back to "int". Limited to leaf-literal initializers; method
 # calls and composite literals still go through the regular passes.
  def pre_scan_simple_local_writes(stmts)
    stmts.each { |sid|
      if @nd_type[sid] == "LocalVariableWriteNode"
        lname = @nd_name[sid]
        if find_var_type(lname) == ""
          st = simple_literal_type(@nd_expression[sid])
          if st != ""
            declare_var(lname, st)
          end
        end
      end
    }
  end

 # ---- Feature detection ----
  def detect_features
 # Set up a temporary scope with main-level locals so feature detection
 # can infer types of local variables correctly
    push_scope
    stmts = get_body_stmts(@root_id)
    pre_scan_simple_local_writes(stmts)
    lnames = "".split(",")
    ltypes = "".split(",")
    empty_p = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            if @nd_type[sid] != "ModuleNode"
              scan_locals(sid, lnames, ltypes, empty_p)
            end
          end
        end
      end
    }
    k = 0
    while k < lnames.length
      declare_var(lnames[k], ltypes[k])
      k = k + 1
    end
    scan_features(@root_id)
    pop_scope
  end

  def scan_features(nid)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "LambdaNode"
      @needs_lambda = 1
    end
    if t == "BeginNode"
      if @nd_rescue_clause[nid] >= 0
        @needs_setjmp = 1
      end
      if @nd_ensure_clause[nid] >= 0
        @needs_setjmp = 1
      end
    end
    if t == "InterpolatedRegularExpressionNode" || t == "InterpolatedMatchLastLineNode"
 # Pre-flag so emit_regexp_runtime fires (defines sp_re_init) even
 # when the program uses ONLY interpolated regexes -- without this,
 # the linker fails on the unreferenced sp_re_init main() call.
      @needs_regexp = 1
    end
    if t == "InterpolatedRegularExpressionNode"
 # Register this AST site so emit_dyn_regex_helpers produces a
 # dedicated `sp_re_dyn_<idx>` cache for it. Idempotent: a node
 # visited twice (e.g. through type-inference + emit passes) keeps
 # its first-assigned index.
      already_dyn = 0
      di = 0
      while di < @dyn_regex_node_ids.length
        if @dyn_regex_node_ids[di] == nid
          already_dyn = 1
        end
        di = di + 1
      end
      if already_dyn == 0
        @dyn_regex_node_ids.push(nid)
        @dyn_regex_flags.push(regex_engine_flags(nid))
      end
    end
    if t == "RegularExpressionNode"
      @needs_regexp = 1
 # Collect pattern and flags
      pat = @nd_unescaped[nid]
      flags = regex_engine_flags(nid)
 # Idempotent: identical patterns share the same compiled global,
 # so a second visit (e.g. via the LocalVariableWriteNode pre-scan
 # below) is a no-op.
      already = 0
      ri0 = 0
      while ri0 < @regexp_patterns.length
        if @regexp_patterns[ri0] == pat
          already = 1
        end
        ri0 = ri0 + 1
      end
      if already == 0
        @regexp_patterns.push(pat)
        @regexp_flags.push(flags)
      end
    end
 # Track `var = /lit/` so a regex held in a local can be dispatched
 # by find_regexp_index. A name with multiple writes (any kind, any
 # regex literal) is marked ambiguous (-1) and falls through.
    if t == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      vid = @nd_expression[nid]
      this_idx = -1
      if vid >= 0 && @nd_type[vid] == "RegularExpressionNode"
 # Register the pattern up front (the recursive scan after this
 # block would do it too, but we need the index now to record
 # the local-name → pattern mapping).
        scan_features(vid)
        rpat = @nd_unescaped[vid]
        ri = 0
        while ri < @regexp_patterns.length
          if @regexp_patterns[ri] == rpat
            this_idx = ri
          end
          ri = ri + 1
        end
      end
      i2 = 0
      found = 0
      while i2 < @local_regex_names.length
        if @local_regex_names[i2] == lname
          found = 1
 # Any second write (regex or not) marks ambiguous.
          if @local_regex_idx[i2] != this_idx
            @local_regex_idx[i2] = -1
          end
        end
        i2 = i2 + 1
      end
      if found == 0
        @local_regex_names.push(lname)
        @local_regex_idx.push(this_idx)
      end
    end
    if t == "ArrayNode"
      et = infer_array_elem_type(nid)
      if et == "str_array"
        @needs_str_array = 1
      else
        if et == "poly_array"
          @needs_rb_value = 1
        else
          if et == "float_array"
            @needs_float_array = 1
          else
            @needs_int_array = 1
          end
        end
      end
      @needs_gc = 1
    end
    if t == "HashNode"
      if is_int_array_lowered_hash(nid) == 1
 # Lowered to Array — same @needs_* flag set as the
 # ArrayNode arm above derives from `infer_array_elem_type`.
        et = infer_int_keyed_hash_as_array_type(nid)
        if et == "str_array"
          @needs_str_array = 1
        elsif et == "poly_array"
          @needs_rb_value = 1
        elsif et == "float_array"
          @needs_float_array = 1
        else
          @needs_int_array = 1
        end
        @needs_gc = 1
      else
        ht = infer_hash_val_type(nid)
        if ht == "str_str_hash"
          @needs_str_str_hash = 1
        elsif ht == "int_str_hash"
          @needs_int_str_hash = 1
          @needs_int_array = 1
        elsif ht == "sym_int_hash"
          @needs_sym_int_hash = 1
        elsif ht == "sym_str_hash"
          @needs_sym_str_hash = 1
        else
          @needs_str_int_hash = 1
        end
        @needs_gc = 1
        @needs_str_array = 1
      end
    end

    if t == "GlobalVariableWriteNode"
 # `alias $copy $orig` -- a $copy = ... write must register
 # the type under $orig's slot, not a separate $copy slot,
 # otherwise the type-consistency check below would later see
 # $orig and $copy as two slots that resolve to the same C
 # global and fire a spurious type-mismatch error.
      gname = resolve_gvar_alias(@nd_name[nid])
      if gname != "$stderr" && gname != "$stdout" && gname != "$?"
        gt = infer_type(@nd_expression[nid])
        if not_in(gname, @gvar_names) == 1
          @gvar_names.push(gname)
          @gvar_types.push(gt)
        else
 # Check type consistency
          gi = 0
          while gi < @gvar_names.length
            if @gvar_names[gi] == gname
              if @gvar_types[gi] != gt && gt != "int" && gt != "nil"
                $stderr.puts "Error: global variable " + gname + " type mismatch: " + @gvar_types[gi] + " vs " + gt
                exit(1)
              end
            end
            gi = gi + 1
          end
        end
      end
    end
    if t == "GlobalVariableReadNode"
 # Same alias resolution as the write side -- $copy reads must
 # land on $orig's slot.
      gname = resolve_gvar_alias(@nd_name[nid])
      if gname != "$stderr" && gname != "$stdout" && gname != "$?"
        if not_in(gname, @gvar_names) == 1
          @gvar_names.push(gname)
 # $PROGRAM_NAME and $0 are Ruby's standard aliases for the
 # program name as a String, populated from argv[0] at emit_main
 # init. Without the string typing, the canonical
 # `__FILE__ == $PROGRAM_NAME` autorun guard would compare
 # against an mrb_int and fail to compile.
          if gname == "$PROGRAM_NAME" || gname == "$0"
            @gvar_types.push("string")
          else
            @gvar_types.push("int")
          end
        end
      end
    end
    if t == "CallNode"
      mname = @nd_name[nid]
 # String methods that always need string helpers
      if mname == "to_s" || mname == "upcase" || mname == "downcase" ||
         mname == "strip" || mname == "chomp" || mname == "chop" || mname == "slice" ||
         mname == "include?" || mname == "start_with?" || mname == "end_with?" ||
         mname == "gsub" || mname == "index" || mname == "sub" || mname == "tr" ||
         mname == "ljust" || mname == "rjust" || mname == "capitalize" ||
         mname == "count" || mname == "<<"
      end
      if mname == "rand" || mname == "srand" || mname == "sample" ||
         mname == "shuffle" || mname == "shuffle!"
        @needs_rand = 1
      end
      if mname == "split"
        @needs_str_array = 1
        @needs_gc = 1
      end
      if mname == "to_sym" || mname == "intern"
        if @nd_receiver[nid] >= 0
 # Fire for poly receivers and not-yet-typed locals
 # (default `int` from LocalVariableReadNode against an
 # empty scope) too, not just statically-known strings.
 # scan_features runs once on @root_id before the
 # yield-arg-type fixpoint resolves block-param receivers,
 # so a shape like `attrs.each { |k, v| row[k.to_sym] = v
 # }` would otherwise miss the flag while
 # compile_int_method_expr still emits `sp_sym_intern(...)`
 # — leaving a linker error against a missing definition.
          rt = infer_type(@nd_receiver[nid])
          if rt == "string" || rt == "poly" || rt == "int"
            @needs_sym_intern = 1
          end
        end
      end
 # `:foo.upcase` / `:foo.downcase` lower to sp_str_upcase /
 # sp_str_downcase on the symbol's name string and re-intern via
 # sp_sym_intern. Mark the dynamic-pool path so it gets emitted.
      if mname == "upcase" || mname == "downcase"
        if @nd_receiver[nid] >= 0
          if infer_type(@nd_receiver[nid]) == "symbol"
            @needs_sym_intern = 1
          end
        end
      end
 # `sym_array.tally` returns a sym_int_hash. The runtime helper
 # sp_SymArray_tally lives next to the sp_SymIntHash typedef which
 # is gated on @needs_sym_int_hash, so flag the dependency.
      if mname == "tally"
        if @nd_receiver[nid] >= 0 && infer_type(@nd_receiver[nid]) == "sym_array"
          @needs_sym_int_hash = 1
        end
      end
 # Methods that need string helpers only when receiver is string
      if mname == "+" || mname == "*" || mname == "reverse"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
 # Long string concat chains emit SP_GC_ROOT temps, so the
 # enclosing function needs SP_GC_SAVE() in its header.
            if mname == "+"
              @needs_gc = 1
            end
          end
        end
      end

      if mname == "new"
        if @nd_receiver[nid] >= 0
          if @nd_type[@nd_receiver[nid]] == "ConstantReadNode"
            @needs_gc = 1
            rn = @nd_name[@nd_receiver[nid]]
            if rn == "Array"
 # Check fill value type for Array.new(n, val)
              args_id2 = @nd_arguments[nid]
              if args_id2 >= 0
                aargs2 = get_args(args_id2)
                if aargs2.length >= 2
                  vt2 = infer_type(aargs2[1])
                  if vt2 == "float"
                    @needs_float_array = 1
                  elsif vt2 == "string"
                    @needs_str_array = 1
                  else
                    @needs_int_array = 1
                  end
                else
                  @needs_int_array = 1
                end
              else
                @needs_int_array = 1
              end
            end
            if rn == "Hash"
              @needs_str_int_hash = 1
            end
            if rn == "StringIO"
              @needs_stringio = 1
            end
          end
        end
      end
      if mname == "to_a"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "range"
            @needs_int_array = 1
            @needs_gc = 1
          end
        end
      end
      if mname == "sort"
        @needs_int_array = 1
        @needs_gc = 1
      end
      if mname == "reduce"
        @needs_int_array = 1
        @needs_gc = 1
      end
      if mname == "inject"
        @needs_int_array = 1
        @needs_gc = 1
      end
      if mname == "reject"
        @needs_int_array = 1
        @needs_gc = 1
      end
      if mname == "raise"
        @needs_setjmp = 1
      end
      if mname == "new"
        if @nd_receiver[nid] >= 0
          if @nd_type[@nd_receiver[nid]] == "ConstantReadNode"
            if @nd_name[@nd_receiver[nid]] == "Fiber"
              @needs_fiber = 1
            end
          end
        end
      end
      if mname == "yield"
        if @nd_receiver[nid] >= 0
          if @nd_type[@nd_receiver[nid]] == "ConstantReadNode"
            if @nd_name[@nd_receiver[nid]] == "Fiber"
              @needs_fiber = 1
            end
          end
        end
      end
      if mname == "current"
        if @nd_receiver[nid] >= 0
          if @nd_type[@nd_receiver[nid]] == "ConstantReadNode"
            if @nd_name[@nd_receiver[nid]] == "Fiber"
              @needs_fiber = 1
            end
          end
        end
      end
      if mname == "catch"
        @needs_setjmp = 1
      end
      if mname == "throw"
        @needs_setjmp = 1
      end
      if mname == "system"
        @needs_system = 1
      end
 # Hash variants are emitted on-demand. A multi-key dig step
 # references every variant of the key family (poly + typed),
 # so flag them all — otherwise the generated C fails to link.
      if mname == "dig"
        if @nd_receiver[nid] >= 0
          drt = infer_type(@nd_receiver[nid])
          if is_hash_type(drt) == 1
            args_id_d = @nd_arguments[nid]
            arg_count_d = 0
            if args_id_d >= 0
              arg_count_d = get_args(args_id_d).length
            end
            if arg_count_d >= 2
              @needs_rb_value = 1
              dargs = get_args(args_id_d)
              dki = 1
              while dki < dargs.length
                dkt = infer_type(dargs[dki])
                if dkt == "symbol"
                  @needs_sym_poly_hash = 1
                  @needs_sym_int_hash = 1
                  @needs_sym_str_hash = 1
                elsif dkt == "string"
                  @needs_str_poly_hash = 1
                  @needs_str_int_hash = 1
                  @needs_str_str_hash = 1
                elsif dkt == "int"
                  @needs_int_str_hash = 1
                end
                dki = dki + 1
              end
            end
          end
        end
      end
      if mname == "keys"
        @needs_str_array = 1
        @needs_gc = 1
      end
      if mname == "values"
        vrt = "int"
        if @nd_receiver[nid] >= 0
          vrt = infer_type(@nd_receiver[nid])
        end
        if vrt == "str_str_hash" || vrt == "int_str_hash"
          @needs_str_array = 1
        else
          @needs_int_array = 1
        end
        @needs_gc = 1
      end
      if mname == "each"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "str_int_hash"
            @needs_str_int_hash = 1
          end
          if rt == "str_str_hash"
            @needs_str_str_hash = 1
          end
          if rt == "int_str_hash"
            @needs_int_str_hash = 1
          end
        end
      end
    end
 # Recurse
    scan_features_children(nid)
  end

 # Push every child node id of `nid` into `acc`. Centralizes the
 # AST slot-by-slot recursion that ~10 different scan/collect passes
 # (scan_features_children, scan_writer_calls, body_has_yield,
 # body_max_yield_arity, ieval_walk, collect_constructed_class_names,
 # subtree_has_setter_on_params, subtree_has_ivar_write, …) used to
 # open-code identically. Slot coverage matches the most-thorough
 # walker (scan_features_children) — adding a new ref slot in alloc
 # only requires updating this one helper.
 #
 # The accumulator-into-an-array shape is deliberate: callers iterate
 # over the result with their own loop, which lets early-exit walkers
 # (`body_has_yield`) bail mid-iteration cleanly. A yielding
 # form would lock the call site into a yield-block-forwarding path
 # and complicate dispatch unnecessarily.
  def push_child_ids(nid, acc)
    if @nd_body[nid] >= 0
      acc.push(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      acc.push(stmts[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      acc.push(@nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      acc.push(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      acc.push(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      acc.push(@nd_else_clause[nid])
    end
    if @nd_receiver[nid] >= 0
      acc.push(@nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      acc.push(@nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      acc.push(args[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      acc.push(conds[k])
      k = k + 1
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      acc.push(elems[k])
      k = k + 1
    end
    parts = parse_id_list(@nd_parts[nid])
    k = 0
    while k < parts.length
      acc.push(parts[k])
      k = k + 1
    end
    if @nd_left[nid] >= 0
      acc.push(@nd_left[nid])
    end
    if @nd_right[nid] >= 0
      acc.push(@nd_right[nid])
    end
    if @nd_block[nid] >= 0
      acc.push(@nd_block[nid])
    end
    if @nd_key[nid] >= 0
      acc.push(@nd_key[nid])
    end
    if @nd_collection[nid] >= 0
      acc.push(@nd_collection[nid])
    end
    if @nd_target[nid] >= 0
      acc.push(@nd_target[nid])
    end
    if @nd_parameters[nid] >= 0
      acc.push(@nd_parameters[nid])
    end
    if @nd_rest[nid] >= 0
      acc.push(@nd_rest[nid])
    end
    if @nd_rescue_clause[nid] >= 0
      acc.push(@nd_rescue_clause[nid])
    end
    if @nd_ensure_clause[nid] >= 0
      acc.push(@nd_ensure_clause[nid])
    end
    if @nd_pattern[nid] >= 0
      acc.push(@nd_pattern[nid])
    end
    if @nd_reference[nid] >= 0
      acc.push(@nd_reference[nid])
    end
    if @nd_constant_path[nid] >= 0
      acc.push(@nd_constant_path[nid])
    end
    if @nd_superclass[nid] >= 0
      acc.push(@nd_superclass[nid])
    end
    reqs = parse_id_list(@nd_requireds[nid])
    k = 0
    while k < reqs.length
      acc.push(reqs[k])
      k = k + 1
    end
    opts = parse_id_list(@nd_optionals[nid])
    k = 0
    while k < opts.length
      acc.push(opts[k])
      k = k + 1
    end
    kws = parse_id_list(@nd_keywords[nid])
    k = 0
    while k < kws.length
      acc.push(kws[k])
      k = k + 1
    end
    excs = parse_id_list(@nd_exceptions[nid])
    k = 0
    while k < excs.length
      acc.push(excs[k])
      k = k + 1
    end
    targs = parse_id_list(@nd_targets[nid])
    k = 0
    while k < targs.length
      acc.push(targs[k])
      k = k + 1
    end
    rights = parse_id_list(@nd_rights[nid])
    k = 0
    while k < rights.length
      acc.push(rights[k])
      k = k + 1
    end
  end

  def scan_features_children(nid)
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      scan_features(cs[k])
      k = k + 1
    end
  end

 # ---- Code generation ----
  def infer_main_call_types
 # Scan main-level code for function calls and infer param types from arguments
    stmts = get_body_stmts(@root_id)
 # First, figure out main local types
    push_scope
    lnames = "".split(",")
    ltypes = "".split(",")
    empty_p = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            scan_locals(sid, lnames, ltypes, empty_p)
          end
        end
      end
    }
    k = 0
    while k < lnames.length
      declare_var(lnames[k], ltypes[k])
      k = k + 1
    end
 # Now scan call sites to update param types
    scan_new_calls(@root_id)
    pop_scope
  end

 # Like infer_type but resolves default "int" from unresolved ivar accessors
  def infer_type_deep(nid)
    at = infer_type(nid)
    if at == "int" && @nd_type[nid] == "CallNode"
      recv = @nd_receiver[nid]
      if recv >= 0
        rt = infer_type(recv)
 # If receiver type is default "int" from an unscoped parameter, try to resolve
        if rt == "int" && @nd_type[recv] == "LocalVariableReadNode"
          vn = @nd_name[recv]
 # Check if it's a method parameter with a known type
          mi = 0
          while mi < @meth_names.length
            pnames = @meth_param_names[mi].split(",")
            ptypes = @meth_param_types[mi].split(",")
            pi = 0
            while pi < pnames.length
              if pnames[pi] == vn && pi < ptypes.length
                if ptypes[pi] != "int"
                  rt = ptypes[pi]
                end
              end
              pi = pi + 1
            end
            mi = mi + 1
          end
        end
        if is_obj_type(rt) == 1
          bt = base_type(rt)
          cname = bt[4, bt.length - 4]
          ci = find_class_idx(cname)
          if ci >= 0
            mname = @nd_name[nid]
            readers = @cls_attr_readers[ci].split(";")
            rk = 0
            while rk < readers.length
              if readers[rk] == mname
 # Resolve ivar type from initialize body
                ivt = resolve_ivar_from_init(ci, "@" + mname)
                if ivt != "" && ivt != "int"
                  return ivt
                end
              end
              rk = rk + 1
            end
          end
        end
      end
    end
    at
  end

 # Resolve an ivar's type by scanning the initialize method body
  def resolve_ivar_from_init(ci, iname)
 # Check if already resolved
    ivt = cls_ivar_type(ci, iname)
    if ivt != "int"
      return ivt
    end
 # Scan initialize body for @ivar = param assignments
    bj = cls_find_method_direct(ci, "initialize")
    if bj >= 0
      bodies = @cls_meth_bodies[ci].split(";")
      bid = bj < bodies.length ? bodies[bj].to_i : -1
      if bid >= 0
        pnames = cls_meth_pnames_get(ci, bj)
        ptypes = cls_meth_ptypes_get(ci, bj)
        resolve_ivar_from_body(ci, bid, iname, pnames, ptypes)
        ivt2 = cls_ivar_type(ci, iname)
        if ivt2 != "int"
          return ivt2
        end
      end
    end
    ""
  end

  def resolve_ivar_from_body(ci, nid, iname, pnames, ptypes)
    if nid < 0
      return
    end
    if @nd_type[nid] == "InstanceVariableWriteNode"
      if @nd_name[nid] == iname
        expr = @nd_expression[nid]
        if expr >= 0 && @nd_type[expr] == "LocalVariableReadNode"
          pn = @nd_name[expr]
          pi = 0
          while pi < pnames.length
            if pnames[pi] == pn && pi < ptypes.length
              pt = ptypes[pi]
              if pt != "int"
                update_ivar_type(ci, iname, pt)
              end
            end
            pi = pi + 1
          end
        end
      end
    end
 # Recurse
    if @nd_body[nid] >= 0
      resolve_ivar_from_body(ci, @nd_body[nid], iname, pnames, ptypes)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      resolve_ivar_from_body(ci, stmts[k], iname, pnames, ptypes)
      k = k + 1
    end
  end

  def detect_poly_params
 # Scan all call sites to detect functions called with different param types
    stmts = get_body_stmts(@root_id)
    i = 0
    while i < stmts.length
      detect_poly_in_node(stmts[i])
      i = i + 1
    end
  end

  def detect_poly_in_node(nid)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      mname = @nd_name[nid]
      if @nd_receiver[nid] < 0
        mi = find_method_idx(mname)
        if mi >= 0
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
            ptypes = @meth_param_types[mi].split(",")
            rest_param_idx = method_rest_index(mi)
            k = 0
            while k < arg_ids.length
              at = infer_type_deep(arg_ids[k])
              if k < ptypes.length
                ct = ptypes[k]
 # Skip explicit rest params; normal int_array params still
 # participate in call-site type checks.
                if k == rest_param_idx
                  k = k + 1
                  next
                end
 # An empty `[]` literal at the call site is
 # compatible with any concrete typed-array param.
 # Skip the ct != at mismatch check so `foo([])`
 # against a body-promoted `str_array` param
 # doesn't bump the param back to poly.
                if is_empty_array_literal(arg_ids[k]) == 1
                  if ct == "str_array" || ct == "float_array" || ct == "sym_array" || is_ptr_array_type(ct) == 1
                    k = k + 1
                    next
                  end
                end
 # an empty `{}` literal at the call site is
 # compatible with any concrete hash variant the body
 # widened the param to. Without this, the body-widened
 # `ct` and the call-site `at` (still `str_int_hash` from
 # the empty-hash default) disagree and detect_poly_params
 # folds the param back to poly, and the call site ends
 # up boxing the hash through poly dispatch.
                if is_empty_hash_literal(arg_ids[k]) == 1
                  if is_hash_type(ct) == 1
                    k = k + 1
                    next
                  end
                end
                if ct != at
                  if ct != "poly"
 # Only mark as poly if both types are meaningful
 # (not just default "int" vs actual type)
                    if ct == "int"
 # First real type seen - update, don't mark poly
                      ptypes[k] = at
                    else
                      if at == "int"
 # Check if arg is a literal int (genuine int value)
                        if k < arg_ids.length
                          if @nd_type[arg_ids[k]] == "IntegerNode"
                            ptypes[k] = "poly"
                            @needs_rb_value = 1
                          end
                        end
 # otherwise arg is int variable, param already has a type - keep it
                      else
 # Check nullable compatibility: T and T? are compatible
                        if base_type(ct) == base_type(at)
 # Same base type — use nullable version
                          if is_nullable_type(at) == 1
                            ptypes[k] = at
                          elsif is_nullable_type(ct) == 0 && is_nullable_pointer_type(ct) == 1
                            ptypes[k] = ct + "?"
                          end
                        elsif at == "nil" && is_nullable_pointer_type(ct) == 1
 # nil + T → T?
                          if is_nullable_type(ct) == 0
                            ptypes[k] = ct + "?"
                          end
                        elsif ct == "nil" && is_nullable_pointer_type(at) == 1
 # T + nil (ct was nil, at is T) → T?
                          ptypes[k] = at + "?"
                        else
 # Genuinely different types - mark poly
                          ptypes[k] = "poly"
                          @needs_rb_value = 1
                        end
                      end
                    end
                  end
                end
              end
              k = k + 1
            end
            @meth_param_types[mi] = ptypes.join(",")
          end
        end
      end
    end
 # Recurse
    if @nd_body[nid] >= 0
      detect_poly_in_node(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      detect_poly_in_node(stmts[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      detect_poly_in_node(@nd_expression[nid])
    end
    if @nd_arguments[nid] >= 0
      detect_poly_in_node(@nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      detect_poly_in_node(args[k])
      k = k + 1
    end
    if @nd_predicate[nid] >= 0
      detect_poly_in_node(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      detect_poly_in_node(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      detect_poly_in_node(@nd_else_clause[nid])
    end
    if @nd_block[nid] >= 0
      detect_poly_in_node(@nd_block[nid])
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      detect_poly_in_node(conds[k])
      k = k + 1
    end
  end

  def detect_poly_locals
 # Detect local variables assigned different types in main scope
    stmts = get_body_stmts(@root_id)
    local_types = "".split(",")
    local_names = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          scan_poly_assigns(sid, local_names, local_types)
        end
      end
    }
  end

  def scan_poly_assigns(nid, names, types)
    if nid < 0
      return
    end
 # Nested DefNode / ClassNode / ModuleNode bodies are separate
 # scopes (see #450 cascade 1 / scan_locals fix). Without these
 # guards, nested-def locals inside a module body leak into
 # main's poly-detection bucket and falsely tag main-level
 # locals as poly.
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      at = infer_type(@nd_expression[nid])
      idx = -1
      k = 0
      while k < names.length
        if names[k] == lname
          idx = k
        end
        k = k + 1
      end
      if idx >= 0
        if types[idx] != at
          old = types[idx]
          if old != "poly"
            if at == "nil" && is_nullable_pointer_type(old) == 1
 # T + nil → T? (nullable)
              if old[old.length - 1] != "?"
                types[idx] = old + "?"
              end
            elsif old == "nil" && is_nullable_pointer_type(at) == 1
 # nil + T → T? (nullable)
              types[idx] = at + "?"
            else
              types[idx] = "poly"
              @needs_rb_value = 1
            end
          end
        end
      else
        names.push(lname)
        types.push(at)
      end
    end
 # Recurse
    if @nd_body[nid] >= 0
      scan_poly_assigns(@nd_body[nid], names, types)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_poly_assigns(stmts[k], names, types)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_poly_assigns(@nd_expression[nid], names, types)
    end
    if @nd_subsequent[nid] >= 0
      scan_poly_assigns(@nd_subsequent[nid], names, types)
    end
    if @nd_else_clause[nid] >= 0
      scan_poly_assigns(@nd_else_clause[nid], names, types)
    end
  end

  def infer_function_body_call_types
 # Scan each top-level method body for calls to other functions
 # and infer param types from local variable types in those bodies
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
 # Build local scope for this function
        push_scope
 # Pin @current_method_name so current_lexical_scope_name's
 # `<Mod>_cls_<m>` peel resolves bare class refs in the
 # body. Without this, `Inner.new(x)` inside `M.make`
 # would find_class_idx("Inner") (returning -1; the class
 # is registered as `M_Inner`) and skip param widening —
 # leaving the inner class's initialize ptypes un-widened.
        saved_method_name = @current_method_name
        @current_method_name = @meth_names[mi]
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        pk = 0
        while pk < pnames.length
          if pnames[pk] != ""
            declare_var(pnames[pk], ptypes[pk])
          end
          pk = pk + 1
        end
 # Scan locals in the body
        lnames = "".split(",")
        ltypes = "".split(",")
        scan_locals(bid, lnames, ltypes, pnames)
        lk = 0
        while lk < lnames.length
          declare_var(lnames[lk], ltypes[lk])
          lk = lk + 1
        end
 # Now scan for calls within this function body
        scan_new_calls(bid)
        @current_method_name = saved_method_name
        pop_scope
      end
      mi = mi + 1
    end
  end

  def scan_locals_first_type(nid, names, types, params)
 # Like scan_locals but never marks poly - just keeps first type seen
    if nid < 0
      return
    end
 # Nested DefNode / ClassNode / ModuleNode bodies are separate
 # scopes — their locals belong to the enclosed unit and must
 # not leak into the parent. Mirrors the scan_locals boundary
 # guard (see #450 cascade 1).
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      if not_in(lname, names) == 1
        if not_in(lname, params) == 1
          names.push(lname)
          types.push(infer_type(@nd_expression[nid]))
        end
      end
    end
    if @nd_type[nid] == "LocalVariableOperatorWriteNode"
      lname = @nd_name[nid]
      if not_in(lname, names) == 1
        if not_in(lname, params) == 1
          names.push(lname)
          types.push("int")
        end
      end
    end
    if @nd_type[nid] == "MultiWriteNode"
      targets = parse_id_list(@nd_targets[nid])
      val_id = @nd_expression[nid]
      ti = 0
      targets.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push(multi_write_target_type(val_id, ti))
            end
          end
        end
        ti = ti + 1
      }
      rest_id = @nd_rest[nid]
      if is_splat_with_target(rest_id) == 1
        st = @nd_expression[rest_id]
        if @nd_type[st] == "LocalVariableTargetNode"
          lname = @nd_name[st]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push(splat_rest_type(val_id))
            end
          end
        end
      end
      rights2 = parse_id_list(@nd_rights[nid])
      r_total = 0
      if val_id >= 0 && @nd_type[val_id] == "ArrayNode"
        r_total = parse_id_list(@nd_elements[val_id]).length
      end
      r_idx = 0
      rights2.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
 # For an ArrayNode literal RHS we know each right's actual
 # element index; use it so heterogeneous literals like
 # [1, "x", 2.0] type each target precisely. Other RHS
 # shapes use index 0 (typed-array element type is uniform).
              t_idx = 0
              if r_total > 0
                t_idx = r_total - rights2.length + r_idx
                if t_idx < 0
                  t_idx = 0
                end
              end
              types.push(multi_write_target_type(val_id, t_idx))
            end
          end
        end
        r_idx = r_idx + 1
      }
    end
 # Recurse
    if @nd_body[nid] >= 0
      scan_locals_first_type(@nd_body[nid], names, types, params)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_locals_first_type(stmts[k], names, types, params)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_locals_first_type(@nd_expression[nid], names, types, params)
    end
    if @nd_predicate[nid] >= 0
      scan_locals_first_type(@nd_predicate[nid], names, types, params)
    end
    if @nd_subsequent[nid] >= 0
      scan_locals_first_type(@nd_subsequent[nid], names, types, params)
    end
    if @nd_else_clause[nid] >= 0
      scan_locals_first_type(@nd_else_clause[nid], names, types, params)
    end
    if @nd_receiver[nid] >= 0
      scan_locals_first_type(@nd_receiver[nid], names, types, params)
    end
    if @nd_arguments[nid] >= 0
      scan_locals_first_type(@nd_arguments[nid], names, types, params)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_locals_first_type(args[k], names, types, params)
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_locals_first_type(conds[k], names, types, params)
      k = k + 1
    end
    if @nd_left[nid] >= 0
      scan_locals_first_type(@nd_left[nid], names, types, params)
    end
    if @nd_right[nid] >= 0
      scan_locals_first_type(@nd_right[nid], names, types, params)
    end
    if @nd_block[nid] >= 0
      scan_locals_first_type(@nd_block[nid], names, types, params)
    end
  end

  def infer_class_body_call_types
 # Scan class method bodies for calls to other methods in the same class.
 # Update called method param types from argument types at call sites.
 # Run multiple passes for propagation.
    pass = 0
 # Stop this local propagation loop once the class-body call type tables
 # stop changing; later passes would rescan the same bodies without
 # teaching any callee a new argument type. Keep the previous pass's
 # signature instead of recomputing it at the top of every pass, because
 # the prior `cur_sig` is exactly the next pass's `prev_sig`.
    prev_sig = class_body_call_type_signature
    while pass < 5
      ci = 0
      while ci < @cls_names.length
        mnames = @cls_meth_names[ci].split(";")
        bodies = @cls_meth_bodies[ci].split(";")
        mi = 0
        while mi < mnames.length
          bid = -1
          if mi < bodies.length
            bid = bodies[mi].to_i
          end
          if bid >= 0
            @current_class_idx = ci
            push_scope
 # Declare params in scope with current types
            pnames_arr = cls_meth_pnames_get(ci, mi)
            ptypes_arr = cls_meth_ptypes_get(ci, mi)
            pk = 0
            while pk < pnames_arr.length
              pt = "int"
              if pk < ptypes_arr.length
                pt = ptypes_arr[pk]
              end
              if pnames_arr[pk] != ""
                declare_var(pnames_arr[pk], pt)
              end
              pk = pk + 1
            end
 # Scan locals using first-type-only (no poly marking)
            lnames = "".split(",")
            ltypes = "".split(",")
            scan_locals_first_type(bid, lnames, ltypes, pnames_arr)
            lk = 0
            while lk < lnames.length
              declare_var(lnames[lk], ltypes[lk])
              lk = lk + 1
            end
 # Second pass: rescan with locals now in scope for better inference
            lnames2 = "".split(",")
            ltypes2 = "".split(",")
            scan_locals_first_type(bid, lnames2, ltypes2, pnames_arr)
            lk2 = 0
            while lk2 < lnames2.length
              if ltypes2[lk2] != "int"
                set_var_type(lnames2[lk2], ltypes2[lk2])
              end
              lk2 = lk2 + 1
            end
 # Scan for calls to other methods in same class
            scan_cls_method_calls(ci, bid)
 # Also scan for constructor calls to infer param types
            scan_new_calls(bid)
            pop_scope
            @current_class_idx = -1
          end
          mi = mi + 1
        end
 # Also iterate this class's class methods (def self.<m>)
 # so a `params.fetch(:k, "")` call inside a `def
 # self.from_raw` widens P.fetch's default param via the
 # receiver-method unify path inside scan_new_calls.
        cm_names = @cls_cmeth_names[ci].split(";")
        cm_bodies = @cls_cmeth_bodies[ci].split(";")
        cm_params = @cls_cmeth_params[ci].split("|")
        cm_ptypes = @cls_cmeth_ptypes[ci].split("|")
        saved_meth_cb = @current_method_name
        cmi = 0
        while cmi < cm_names.length
          cbid = -1
          if cmi < cm_bodies.length
            cbid = cm_bodies[cmi].to_i
          end
          if cbid >= 0
            @current_class_idx = ci
            @current_method_name = @cls_names[ci] + "_cls_" + cm_names[cmi]
            push_scope
            cpnames = "".split(",")
            cptypes = "".split(",")
            if cmi < cm_params.length
              cpnames = cm_params[cmi].split(",")
            end
            if cmi < cm_ptypes.length
              cptypes = cm_ptypes[cmi].split(",")
            end
            cpk = 0
            while cpk < cpnames.length
              cpt = "int"
              if cpk < cptypes.length
                cpt = cptypes[cpk]
              end
              if cpnames[cpk] != ""
                declare_var(cpnames[cpk], cpt)
              end
              cpk = cpk + 1
            end
            cml = "".split(",")
            cmt = "".split(",")
            scan_locals_first_type(cbid, cml, cmt, cpnames)
            clk = 0
            while clk < cml.length
              declare_var(cml[clk], cmt[clk])
              clk = clk + 1
            end
            scan_new_calls(cbid)
            pop_scope
            @current_class_idx = -1
          end
          cmi = cmi + 1
        end
        @current_method_name = saved_meth_cb
        ci = ci + 1
      end
      cur_sig = class_body_call_type_signature
      if cur_sig == prev_sig
        break
      end
      prev_sig = cur_sig
      pass = pass + 1
    end
  end

  def scan_cls_method_calls(ci, nid)
    if nid < 0
      return
    end
 # Apply the same is_a? narrow that scan_new_calls uses, so a
 # self call inside the then-arm sees the narrowed receiver
 # type.
    if @nd_type[nid] == "IfNode"
      pred = @nd_predicate[nid]
      if pred >= 0
        scan_cls_method_calls(ci, pred)
      end
      parsed = parse_is_a_predicate(pred)
      narrow_var = parsed[0]
      narrow_t = parsed[1]
      if narrow_var != ""
        push_type_narrow(narrow_var, narrow_t)
      end
      then_body = @nd_body[nid]
      if then_body >= 0
        scan_cls_method_calls(ci, then_body)
      end
      if narrow_var != ""
        pop_type_narrow
      end
      sub = @nd_subsequent[nid]
      if sub >= 0
        scan_cls_method_calls(ci, sub)
      end
      else_body = @nd_else_clause[nid]
      if else_body >= 0
        scan_cls_method_calls(ci, else_body)
      end
      return
    end
    if @nd_type[nid] == "CallNode"
      mname = @nd_name[nid]
 # Handle implicit self calls (no receiver) to same-class methods
      if @nd_receiver[nid] < 0
        midx = cls_find_method_direct(ci, mname)
        if midx >= 0
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
 # Unify rather than only-widen-from-int. A no-recv
 # self-call inside the same class is the path that
 # e.g. `def boot; add_mappings(0x..0x, ...); end`
 # inside CPU#boot lands on; disagreeing arg types
 # from different self-calls need to widen to poly
 # rather than freezing on the first non-int call site.
            ptypes = cls_meth_ptypes_get(ci, midx)
            if ptypes.length > 0
              pnames = cls_meth_pnames_get(ci, midx)
              widen_ptypes_from_args(arg_ids, pnames, ptypes)
              cls_meth_ptypes_put(ci, midx, ptypes)
            end
          end
        end
      end
    end
 # Recurse into children
    if @nd_body[nid] >= 0
      scan_cls_method_calls(ci, @nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_cls_method_calls(ci, stmts[k])
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      scan_cls_method_calls(ci, @nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      scan_cls_method_calls(ci, @nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_cls_method_calls(ci, args[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_cls_method_calls(ci, @nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_cls_method_calls(ci, @nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_cls_method_calls(ci, @nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_cls_method_calls(ci, @nd_else_clause[nid])
    end
    if @nd_left[nid] >= 0
      scan_cls_method_calls(ci, @nd_left[nid])
    end
    if @nd_right[nid] >= 0
      scan_cls_method_calls(ci, @nd_right[nid])
    end
    if @nd_block[nid] >= 0
      scan_cls_method_calls(ci, @nd_block[nid])
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_cls_method_calls(ci, elems[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_cls_method_calls(ci, conds[k])
      k = k + 1
    end
  end

  def fix_nil_ivar_self_refs
    ci = 0
    while ci < @cls_names.length
      cname = @cls_names[ci]
      writers = @cls_attr_writers[ci].split(";")
      names = @cls_ivar_names[ci].split(";")
      types = @cls_ivar_types[ci].split(";")
      changed = 0
      k = 0
      while k < names.length
        if k < types.length && (types[k] == "nil" || types[k] == "poly")
 # Check if this ivar has an attr_writer
          ibase = names[k]
          if ibase.length > 1 && ibase[0] == "@"
            ibase = ibase[1, ibase.length - 1]
          end
          wk = 0
          while wk < writers.length
            if writers[wk] == ibase
              types[k] = "obj_" + cname + "?"
              changed = 1
            end
            wk = wk + 1
          end
        end
        k = k + 1
      end
      if changed == 1
        @cls_ivar_types[ci] = types.join(";")
        @cls_ivar_types_version = @cls_ivar_types_version + 1
      end
      ci = ci + 1
    end
  end

 # Build a string fingerprint of the arrays that iterative type inference
 # refines. Identical fingerprints between successive iterations means a
 # fixed point has been reached and further iterations are wasted work.
  def inference_signature
    @meth_return_types.join("|") + "/" + @cls_ivar_types.join("|") + "/" + @meth_param_types.join("|") + "/" + @cls_meth_ptypes.join("/")
  end

 # `inference_signature` covers the wider analyze-phase fixpoint, including
 # return and ivar types that `infer_class_body_call_types` does not refine
 # directly. This narrower fingerprint tracks only the param-type tables that
 # can affect the next class-body call pass, so an unrelated wider signature
 # change does not force this local loop to spend all five passes.
  def class_body_call_type_signature
    @meth_param_types.join("|") + "/" + @cls_meth_ptypes.join("/") + "/" + @cls_cmeth_ptypes.join("/")
  end


 # End-to-end whole-program analysis. Splits cleanly out of `compile`
 # so the same work can be invoked once and its results serialized
 # into an IR file, then loaded by a separate codegen step that runs
 # only `generate_code`. Everything that mutates analysis-derived
 # state (the iterative type-inference fixpoint, `detect_features`,
 # `detect_value_types`, `recalc_needs_gc`, sym/toplevel-ivar
 # collection, live-method computation) belongs here so codegen can
 # treat its inputs as read-only.
  def analyze_phase
    collect_all
    infer_main_call_types
    infer_function_body_call_types
    infer_class_body_call_types
    infer_ieval_body_call_types
    detect_poly_locals
 # Iterative type inference: converge param types, return types, ivar types.
 # Stop early when the signature of these three arrays stops changing.
    iter = 0
    prev_sig = inference_signature
    while iter < 4
      infer_all_returns
      infer_function_body_call_types
      infer_class_body_call_types
      infer_ivar_types_from_writers
      infer_param_array_type_from_body
      narrow_param_types_from_body_method_calls
      infer_string_param_from_body
      infer_param_type_from_callee_slot
      narrow_param_hash_types_from_body_writes
 # propagate hash-each block-arg types into
 # nested cmeth/method-call param widening. Runs inside
 # the iterative loop so a later iteration of
 # narrow_param_hash_types_from_body_writes (which may
 # pin the enclosing param to a more specific hash variant)
 # gets its k/v types fed downstream too.
      widen_cmeths_via_hash_each_blocks
      detect_poly_params
      cur_sig = inference_signature
      if cur_sig == prev_sig
        break
      end
      prev_sig = cur_sig
      iter = iter + 1
    end
 # Fix nil/poly-typed ivars with attr_writer to nullable self type
 # e.g. @left = nil in Node with attr_accessor :left → obj_Node?
 # Must run after iterative loop to override poly from type conflicts
    fix_nil_ivar_self_refs
 # Re-run returns with corrected ivar types
    infer_all_returns
    infer_function_body_call_types
    infer_class_body_call_types
    infer_ivar_types_from_writers
    infer_all_returns
 # Param types are now stable, so module ivar refinement (which
 # infers hash / array specialization from `@h[k] = v` writes in
 # class-method bodies) sees the right key / value types instead
 # of the placeholder "int" they'd carry at module-collect time.
 # After refining, re-run return / call-type inference so methods
 # that read or push the refined const see the new shape (e.g. a
 # `def self.add(s); @items << s; end` whose return is now
 # `sp_StrArray *` rather than the placeholder `sp_IntArray *`).
    refine_all_module_ivar_types
    infer_all_returns
    infer_function_body_call_types
    infer_class_body_call_types
    infer_all_returns
 # Fix lambda return types based on call-site usage
    fix_lambda_return_types
 # Pre-detect bigint variables before feature detection
    pre_detect_bigint
    detect_features
 # The remaining state-mutating steps used to live at the top of
 # `generate_code`. Moving them into the analysis phase means a
 # later codegen step can rely on @cls_is_value_type, @needs_gc,
 # @sym_names, @toplevel_ivar_*, and @cls_cmeth_live being already
 # populated.
    detect_value_types
    recalc_needs_gc
    collect_sym_names
    scan_toplevel_ivars(@root_id)
    compute_live_cls_methods
    compute_live_instance_methods
    @analysis_frozen = 1
    precompute_all_scope_decls
    annotate_all_node_types
    infer_proc_blk_param_types
    if ENV["SP_POLY_REPORT"] == "1"
      emit_poly_report
    end
    if ENV["SP_POLY_REPORT"] == "2"
      emit_poly_report
      emit_poly_breakdown
    end
  end

 # B1 discovery (STALIN.md §11): count slot types that landed on
 # poly / nullable-poly / *_poly_hash / poly_array. Output to
 # stderr after all analyze passes finish, gated by envvar to
 # keep production builds noise-free. The breakdown drives the
 # priority of STALIN.md Step 2 (param narrowing) / Step 3 (is_a?
 # narrowing) — high poly counts mean polyvariance work has ROI,
 # low counts mean the inference is already pinning concrete
 # types and the effort is elsewhere.
  def emit_poly_report
    param_total = 0
    param_poly = 0
    param_poly_array = 0
    param_poly_hash = 0
    param_nullable_poly = 0
    lv_total = 0
    lv_poly = 0
    lv_poly_array = 0
    lv_poly_hash = 0
    lv_nullable_poly = 0
    ivar_total = 0
    ivar_poly = 0
    ivar_poly_array = 0
    ivar_poly_hash = 0
    ret_total = 0
    ret_poly = 0

 # Top-level method params + returns.
    rmi = 0
    while rmi < @meth_names.length
      rpt = @meth_param_types[rmi].split(",")
      rpi = 0
      while rpi < rpt.length
        if rpt[rpi] != ""
          param_total = param_total + 1
          rb = base_type(rpt[rpi])
          if rb == "poly"
            param_poly = param_poly + 1
            if rpt[rpi].end_with?("?")
              param_nullable_poly = param_nullable_poly + 1
            end
          elsif rb == "poly_array"
            param_poly_array = param_poly_array + 1
          elsif rpt[rpi].include?("poly_hash")
            param_poly_hash = param_poly_hash + 1
          end
        end
        rpi = rpi + 1
      end
      ret_total = ret_total + 1
      if rmi < @meth_return_types.length
        rb_ret = base_type(@meth_return_types[rmi])
        if rb_ret == "poly"
          ret_poly = ret_poly + 1
        end
      end
      rmi = rmi + 1
    end

 # Class instance method + cmeth params + returns.
    rci = 0
    while rci < @cls_names.length
      r_im_pall = @cls_meth_ptypes[rci].split("|")
      r_im_ret = @cls_meth_returns[rci].split(";")
      r_imj = 0
      while r_imj < r_im_pall.length
        r_im_pt = r_im_pall[r_imj].split(",")
        r_imk = 0
        while r_imk < r_im_pt.length
          if r_im_pt[r_imk] != ""
            param_total = param_total + 1
            r_imb = base_type(r_im_pt[r_imk])
            if r_imb == "poly"
              param_poly = param_poly + 1
              if r_im_pt[r_imk].end_with?("?")
                param_nullable_poly = param_nullable_poly + 1
              end
            elsif r_imb == "poly_array"
              param_poly_array = param_poly_array + 1
            elsif r_im_pt[r_imk].include?("poly_hash")
              param_poly_hash = param_poly_hash + 1
            end
          end
          r_imk = r_imk + 1
        end
        r_imj = r_imj + 1
      end
      r_imrj = 0
      while r_imrj < r_im_ret.length
        if r_im_ret[r_imrj] != ""
          ret_total = ret_total + 1
          r_imrb = base_type(r_im_ret[r_imrj])
          if r_imrb == "poly"
            ret_poly = ret_poly + 1
          end
        end
        r_imrj = r_imrj + 1
      end
      r_cmpall = @cls_cmeth_ptypes[rci].split("|")
      r_cmret = @cls_cmeth_returns[rci].split(";")
      r_cmj = 0
      while r_cmj < r_cmpall.length
        r_cmpt = r_cmpall[r_cmj].split(",")
        r_cmk = 0
        while r_cmk < r_cmpt.length
          if r_cmpt[r_cmk] != ""
            param_total = param_total + 1
            r_cmb = base_type(r_cmpt[r_cmk])
            if r_cmb == "poly"
              param_poly = param_poly + 1
              if r_cmpt[r_cmk].end_with?("?")
                param_nullable_poly = param_nullable_poly + 1
              end
            elsif r_cmb == "poly_array"
              param_poly_array = param_poly_array + 1
            elsif r_cmpt[r_cmk].include?("poly_hash")
              param_poly_hash = param_poly_hash + 1
            end
          end
          r_cmk = r_cmk + 1
        end
        r_cmj = r_cmj + 1
      end
      r_cmrj = 0
      while r_cmrj < r_cmret.length
        if r_cmret[r_cmrj] != ""
          ret_total = ret_total + 1
          r_cmrb = base_type(r_cmret[r_cmrj])
          if r_cmrb == "poly"
            ret_poly = ret_poly + 1
          end
        end
        r_cmrj = r_cmrj + 1
      end
 # ivars
      r_ivt = @cls_ivar_types[rci].split(";")
      r_ivj = 0
      while r_ivj < r_ivt.length
        if r_ivt[r_ivj] != ""
          ivar_total = ivar_total + 1
          r_ivb = base_type(r_ivt[r_ivj])
          if r_ivb == "poly"
            ivar_poly = ivar_poly + 1
          elsif r_ivb == "poly_array"
            ivar_poly_array = ivar_poly_array + 1
          elsif r_ivt[r_ivj].include?("poly_hash")
            ivar_poly_hash = ivar_poly_hash + 1
          end
        end
        r_ivj = r_ivj + 1
      end
      rci = rci + 1
    end

 # LVs from @nd_scope_types — covers all bodies (top-level,
 # instance methods, cmeths, main).
    r_nd = 0
    while r_nd < @nd_scope_types.length
      r_st = @nd_scope_types[r_nd]
      if r_st != ""
        r_pieces = r_st.split("|")
        r_pp = 0
        while r_pp < r_pieces.length
          if r_pieces[r_pp] != ""
            lv_total = lv_total + 1
            r_lb = base_type(r_pieces[r_pp])
            if r_lb == "poly"
              lv_poly = lv_poly + 1
              if r_pieces[r_pp].end_with?("?")
                lv_nullable_poly = lv_nullable_poly + 1
              end
            elsif r_lb == "poly_array"
              lv_poly_array = lv_poly_array + 1
            elsif r_pieces[r_pp].include?("poly_hash")
              lv_poly_hash = lv_poly_hash + 1
            end
          end
          r_pp = r_pp + 1
        end
      end
      r_nd = r_nd + 1
    end

    $stderr.puts "=== SP_POLY_REPORT ==="
    $stderr.puts "params:  total=" + param_total.to_s + " poly=" + param_poly.to_s + " (nullable=" + param_nullable_poly.to_s + ") poly_array=" + param_poly_array.to_s + " poly_hash=" + param_poly_hash.to_s
    $stderr.puts "locals:  total=" + lv_total.to_s + " poly=" + lv_poly.to_s + " (nullable=" + lv_nullable_poly.to_s + ") poly_array=" + lv_poly_array.to_s + " poly_hash=" + lv_poly_hash.to_s
    $stderr.puts "ivars:   total=" + ivar_total.to_s + " poly=" + ivar_poly.to_s + " poly_array=" + ivar_poly_array.to_s + " poly_hash=" + ivar_poly_hash.to_s
    $stderr.puts "returns: total=" + ret_total.to_s + " poly=" + ret_poly.to_s
    $stderr.puts "======================"
  end

 # Detailed breakdown (SP_POLY_REPORT=2). Dump location names
 # of every poly slot so the operator can see which classes /
 # methods cluster the poly. Used to scope STALIN.md Step 2
 # (param narrowing) — high concentration in a few methods
 # means a targeted narrowing pass yields the most.
  def emit_poly_breakdown
    $stderr.puts "--- BREAKDOWN ---"
 # Top-level + module class methods params.
    bmi = 0
    while bmi < @meth_names.length
      bpn = @meth_param_names[bmi].split(",")
      bpt = @meth_param_types[bmi].split(",")
      bj = 0
      while bj < bpn.length
        if bj < bpt.length
          bb = base_type(bpt[bj])
          if bb == "poly" || bb == "poly_array" || bpt[bj].include?("poly_hash")
            $stderr.puts "param: " + @meth_names[bmi] + "(" + bpn[bj] + ") : " + bpt[bj]
          end
        end
        bj = bj + 1
      end
      if bmi < @meth_return_types.length
        b_rb = base_type(@meth_return_types[bmi])
        if b_rb == "poly"
          $stderr.puts "return: " + @meth_names[bmi] + " : " + @meth_return_types[bmi]
        end
      end
      bmi = bmi + 1
    end
 # Class instance methods + cmeths params.
    bci = 0
    while bci < @cls_names.length
      bcn = @cls_names[bci]
      b_im_pn = @cls_meth_params[bci].split("|")
      b_im_pt = @cls_meth_ptypes[bci].split("|")
      b_im_mn = @cls_meth_names[bci].split(";")
      b_im_rn = @cls_meth_returns[bci].split(";")
      b_imj = 0
      while b_imj < b_im_pn.length
        b_im_n = b_im_pn[b_imj].split(",")
        b_im_t = b_im_pt[b_imj].split(",")
        b_im_mname = b_imj < b_im_mn.length ? b_im_mn[b_imj] : "?"
        b_imk = 0
        while b_imk < b_im_n.length
          if b_imk < b_im_t.length
            b_im_b = base_type(b_im_t[b_imk])
            if b_im_b == "poly" || b_im_b == "poly_array" || b_im_t[b_imk].include?("poly_hash")
              $stderr.puts "param: " + bcn + "#" + b_im_mname + "(" + b_im_n[b_imk] + ") : " + b_im_t[b_imk]
            end
          end
          b_imk = b_imk + 1
        end
        if b_imj < b_im_rn.length && b_im_rn[b_imj] != ""
          b_im_rb = base_type(b_im_rn[b_imj])
          if b_im_rb == "poly"
            $stderr.puts "return: " + bcn + "#" + b_im_mname + " : " + b_im_rn[b_imj]
          end
        end
        b_imj = b_imj + 1
      end
      b_cm_pn = @cls_cmeth_params[bci].split("|")
      b_cm_pt = @cls_cmeth_ptypes[bci].split("|")
      b_cm_mn = @cls_cmeth_names[bci].split(";")
      b_cm_rn = @cls_cmeth_returns[bci].split(";")
      b_cmj = 0
      while b_cmj < b_cm_pn.length
        b_cm_n = b_cm_pn[b_cmj].split(",")
        b_cm_t = b_cm_pt[b_cmj].split(",")
        b_cm_mname = b_cmj < b_cm_mn.length ? b_cm_mn[b_cmj] : "?"
        b_cmk = 0
        while b_cmk < b_cm_n.length
          if b_cmk < b_cm_t.length
            b_cm_b = base_type(b_cm_t[b_cmk])
            if b_cm_b == "poly" || b_cm_b == "poly_array" || b_cm_t[b_cmk].include?("poly_hash")
              $stderr.puts "param: " + bcn + "." + b_cm_mname + "(" + b_cm_n[b_cmk] + ") : " + b_cm_t[b_cmk]
            end
          end
          b_cmk = b_cmk + 1
        end
        if b_cmj < b_cm_rn.length && b_cm_rn[b_cmj] != ""
          b_cm_rb = base_type(b_cm_rn[b_cmj])
          if b_cm_rb == "poly"
            $stderr.puts "return: " + bcn + "." + b_cm_mname + " : " + b_cm_rn[b_cmj]
          end
        end
        b_cmj = b_cmj + 1
      end
      b_ivn = @cls_ivar_names[bci].split(";")
      b_ivt = @cls_ivar_types[bci].split(";")
      b_ivj = 0
      while b_ivj < b_ivn.length
        if b_ivj < b_ivt.length
          b_iv_b = base_type(b_ivt[b_ivj])
          if b_iv_b == "poly" || b_iv_b == "poly_array" || b_ivt[b_ivj].include?("poly_hash")
            $stderr.puts "ivar: " + bcn + "." + b_ivn[b_ivj] + " : " + b_ivt[b_ivj]
          end
        end
        b_ivj = b_ivj + 1
      end
      bci = bci + 1
    end
    $stderr.puts "-----------------"
  end


 # ============================================================
 # Emission
 # ============================================================
 #
 # End of pre-emission analysis. From here down, the codegen
 # consumes the tables built above and writes C: header, runtime
 # blocks, struct/forward decls, class methods, top-level methods,
 # main(). emit_header is the entry point; generate_code (above)
 # orchestrates the order.

  def pre_detect_bigint
    stmts = get_body_stmts(@root_id)
    bigint_names = "".split(",")
    stmts.each { |sid|
      scan_bigint_candidates(sid, bigint_names)
    }
    if bigint_names.length > 0
      @needs_bigint = 1
    end
  end

 # Detect variables that need bigint promotion
 # Pattern: x = x * y (or x *= y) inside a while loop

  def scan_bigint_candidates(nid, bigint_names)
    if nid < 0
      return
    end
 # x *= y inside while — candidate
    if @nd_type[nid] == "WhileNode"
      body = @nd_body[nid]
      if body >= 0
        scan_bigint_in_loop(body, bigint_names)
      end
    end
 # Recurse
    if @nd_body[nid] >= 0
      scan_bigint_candidates(@nd_body[nid], bigint_names)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_bigint_candidates(stmts[k], bigint_names)
      k = k + 1
    end
  end

 # Scan loop for simple assignments (x = y) and store as delimited string
 # Format: "dest1:src1,dest2:src2,..."
  def scan_loop_assigns(nid)
    if nid < 0
      return
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      expr = @nd_expression[nid]
      if expr >= 0 && @nd_type[expr] == "LocalVariableReadNode"
        @bi_assigns = @bi_assigns + @nd_name[nid] + ":" + @nd_name[expr] + ","
      end
    end
    if @nd_body[nid] >= 0
      scan_loop_assigns(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_loop_assigns(stmts[k])
      k = k + 1
    end
    if @nd_subsequent[nid] >= 0
      scan_loop_assigns(@nd_subsequent[nid])
    end
  end

 # Check if var_name can reach target_name via assignment chains
 # Assignment map is stored in @bi_assigns as "dest:src,dest:src,..."
  def bi_reaches(var_name, target_name, depth)
    if var_name == target_name
      return 1
    end
    if depth > 10
      return 0
    end
 # Search for assignments where src == var_name
    pairs = @bi_assigns.split(",")
    i = 0
    while i < pairs.length
      parts = pairs[i].split(":")
      if parts.length == 2
        if parts[1] == var_name
          if bi_reaches(parts[0], target_name, depth + 1) == 1
            return 1
          end
        end
      end
      i = i + 1
    end
    return 0
  end

 # Check if addition x = a + b has fibonacci-like growth (both operands
 # are variables that reach x via the assignment chain). Rejects i = i + 1
 # where one side is a constant.
  def add_is_unbounded(lname, expr)
    recv = @nd_receiver[expr]
    left_reaches = 0
    if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
      if bi_reaches(lname, @nd_name[recv], 0) == 1
        left_reaches = 1
      end
    end
    right_reaches = 0
    args_id = @nd_arguments[expr]
    if args_id != nil && args_id >= 0
      aargs = get_args(args_id)
      if aargs.length > 0 && @nd_type[aargs[0]] == "LocalVariableReadNode"
        if bi_reaches(lname, @nd_name[aargs[0]], 0) == 1
          right_reaches = 1
        end
      end
    end
 # Both sides must be reachable (fibonacci: c = a + b, a ← b, b ← c)
    if left_reaches == 1 && right_reaches == 1
      return 1
    end
    0
  end

 # Check if binary op x = a OP b has unbounded growth (self-referential via assigns)
  def op_is_unbounded(lname, expr)
    recv = @nd_receiver[expr]
    if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
      op = @nd_name[recv]
      if bi_reaches(lname, op, 0) == 1
        return 1
      end
    end
    args_id = @nd_arguments[expr]
    if args_id != nil && args_id >= 0
      aargs = get_args(args_id)
      if aargs.length > 0 && @nd_type[aargs[0]] == "LocalVariableReadNode"
        op = @nd_name[aargs[0]]
        if bi_reaches(lname, op, 0) == 1
          return 1
        end
      end
    end
    return 0
  end

  def scan_bigint_in_loop_node(nid, bigint_names)
    if nid < 0
      return
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      expr = @nd_expression[nid]
      if expr >= 0 && @nd_type[expr] == "CallNode"
        op = @nd_name[expr]
        if op == "*" || op == "**"
          if op_is_unbounded(lname, expr) == 1
            if not_in(lname, bigint_names) == 1
              bigint_names.push(lname)
            end
          end
        end
 # For +, only promote when BOTH operands are variables that
 # reach lname (fibonacci pattern: c = a + b where a,b grow).
 # Reject i = i + 1 (constant RHS → linear, fits int64).
        if op == "+"
          if add_is_unbounded(lname, expr) == 1
            if not_in(lname, bigint_names) == 1
              bigint_names.push(lname)
            end
          end
        end
      end
    end
    if @nd_type[nid] == "LocalVariableOperatorWriteNode"
      bop = @nd_binop[nid]
      if bop == "*" || bop == "**"
        lname = @nd_name[nid]
        if not_in(lname, bigint_names) == 1
          bigint_names.push(lname)
        end
      end
 # += is only unbounded if self-referential with another growing var
 # (not detected here since OpWriteNode is always x += expr)
    end
    if @nd_body[nid] >= 0
      scan_bigint_in_loop_node((@nd_body[nid]), bigint_names)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_bigint_in_loop_node(stmts[k], bigint_names)
      k = k + 1
    end
    if @nd_subsequent[nid] >= 0
      scan_bigint_in_loop_node((@nd_subsequent[nid]), bigint_names)
    end
  end

  def scan_bigint_in_loop(nid, bigint_names)
 # First pass: collect all simple assignments as delimited string
    @bi_assigns = ""
    scan_loop_assigns(nid)
 # Second pass: find multiplications and check if they're unbounded
    scan_bigint_in_loop_node(nid, bigint_names)
  end

 # Bigint promotion helpers — moved here from codegen.rb so the
 # cache-fill pass (refine_locals_multi_pass_full) can run identical
 # promotion logic before walk_and_cache visits the body. Keeping
 # them here also means codegen no longer needs them once it stops
 # doing its own scope refinement.
  def detect_bigint_vars(stmts, names, types)
    bigint_names = "".split(",")
    stmts.each { |sid|
      scan_bigint_candidates(sid, bigint_names)
    }
    k = 0
    while k < bigint_names.length
      ni = 0
      while ni < names.length
        if names[ni] == bigint_names[k]
          if types[ni] == "int"
            types[ni] = "bigint"
            @needs_bigint = 1
          end
        end
        ni = ni + 1
      end
      k = k + 1
    end
    if @needs_bigint == 1
      scan_bigint_propagate(stmts, names, types)
    end
  end

  def scan_bigint_propagate(stmts, names, types)
    changed = 1
    while changed == 1
      changed = 0
      stmts.each { |sid|
        changed = propagate_bigint_node(sid, names, types, changed)
      }
    end
  end

  def expr_uses_bigint(nid, names, types)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "LocalVariableReadNode"
      vn = @nd_name[nid]
      i = 0
      while i < names.length
        if names[i] == vn && types[i] == "bigint"
          return 1
        end
        i = i + 1
      end
      return 0
    end
    if @nd_type[nid] == "CallNode"
      if @nd_receiver[nid] >= 0
        if expr_uses_bigint(@nd_receiver[nid], names, types) == 1
          return 1
        end
      end
      args_id = @nd_arguments[nid]
      if args_id != nil && args_id >= 0
        aargs = get_args(args_id)
        ak = 0
        while ak < aargs.length
          if expr_uses_bigint(aargs[ak], names, types) == 1
            return 1
          end
          ak = ak + 1
        end
      end
    end
    if @nd_expression[nid] >= 0
      if expr_uses_bigint(@nd_expression[nid], names, types) == 1
        return 1
      end
    end
    if @nd_body[nid] >= 0
      if expr_uses_bigint(@nd_body[nid], names, types) == 1
        return 1
      end
    end
    st = parse_id_list(@nd_stmts[nid])
    si = 0
    while si < st.length
      if expr_uses_bigint(st[si], names, types) == 1
        return 1
      end
      si = si + 1
    end
    0
  end

  def propagate_bigint_node(nid, names, types, changed)
    if nid < 0
      return changed
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      expr = @nd_expression[nid]
      if expr >= 0 && expr_uses_bigint(expr, names, types) == 1
        li = 0
        while li < names.length
          if names[li] == lname && types[li] == "int"
            types[li] = "bigint"
            @needs_bigint = 1
            changed = 1
          end
          li = li + 1
        end
      end
    end
    if @nd_body[nid] >= 0
      changed = propagate_bigint_node(@nd_body[nid], names, types, changed)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      changed = propagate_bigint_node(stmts[k], names, types, changed)
      k = k + 1
    end
    changed
  end




 # Symbol-keyed hash with integer values. Keys are sp_sym (mrb_int);
 # the empty-slot sentinel is -1 (= invalid sp_sym, same as default).

 # Symbol-keyed hash with string values.

 # Symbol type Phase 2, Step 1: collect all SymbolNode content strings
 # into @sym_names as a separate pass (dedup, stable order).
  def collect_sym_names
 # Build into a local array and assign at the end.
 # (Pushing directly to @sym_names in this loop triggers a
 # self-host codegen regression — see HANDOFF notes.)
    local = "".split(",")
    i = 0
    while i < @nd_type.length
      t = @nd_type[i]
      if t == "SymbolNode"
        sname = @nd_content[i]
        if not_in(sname, local) == 1
          local.push(sname)
        end
      end
 # Also collect "literal".to_sym / .intern receivers so the
 # static-intern optimization can resolve them to SPS_ constants.
      if t == "CallNode"
        mn = @nd_name[i]
        if mn == "to_sym" || mn == "intern"
          r = @nd_receiver[i]
          if r >= 0 && @nd_type[r] == "StringNode"
            lname = @nd_content[r]
            if not_in(lname, local) == 1
              local.push(lname)
            end
          end
        end
      end
      i = i + 1
    end
    @sym_names = local
  end

 # Symbol type Phase 2, Step 2: emit the intern table and helpers.
 # SymbolNode now compiles to sp_sym values that index into sp_sym_names.

 # Index of symbol name in @sym_names, or -1 if not found.
  def sym_name_index(name)
    i = 0
    while i < @sym_names.length
      if @sym_names[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

 # Compile an expression in a string-context. Wraps with sp_sym_to_s
 # when the expression has type "symbol", otherwise returns the raw
 # expression. Used at boundaries where Symbol values flow into APIs
 # that still expect const char * (catch/throw tag, hash key, etc.).

 # Compile a symbol literal (by name) to a sp_sym C expression.
 # Prefers SPS_<name> for valid-C-identifier names, otherwise emits
 # the raw integer cast.
  def compile_symbol_literal(name)
    idx = sym_name_index(name)
    if idx < 0
 # Should not happen — collect_sym_names has already run.
      return "sp_sym_intern(" + c_string_literal(name) + ")"
    end
    if sym_is_c_ident(name) == 1
      return "SPS_" + name
    end
    "((sp_sym)" + idx.to_s + ")"
  end

 # True (1) iff s is a non-empty valid C identifier: [A-Za-z_][A-Za-z0-9_]*
  def sym_is_c_ident(s)
    if s.length == 0
      return 0
    end
    i = 0
    while i < s.length
      ch = s[i]
      ok = 0
      if ch == "_"
        ok = 1
      end
      if ch >= "A" && ch <= "Z"
        ok = 1
      end
      if ch >= "a" && ch <= "z"
        ok = 1
      end
      if i > 0 && ch >= "0" && ch <= "9"
        ok = 1
      end
      if ok == 0
        return 0
      end
      i = i + 1
    end
    1
  end


 # Per-call-site cached helper for InterpolatedRegularExpressionNode.
 # Each AST source location gets a `sp_re_dyn_<idx>(const char *new_pat)`
 # function with its own function-scope statics for the cached pattern
 # string and compiled engine pattern. On a cache hit (strcmp match) we
 # return the existing pattern; on a miss we re_free the old one and
 # recompile with the call-site's baked flags. This bounds heap to one
 # `mrb_regexp_pattern` per call site (count fixed at AOT compile time)
 # and matches Ruby's per-source-location dynamic-regexp cache. The old
 # `sp_re_runtime_compile` leaked a fresh pattern every evaluation.

 # ---- Struct emission ----

  def is_value_type_ivar(t)
    if t == "int" || t == "float" || t == "bool" || t == "string"
      return 1
    end
    if is_obj_type(t) == 1
      cname = t[4, t.length - 4]
      ci2 = find_class_idx(cname)
      if ci2 >= 0
        if @cls_is_value_type[ci2] == 1
          return 1
        end
      end
    end
    0
  end


 # The C expression that should be used wherever bare `self`
 # would normally appear (e.g. as the first arg to a same-class
 # method dispatch). Defaults to `"self"`; default-arg inlining
 # overrides it via `@self_override`.

  def subtree_has_ivar_write(nid)
    if nid < 0 || nid >= @nd_count
      return 0
    end
    t = @nd_type[nid]
    if t == "InstanceVariableWriteNode" || t == "InstanceVariableOperatorWriteNode" || t == "InstanceVariableTargetNode"
      return 1
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      if subtree_has_ivar_write(cs[k]) == 1
        return 1
      end
      k = k + 1
    end
    0
  end

  def is_simple_writer_method(mn, bid)
 # Check if method is a simple attr_writer pattern: def x=(v); @x = v; end
 # The RHS must be a bare reference to the parameter — `@x = v * 2`
 # is NOT a simple writer and must not bypass dispatch.
    if mn.length <= 1 || mn[mn.length - 1] != "="
      return 0
    end
    if bid < 0 || bid >= @nd_count
      return 0
    end
 # Find the single InstanceVariableWriteNode body (directly or wrapped
 # in a StatementsNode of length 1).
    t = @nd_type[bid]
    iv_id = -1
    if t == "InstanceVariableWriteNode"
      iv_id = bid
    elsif t == "StatementsNode"
      stmts = @nd_stmts[bid]
      if stmts != ""
        parts = stmts.split(",")
        if parts.length == 1
          sid = parts[0].to_i
          if sid >= 0 && sid < @nd_count && @nd_type[sid] == "InstanceVariableWriteNode"
            iv_id = sid
          end
        end
      end
    end
    if iv_id < 0
      return 0
    end
 # RHS must be a bare LocalVariableReadNode for the writer's single param.
    rhs = @nd_value[iv_id]
    if rhs < 0 || @nd_type[rhs] != "LocalVariableReadNode"
      return 0
    end
    1
  end

  def cls_has_self_mutating_methods(ci)
    mnames_str = @cls_meth_names[ci]
    if mnames_str == ""
      return 0
    end
    mnames = mnames_str.split(";")
    bodies = @cls_meth_bodies[ci].split(";")
    writers = @cls_attr_writers[ci].split(";")
    mi = 0
    while mi < mnames.length
      mn = mnames[mi]
      if mn != "initialize"
 # Skip registered attr_writers
        is_writer = 0
        bname = ""
        if mn.length > 1 && mn[mn.length - 1] == "="
          bname = mn[0, mn.length - 1]
          wi = 0
          while wi < writers.length
            if writers[wi] == bname
              is_writer = 1
            end
            wi = wi + 1
          end
        end
 # Also skip simple writer methods: def x=(v); @x = v; end
        if is_writer == 0 && mi < bodies.length
          bid = bodies[mi].to_i
          if is_simple_writer_method(mn, bid) == 1
            is_writer = 1
          end
        end
        if is_writer == 0 && mi < bodies.length
          bid = bodies[mi].to_i
          if bid >= 0 && subtree_has_ivar_write(bid) == 1
            return 1
          end
        end
      end
      mi = mi + 1
    end
    0
  end

  def auto_register_attr_writers
 # Detect manual attr_writer patterns: def x=(v); @x = v; end
 # and register them as attr_writers for direct field access
    i = 0
    while i < @cls_names.length
      mnames_str = @cls_meth_names[i]
      if mnames_str != ""
        mnames = mnames_str.split(";")
        bodies = @cls_meth_bodies[i].split(";")
        writers = @cls_attr_writers[i].split(";")
        mi = 0
        while mi < mnames.length
          mn = mnames[mi]
          bname = ""
          if mn.length > 1 && mn[mn.length - 1] == "="
            bname = mn[0, mn.length - 1]
 # Check if already registered
            already = 0
            wi = 0
            while wi < writers.length
              if writers[wi] == bname
                already = 1
              end
              wi = wi + 1
            end
            if already == 0 && mi < bodies.length
              bid = bodies[mi].to_i
              if is_simple_writer_method(mn, bid) == 1
                append_attr_writer(i, bname)
              end
            end
          end
          mi = mi + 1
        end
      end
      i = i + 1
    end
  end

  def is_simple_reader_method(mn, bid)
 # Check if method is a simple attr_reader pattern: def x; @x; end
    if bid < 0 || bid >= @nd_count
      return 0
    end
    t = @nd_type[bid]
    if t == "StatementsNode"
      stmts = @nd_stmts[bid]
      if stmts != ""
        parts = stmts.split(",")
        if parts.length == 1
          sid = parts[0].to_i
          if sid >= 0 && sid < @nd_count
            if @nd_type[sid] == "InstanceVariableReadNode"
              iname = @nd_name[sid]
              if iname == "@" + mn
                return 1
              end
            end
          end
        end
      end
    end
    if t == "InstanceVariableReadNode"
      iname = @nd_name[bid]
      if iname == "@" + mn
        return 1
      end
    end
    0
  end

  def auto_register_attr_readers
    i = 0
    while i < @cls_names.length
      mnames_str = @cls_meth_names[i]
      if mnames_str != ""
        mnames = mnames_str.split(";")
        bodies = @cls_meth_bodies[i].split(";")
        readers = @cls_attr_readers[i].split(";")
        mi = 0
        while mi < mnames.length
          mn = mnames[mi]
          if mn != "initialize" && mn.length > 0 && mn[mn.length - 1] != "="
            already = 0
            ri = 0
            while ri < readers.length
              if readers[ri] == mn
                already = 1
              end
              ri = ri + 1
            end
            if already == 0 && mi < bodies.length
              bid = bodies[mi].to_i
              if is_simple_reader_method(mn, bid) == 1
                append_attr_reader(i, mn)
              end
            end
          end
          mi = mi + 1
        end
      end
      i = i + 1
    end
  end

  def subtree_has_setter_on_params(nid, param_names)
    if nid < 0 || nid >= @nd_count
      return ""
    end
    t = @nd_type[nid]
 # Check: CallNode with setter name, receiver is a param
    if t == "CallNode"
      mn = @nd_name[nid]
      if mn != "" && mn.length > 1 && mn[mn.length - 1] == "="
        recv = @nd_receiver[nid]
        if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
          vname = @nd_name[recv]
          pi2 = 0
          while pi2 < param_names.length
            if param_names[pi2] == vname
              return vname
            end
            pi2 = pi2 + 1
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      r = subtree_has_setter_on_params(cs[k], param_names)
      if r != ""
        return r
      end
      k = k + 1
    end
    ""
  end

 # Walk `nid`'s subtree and collect every `Cls.new(...)` class name
 # into `out`. Used by detect_poly_returned_types to enumerate the
 # classes returned (directly or via a temp) from a poly-returning
 # method body.
  def collect_constructed_class_names(nid, out)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "new"
        recv = @nd_receiver[nid]
        if recv >= 0
          cname = constructor_class_name(recv)
          if cname != "" && find_class_idx(cname) >= 0
            obj_t = "obj_" + cname
            if not_in(obj_t, out) == 1
              out.push(obj_t)
            end
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_constructed_class_names(cs[k], out)
      k = k + 1
    end
  end

  def detect_poly_returned_types
 # Find object types `obj_<C>` constructed inside a method whose
 # inferred return type is `poly`. The return path boxes the value
 # into an `sp_RbVal` (`void *` payload); a value-type-eligible
 # class would emit `sp_box_obj(sp_<C>_new(...), ci)` which feeds a
 # struct-by-value into a `void *` slot — a C type error. Excluding
 # such classes from the value-type optimization keeps `<C>` heap-
 # allocated, so the constructor returns `sp_<C> *` and boxing is
 # well-typed. Mirrors the ptr_array exclusion .
    @poly_returned_types = "".split(",")
    mi = 0
    while mi < @meth_names.length
      if mi < @meth_return_types.length && @meth_return_types[mi] == "poly"
        bid = @meth_body_ids[mi]
        if bid >= 0
          collect_constructed_class_names(bid, @poly_returned_types)
        end
      end
      mi = mi + 1
    end
    ci = 0
    while ci < @cls_names.length
      bodies = @cls_meth_bodies[ci].split(";")
      returns = @cls_meth_returns[ci].split(";")
      mj = 0
      while mj < bodies.length
        if mj < returns.length && returns[mj] == "poly"
          bid = bodies[mj].to_i
          if bid >= 0
            collect_constructed_class_names(bid, @poly_returned_types)
          end
        end
        mj = mj + 1
      end
      ci = ci + 1
    end
  end

 # Track classes whose instances flow into a poly-typed param
 # slot at any call site. The boxing helper `sp_box_obj(p, ci)`
 # takes `void *p`; a value-type-eligible class would emit
 # `sp_box_obj(sp_<C>_new(...), ci)` or `sp_box_obj(local, ci)`
 # where `local` is the value-type struct, feeding a
 # struct-by-value into a `void *` slot — a C type error.
 # Excluding such classes from the value-type optimization keeps
 # `<C>` heap-allocated, so the boxing argument is always a
 # stable pointer.
 #
 # Surfaces when kwargs widening collapses two or more concrete
 # obj-typed call sites for the same kwarg into "poly" — at that
 # point the call site starts boxing the instance, hitting the
 # same struct-by-void-* mismatch the ptr_array / poly-return
 # passes already guard against.
  def detect_poly_arg_passed_types
    @poly_arg_passed_types = "".split(",")
    nid = 0
    while nid < @nd_type.length
      if @nd_type[nid] == "CallNode"
 # Top-level / module class method (no recv, or recv is module
 # constant). Look up the callee in @meth_*.
        mfn = ""
        recv = @nd_receiver[nid]
        mname = @nd_name[nid]
        if recv < 0
          mfn = mname
        else
          if @nd_type[recv] == "ConstantReadNode" || @nd_type[recv] == "ConstantPathNode"
            rcn = resolve_const_ref_name(recv)
            if rcn != "" && module_name_exists(rcn) == 1
              mfn = rcn + "_cls_" + mname
            end
          end
        end
        if mfn != ""
          mi = find_method_idx(mfn)
          if mi >= 0
            pnames = @meth_param_names[mi].split(",")
            ptypes = @meth_param_types[mi].split(",")
            args_id = @nd_arguments[nid]
            if args_id >= 0
              arg_ids = get_args(args_id)
              record_obj_args_into_poly_params(arg_ids, pnames, ptypes)
            end
          end
        end
 # Constructor `<C>.new(args)` — params live in @cls_meth_*.
        if mname == "new" && recv >= 0
          cname2 = constructor_class_name(recv)
          if cname2 != ""
            ci2 = find_class_idx(cname2)
            if ci2 >= 0
              init_idx = cls_find_method_direct(ci2, "initialize")
              if init_idx >= 0
                all_params = @cls_meth_params[ci2].split("|")
                all_ptypes = @cls_meth_ptypes[ci2].split("|")
                pnames2 = "".split(",")
                ptypes2 = "".split(",")
                if init_idx < all_params.length
                  pnames2 = all_params[init_idx].split(",")
                end
                if init_idx < all_ptypes.length
                  ptypes2 = all_ptypes[init_idx].split(",")
                end
                args_id2 = @nd_arguments[nid]
                if args_id2 >= 0
                  record_obj_args_into_poly_params(get_args(args_id2), pnames2, ptypes2)
                end
              end
            end
          end
        end
      end
      nid = nid + 1
    end
  end

  def record_obj_args_into_poly_params(arg_ids, pnames, ptypes)
    pos_idx = 0
    ai = 0
    while ai < arg_ids.length
      aid = arg_ids[ai]
      if @nd_type[aid] == "KeywordHashNode"
        elems = parse_id_list(@nd_elements[aid])
        ei = 0
        while ei < elems.length
          if @nd_type[elems[ei]] == "AssocNode"
            key_id = @nd_key[elems[ei]]
            val_id = @nd_expression[elems[ei]]
            if key_id >= 0 && val_id >= 0 && @nd_type[key_id] == "SymbolNode"
              kname = @nd_content[key_id]
              if kname == ""
                kname = @nd_name[key_id]
              end
              pi = 0
              while pi < pnames.length
                if pnames[pi] == kname && pi < ptypes.length && ptypes[pi] == "poly"
                  at = infer_type(val_id)
                  if is_obj_type(at) == 1
                    if not_in(at, @poly_arg_passed_types) == 1
                      @poly_arg_passed_types.push(at)
                    end
                  end
                end
                pi = pi + 1
              end
            end
          end
          ei = ei + 1
        end
      else
        if pos_idx < ptypes.length && ptypes[pos_idx] == "poly"
          at = infer_type(aid)
          if is_obj_type(at) == 1
            if not_in(at, @poly_arg_passed_types) == 1
              @poly_arg_passed_types.push(at)
            end
          end
        end
        pos_idx = pos_idx + 1
      end
      ai = ai + 1
    end
  end

  def detect_ptr_array_stored_types
 # Find object types `obj_<C>` that appear as the element type of an
 # array literal. Such an array becomes a `sp_PtrArray *` whose
 # `_push` takes `void *`; if `<C>` were optimized into a value type
 # then `sp_<C>_new(...)` would return the struct by value and the
 # generated push call would be a C type error.
    @ptr_array_stored_types = "".split(",")
    nid = 0
    while nid < @nd_type.length
      if @nd_type[nid] == "ArrayNode"
        at = infer_array_elem_type(nid)
        if is_ptr_array_type(at) == 1
          obj_t = ptr_array_elem_type(at)
          if is_obj_type(obj_t) == 1
            if not_in(obj_t, @ptr_array_stored_types) == 1
              @ptr_array_stored_types.push(obj_t)
            end
          end
        end
      end
 # Push-promotion path : an empty `[]` grows into an
 # `obj_<C>_ptr_array` via later `push(Foo.new(...))` / `<< Foo.new(...)`
 # calls. The literal-walk above doesn't see this because no
 # `[Foo.new(...)]` literal exists. Inspect every push-style call
 # and, if the argument's inferred type is `obj_<C>`, add `<C>`
 # to the exclusion so it stays heap-allocated.
      if @nd_type[nid] == "CallNode"
        mname = @nd_name[nid]
        if mname == "push" || mname == "<<" || mname == "unshift" || mname == "prepend"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            push_args = get_args(args_id)
            pk = 0
            while pk < push_args.length
              at_push = infer_type(push_args[pk])
              if is_obj_type(at_push) == 1
                if not_in(at_push, @ptr_array_stored_types) == 1
                  @ptr_array_stored_types.push(at_push)
                end
              end
              pk = pk + 1
            end
          end
        end
      end
      nid = nid + 1
    end
  end

  def detect_param_mutated_types
 # Find classes whose instances are mutated when passed as method parameters
    @param_mutated_types = "".split(",")
    i = 0
    while i < @cls_names.length
      mnames_str = @cls_meth_names[i]
      if mnames_str != ""
        mnames = mnames_str.split(";")
        all_params = @cls_meth_params[i].split("|")
        all_ptypes = @cls_meth_ptypes[i].split("|")
        bodies = @cls_meth_bodies[i].split(";")
        mi = 0
        while mi < mnames.length
          if mi < bodies.length && mi < all_params.length
            bid = bodies[mi].to_i
            pnames = all_params[mi].split(",")
            ptypes = "".split(",")
            if mi < all_ptypes.length
              ptypes = all_ptypes[mi].split(",")
            end
 # Collect object-type param names
            obj_param_names = "".split(",")
            obj_param_types = "".split(",")
            pj = 0
            while pj < pnames.length
              pt = "int"
              if pj < ptypes.length
                pt = ptypes[pj]
              end
              if is_obj_type(pt) == 1
                obj_param_names.push(pnames[pj])
                obj_param_types.push(pt)
              end
              pj = pj + 1
            end
            if obj_param_names.length > 0 && bid >= 0
              mutated_name = subtree_has_setter_on_params(bid, obj_param_names)
              if mutated_name != ""
 # Find the type of the mutated param
                pj = 0
                while pj < obj_param_names.length
                  if obj_param_names[pj] == mutated_name
                    @param_mutated_types.push(obj_param_types[pj])
                  end
                  pj = pj + 1
                end
              end
            end
          end
          mi = mi + 1
        end
      end
      i = i + 1
    end
 # Also check toplevel functions
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      pnames = @meth_param_names[mi].split(",")
      ptypes = @meth_param_types[mi].split(",")
      obj_param_names = "".split(",")
      obj_param_types = "".split(",")
      pj = 0
      while pj < pnames.length
        pt = "int"
        if pj < ptypes.length
          pt = ptypes[pj]
        end
        if is_obj_type(pt) == 1
          obj_param_names.push(pnames[pj])
          obj_param_types.push(pt)
        end
        pj = pj + 1
      end
      if obj_param_names.length > 0 && bid >= 0
        mutated_name = subtree_has_setter_on_params(bid, obj_param_names)
        if mutated_name != ""
          pj = 0
          while pj < obj_param_names.length
            if obj_param_names[pj] == mutated_name
              if not_in(obj_param_types[pj], @param_mutated_types) == 1
                @param_mutated_types.push(obj_param_types[pj])
              end
            end
            pj = pj + 1
          end
        end
      end
      mi = mi + 1
    end
  end

 # Build the set of class indices whose instances are captured by a
 # `method(:foo)` / `<obj>.method(:foo)` and end up stored in a
 # heap-allocated Method's `@self_obj`. Such classes must stay
 # heap-allocated — value-type optimization would put `self` on the
 # caller's stack, and the captured pointer would dangle when the
 # binding method returns. .
 #
 # The walk needs scope set up so `infer_type` on a chained
 # `<recv>.method(:foo)` can resolve `<recv>` (a local, an ivar via
 # attr_reader, or a chained call) back to its `obj_<X>` type.
  def detect_method_taken_classes
    @method_taken_class_indices = "".split(",")
 # Walk class methods first.
    i = 0
    while i < @cls_names.length
      mnames_str = @cls_meth_names[i]
      if mnames_str != ""
        mnames = mnames_str.split(";")
        all_params = @cls_meth_params[i].split("|")
        all_ptypes = @cls_meth_ptypes[i].split("|")
        bodies = @cls_meth_bodies[i].split(";")
        saved_ci = @current_class_idx
        @current_class_idx = i
        mi = 0
        while mi < mnames.length
          if mi < bodies.length
            bid = bodies[mi].to_i
            if bid >= 0
              pnames = ""
              ptypes = ""
              if mi < all_params.length
                pnames = all_params[mi]
              end
              if mi < all_ptypes.length
                ptypes = all_ptypes[mi]
              end
              method_taken_walk_with_scope(bid, pnames, ptypes, i)
            end
          end
          mi = mi + 1
        end
        @current_class_idx = saved_ci
      end
      i = i + 1
    end
 # Walk top-level methods.
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        method_taken_walk_with_scope(bid, @meth_param_names[mi], @meth_param_types[mi], -1)
      end
      mi = mi + 1
    end
 # Walk the top-level main script body too — `bm = make().method(:foo)`
 # at script scope must propagate the captured-receiver class into
 # the heap-stay set just like the same shape inside a method body.
    if @nd_type[@root_id] == "ProgramNode"
      stmts = get_body_stmts(@root_id)
      push_scope
      lnames = "".split(",")
      ltypes = "".split(",")
      sk = 0
      while sk < stmts.length
        scan_locals(stmts[sk], lnames, ltypes, "".split(","))
        sk = sk + 1
      end
      lk = 0
      while lk < lnames.length
        declare_var(lnames[lk], ltypes[lk])
        lk = lk + 1
      end
      sk = 0
      while sk < stmts.length
        collect_method_taken(stmts[sk], -1)
        sk = sk + 1
      end
      pop_scope
    end
  end

  def method_taken_walk_with_scope(bid, pnames_str, ptypes_str, enclosing_ci)
    push_scope
    pnames = pnames_str.split(",")
    ptypes = ptypes_str.split(",")
    k = 0
    while k < pnames.length
      pt = "int"
      if k < ptypes.length
        pt = ptypes[k]
      end
      declare_var(pnames[k], pt)
      k = k + 1
    end
    lnames = "".split(",")
    ltypes = "".split(",")
    scan_locals(bid, lnames, ltypes, pnames)
    lk = 0
    while lk < lnames.length
      declare_var(lnames[lk], ltypes[lk])
      lk = lk + 1
    end
    collect_method_taken(bid, enclosing_ci)
    pop_scope
  end

  def collect_method_taken(nid, enclosing_ci)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "method"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 1
          recv = @nd_receiver[nid]
          target = -1
          if recv < 0
            target = enclosing_ci
          else
            rt = infer_type(recv)
            if is_obj_type(rt) == 1
              target = find_class_idx(rt[4, rt.length - 4])
            end
          end
          if target >= 0 && not_in(target.to_s, @method_taken_class_indices) == 1
            @method_taken_class_indices.push(target.to_s)
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_method_taken(cs[k], enclosing_ci)
      k = k + 1
    end
  end

  def recalc_needs_gc
 # Recalculate @needs_gc: only needed if non-value-type classes are used
    @needs_gc = 0
 # Non-value-type class exists → GC needed
    i = 0
    while i < @cls_names.length
      if @cls_is_value_type[i] == 0
        @needs_gc = 1
      end
      i = i + 1
    end
 # If there were other GC triggers (arrays, hashes, etc.) but no heap classes,
 # those built-in types handle their own memory (malloc/free), not GC.
 # However, IntArray/StrArray etc. ARE GC-allocated, so we need to check.
    if @needs_gc == 0
      if @needs_int_array == 1 || @needs_float_array == 1 || @needs_str_array == 1
        @needs_gc = 1
      end
      if @needs_str_int_hash == 1 || @needs_str_str_hash == 1 || @needs_int_str_hash == 1
        @needs_gc = 1
      end
      if @needs_sym_int_hash == 1 || @needs_sym_str_hash == 1
        @needs_gc = 1
      end
      if @needs_mutable_str == 1 || @needs_stringio == 1
        @needs_gc = 1
      end
      if @needs_rb_value == 1 || @needs_lambda == 1 || @needs_fiber == 1
        @needs_gc = 1
      end
      if @needs_bigint == 1
        @needs_gc = 1
      end
    end
  end

  def detect_value_types
    auto_register_attr_readers
    auto_register_attr_writers
    detect_param_mutated_types
    detect_ptr_array_stored_types
    detect_poly_returned_types
    detect_poly_arg_passed_types
    detect_method_taken_classes
 # Multiple passes: value type detection depends on other classes
    2.times do
      i = 0
      while i < @cls_names.length
        names = @cls_ivar_names[i].split(";")
        types = @cls_ivar_types[i].split(";")
 # Value-type candidates: small immutable scalar classes.
 # Limit to 8 ivars so the struct stays register-friendly.
        if names.length > 0 && names.length <= 8
          all_val = 1
          j = 0
          while j < types.length
            if is_value_type_ivar(types[j]) == 0
              all_val = 0
            end
            j = j + 1
          end
 # Exclude classes with self-mutating methods or attr_writers
          if all_val == 1
            if cls_has_self_mutating_methods(i) == 1
              all_val = 0
            end
            writers = @cls_attr_writers[i].split(";")
            if writers.length > 0 && writers[0] != ""
              all_val = 0
            end
          end
 # Exclude classes involved in inheritance
          if all_val == 1
 # Has a parent class
            if @cls_parents[i] != ""
              all_val = 0
            end
 # Has subclasses
            si = 0
            while si < @cls_names.length
              if @cls_parents[si] == @cls_names[i]
                all_val = 0
              end
              si = si + 1
            end
          end
 # Exclude classes whose instances are param-mutated
          if all_val == 1
            type_str = "obj_" + @cls_names[i]
            pmi = 0
            while pmi < @param_mutated_types.length
              if @param_mutated_types[pmi] == type_str
                all_val = 0
              end
              pmi = pmi + 1
            end
          end
 # Exclude classes whose instances are pushed into a ptr_array
 # (array literal of `obj_<C>` becomes a `sp_PtrArray *` whose
 # `_push` takes `void *`; a value-type return from `Foo.new`
 # is a struct by value and can't be passed through `void *`).
          if all_val == 1
            type_str = "obj_" + @cls_names[i]
            psi = 0
            while psi < @ptr_array_stored_types.length
              if @ptr_array_stored_types[psi] == type_str
                all_val = 0
              end
              psi = psi + 1
            end
          end
 # Exclude classes constructed inside a method whose inferred
 # return type is `poly`. The poly return path boxes via
 # `sp_box_obj(sp_<C>_new(...), ci)` — same struct-by-value /
 # void* mismatch as the ptr_array case .
          if all_val == 1
            type_str = "obj_" + @cls_names[i]
            pri = 0
            while pri < @poly_returned_types.length
              if @poly_returned_types[pri] == type_str
                all_val = 0
              end
              pri = pri + 1
            end
          end
 # Same value-type exclusion for classes whose instances
 # flow into a poly-typed param at any call site. When
 # kwargs widening collapses two obj types into "poly",
 # the call site boxes a value-type instance and
 # mismatches sp_box_obj's `void *` slot.
          if all_val == 1
            obj_type_str = "obj_" + @cls_names[i]
            pai = 0
            while pai < @poly_arg_passed_types.length
              if @poly_arg_passed_types[pai] == obj_type_str
                all_val = 0
              end
              pai = pai + 1
            end
          end
 # Exclude classes whose instances are captured by a Method
 # (via `method(:foo)` on self, or `<obj>.method(:foo)` from
 # anywhere). The captured receiver must be a stable heap
 # pointer; value-type instances live on the caller's stack
 # and the pointer would dangle once the binding method
 # returns. .
          if all_val == 1
            if not_in(i.to_s, @method_taken_class_indices) == 0
              all_val = 0
            end
          end
          if all_val == 1
            @cls_is_value_type[i] = 1
          end
        end
        i = i + 1
      end
    end
 # SRA eligibility (Phase 2a): like value-type but allows attr_writer.
 # The per-instance escape check happens separately at use sites.
    i = 0
    while i < @cls_names.length
      if @cls_is_value_type[i] == 1
 # Already handled as value-type; SRA redundant for these.
        i = i + 1
        next
      end
      names = @cls_ivar_names[i].split(";")
      types = @cls_ivar_types[i].split(";")
      eligible = 1
      if names.length == 0 || names.length > 8
        eligible = 0
      end
      j = 0
      while eligible == 1 && j < types.length
        t = types[j]
        if t != "int" && t != "float" && t != "bool"
          eligible = 0
        end
        j = j + 1
      end
 # No inheritance
      if eligible == 1 && @cls_parents[i] != ""
        eligible = 0
      end
      if eligible == 1
        si = 0
        while si < @cls_names.length
          if @cls_parents[si] == @cls_names[i]
            eligible = 0
          end
          si = si + 1
        end
      end
 # Only initialize + attr_* methods (no custom methods).
      if eligible == 1
        mnames = @cls_meth_names[i].split(";")
        readers = @cls_attr_readers[i].split(";")
        writers = @cls_attr_writers[i].split(";")
        mk = 0
        while mk < mnames.length
          mn = mnames[mk]
 # allowed: initialize, any attr_reader/writer name
          if mn != "initialize" && not_in(mn, readers) == 1 && not_in(mn, writers) == 1
            eligible = 0
          end
          mk = mk + 1
        end
      end
 # Method-captured receivers must stay heap-allocated. SRA
 # explodes the instance into stack-resident scalars, which is
 # the same dangling-pointer hazard as value-type. Defensive:
 # current test corpus doesn't exercise an SRA-eligible class
 # (initialize + attr_* only) being captured, but a future
 # `class X; attr_accessor :n; end` followed by
 # `X.new(...).method(:n)` would. .
      if eligible == 1
        if not_in(i.to_s, @method_taken_class_indices) == 0
          eligible = 0
        end
      end
      if eligible == 1
        @cls_is_sra[i] = 1
      end
      i = i + 1
    end
  end

 # Return "static inline " for short methods so gcc has permission
 # to inline them, or "static " otherwise. Body of ≤ 3 statements,
 # no yield, and not self-recursive are considered inlineable.


 # Return 1 if any CallNode in the subtree invokes mname.








  def ivar_in_chain(ci, iname)
    names = @cls_ivar_names[ci].split(";")
    k = 0
    while k < names.length
      if names[k] == iname
        return 1
      end
      k = k + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return ivar_in_chain(pi, iname)
      end
    end
    0
  end





 # ---- Forward declarations ----



 # Build the block fn-pointer C signature from the yield call
 # sites' inferred arg types. block_params_csig alone returns
 # `mrb_int, mrb_int, ...` regardless of what's yielded; this
 # variant inspects the body so a method that yields (String,
 # sp_RbVal) gets `void (*)(const char *, sp_RbVal, void *)` —
 # matching the emitted `_block(lv_k, lv_v, _benv)` call site.
 #
 # Called BEFORE the function's scope has been pushed, so
 # locally-declared vars referenced in yield args (`yield k, v`
 # where `k`, `v` were assigned from typed expressions) are not
 # in @scope_names. Run scan_locals on the body and push a
 # temporary scope so body_yield_arg_types' infer_type calls
 # resolve those reads. Re-runs at both forward-decl emit time
 # and impl emit time; results converge once @meth_* / @cls_*
 # tables stabilize through the type-inference fixpoint.

 # Walk `nid` for YieldNode, accumulating the per-position arg
 # type into `types`. Stops at nested DefNode boundaries (those
 # introduce a new method scope with its own yield arity).
 # Mirrors body_max_yield_arity's traversal shape.


 # Max number of args used in any `yield` inside the top-level method
 # at @meth_body_ids[mi]. Floor of 1 — yield-using methods always have
 # at least one mrb_int slot (the no-arg `yield` form is padded to 0).

 # Same as method_yield_arity, but resolved through the class method
 # body table @cls_meth_bodies (parallel to @cls_meth_has_yield).

 # Comma-joined string of `arity` mrb_int slots — the variable-arity
 # portion of the `_block` function-pointer signature.

 # Returns 1 if the (ci, midx) method declares a `&block` parameter,
 # 0 otherwise. Ruby syntax requires `&block` to be the trailing
 # param, so we check only the last slot — a proc-typed slot in any
 # other position is a positional proc argument, not a block param.
 # Mirrors cls_method_has_yield: call sites use it to decide whether
 # to omit the trailing &block slot from default-padding.

 # Returns the name of a method's `&block` parameter (the trailing
 # proc-typed slot in pnames), or "" if the method doesn't take
 # one. Ruby syntax requires `&block` to be the trailing param, so
 # a proc-typed slot in any other position is a positional proc
 # argument. Mirrors cls_method_has_block_param's trailing-only
 # check. Used at method-emit time to set @current_method_block_param
 # so block_given? can resolve to (lv_<name> != NULL).

  def cls_find_method_direct(ci, mname)
    ck = ci.to_s + ":" + mname
    if @cls_meth_idx_cache.key?(ck)
      return @cls_meth_idx_cache[ck]
    end
    mnames = @cls_meth_names[ci].split(";")
    j = 0
    while j < mnames.length
      if mnames[j] == mname
        @cls_meth_idx_cache[ck] = j
        return j
      end
      j = j + 1
    end
    @cls_meth_idx_cache[ck] = -1
    -1
  end

 # Helpers for the per-method split-join boilerplate around
 # @cls_meth_ptypes / @cls_cmeth_ptypes / @cls_meth_params /
 # @cls_cmeth_params. Each table stores per-class strings where
 # methods are pipe-separated (`|`) and per-method names/types
 # are comma-separated (`,`). Without these, every read site
 # opens with two split() calls + bound checks and every write
 # site closes with two join() calls.
  def cls_meth_ptypes_get(ci, midx)
    if ci < 0 || ci >= @cls_meth_ptypes.length || midx < 0
      return "".split(",")
    end
    if @cmp_outer_version != @cls_meth_ptypes_version || @cmp_outer_ci != ci
      @cmp_outer_split = @cls_meth_ptypes[ci].split("|")
      @cmp_outer_ci = ci
      @cmp_outer_version = @cls_meth_ptypes_version
    end
    if midx >= @cmp_outer_split.length
      return "".split(",")
    end
    @cmp_outer_split[midx].split(",")
  end

  def cls_meth_ptypes_put(ci, midx, ptypes)
    if ci < 0 || ci >= @cls_meth_ptypes.length || midx < 0
      return
    end
    if @cmp_outer_version != @cls_meth_ptypes_version || @cmp_outer_ci != ci
      @cmp_outer_split = @cls_meth_ptypes[ci].split("|")
      @cmp_outer_ci = ci
      @cmp_outer_version = @cls_meth_ptypes_version
    end
    if midx >= @cmp_outer_split.length
      return
    end
    @cmp_outer_split[midx] = ptypes.join(",")
    @cls_meth_ptypes[ci] = @cmp_outer_split.join("|")
    @cls_meth_ptypes_version = @cls_meth_ptypes_version + 1
 # The cache slot was mutated in place to match the new joined
 # string, so it stays valid — keep the local version in sync
 # so subsequent reads with the same ci skip the outer split.
    @cmp_outer_version = @cls_meth_ptypes_version
  end

  def cls_meth_pnames_get(ci, midx)
    if ci < 0 || ci >= @cls_meth_params.length || midx < 0
      return "".split(",")
    end
    if @cmn_outer_version != @cls_meth_params_version || @cmn_outer_ci != ci
      @cmn_outer_split = @cls_meth_params[ci].split("|")
      @cmn_outer_ci = ci
      @cmn_outer_version = @cls_meth_params_version
    end
    if midx >= @cmn_outer_split.length
      return "".split(",")
    end
    @cmn_outer_split[midx].split(",")
  end

  def cls_cmeth_ptypes_get(ci, midx)
    if ci < 0 || ci >= @cls_cmeth_ptypes.length || midx < 0
      return "".split(",")
    end
    if @ccmp_outer_version != @cls_cmeth_ptypes_version || @ccmp_outer_ci != ci
      @ccmp_outer_split = @cls_cmeth_ptypes[ci].split("|")
      @ccmp_outer_ci = ci
      @ccmp_outer_version = @cls_cmeth_ptypes_version
    end
    if midx >= @ccmp_outer_split.length
      return "".split(",")
    end
    @ccmp_outer_split[midx].split(",")
  end

  def cls_cmeth_ptypes_put(ci, midx, ptypes)
    if ci < 0 || ci >= @cls_cmeth_ptypes.length || midx < 0
      return
    end
    if @ccmp_outer_version != @cls_cmeth_ptypes_version || @ccmp_outer_ci != ci
      @ccmp_outer_split = @cls_cmeth_ptypes[ci].split("|")
      @ccmp_outer_ci = ci
      @ccmp_outer_version = @cls_cmeth_ptypes_version
    end
    if midx >= @ccmp_outer_split.length
      return
    end
    @ccmp_outer_split[midx] = ptypes.join(",")
    @cls_cmeth_ptypes[ci] = @ccmp_outer_split.join("|")
    @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
    @ccmp_outer_version = @cls_cmeth_ptypes_version
  end

  def cls_cmeth_pnames_get(ci, midx)
    if ci < 0 || ci >= @cls_cmeth_params.length || midx < 0
      return "".split(",")
    end
    if @ccmn_outer_version != @cls_cmeth_params_version || @ccmn_outer_ci != ci
      @ccmn_outer_split = @cls_cmeth_params[ci].split("|")
      @ccmn_outer_ci = ci
      @ccmn_outer_version = @cls_cmeth_params_version
    end
    if midx >= @ccmn_outer_split.length
      return "".split(",")
    end
    @ccmn_outer_split[midx].split(",")
  end

 # Walk the inheritance chain starting at class `ci` looking for the
 # first ancestor that defines `mname` directly. Returns that class's
 # name, or "" if no ancestor defines it. Used by the
 # `method(:foo)` codegen to build the C symbol of the bound function:
 # an inherited method's generated C function lives under the
 # *defining* class (`sp_Parent_foo`), not the receiver's own class
 # (`sp_Child_foo`), so `&sp_Child_foo` would be an unresolved symbol
 # at link time. When this returns "" the codegen falls back to a
 # null fn_ptr — Ruby's NoMethodError-on-invoke equivalent. .


  def find_init_class(ci)
 # Find which class in the chain has initialize
    init_idx = cls_find_method_direct(ci, "initialize")
    if init_idx >= 0
      return ci
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return find_init_class(pi)
      end
    end
    -1
  end





 # ---- Emit class methods ----

 # Dead-code elimination for class methods. An uncalled
 # `def self.factory(attrs); new(attrs); ...; end` on a parent
 # class whose own `initialize` has different arity would emit a
 # body that doesn't C-compile (`sp_<class>_new(args)` against a
 # 0-arg constructor; or, with a default-arg `initialize`,
 # against a typed param the uncalled method itself can't have
 # inferred). The reachability set seeds from explicit
 # `<Class>.<m>(...)` call sites in the AST, then propagates
 # through bare/self calls inside live cls method bodies; anything
 # not reached is skipped at forward-decl + body emit time.
 #
 # Live entries are stored as `<ClassName>::<methodName>` joined
 # by `;` in @cls_cmeth_live. Idempotent: marking an already-live
 # entry is a no-op.
  def compute_live_cls_methods
    @cls_cmeth_live = ""
    collect_cls_calls(@root_id, -1)
    iter = 0
    prev_len = @cls_cmeth_live.length
    while iter < 8
      i = 0
      while i < @cls_names.length
        cmnames = @cls_cmeth_names[i].split(";")
        cm_bodies = @cls_cmeth_bodies[i].split(";")
        j = 0
        while j < cmnames.length
          if cmnames[j] != "" && cls_cmeth_is_live(i, cmnames[j]) == 1
            bid = -1
            if j < cm_bodies.length
              bs = cm_bodies[j]
              if bs != ""
                bid = bs.to_i
              end
            end
            if bid > 0
              collect_cls_calls(bid, i)
            end
          end
          j = j + 1
        end
        i = i + 1
      end
      cur_len = @cls_cmeth_live.length
      if cur_len == prev_len
        return
      end
      prev_len = cur_len
      iter = iter + 1
    end
  end
 # . Mark instance methods reachable from the program
 # entry. Anything not marked gets a stub body at emit time
 # (`(void)params; return default;`) so an uncalled `def f(x); @typed = x; end`
 # whose param defaulted to int doesn't fail C-compile against a
 # narrower ivar slot.
 #
 # Conservative liveness: a method `M` on class `C` is live iff:
 # 1. `M` == "initialize" (constructor synth always reaches it).
 # 2. Some CallNode anywhere in the program names `M`.
 # 3. Some SymbolNode anywhere has value `M` (covers method(:M) /
 # define_method(:M) / send(:M, ...) reflection sites).
 # This over-approximates -- a method named "size" on user class `C`
 # is marked live whenever ANY call site (even an unrelated
 # `arr.size`) names "size". That's fine for the DCE goal; the
 # downside is false-negatives (a genuinely-unused method we keep)
 # rather than false-positives (a live method we strip).
  def compute_live_instance_methods
    @cls_meth_live = ""
 # Step 1: methods whose name is always-implicitly-dispatched are
 # marked live unconditionally. `<=>` is reached by Comparable's
 # `<` / `>` etc. operators (compile_call_expr's cmp_owner arm
 # synthesises the call, so `<=>` itself never appears as a
 # CallNode name in the AST — and the conservative "name appears
 # somewhere" rule below would miss it). Same for the bracket
 # operators, common conversion methods, and `inspect` / `to_s`
 # called by string interpolation, `puts`, etc.
    always_live = ["initialize", "<=>", "==", "!=", "eql?", "hash",
                   "to_s", "inspect", "to_a", "to_i", "to_f", "to_str",
                   "[]", "[]=", "each", "<", ">", "<=", ">=",
                   "+", "-", "*", "/", "%", "<<", "coerce", "<<="]
    ci = 0
    while ci < @cls_names.length
      mnames_init = @cls_meth_names[ci].split(";")
      mj_init = 0
      while mj_init < mnames_init.length
        ai = 0
        while ai < always_live.length
          if mnames_init[mj_init] == always_live[ai]
            cls_meth_mark_live(ci, always_live[ai])
          end
          ai = ai + 1
        end
        mj_init = mj_init + 1
      end
      ci = ci + 1
    end
 # Step 2/3: collect every CallNode name + SymbolNode value, mark
 # methods on every class that has them.
 # `"".split(",")` initializer types `used` as a str_array, since
 # `acc` in collect_used_method_names is pushed strings only.
    used = "".split(",")
    collect_used_method_names(@root_id, used)
 # Lifted instance_eval blocks: their bodies were detached from
 # @root_id by ieval_rewrite_call and stashed in @ieval_body_ids,
 # so a walk from @root_id misses any `get("/")` / similar call
 # buried inside `app.instance_eval { get("/") }`. Walk them too.
    ie = 0
    while ie < @ieval_body_ids.length
      collect_used_method_names(@ieval_body_ids[ie], used)
      ie = ie + 1
    end
 # User-defined method bodies: top-level methods (@meth_body_ids)
 # and instance / class methods (@cls_meth_bodies + @cls_cmeth_bodies).
 # The @root_id walk hits class definitions but those routes only to
 # ClassNode -> body, not to each method body's nested call sites.
    mi_b = 0
    while mi_b < @meth_body_ids.length
      collect_used_method_names(@meth_body_ids[mi_b], used)
      mi_b = mi_b + 1
    end
    ci_b = 0
    while ci_b < @cls_names.length
      mb_list = @cls_meth_bodies[ci_b].split(";")
      mbk = 0
      while mbk < mb_list.length
        bid_mb = mb_list[mbk].to_i
        if bid_mb > 0
          collect_used_method_names(bid_mb, used)
        end
        mbk = mbk + 1
      end
      cmb_list = @cls_cmeth_bodies[ci_b].split(";")
      cmbk = 0
      while cmbk < cmb_list.length
        bid_cmb = cmb_list[cmbk].to_i
        if bid_cmb > 0
          collect_used_method_names(bid_cmb, used)
        end
        cmbk = cmbk + 1
      end
      ci_b = ci_b + 1
    end
    ui = 0
    while ui < used.length
      uname = used[ui]
      ci2 = 0
      while ci2 < @cls_names.length
        mns = @cls_meth_names[ci2].split(";")
        mj2 = 0
        while mj2 < mns.length
          if mns[mj2] == uname
            cls_meth_mark_live(ci2, uname)
          end
          mj2 = mj2 + 1
        end
        ci2 = ci2 + 1
      end
      ui = ui + 1
    end
  end

 # Walk the AST collecting CallNode names + SymbolNode values into
 # `acc`. Used by compute_live_instance_methods. Iterative (explicit
 # stack via `push_child_ids`) so we don't recurse into 600+-deep
 # method bodies and blow the stack.
  def collect_used_method_names(nid, acc)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "CallNode"
      cname = @nd_name[nid]
      if cname != "" && not_in(cname, acc) == 1
        acc.push(cname)
      end
    end
    if t == "SymbolNode"
      sval = @nd_content[nid]
      if sval != "" && not_in(sval, acc) == 1
        acc.push(sval)
      end
    end
    cs = []
    push_child_ids(nid, cs)
    ck = 0
    while ck < cs.length
      collect_used_method_names(cs[ck], acc)
      ck = ck + 1
    end
  end

  def cls_meth_is_live(ci, mname)
    if ci < 0 || ci >= @cls_names.length
      return 0
    end
    if @cls_meth_live == nil || @cls_meth_live == ""
      return 0
    end
    needle = ";" + @cls_names[ci] + "::" + mname + ";"
    haystack = ";" + @cls_meth_live + ";"
    if haystack.include?(needle)
      return 1
    end
    0
  end

  def cls_meth_mark_live(ci, mname)
    if ci < 0 || ci >= @cls_names.length
      return
    end
    if cls_meth_is_live(ci, mname) == 1
      return
    end
    entry = @cls_names[ci] + "::" + mname
    if @cls_meth_live == ""
      @cls_meth_live = entry
    else
      @cls_meth_live = @cls_meth_live + ";" + entry
    end
  end


  def cls_cmeth_is_live(ci, mname)
    if ci < 0 || ci >= @cls_names.length
      return 0
    end
    if @cls_cmeth_live == nil || @cls_cmeth_live == ""
      return 0
    end
 # Sentinel-bracketed lookup so "Foo::bar" doesn't match
 # "Foo::barbaz". `String#include?` returns a clean true/false in
 # both CRuby and spinel; `String#index` works in CRuby but in
 # spinel returns mrb_int (-1 / 0+) and `ix == nil` compiles to
 # `ix == 0`, misclassifying the first-listed entry as missing.
    needle = ";" + @cls_names[ci] + "::" + mname + ";"
    haystack = ";" + @cls_cmeth_live + ";"
    if haystack.include?(needle)
      return 1
    end
    0
  end

  def cls_cmeth_mark_live(ci, mname)
    if ci < 0 || ci >= @cls_names.length
      return
    end
    cmnames = @cls_cmeth_names[ci].split(";")
    has = 0
    k = 0
    while k < cmnames.length
      if cmnames[k] == mname
        has = 1
      end
      k = k + 1
    end
    if has == 0
      return
    end
    if cls_cmeth_is_live(ci, mname) == 1
      return
    end
    entry = @cls_names[ci] + "::" + mname
    if @cls_cmeth_live == ""
      @cls_cmeth_live = entry
    else
      @cls_cmeth_live = @cls_cmeth_live + ";" + entry
    end
  end

 # Walk the subtree at `nid`, marking cls methods reached by:
 # - `<Const>.<m>(...)` (any context)
 # - `self.<m>(...)` (only when ctx_ci >= 0, i.e.
 # we're walking inside a cls method body of class ctx_ci)
 # - bare `<m>(...)` with no recv (ditto; `<m>` resolves to
 # a cls method on ctx_ci if one exists with that name)
 #
 # ctx_ci is propagated unchanged through normal nodes, but cleared
 # to -1 when descending into a DefNode body (a nested method has
 # its own scope; bare calls there resolve to top-level methods or
 # to that DefNode's own class context, neither of which we want
 # to attribute to the outer cls method).
  def collect_cls_calls(nid, ctx_ci)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      recv = @nd_receiver[nid]
      mname = @nd_name[nid]
      if recv >= 0
        if @nd_type[recv] == "ConstantReadNode" || @nd_type[recv] == "ConstantPathNode"
          cname = constructor_class_name(recv)
          if cname != ""
 # constructor_class_name resolves via lexical scope, but
 # this DCE walker runs without method-body context. Fall
 # back to suffix-matching when the unscoped name doesn't
 # match a registered class — `CPU` (called inside
 # `Optcarrot::CPU`) needs to mark `Optcarrot_CPU::poke_nop`
 # live, but cname is plain `CPU`. Walks every cls name
 # and marks any matching suffix.
            cm_lit_for_method = ""
            if mname == "method"
              args_ic = @nd_arguments[nid]
              if args_ic >= 0
                ac_arg_ids = get_args(args_ic)
                if ac_arg_ids.length >= 1
                  cm_lit_for_method = @nd_content[ac_arg_ids[0]]
                  if cm_lit_for_method == ""
                    cm_lit_for_method = @nd_name[ac_arg_ids[0]]
                  end
                end
              end
            end
            mark_idx = 0
            while mark_idx < @cls_names.length
              cn_full = @cls_names[mark_idx]
              if cn_full == cname || cn_full.end_with?("_" + cname)
                cls_cmeth_mark_live(mark_idx, mname)
 # `Klass.method(:cls_meth)` — keeps the *named* cls
 # method live so the adapter trampoline emitted by
 # compile_constant_recv_expr resolves to a real
 # symbol at link time. Without this, the cls method
 # is DCE'd (no direct call site, only an indirect
 # bind) and the adapter references an undefined
 # function.
                if cm_lit_for_method != ""
                  cls_cmeth_mark_live(mark_idx, cm_lit_for_method)
                end
              end
              mark_idx = mark_idx + 1
            end
          end
        end
        if @nd_type[recv] == "SelfNode" && ctx_ci >= 0
          cls_cmeth_mark_live(ctx_ci, mname)
        end
 # `<Module>.<acc>.<method>` where `<Module>.<acc>` was
 # constant-folded by resolve_module_singleton_accessors to
 # one or more class names. The receiver is a CallNode at
 # AST level, so the ConstantReadNode branch above doesn't
 # catch it; look up the fold and mark each resolved
 # class's cls method live.
        if @nd_type[recv] == "CallNode"
          inner_recv = @nd_receiver[recv]
          if inner_recv >= 0 && @nd_type[inner_recv] == "ConstantReadNode"
            mod_name = @nd_name[inner_recv]
            resolved = module_acc_resolved(mod_name, @nd_name[recv])
            if resolved != "" && resolved != "?"
              rnames = resolved.split(";")
              ri = 0
              while ri < rnames.length
                tci2 = find_class_idx(rnames[ri])
                if tci2 >= 0
                  cls_cmeth_mark_live(tci2, mname)
                end
                ri = ri + 1
              end
            end
          end
 # `<obj>.class.<cmeth>` — recv is a `class`
 # CallNode. The lowered call dispatches to <C>::cmeth at
 # codegen time when the inner recv has a known obj_<C>
 # type. At DCE time we don't have method-body scope
 # context (so infer_type on a LocalVariableReadNode here
 # falls back to "int"), so over-approximate: mark cmeth
 # live on every class that defines one. Matches the
 # ConstantReadNode / SymbolNode arms above which take the
 # same shape — DCE prefers false-negatives (a kept-but-
 # unused method) over false-positives (a stripped live
 # method that produces a linker error).
          if @nd_name[recv] == "class"
            mark_idx = 0
            while mark_idx < @cls_names.length
              cls_cmeth_mark_live(mark_idx, mname)
              mark_idx = mark_idx + 1
            end
          end
        end
      else
        if ctx_ci >= 0
          cls_cmeth_mark_live(ctx_ci, mname)
        end
      end
    end
 # In phase A (ctx_ci < 0, walking top-level / class-body /
 # module-body context), descend into DefNode bodies so
 # `<Const>.<m>(...)` call sites inside instance methods get
 # picked up — `def initialize; @x = TheClass.new_inner; end`
 # otherwise leaves `TheClass::new_inner` un-marked and DCE
 # drops the body while the call site remains, producing a
 # linker error. Bare/self calls inside the descended body
 # stay unmatched because ctx_ci is still -1.
 #
 # In phase B (ctx_ci >= 0, walking inside a live cls method
 # body), skip nested DefNodes — their scope differs and
 # bare/self calls inside should not be attributed to the
 # outer cls method's class.
    if @nd_type[nid] == "DefNode" && ctx_ci >= 0
      return
    end
    if @nd_body[nid] >= 0
      collect_cls_calls(@nd_body[nid], ctx_ci)
    end
    cs_stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < cs_stmts.length
      collect_cls_calls(cs_stmts[k], ctx_ci)
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      collect_cls_calls(@nd_receiver[nid], ctx_ci)
    end
    if @nd_arguments[nid] >= 0
      collect_cls_calls(@nd_arguments[nid], ctx_ci)
    end
    cs_args = parse_id_list(@nd_args[nid])
    k = 0
    while k < cs_args.length
      collect_cls_calls(cs_args[k], ctx_ci)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      collect_cls_calls(@nd_expression[nid], ctx_ci)
    end
    if @nd_predicate[nid] >= 0
      collect_cls_calls(@nd_predicate[nid], ctx_ci)
    end
    if @nd_subsequent[nid] >= 0
      collect_cls_calls(@nd_subsequent[nid], ctx_ci)
    end
    if @nd_else_clause[nid] >= 0
      collect_cls_calls(@nd_else_clause[nid], ctx_ci)
    end
    if @nd_left[nid] >= 0
      collect_cls_calls(@nd_left[nid], ctx_ci)
    end
    if @nd_right[nid] >= 0
      collect_cls_calls(@nd_right[nid], ctx_ci)
    end
    if @nd_block[nid] >= 0
      collect_cls_calls(@nd_block[nid], ctx_ci)
    end
    cs_elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < cs_elems.length
      collect_cls_calls(cs_elems[k], ctx_ci)
      k = k + 1
    end
    cs_conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < cs_conds.length
      collect_cls_calls(cs_conds[k], ctx_ci)
      k = k + 1
    end
    cs_parts = parse_id_list(@nd_parts[nid])
    k = 0
    while k < cs_parts.length
      collect_cls_calls(cs_parts[k], ctx_ci)
      k = k + 1
    end
  end






 # Builds the trailing portion of a call-args list — each non-empty
 # piece prefixed with ", ", empties skipped. Returns "" when both
 # are empty. Mirrors build_params_str: callers concatenate the
 # result onto a self/recv prefix to form the full arg list.

 # ---- Emit top-level methods ----

 # `END { ... }` -- one zero-arg static C function per registered
 # PostExecutionNode body. main() registers them via atexit() at
 # startup. atexit invokes handlers LIFO, matching CRuby's
 # reverse-of-source-order END execution.




 # Returns 1 if `nid` is an explicit literal value (not a placeholder or
 # inferred fallback). Used by scan_locals to distinguish a genuine int
 # write like `x = 1` from a defaulted "int" from an unresolved read.
  def is_literal_value_expr(nid)
    if nid < 0
      return 0
    end
    t = @nd_type[nid]
    if t == "IntegerNode"
      return 1
    end
    if t == "FloatNode"
      return 1
    end
    if t == "StringNode"
      return 1
    end
    if t == "SymbolNode"
      return 1
    end
    if t == "TrueNode" || t == "FalseNode"
      return 1
    end
    0
  end

  def scan_locals(nid, names, types, params)
    if nid < 0
      return
    end
 # Nested DefNode / ClassNode / ModuleNode bodies are separate
 # scopes — their locals belong to the enclosed method / class /
 # module and must not leak into the parent scope. Without these
 # guards, scan_locals called from infer_main_call_types (or any
 # outer-context caller) walks into every nested method body and
 # records each `local = ...` write as if it were a sibling of
 # main's own locals. Two functions with the same local name
 # but different inferred types then widen to "poly" via the
 # repeated-write merge below, propagating poly into call-site
 # arg types and (via the bare-recv / module-dispatch widening
 # arms in scan_new_calls) into the callee's param types
 # — the topology behind #450 cascade 1.
    if @nd_type[nid] == "DefNode"
      return
    end
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode"
      return
    end
 # Parallel to `names`: "1" if this local's current stored type was set
 # by an explicit literal write, "" otherwise. Reset when called with
 # a fresh (empty) names array.
    if names.length == 0
      @scan_literal_flags = "".split(",")
 # Parallel to `names`: "1" if every write to this local so far was
 # an empty `[]` literal — used to defer the array element type
 # until first `push` . A subsequent write with a
 # concrete element resets the flag to "".
      @scan_empty_flags = "".split(",")
 # Parallel to `names`: "1" if the FIRST write to this local was an
 # empty `{}` literal — promote_empty_hash_local_writes uses this to
 # decide whether to refine str_int_hash (the empty-hash default)
 # to a more specific variant on first []= write.
      @scan_empty_hash_flags = "".split(",")
    end
    if @nd_type[nid] == "MultiWriteNode"
      targets = parse_id_list(@nd_targets[nid])
      val_id2 = @nd_expression[nid]
      ti2 = 0
      targets.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push(multi_write_target_type(val_id2, ti2))
              @scan_literal_flags.push("")
              @scan_empty_flags.push("")
              @scan_empty_hash_flags.push("")
            end
          end
        end
        if @nd_type[tid] == "MultiTargetNode"
 # Nested LHS: `a, (b, c) = 1, [2, 3]`. The inner targets'
 # local-variable names need to be declared in this scope
 # too, with the per-element type derived from the matching
 # outer-RHS slot's array type.
          inner_targets = parse_id_list(@nd_targets[tid])
          inner_slot_type = multi_write_target_type(val_id2, ti2)
          inner_elem_type = nested_target_elem_type(inner_slot_type)
          inner_targets.each { |inner_tid|
            if @nd_type[inner_tid] == "LocalVariableTargetNode"
              ilname = @nd_name[inner_tid]
              if not_in(ilname, names) == 1
                if not_in(ilname, params) == 1
                  names.push(ilname)
                  types.push(inner_elem_type)
                  @scan_literal_flags.push("")
                  @scan_empty_flags.push("")
                  @scan_empty_hash_flags.push("")
                end
              end
            end
          }
        end
        ti2 = ti2 + 1
      }
      rest_id2 = @nd_rest[nid]
      if is_splat_with_target(rest_id2) == 1
        st = @nd_expression[rest_id2]
        if @nd_type[st] == "LocalVariableTargetNode"
          lname = @nd_name[st]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push(splat_rest_type(val_id2))
              @scan_literal_flags.push("")
              @scan_empty_flags.push("")
              @scan_empty_hash_flags.push("")
            end
          end
        end
      end
      rights3 = parse_id_list(@nd_rights[nid])
      r_total2 = 0
      if val_id2 >= 0 && @nd_type[val_id2] == "ArrayNode"
        r_total2 = parse_id_list(@nd_elements[val_id2]).length
      end
      r_idx2 = 0
      rights3.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              t_idx2 = 0
              if r_total2 > 0
                t_idx2 = r_total2 - rights3.length + r_idx2
                if t_idx2 < 0
                  t_idx2 = 0
                end
              end
              types.push(multi_write_target_type(val_id2, t_idx2))
              @scan_literal_flags.push("")
              @scan_empty_flags.push("")
              @scan_empty_hash_flags.push("")
            end
          end
        end
        r_idx2 = r_idx2 + 1
      }
      if @nd_expression[nid] >= 0
        scan_locals(@nd_expression[nid], names, types, params)
      end
      return
    end
    if @nd_type[nid] == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      if not_in(lname, names) == 1
        if not_in(lname, params) == 1
          names.push(lname)
          types.push(infer_type(@nd_expression[nid]))
          if is_literal_value_expr(@nd_expression[nid]) == 1
            @scan_literal_flags.push("1")
          else
            @scan_literal_flags.push("")
          end
 # Track empty-array literal so a later push() can promote
 # the local's element type .
          if is_empty_array_literal(@nd_expression[nid]) == 1
            @scan_empty_flags.push("1")
          else
            @scan_empty_flags.push("")
          end
 # Track empty-hash literal so a later []= write can promote
 # the local's hash variant from the str_int_hash default to
 # whatever key/value types the first []= pins.
          if is_empty_hash_literal(@nd_expression[nid]) == 1
            @scan_empty_hash_flags.push("1")
          else
            @scan_empty_hash_flags.push("")
          end
        end
      else
        if not_in(lname, params) == 1
 # Check if type changed
          at = infer_type(@nd_expression[nid])
 # Concrete (non-empty) array overwrite clears the deferred
 # element-type flag — a `[1,2,3]` write commits to int_array.
          if is_empty_array_literal(@nd_expression[nid]) == 0
            ei = 0
            while ei < names.length
              if names[ei] == lname && ei < @scan_empty_flags.length
                @scan_empty_flags[ei] = ""
              end
              ei = ei + 1
            end
          end
 # Same shape for empty-hash flag: a non-empty-hash overwrite
 # commits the local to whatever concrete hash type was
 # assigned, so a later []= shouldn't trigger promotion.
          if is_empty_hash_literal(@nd_expression[nid]) == 0
            ehi = 0
            while ehi < names.length
              if names[ehi] == lname && ehi < @scan_empty_hash_flags.length
                @scan_empty_hash_flags[ehi] = ""
              end
              ehi = ehi + 1
            end
          end
          ki = 0
          while ki < names.length
            if names[ki] == lname
              if types[ki] != at
                if types[ki] != "poly"
                  if is_array_type(types[ki]) == 1 && is_array_type(at) == 1
                    types[ki] = unify_call_types(types[ki], at, @nd_expression[nid])
                    if types[ki] == "poly_array"
                      @needs_rb_value = 1
                      @needs_gc = 1
                    end
                    ki = ki + 1
                    next
                  end
 # Genuine polymorphism: both the first write and this
 # write were explicit literals, and their types differ.
 # This catches `x = 1; x = "hello"` which the legacy
 # "int is fallback" rule below would silently coerce.
                  if ki < @scan_literal_flags.length && @scan_literal_flags[ki] == "1" && is_literal_value_expr(@nd_expression[nid]) == 1 && at != "nil" && types[ki] != "nil"
                    types[ki] = "poly"
                    @needs_rb_value = 1
                    @scan_literal_flags[ki] = ""
                    ki = ki + 1
                    next
                  end
 # Don't mark poly if new type is fallback "int" and existing is richer
                  if at != "int"
                    if types[ki] == "int"
                      types[ki] = at
                    elsif at == "nil" && is_nullable_pointer_type(types[ki]) == 1
                      if types[ki][types[ki].length - 1] != "?"
                        types[ki] = types[ki] + "?"
                      end
                    elsif types[ki] == "nil" && is_nullable_pointer_type(at) == 1
                      if is_nullable_type(at) == 1
                        types[ki] = at
                      else
                        types[ki] = at + "?"
                      end
                    elsif base_type(types[ki]) == at
 # T? and T are compatible — keep T?
                    elsif base_type(at) == types[ki]
 # T and T? → upgrade to T?
                      types[ki] = at
                    elsif base_type(types[ki]) == base_type(at)
 # T? and T? — same base
                    else
                      types[ki] = "poly"
                      @needs_rb_value = 1
                    end
                  end
                end
              end
            end
            ki = ki + 1
          end
        end
      end
    end
    if @nd_type[nid] == "LocalVariableOperatorWriteNode"
      lname = @nd_name[nid]
      rhs_type = infer_type(@nd_expression[nid])
      vtype = "int"
      if rhs_type == "float"
        vtype = "float"
      end
      if not_in(lname, names) == 1
        if not_in(lname, params) == 1
          names.push(lname)
          types.push(vtype)
        end
      else
        if not_in(lname, params) == 1
 # If RHS is float, promote to float
          if rhs_type == "float"
            ki = 0
            while ki < names.length
              if names[ki] == lname
                if types[ki] == "int"
                  types[ki] = "float"
                end
              end
              ki = ki + 1
            end
          end
        end
      end
    end
 # Detect array element type from push/<<: arr.push(x) or arr << x
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "push" || @nd_name[nid] == "<<"
        recv = @nd_receiver[nid]
        if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
          arr_name = @nd_name[recv]
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length > 0
              arg_type = infer_type(aargs[0])
 # If arg is arr[i] where arr is in names, get element type
              if arg_type == "int" && @nd_type[aargs[0]] == "CallNode"
                if @nd_name[aargs[0]] == "[]"
                  arr_recv = @nd_receiver[aargs[0]]
                  if arr_recv >= 0 && @nd_type[arr_recv] == "LocalVariableReadNode"
                    arn = @nd_name[arr_recv]
                    ni = 0
                    while ni < names.length
                      if names[ni] == arn
                        if types[ni] == "str_array"
                          arg_type = "string"
                        end
                        if types[ni] == "float_array"
                          arg_type = "float"
                        end
                      end
                      ni = ni + 1
                    end
                  end
                end
              end
              if is_obj_type(arg_type) == 1
                target_type = arg_type + "_ptr_array"
                @needs_gc = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = target_type
                    end
                  end
                  ki = ki + 1
                end
              elsif arg_type == "string"
                @needs_str_array = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = "str_array"
                    end
                  end
                  ki = ki + 1
                end
              elsif arg_type == "float"
                @needs_float_array = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = "float_array"
                    end
                  end
                  ki = ki + 1
                end
              elsif arg_type == "symbol"
 # sym_array uses sp_IntArray storage, so int_array
 # helpers stay required even after promotion.
                @needs_int_array = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = "sym_array"
                    end
                  end
                  ki = ki + 1
                end
              elsif arg_type == "poly"
                @needs_rb_value = 1
                @needs_gc = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = "poly_array"
                    end
                  end
                  ki = ki + 1
                end
              elsif is_hash_type(arg_type) == 1 || is_array_type(arg_type) == 1 || is_ptr_array_type(arg_type) == 1
 # Hash or typed-array element: spinel has no
 # <hash>_ptr_array / <array>_ptr_array_ptr_array slot,
 # so box each push via poly_array. Mirrors the literal-
 # array inference shape at infer_array_elem_type_from_ids
 # (line 2400+ / 2414+).
                @needs_rb_value = 1
                @needs_gc = 1
                ki = 0
                while ki < names.length
                  if names[ki] == arr_name
                    if types[ki] == "int_array"
                      types[ki] = "poly_array"
                    end
                  end
                  ki = ki + 1
                end
              end
            end
          end
        end
      end
    end
 # Empty-array param promotion at instance-method call sites
 # (`obj.method(arg)`). Same forward/backward propagation as
 # the top-level branch below, but reads/writes the per-class
 # @cls_meth_ptypes / @cls_meth_ptypes_empty storage.
    if @nd_type[nid] == "CallNode"
      icm_recv = @nd_receiver[nid]
      if icm_recv >= 0
        icm_rt = infer_type(icm_recv)
 # When the receiver is a local declared in this same
 # scan_locals pass (`r = Recorder.new` followed by `r.method(...)`),
 # infer_type still returns "int" because we haven't called
 # declare_var yet. Fall back to the names/types accumulator.
        if icm_rt == "int" && @nd_type[icm_recv] == "LocalVariableReadNode"
          icm_recv_name = @nd_name[icm_recv]
          icm_ni0 = 0
          while icm_ni0 < names.length
            if names[icm_ni0] == icm_recv_name
              icm_rt = types[icm_ni0]
            end
            icm_ni0 = icm_ni0 + 1
          end
        end
        if is_obj_type(icm_rt) == 1
          icm_cname = icm_rt[4, icm_rt.length - 4]
          icm_ci = find_class_idx(icm_cname)
          if icm_ci >= 0
            icm_mname = @nd_name[nid]
            icm_midx = cls_find_method_direct(icm_ci, icm_mname)
 # Walk parents if not found on the receiver class itself
            icm_owner_ci = icm_ci
            if icm_midx < 0
              icm_owner_name = find_method_owner(icm_ci, icm_mname)
              if icm_owner_name != ""
                icm_owner_ci = find_class_idx(icm_owner_name)
                if icm_owner_ci >= 0
                  icm_midx = cls_find_method_direct(icm_owner_ci, icm_mname)
                end
              end
            end
            if icm_midx >= 0
              icm_args_id = @nd_arguments[nid]
              if icm_args_id >= 0
                icm_aargs = get_args(icm_args_id)
                icm_all_ptypes = @cls_meth_ptypes[icm_owner_ci].split("|")
                icm_all_empty = @cls_meth_ptypes_empty[icm_owner_ci].split("|")
                icm_ptypes = "".split(",")
                icm_empties = "".split(",")
                if icm_midx < icm_all_ptypes.length
                  icm_ptypes = icm_all_ptypes[icm_midx].split(",")
                end
                if icm_midx < icm_all_empty.length
                  icm_empties = icm_all_empty[icm_midx].split(",")
                end
                icm_changed = 0
                icm_k = 0
                while icm_k < icm_aargs.length
                  icm_arg_id = icm_aargs[icm_k]
                  icm_arg_is_empty = is_empty_array_literal(icm_arg_id)
                  icm_local_idx = -1
                  if @nd_type[icm_arg_id] == "LocalVariableReadNode"
                    icm_arg_lname = @nd_name[icm_arg_id]
                    icm_ni = 0
                    while icm_ni < names.length
                      if names[icm_ni] == icm_arg_lname
                        icm_local_idx = icm_ni
                      end
                      icm_ni = icm_ni + 1
                    end
                    if icm_local_idx >= 0 && icm_local_idx < @scan_empty_flags.length
                      if @scan_empty_flags[icm_local_idx] == "1"
                        icm_arg_is_empty = 1
                      end
                    end
                  end
                  if icm_arg_is_empty == 1
                    while icm_empties.length <= icm_k
                      icm_empties.push("")
                    end
                    if icm_empties[icm_k] != "1"
                      icm_empties[icm_k] = "1"
                      icm_changed = 1
                    end
                  end
                  if icm_local_idx >= 0 && icm_k < icm_ptypes.length
                    icm_pt = icm_ptypes[icm_k]
                    if types[icm_local_idx] == "int_array" && icm_local_idx < @scan_empty_flags.length && @scan_empty_flags[icm_local_idx] == "1"
                      if icm_pt == "str_array"
                        types[icm_local_idx] = "str_array"
                        @needs_str_array = 1
                      end
                      if icm_pt == "float_array"
                        types[icm_local_idx] = "float_array"
                        @needs_float_array = 1
                      end
                      if icm_pt == "sym_array"
                        types[icm_local_idx] = "sym_array"
                      end
                    end
                  end
                  icm_k = icm_k + 1
                end
                if icm_changed == 1
                  icm_all_empty[icm_midx] = icm_empties.join(",")
                  @cls_meth_ptypes_empty[icm_owner_ci] = icm_all_empty.join("|")
                end
              end
            end
          end
        end
      end
    end
 # Empty-array param promotion at top-level function call
 # sites. Two directions in one place:
 # (a) Forward: if `arg` is `[]` literal or a local with the
 # empty flag set, mark @meth_param_empty[mi][k] = "1" so
 # a later body-promotion pass can refine the param type.
 # (b) Backward: if @meth_param_types[mi][k] has already been
 # promoted to a concrete typed-array (str_array, etc.)
 # and `arg` is a local with the empty flag, upgrade the
 # local's type to match — this propagates the deferred
 # resolution back to the caller's variable.
    if @nd_type[nid] == "CallNode"
      if @nd_receiver[nid] < 0
        ea_mname = @nd_name[nid]
        ea_mi = find_method_idx(ea_mname)
        if ea_mi >= 0
          ea_args_id = @nd_arguments[nid]
          if ea_args_id >= 0
            ea_aargs = get_args(ea_args_id)
            ea_ptypes = @meth_param_types[ea_mi].split(",")
            ea_empty_str = ""
            if ea_mi < @meth_param_empty.length
              ea_empty_str = @meth_param_empty[ea_mi]
            end
            ea_empties = ea_empty_str.split(",")
            ea_changed = 0
            ea_k = 0
            while ea_k < ea_aargs.length
              ea_arg_id = ea_aargs[ea_k]
              ea_arg_is_empty = is_empty_array_literal(ea_arg_id)
              ea_local_idx = -1
              if @nd_type[ea_arg_id] == "LocalVariableReadNode"
                ea_arg_lname = @nd_name[ea_arg_id]
                ea_ni = 0
                while ea_ni < names.length
                  if names[ea_ni] == ea_arg_lname
                    ea_local_idx = ea_ni
                  end
                  ea_ni = ea_ni + 1
                end
                if ea_local_idx >= 0 && ea_local_idx < @scan_empty_flags.length
                  if @scan_empty_flags[ea_local_idx] == "1"
                    ea_arg_is_empty = 1
                  end
                end
              end
              if ea_arg_is_empty == 1
                while ea_empties.length <= ea_k
                  ea_empties.push("")
                end
                if ea_empties[ea_k] != "1"
                  ea_empties[ea_k] = "1"
                  ea_changed = 1
                end
              end
 # Backward: param already promoted, lift the local too.
              if ea_local_idx >= 0 && ea_k < ea_ptypes.length
                ea_pt = ea_ptypes[ea_k]
                if types[ea_local_idx] == "int_array" && ea_local_idx < @scan_empty_flags.length && @scan_empty_flags[ea_local_idx] == "1"
                  if ea_pt == "str_array"
                    types[ea_local_idx] = "str_array"
                    @needs_str_array = 1
                  end
                  if ea_pt == "float_array"
                    types[ea_local_idx] = "float_array"
                    @needs_float_array = 1
                  end
                  if ea_pt == "sym_array"
                    types[ea_local_idx] = "sym_array"
                  end
                end
              end
              ea_k = ea_k + 1
            end
            if ea_changed == 1
              @meth_param_empty[ea_mi] = ea_empties.join(",")
            end
          end
        end
      end
    end
 # `local[k] = v` on a local declared as the empty-hash default
 # (str_int_hash from `local = {}`) — promote based on the actual
 # key/value types so the C declaration picks the matching
 # sp_*Hash struct. Mirrors the ivar-side promotion in
 # scan_writer_calls. Only fires when @scan_empty_hash_flags
 # confirms every prior write to this local was an empty-hash
 # literal — concretely-typed hashes (`h = {"a" => 1}`) keep
 # their declared type even when later []= writes mix value
 # types.
 # IndexOrWriteNode (`h[k] ||= v`), IndexAndWriteNode (`h[k] &&= v`),
 # IndexOperatorWriteNode (`h[k] += v` etc.) — each implicitly does
 # an `h[k] = ...` write, so widen the local hash type the same way
 # as the regular `[]=` CallNode below. Without this, optcarrots
 # `entries = {}; entries[key] ||= [...]` leaves `entries` at the
 # str_int_hash default — `entries[key]` reads return mrb_int and
 # the surrounding `.map { entries[key] }` collapses to IntArray,
 # losing the cls_id chain on the value pointers.
    if @nd_type[nid] == "IndexOrWriteNode" || @nd_type[nid] == "IndexAndWriteNode" || @nd_type[nid] == "IndexOperatorWriteNode"
      iow_recv = @nd_receiver[nid]
      iow_args_id = @nd_arguments[nid]
      iow_val = @nd_expression[nid]
      if iow_recv >= 0 && iow_args_id >= 0 && iow_val >= 0 && @nd_type[iow_recv] == "LocalVariableReadNode"
        iow_hname = @nd_name[iow_recv]
        iow_aargs = get_args(iow_args_id)
        if iow_aargs.length >= 1
          iow_ki = 0
          while iow_ki < names.length
            if names[iow_ki] == iow_hname
              iow_cur = types[iow_ki]
              iow_promotable = (iow_cur == "str_int_hash" ||
                                iow_cur == "str_str_hash" ||
                                iow_cur == "int_str_hash" ||
                                iow_cur == "sym_int_hash" ||
                                iow_cur == "sym_str_hash")
              if iow_promotable && iow_ki < @scan_empty_hash_flags.length && @scan_empty_hash_flags[iow_ki] == "1"
                iow_kt = scan_locals_arg_type(iow_aargs[0], names, types, params)
                iow_vt = scan_locals_arg_type(iow_val, names, types, params)
                iow_promoted = promote_empty_hash_for(iow_kt, iow_vt)
                if iow_promoted != "" && iow_promoted != iow_cur
                  types[iow_ki] = iow_promoted
                  if iow_promoted == "str_poly_hash" || iow_promoted == "sym_poly_hash"
                    @scan_empty_hash_flags[iow_ki] = ""
                  end
                  if iow_promoted == "str_str_hash"
                    @needs_str_str_hash = 1
                  elsif iow_promoted == "int_str_hash"
                    @needs_int_str_hash = 1
                    @needs_int_array = 1
                  elsif iow_promoted == "sym_int_hash"
                    @needs_sym_int_hash = 1
                  elsif iow_promoted == "sym_str_hash"
                    @needs_sym_str_hash = 1
                  elsif iow_promoted == "str_poly_hash"
                    @needs_str_poly_hash = 1
                    @needs_rb_value = 1
                  elsif iow_promoted == "sym_poly_hash"
                    @needs_sym_poly_hash = 1
                    @needs_rb_value = 1
                  end
                end
              end
            end
            iow_ki = iow_ki + 1
          end
        end
      end
    end
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "[]="
        recv = @nd_receiver[nid]
        if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
          hname = @nd_name[recv]
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length >= 2
              ki = 0
              while ki < names.length
                if names[ki] == hname
                  cur = types[ki]
 # An empty-hash-tracked local is still "promotable"
 # while it sits on str_int_hash (the default empty
 # shape) OR on any non-poly hash variant we previously
 # promoted it to from str_int_hash. a
 # later pass discovers the iterated value type is
 # actually poly (each-rebuild over a poly_hash whose
 # source type wasn't known at first scan), and we
 # need to escalate sym_int_hash → sym_poly_hash etc.
                  promotable = (cur == "str_int_hash" ||
                                cur == "str_str_hash" ||
                                cur == "int_str_hash" ||
                                cur == "sym_int_hash" ||
                                cur == "sym_str_hash")
                  if promotable && ki < @scan_empty_hash_flags.length && @scan_empty_hash_flags[ki] == "1"
 # Block params (e.g. `|k, v|` in `each`) aren't
 # yet `declare_var`'d when scan_locals walks
 # the block body, so a bare `infer_type(v)`
 # falls back to "int" even though scan_locals
 # has already collected v's actual type into
 # `types[]`. Prefer that local types array when
 # the argument is a LocalVariableReadNode we
 # recorded; fall back to infer_type otherwise.
                    key_type = scan_locals_arg_type(aargs[0], names, types, params)
                    val_type = scan_locals_arg_type(aargs[aargs.length - 1], names, types, params)
                    promoted = promote_empty_hash_for(key_type, val_type)
                    if promoted != "" && promoted != cur
                      types[ki] = promoted
 # Clear the flag only when reaching the terminal
 # poly_hash variant — int/str variants still need
 # to be re-promotable on a later poly write.
                      if promoted == "str_poly_hash" || promoted == "sym_poly_hash"
                        @scan_empty_hash_flags[ki] = ""
                      end
                      if promoted == "str_str_hash"
                        @needs_str_str_hash = 1
                      elsif promoted == "int_str_hash"
                        @needs_int_str_hash = 1
                        @needs_int_array = 1
                      elsif promoted == "sym_int_hash"
                        @needs_sym_int_hash = 1
                      elsif promoted == "sym_str_hash"
                        @needs_sym_str_hash = 1
                      elsif promoted == "str_poly_hash"
                        @needs_str_poly_hash = 1
                        @needs_rb_value = 1
                      elsif promoted == "sym_poly_hash"
                        @needs_sym_poly_hash = 1
                        @needs_rb_value = 1
                      elsif promoted == "poly_poly_hash"
                        @needs_poly_poly_hash = 1
                        @needs_rb_value = 1
                      end
                    end
                  end
                end
                ki = ki + 1
              end
            end
          end
        end
      end
    end
    if @nd_type[nid] == "ForNode"
      tgt = @nd_target[nid]
      if tgt >= 0
        if @nd_type[tgt] == "LocalVariableTargetNode"
          lname = @nd_name[tgt]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
 # Infer element type from collection
              elem_type = "int"
              coll = @nd_collection[nid]
              if coll >= 0
                ct = infer_type(coll)
                if ct == "str_array"
                  elem_type = "string"
                elsif ct == "float_array"
                  elem_type = "float"
                elsif is_ptr_array_type(ct) == 1
                  elem_type = ptr_array_elem_type(ct)
                end
              end
              names.push(lname)
              types.push(elem_type)
              @scan_literal_flags.push("")
              @scan_empty_flags.push("")
              @scan_empty_hash_flags.push("")
            end
          end
        end
      end
    end
 # Rescue reference (=> e) needs to be declared as a local
    if @nd_type[nid] == "RescueNode"
      ref = @nd_reference[nid]
      if ref >= 0
        lname = @nd_name[ref]
        if not_in(lname, names) == 1
          if not_in(lname, params) == 1
            names.push(lname)
            types.push("string")
            @scan_literal_flags.push("")
            @scan_empty_flags.push("")
            @scan_empty_hash_flags.push("")
          end
        end
      end
    end
 # Detect << on string local variable: widen to mutable_str
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "<<"
        recv = @nd_receiver[nid]
        if recv >= 0
          if @nd_type[recv] == "LocalVariableReadNode"
            vname = @nd_name[recv]
            wi = 0
            while wi < names.length
              if names[wi] == vname
                if types[wi] == "string"
                  types[wi] = "mutable_str"
                  @needs_mutable_str = 1
                end
              end
              wi = wi + 1
            end
          end
        end
      end
    end
 # Block parameters need to be declared as locals
    if @nd_type[nid] == "CallNode"
      blk = @nd_block[nid]
      if blk >= 0
        bp = @nd_parameters[blk]
        if bp >= 0 && @nd_type[bp] == "NumberedParametersNode"
          nmax = @nd_value[bp]
          nk = 0
          while nk < nmax
            nbname = "_" + (nk + 1).to_s
            if not_in(nbname, names) == 1
              if not_in(nbname, params) == 1
                names.push(nbname)
 # Infer type from receiver element type
                nrt = ""
                if @nd_receiver[nid] >= 0
                  nrt = infer_type(@nd_receiver[nid])
                  if nrt == "int" && @nd_type[@nd_receiver[nid]] == "LocalVariableReadNode"
                    nrname = @nd_name[@nd_receiver[nid]]
                    nri = 0
                    while nri < names.length
                      if names[nri] == nrname
                        nrt = types[nri]
                      end
                      nri = nri + 1
                    end
                  end
                end
                if nrt == "str_array"
                  types.push("string")
                elsif nrt == "float_array"
                  types.push("float")
                elsif nrt == "sym_array"
                  types.push("symbol")
                elsif is_ptr_array_type(nrt) == 1
 # When the iterated element is itself an array and the
 # block uses _1, _2, ... (max >= 2), Ruby destructures
 # the yielded sub-array into the numbered slots. Each
 # _i then takes the *inner* element type, not the
 # outer ptr_array element type.
                  ptr_elem = ptr_array_elem_type(nrt)
                  if nmax >= 2 && is_array_type(ptr_elem) == 1
                    types.push(elem_type_of_array(ptr_elem))
                  else
                    types.push(ptr_elem)
                  end
                else
                  types.push("int")
                end
                @scan_literal_flags.push("")
                @scan_empty_flags.push("")
                @scan_empty_hash_flags.push("")
              end
            end
            nk = nk + 1
          end
        end
        if bp >= 0 && @nd_type[bp] != "NumberedParametersNode"
          inner = @nd_parameters[bp]
          if inner >= 0
            reqs = parse_id_list(@nd_requireds[inner])
            bk = 0
            while bk < reqs.length
              bname = @nd_name[reqs[bk]]
              if not_in(bname, names) == 1
                if not_in(bname, params) == 1
                  names.push(bname)
                  @scan_literal_flags.push("")
                  @scan_empty_flags.push("")
                  @scan_empty_hash_flags.push("")
 # Infer type from receiver context
                  recv_type = ""
                  if @nd_receiver[nid] >= 0
                    recv_type = infer_type(@nd_receiver[nid])
 # If type is int and receiver is local var, check names array
                    if recv_type == "int"
                      if @nd_type[@nd_receiver[nid]] == "LocalVariableReadNode"
                        rname = @nd_name[@nd_receiver[nid]]
                        ri = 0
                        while ri < names.length
                          if names[ri] == rname
                            recv_type = types[ri]
                          end
                          ri = ri + 1
                        end
                      end
                    end
 # For chained calls like int_str_hash.keys.each, infer_type
 # returns "str_array" because map's type isn't in @scope_names
 # during the scan. Resolve by checking the names array.
                    if recv_type == "str_array"
                      rnode = @nd_receiver[nid]
                      if @nd_type[rnode] == "CallNode" && @nd_name[rnode] == "keys"
                        krnode = @nd_receiver[rnode]
                        if krnode >= 0 && @nd_type[krnode] == "LocalVariableReadNode"
                          krname = @nd_name[krnode]
                          kri = 0
                          while kri < names.length
                            if names[kri] == krname && types[kri] == "int_str_hash"
                              recv_type = "int_array"
                            end
                            kri = kri + 1
                          end
                        end
                      end
                    end
                  end
                  mname = @nd_name[nid]
                  if mname == "scan"
                    types.push("string")
                    bk = bk + 1
                    next
                  end
 # string-yield sub-variant: when calling
 # a yield-bearing method on a user class
 # (`c.each { |k| ... }`), infer the block param's
 # type from the method's yield-arg type. Without
 # this, `k` defaults to mrb_int at the parent
 # scope, but the yield expansion assigns the
 # method's `n = @keys[i]` (a `const char *`) to
 # it -- mismatch.
                  if is_obj_type(recv_type) == 1
                    cn_each = recv_type[4, recv_type.length - 4]
                    cci_each = find_class_idx(cn_each)
                    if cci_each >= 0
                      midx_each = cls_find_method(cci_each, mname)
                      if midx_each >= 0 && cls_method_has_yield(cci_each, midx_each) == 1
                        owner_each = find_method_owner(cci_each, mname)
                        owner_ci_each = find_class_idx(owner_each)
                        if owner_ci_each >= 0
                          owner_midx_each = cls_find_method(owner_ci_each, mname)
                          arity_each = cls_method_yield_arity(owner_ci_each, owner_midx_each)
                          ytypes_each = "".split(",")
                          ka = 0
                          while ka < arity_each
                            ytypes_each.push("")
                            ka = ka + 1
                          end
                          ybid_each = cls_method_body_id(owner_ci_each, owner_midx_each)
 # Push a temporary scope with the yielding
 # method's locals declared so body_yield_arg_types'
 # infer_type calls resolve LocalVariableReadNodes
 # to their actual types -- without this, a yield
 # arg like `n` (a `const char *` local in the
 # method) would just read as `int` (the no-scope
 # default) and we'd miss the body-driven type.
                          if ybid_each >= 0
                            push_scope
                            yl_names = "".split(",")
                            yl_types = "".split(",")
                            saved_ci_each = @current_class_idx
                            @current_class_idx = owner_ci_each
                            scan_locals(ybid_each, yl_names, yl_types, "".split(","))
                            yj = 0
                            while yj < yl_names.length
                              declare_var(yl_names[yj], yl_types[yj])
                              yj = yj + 1
                            end
                            body_yield_arg_types(ybid_each, ytypes_each)
                            @current_class_idx = saved_ci_each
                            pop_scope
                          end
                          if bk < ytypes_each.length && ytypes_each[bk] != "" && ytypes_each[bk] != "int"
                            types.push(ytypes_each[bk])
                            bk = bk + 1
                            next
                          end
                        end
                      end
                    end
                  end
                  if mname == "times" || mname == "upto" || mname == "downto"
                    types.push("int")
                  elsif mname == "each" || mname == "each_pair" || mname == "map" || mname == "select" || mname == "filter" || mname == "reject" || mname == "find" || mname == "detect" || mname == "any?" || mname == "all?" || mname == "none?" || mname == "one?" || mname == "count" || mname == "min" || mname == "max" || mname == "sum" || mname == "min_by" || mname == "max_by" || mname == "sort_by" || mname == "flat_map" || mname == "filter_map" || mname == "cycle" || mname == "partition"
 # Element iteration: infer block param from collection type
                    if recv_type == "str_array"
                      types.push("string")
                    elsif recv_type == "float_array"
                      types.push("float")
                    elsif recv_type == "sym_array"
                      types.push("symbol")
                    elsif recv_type == "str_int_hash"
                      if bk == 0
                        types.push("string")
                      else
                        types.push("int")
                      end
                    elsif recv_type == "int_str_hash"
                      if bk == 0
                        types.push("int")
                      else
                        types.push("string")
                      end
                    elsif recv_type == "str_str_hash"
                      types.push("string")
                    elsif recv_type == "sym_int_hash"
                      if bk == 0
                        types.push("symbol")
                      else
                        types.push("int")
                      end
                    elsif recv_type == "sym_str_hash"
                      if bk == 0
                        types.push("symbol")
                      else
                        types.push("string")
                      end
                    elsif recv_type == "sym_poly_hash"
                      if bk == 0
                        types.push("symbol")
                      else
                        types.push("poly")
                        @needs_rb_value = 1
                      end
                    elsif recv_type == "str_poly_hash"
                      if bk == 0
                        types.push("string")
                      else
                        types.push("poly")
                        @needs_rb_value = 1
                      end
                    elsif recv_type == "poly_array"
                      types.push("poly")
                      @needs_rb_value = 1
                    elsif is_ptr_array_type(recv_type) == 1
                      types.push(ptr_array_elem_type(recv_type))
                    else
                      types.push("int")
                    end
                  elsif mname == "zip"
 # Both params get element type from receiver
                    if recv_type == "str_array"
                      types.push("string")
                    elsif recv_type == "float_array"
                      types.push("float")
                    else
                      types.push("int")
                    end
                  elsif mname == "each_with_index"
                    if bk == 0
 # Element
                      if recv_type == "str_array"
                        types.push("string")
                      elsif recv_type == "sym_array"
                        types.push("symbol")
                      elsif recv_type == "float_array"
                        types.push("float")
                      elsif is_ptr_array_type(recv_type) == 1
                        types.push(ptr_array_elem_type(recv_type))
                      else
                        types.push("int")
                      end
                    else
 # Index
                      types.push("int")
                    end
                  elsif mname == "each_char" || mname == "each_line"
                    types.push("string")
                  elsif mname == "each_byte"
                    types.push("int")
                  elsif mname == "tap" || mname == "then" || mname == "yield_self"
 # Block param gets receiver type
                    types.push(recv_type)
                  elsif mname == "each_with_object"
                    if bk == 0
 # Element
                      if recv_type == "str_array"
                        types.push("string")
                      elsif recv_type == "float_array"
                        types.push("float")
                      else
                        types.push("int")
                      end
                    else
 # Object accumulator — infer from first argument
                      args_id = @nd_arguments[nid]
                      if args_id >= 0
                        aargs = get_args(args_id)
                        if aargs.length > 0
                          types.push(infer_type(aargs[0]))
                          bk = bk + 1
                          next
                        end
                      end
                      types.push("int")
                    end
                  elsif mname == "each_slice" || mname == "each_cons"
 # Block param is a sub-array of the same type
                    if recv_type == "str_array" || recv_type == "float_array" || recv_type == "int_array"
                      types.push(recv_type)
                    else
                      types.push("int_array")
                    end
                  elsif mname == "reduce" || mname == "inject"
                    if bk == 0
 # Accumulator: infer from initial value argument
                      args_id = @nd_arguments[nid]
                      if args_id >= 0
                        aargs = get_args(args_id)
                        if aargs.length > 0
                          types.push(infer_type(aargs[0]))
                          bk = bk + 1
                          next
                        end
                      end
                      types.push("int")
                    else
 # Element
                      if recv_type == "str_array"
                        types.push("string")
                      elsif recv_type == "float_array"
                        types.push("float")
                      else
                        types.push("int")
                      end
                    end
                  else
                    types.push("int")
                  end
                end
              end
              bk = bk + 1
            end
          end
        end
      end
    end
 # Recurse
    scan_locals_children(nid, names, types, params)
  end

  def scan_locals_children(nid, names, types, params)
    if @nd_body[nid] >= 0
      scan_locals(@nd_body[nid], names, types, params)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_locals(stmts[k], names, types, params)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_locals(@nd_expression[nid], names, types, params)
    end
    if @nd_predicate[nid] >= 0
      scan_locals(@nd_predicate[nid], names, types, params)
    end
    if @nd_subsequent[nid] >= 0
      scan_locals(@nd_subsequent[nid], names, types, params)
    end
    if @nd_else_clause[nid] >= 0
      scan_locals(@nd_else_clause[nid], names, types, params)
    end
    if @nd_arguments[nid] >= 0
      scan_locals(@nd_arguments[nid], names, types, params)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_locals(args[k], names, types, params)
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_locals(conds[k], names, types, params)
      k = k + 1
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_locals(elems[k], names, types, params)
      k = k + 1
    end
    if @nd_left[nid] >= 0
      scan_locals(@nd_left[nid], names, types, params)
    end
    if @nd_right[nid] >= 0
      scan_locals(@nd_right[nid], names, types, params)
    end
    if @nd_block[nid] >= 0
      scan_locals(@nd_block[nid], names, types, params)
    end
    if @nd_receiver[nid] >= 0
      scan_locals(@nd_receiver[nid], names, types, params)
    end
    if @nd_collection[nid] >= 0
      scan_locals(@nd_collection[nid], names, types, params)
    end
    if @nd_rescue_clause[nid] >= 0
      scan_locals(@nd_rescue_clause[nid], names, types, params)
    end
    if @nd_ensure_clause[nid] >= 0
      scan_locals(@nd_ensure_clause[nid], names, types, params)
    end
  end

  def not_in(name, arr)
    k = 0
    while k < arr.length
      if arr[k] == name
        return 0
      end
      k = k + 1
    end
    1
  end

 # Scan locals introduced by constant-initializer RHS expressions —
 # those run inside main() before the user stmts, so any block
 # params they introduce (`FRAME = [...].map { |n| ... }` and the
 # multi-write form `A, B = [...].map { |n| ... }`) need their
 # `lv_<bp>` decls in main's scope. Covers both `@const_expr_ids`
 # (single-const inits) and `@multi_const_inits` (the multi-write
 # form, where the RHS lives on a MultiWriteNode).


 # ---- Main emission ----
 # Emit the cls_id-aware obj hash/eql dispatch shims that
 # sp_PolyPolyHash uses for OBJ-tag keys. The default runtime
 # behavior is pointer identity; this dispatch overrides it for
 # classes whose `eql?` semantics are content-based:
 #
 # - sp_Method (when the class is in scope): equal iff bound
 # receiver + fn_ptr match — covers `obj.method(:foo)` dedup.
 # - sp_IntArray (SP_BUILTIN_INT_ARRAY = -1): element-wise content
 # compare — covers `entries[[a, b]] ||= ...` array-keyed Hash
 # patterns.


 # Compile a node for use as a C scalar condition. Value-type objects
 # are passed by value (a struct), and C rejects them as scalars in
 # `if (...)` etc. In Ruby every non-nil/non-false object is truthy,
 # so wrap the expression in a comma operator that evaluates it for
 # side effects then yields 1.


  def or_result_type(nid)
    lt = infer_type(@nd_left[nid])
    rt = infer_type(@nd_right[nid])
    if lt == rt
      return lt
    end
    if lt == "nil"
      return rt
    end
    if rt == "nil"
      if is_nullable_pointer_type(lt) == 1 && is_nullable_type(lt) == 0
        return lt + "?"
      end
      return lt
    end
    "poly"
  end

 # ---- Expression compiler ----

  def c_string_literal(s)
 # Input `s` is the runtime Ruby string content (already-cooked: any
 # backslash in `s` is a literal backslash, NOT an escape introducer).
 # We C-escape the small set that needs it: backslash, double-quote,
 # newline, carriage return, tab. Everything else copies through.
 # The previous version treated `s` as if it still carried Ruby
 # escapes, so a 2-char input "\\n" (backslash + n) wrongly collapsed
 # to a C newline; that bug is what made `"hello\\nworld\\n".lines`
 # emit invalid C with a literal newline inside a string literal.
    result = "\""
    i = 0
    while i < s.length
      ch = s[i]
      if ch == bsl
        result = result + bsl + bsl
      else
        if ch == "\""
          result = result + bsl + "\""
        else
          if ch == 10.chr
            result = result + bsl_n
          else
            if ch == 13.chr
              result = result + bsl + "r"
            else
              if ch == 9.chr
                result = result + bsl + "t"
              else
                result = result + ch
              end
            end
          end
        end
      end
      i = i + 1
    end
 # Prepend 0xff marker byte so GC can identify static literals.
 # Return form: (&("\xff" "content")[1]) — same pointer value as the
 # legacy ("\xff" "content" + 1) idiom, but uses array indexing so
 # clang doesn't flag it under -Wstring-plus-int.
    "(&(\"\\xff\" " + result + "\")[1])"
  end



 # True if `t` is a GC-allocated pointer that could be swept by
 # sp_gc_collect if held only as a C-stack temp when the collector
 # runs mid-expression.

 # Compile `nid`, and if it's a call expression whose result is a
 # GC-allocated pointer, bind that result to a rooted temp variable
 # so a subsequent mid-expression sp_gc_collect cannot sweep it.
 # For non-call expressions (locals, ivars, literals) rooting is
 # either already in place or unnecessary.


 # Like compile_arg0, but unboxes the result to mrb_int when the
 # argument's static type is poly. Use at call sites that pass the
 # arg directly to a C function expecting `mrb_int` (sp_IntArray_get,
 # sp_IntArray_push integer-element variants, runtime helpers that
 # take an int index, etc.). Without unboxing, gcc rejects passing
 # `sp_RbVal` to a `mrb_int` parameter.

 # Like compile_arg0, but converts symbol-typed arg to const char *
 # (sp_sym_to_s wrap). Use for callsites that need a string key.


 # --- Fiber capture helpers ---






 # Detect whether a Fiber.new block body references `self` — directly
 # (`SelfNode`), via `@ivar` reads/writes, or via a no-receiver method
 # call inside a class body. Used by `compile_fiber_new` to decide
 # whether to thread an explicit `self` capture through the fiber's
 # `_cap` struct so the body's emitted C can resolve `self->iv_X` and
 # `sp_<Class>_<method>(self)` call sites without the surrounding
 # method's `self` parameter.



 # Returns the C expression for a CallNode. Symmetric with
 # `infer_call_type` (which returns the call's C type) — see the
 # docstring there for the maintenance rule on adding new shapes.
 # Branch order in this function mirrors infer_call_type's order so
 # the two stay diff-able.






 # Collect flattened parts of a string concat chain: a + b + c → [a, b, c]
 # Returns compiled expression strings. Only flattens up to 4 parts.



 # Resolve an implicit `new` (recv-less) inside a `def self.<m>`
 # body to <CurrentClass>.new, so a factory like `def
 # self.from_raw(p); instance = new; instance.x = ...; end`
 # works. Mirrors the way implicit `self` inside instance methods
 # routes recv-less calls to the enclosing class.

  def current_class_method_owning_class
    if @current_method_name == ""
      return ""
    end
 # Two formats land here: scan_writer_calls pins
 # "<Class>_cls_<m>" so we can split on the marker; emit-side
 # sets just "<m>" and relies on @current_class_idx for context.
    cls_idx = @current_method_name.index("_cls_")
    if cls_idx != nil && cls_idx >= 0
      return @current_method_name[0, cls_idx]
    end
    if @current_class_idx >= 0 && @current_class_idx < @cls_names.length
      cm = @cls_cmeth_names[@current_class_idx].split(";")
      k = 0
      while k < cm.length
        if cm[k] == @current_method_name
          return @cls_names[@current_class_idx]
        end
        k = k + 1
      end
    end
    ""
  end




 # Resolve the literal RangeNode behind a method receiver, peeking
 # through a single ParenthesesNode wrap. Returns -1 when the receiver
 # isn't a literal range — in which case the runtime sp_Range struct is
 # used and exclude_end isn't tracked.


 # Symbol methods. rc is a sp_sym expression.





 # Multi-key Hash#dig walks across poly slots: each step dispatches
 # on acc.cls_id to pick a concrete-hash variant (cls_ids stamped in
 # box_value_to_poly via cls_id_for_hash_type). A cls_id that matches
 # no known variant collapses the walk to nil, covering both "key
 # missing mid-walk" and "value at this depth isn't a hash".

 # Caller must pass `key_expr` typed for recv_type's key family —
 # compile_hash_dig validates this with hash_key_matches_recv first.

 # Non-poly inner hashes need a has_key guard: their typed `_get`
 # returns 0/"" on miss, which sp_box_int/sp_box_str would otherwise
 # round-trip as a genuine 0/"" leaf indistinguishable from a real
 # value. Poly inner hashes don't need the guard — their `_get`
 # already returns sp_box_nil() on miss.











 # Inferred return type for `recv.mname(...)` when `recv` is poly.
 # If every user class that defines mname agrees on the return type,
 # that concrete type is used. If any two disagree, the call is
 # genuinely polymorphic and the caller must treat the result as
 # an sp_RbVal.
 # Returns 1 if class `ci` declares `mname` as an attr_reader (in
 # which case `obj.<mname>` reads `obj->iv_<mname>`).
  def cls_has_attr_reader(ci, mname)
    readers = @cls_attr_readers[ci].split(";")
    j = 0
    while j < readers.length
      if readers[j] == mname
        return 1
      end
      j = j + 1
    end
    0
  end

 # Decide whether a `<poly>[k]` call site (the outer `[]` in
 # `arr[i][k]` chains) can return int instead of poly. The poly
 # value came from `arr[i]` where `arr` is a poly_array; if every
 # element kind observed for `arr`'s slot has an int-returning
 # `[]` (IntArray + Method, the optcarrot `__fetch__` shape), the
 # outer dispatch is guaranteed to land on a poly carrying an int
 # tag, and we can read it as `mrb_int` directly. Returns 0
 # whenever the chain doesn't fit the pattern, so the default
 # poly-typed temp is preserved for everything else.
  def poly_index_narrow_int(nid)
    recv_id = @nd_receiver[nid]
    if recv_id < 0
      return 0
    end
 # The receiver must itself be a `[]` call (`arr[i]`).
    if @nd_type[recv_id] != "CallNode" || @nd_name[recv_id] != "[]"
      return 0
    end
    inner_recv = @nd_receiver[recv_id]
    if inner_recv < 0
      return 0
    end
 # Resolve the inner receiver to an ivar (directly, or via a
 # one-step LV alias from `lv = @ivar`).
    iname = ""
    if @nd_type[inner_recv] == "InstanceVariableReadNode"
      iname = @nd_name[inner_recv]
    elsif @nd_type[inner_recv] == "LocalVariableReadNode"
      lv_name = @nd_name[inner_recv]
 # Codegen-time scope alias takes priority (set by the LV-write
 # handler when `lv = @ivar` is emitted in this scope).
      iname = find_var_ivar_alias(lv_name)
 # Fallback for inference-time (`infer_type` before codegen,
 # when the scope stack hasn't been populated yet): walk the
 # enclosing method bodies for a single, unambiguous
 # `lv_name = @ivar` write. If the LV is reassigned from a
 # non-ivar elsewhere, leave iname empty and let the caller
 # bail out — the alias is no longer load-bearing.
      if iname == ""
        iname = find_lv_ivar_alias_in_ast(lv_name)
      end
    end
    if iname == "" || @current_class_idx < 0
      return 0
    end
 # The slot must currently be a poly_array — the only shape that
 # produces a poly via `[i]`.
    slot_t = cls_ivar_type(@current_class_idx, iname)
    if slot_t != "poly_array"
      return 0
    end
 # Derive the heterogeneous element kinds from
 # `cls_ivar_observed_types`. Each entry there is a slot type
 # the ivar held at some scan iteration: `int_array` means int
 # elements were stored, `obj_Method_ptr_array` means Method
 # elements, etc. The final `poly_array` entry just records the
 # widened state and adds no info. A bare `poly` observation
 # (whole-ivar `@x = something_poly`) is unknown — bail out.
    obs = cls_ivar_observed_types_for(@current_class_idx, iname)
    if obs == ""
      return 0
    end
    distinct = obs.split(",")
    saw_any = 0
    di = 0
    while di < distinct.length
      t = distinct[di]
      if t == "" || t == "poly_array"
 # ignore — uninformative
      elsif t == "int_array" || t == "obj_Method_ptr_array"
 # int-returning element kind
        saw_any = 1
      else
 # any other observed slot type (str_array, float_array,
 # other ptr_array variants, "poly" whole-ivar writes…) is
 # not safely narrowable.
        return 0
      end
      di = di + 1
    end
    saw_any
  end

 # Inference-time fallback for resolving `lv_name -> @ivar` when the
 # codegen scope alias isn't available yet. Walks every method body
 # in the current class for `lv_name = @ivar` writes. Returns the
 # ivar name only if the LV is unambiguously aliased (one ivar
 # source, no non-ivar reassignment); empty string otherwise. Cached
 # in `@lv_alias_cache_<class>:<lv>` to keep the per-narrow cost
 # down — this is called during type inference, which runs many
 # iterations.
  def find_lv_ivar_alias_in_ast(lv_name)
    if @current_class_idx < 0
      return ""
    end
    cache_key = @current_class_idx.to_s + ":" + lv_name
    if @lv_alias_cache.key?(cache_key)
      return @lv_alias_cache[cache_key]
    end
    found = ""
    bodies = @cls_meth_bodies[@current_class_idx].split(";")
    bi = 0
    while bi < bodies.length
      bid = bodies[bi].to_i
      if bid >= 0
        r = scan_lv_alias_for(bid, lv_name)
        if r == "?"
 # Ambiguous: at least one non-ivar write to this LV. The
 # alias is unstable and unsafe to use for narrowing.
          found = ""
          break
        end
        if r != ""
          if found != "" && found != r
            found = ""
            break
          end
          found = r
        end
      end
      bi = bi + 1
    end
    @lv_alias_cache[cache_key] = found
    found
  end

 # Recursive AST walk under `nid`. Returns:
 # "" — no `lv_name = ...` write seen
 # "?" — `lv_name = <non-ivar>` write seen (alias is unstable)
 # "@x" — exactly one ivar source `lv_name = @x` seen
  def scan_lv_alias_for(nid, lv_name)
    if nid < 0
      return ""
    end
    found = ""
    if @nd_type[nid] == "LocalVariableWriteNode" && @nd_name[nid] == lv_name
      ex_id = @nd_expression[nid]
      if ex_id >= 0 && @nd_type[ex_id] == "InstanceVariableReadNode"
        found = @nd_name[ex_id]
      else
        return "?"
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      r = scan_lv_alias_for(cs[k], lv_name)
      if r == "?"
        return "?"
      end
      if r != ""
        if found != "" && found != r
          return "?"
        end
        found = r
      end
      k = k + 1
    end
    found
  end

 # Read the comma-separated whole-ivar observation list for a given
 # ivar by name. Used by `poly_index_narrow_int`.
  def cls_ivar_observed_types_for(ci, iname)
    if ci < 0 || ci >= @cls_ivar_observed_types.length
      return ""
    end
    names = @cls_ivar_names[ci].split(";")
    parts = @cls_ivar_observed_types[ci].split(";", -1)
    k = 0
    while k < names.length
      if names[k] == iname && k < parts.length
        return parts[k]
      end
      k = k + 1
    end
    ""
  end

  def poly_dispatch_return_type(mname)
    if mname == "[]"
      @needs_rb_value = 1
      return "poly"
    end
 # Methods that conventionally return the receiver's
 # representation (or compatible-shape). dup preserves storage
 # so the result is the same poly. each returns self (Ruby's
 # convention). merge returns a fresh hash of the same family.
 # to_h is identity for hashes. Without surfacing these as
 # poly, the result_tmp in the poly-recv dispatcher is declared
 # mrb_int and the per-cls_id arm's `tmp = sp_box_obj(...)`
 # assignment silently no-ops.
    if mname == "dup" || mname == "each" || mname == "to_h" || mname == "merge"
      @needs_rb_value = 1
      return "poly"
    end
 # Setters: mname ends with "=" and at least one class has an
 # attr_writer for the bare name. Return type is the ivar type
 # (Ruby returns the rhs from `x = v`); without this, the result
 # tmp's C type defaults to `mrb_int` and `tmp = rhs` mismatches
 # for non-int slots.
    setter_bname = ""
    if mname.length > 1 && mname[mname.length - 1] == "="
      setter_bname = mname[0, mname.length - 1]
    end
    common = ""
    ci = 0
    while ci < @cls_names.length
      rt = ""
      if cls_find_method_direct(ci, mname) >= 0
        rt = cls_method_return(ci, mname)
      elsif cls_has_attr_reader(ci, mname) == 1
 # An attr_reader returns the ivar type. .
        rt = cls_ivar_type(ci, "@" + mname)
      elsif setter_bname != "" && cls_has_attr_writer(ci, setter_bname) == 1
 # An attr_writer setter returns the ivar's type.
        rt = cls_ivar_type(ci, "@" + setter_bname)
      end
      if rt != ""
        if common == ""
          common = rt
        elsif common != rt
          return "poly"
        end
      end
      ci = ci + 1
    end
    common == "" ? "int" : common
  end

 # Runtime tag-check for `<poly>.is_a?(<klass>)` / `kind_of?` /
 # `instance_of?`. Returns a C boolean expression for the named
 # built-in class (Integer, String, Float, etc.), including
 # mixin-style names that match every value (Object, Kernel,
 # BasicObject, Comparable). Returns "" when the name has no
 # SP_TAG_* mapping (caller falls back to user-class dispatch).


 # Emit branches for the built-in (negative cls_id) entries. Each
 # entry maps a (SP_BUILTIN_*, method) pair to a C expression.
 # Adding a new built-in type means one more `if` branch here.

 # Try to compile str[i] <op> "c" as direct char comparison
 # Returns "" if not applicable


 # Box an already-compiled value of static type `at` into an sp_RbVal.
 # Mirrors box_expr_to_poly but operates on a raw (type, value) pair so
 # callers that already have temps don't have to re-emit the expr.
 # Unbox an sp_RbVal expression `val` into the C representation
 # of `at`. Used at sites where the destructure / cls_id-aware
 # dispatch produced an sp_RbVal but the consumer slot is a
 # concrete C type (mrb_int, sp_IntArray *, const char *, ...).
 # No runtime cls_id check — caller has already narrowed via
 # static type / wrapping cls_id dispatch.





 # Emit a chained `@a = @b = ... = expr` write as one rhs evaluation
 # plus N per-slot stores. Caller has verified that `nid` is an
 # InstanceVariableWriteNode whose expression is itself an
 # InstanceVariableWriteNode. Each slot is typed independently (some
 # widened to poly by scan_writer_calls, others kept native), so the
 # store boxes through sp_RbVal only for the poly slots.

 # Emit a runtime loop that pushes every element of the array `src_expr`
 # (a node id whose value is some typed array) onto the destination
 # int_array variable `dst`. Used when expanding `*args` into a rest
 # parameter that will be received as sp_IntArray *.

 # Read an element of a typed array as an mrb_int (so it fits int param
 # slots and the int_array rest bundle uniformly).

 # Same as array_get_as_int_expr but returns the element in its native
 # C type (used when the param slot is typed, e.g. const char *).

 # Splat-aware companion to compile_call_args_with_defaults. Handles a
 # single SplatNode in positional args. The conceptual positional list
 # is (prefix... ++ splat_array ++ suffix...); fixed params eat from the
 # left; the rest param (if any) gets the remainder.








  def find_method_owner(ci, mname)
    if ci < 0
      return ""
    end
    mnames = @cls_meth_names[ci].split(";")
    j = 0
    while j < mnames.length
      if mnames[j] == mname
        return @cls_names[ci]
      end
      j = j + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return find_method_owner(pi, mname)
      end
    end
    ""
  end

 # if midx is out of range or the body id is invalid. Centralises
 # the @cls_meth_bodies[ci].split(";")[midx].to_i parse so detectors
 # don't have to inline it.
  def cls_method_body_id(ci, midx)
    bodies = @cls_meth_bodies[ci].split(";")
    if midx >= bodies.length
      return -1
    end
    bid = bodies[midx].to_i
    if bid < 0
      return -1
    end
    bid
  end

 # Walk `nid` for YieldNode, accumulating the per-position arg
 # type into `types`. Stops at nested DefNode boundaries (those
 # introduce a new method scope with its own yield arity).
 # Mirrors body_max_yield_arity's traversal shape.
  def body_yield_arg_types(nid, types)
    if nid < 0
      return
    end
    if @nd_type[nid] == "YieldNode"
      if @nd_arguments[nid] >= 0
        args = get_args(@nd_arguments[nid])
        k = 0
        while k < args.length
          if k < types.length
            at = infer_type(args[k])
            if types[k] == ""
              types[k] = at
            elsif types[k] != at
              types[k] = unify_call_types(types[k], at, args[k])
            end
          end
          k = k + 1
        end
      end
      return
    end
    if @nd_type[nid] == "DefNode"
      return
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      body_yield_arg_types(cs[k], types)
      k = k + 1
    end
  end

  def cls_method_has_yield(ci, midx)
    ystr = @cls_meth_has_yield[ci].split(";")
    if midx < ystr.length
      if ystr[midx] == "1"
        return 1
      end
    end
    0
  end

 # Max number of args used in any `yield` inside the top-level method
 # at @meth_body_ids[mi]. Floor of 1 — yield-using methods always have
 # at least one mrb_int slot (the no-arg `yield` form is padded to 0).
  def method_yield_arity(mi)
    if mi < 0 || mi >= @meth_body_ids.length
      return 1
    end
    body_max_yield_arity(@meth_body_ids[mi], 1)
  end

 # Same as method_yield_arity, but resolved through the class method
 # body table @cls_meth_bodies (parallel to @cls_meth_has_yield).
 # Mirrors body_has_yield's recursion shape. `current` carries the running
 # max so callers can seed a floor (1, since every yield-using method needs
 # at least one mrb_int slot in `_block`'s signature).
  def body_max_yield_arity(nid, current)
    if nid < 0
      return current
    end
    if @nd_type[nid] == "YieldNode"
      n = 0
      if @nd_arguments[nid] >= 0
        n = get_args(@nd_arguments[nid]).length
      end
      if n < 1
        n = 1
      end
      if n > current
        current = n
      end
    end
    if @nd_type[nid] == "DefNode"
      return current
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      current = body_max_yield_arity(cs[k], current)
      k = k + 1
    end
    current
  end

 # ---- Return type inference ----

 # Narrow pre-pass for `rewrite_instance_eval_calls`: walk top-level
 # CallNodes shaped `recv.method(args)` where recv resolves to an
 # obj_<C> via top-level scope, and let scan_new_calls' receiver-method
 # branch widen the class method's ptypes. Without this, a method-param

  def cls_method_yield_arity(ci, midx)
    if ci < 0 || midx < 0
      return 1
    end
    bodies = @cls_meth_bodies[ci].split(";")
    if midx >= bodies.length
      return 1
    end
    bid = bodies[midx].to_i
    body_max_yield_arity(bid, 1)
  end


 # `recv OP rhs` lowering for an obj-typed receiver. When the
 # receiver's class (or an ancestor) defines `op` as a user method,
 # returns the C expression `sp_<owner>_<op>(<recv_c>, <rhs_c>)`.
 # Returns "" when no dispatch applies (caller falls back to its
 # inline path, or to `warn_unresolved_call`).



 # Compile an `ArrayNode` literal as `sp_PolyArray *`, regardless
 # of the inferred elem type. Used by the nested-array path in
 # the poly_array branch so 3D-and-deeper arrays preserve cls_id
 # tags through every level (e.g. an inner `[1,2,3]` ends up
 # boxed via `sp_box_int_array` so the dispatch can still
 # `sp_IntArray_get` on it).


 # Body of compile_array_literal, parameterised on the list of value
 # node ids and the resolved array element type. Lets a HashNode
 # whose keys are 0..N-1 (lowered to Array) reuse the same emission
 # paths by feeding in the AssocNode rhs ids without needing an
 # ArrayNode wrapper.

 # HashNode literal whose keys are 0..N-1 lowered to an Array
 # literal. Reuses compile_array_literal_from_ids by mapping each
 # AssocNode to its rhs value id.


 # ---- Statement compiler ----

 # Emit assignment of `value_expr` to a single MultiWrite target node
 # (LocalVariableTargetNode or InstanceVariableTargetNode). Centralized
 # so the splat path doesn't have to duplicate the InstanceVariable
 # special-cases (module-method-promoted ivar handling).
 # Assign `value_expr` (whose static C type is `value_type`) into the
 # multi-write target node. When the local target's slot is `poly` and
 # the source value isn't already boxed, the value is boxed first so a
 # heterogeneous RHS like `a, b, c = [1, "b", 2.0]` lands in the right
 # tagged-union slots.

 # Handle `a, *b = rhs` / `*a, b = rhs` / `a, *b, c = rhs`.
 # `lefts` are pre-splat targets, `rest_id` is the SplatNode (its
 # expression is the splat target), `rights` are post-splat targets.

 # The element type of an array-typed MultiTarget slot. For
 # `a, (b, c) = 1, [2, 3]` the slot value_type is "int_array" and
 # this returns "int" so b/c are declared as mrb_int. Mirrors the
 # array-prefix dispatch in compile_nested_multi_target.
  def nested_target_elem_type(slot_type)
    if slot_type == "int_array"
      return "int"
    end
    if slot_type == "float_array"
      return "float"
    end
    if slot_type == "str_array"
      return "string"
    end
    if slot_type == "sym_array"
      return "sym"
    end
    "int"
  end

 # Recursively unpack a `MultiTargetNode` slot in a multi-assign.
 # `value_expr` holds the value of this slot (always an array-typed
 # expression, since the source is `a, (b, c), d = ..., [2, 3], ...`).
 # We dispatch on the array's element type and emit per-target
 # index reads.




 # C expression for computing length of a value of the given type.
 # Returns "" if the type doesn't have a hoist-friendly length op.

 # Scan a while-body for any mutation of a local variable (by name).
 # Returns 1 if any mutating method call is found on the receiver
 # (push/pop/shift/unshift/<< / []= / delete / clear / insert /
 # replace / concat). Used to block unsafe hoisting.

 # Return the local variable name on which .length/.size is called
 # inside a comparison predicate (for mutation scanning). Empty if
 # the predicate doesn't match the hoist pattern.

 # Check if while condition uses .length/.size and hoist if safe.
 # Supports string, arrays, and hashes.























  def lambda_var_ret_type(vname)
    i = 0
    while i < @lambda_var_ret_names.length
      if @lambda_var_ret_names[i] == vname
        return @lambda_var_ret_types[i]
      end
      i = i + 1
    end
    ""
  end






 # Build the proc-fn body prelude that unpacks the args array passed
 # to the uniform `(void *_cap, mrb_int *args)` signature into named
 # `lv_<bp>` locals — one `mrb_int lv_<bp> = args[<idx>];` line per
 # block param. Used at both proc-fn body emit sites (captures and
 # no-captures branches; identical shape).



 # `arr[start, len] = src` where arr is a poly-typed slot (sp_RbVal).
 # Dispatches by runtime cls_id of arr and src and emits a per-index
 # copy. Same-length only — `src.length` must equal `len`. Falls
 # back silently when src isn't an array (no copy emitted).

 # Compile `arr[start, len] = src` slice assignment. Replaces
 # `len` elements of `arr` starting at `start` with the elements
 # of `src`. Same-length only — `src.length` must equal `len` at
 # runtime; resize semantics not implemented. Each index `i in
 # 0...len` is emitted as `arr[start + i] = src[i]`.

 # Compile `recv[idx] OP= value` (IndexOperatorWriteNode).
 #
 # Emitted as a get-modify-set against the appropriate typed container,
 # in a block scope so that the receiver and index are each evaluated
 # exactly once. Falls through silently for receiver types we don't
 # handle yet — currently float_array, int_array, and the four numeric
 # hash variants. Compound-assign on string arrays / poly hashes / str
 # hashes is rarely useful and would need per-type semantics.

 # `obj.attr <op>= val` family helper -- builds the
 # temp-receiver-once C block for typed-instance receivers backed by
 # an attr_accessor (or struct field). For non-attr receivers, we
 # exit with a precise error rather than fall through to an
 # incorrect emission. The caller passes a fragment that takes the
 # temp variable name and returns the C body to execute.
 #
 # Spec note: even for the typed-attr-accessor case the receiver is
 # evaluated exactly ONCE -- the source `obj.bar += val` is
 # `tmp = obj; tmp.bar = tmp.bar + val`, NOT `obj.bar = obj.bar + val`.
 # The temp pattern matters when the receiver expression has side
 # effects (e.g. `next_holder().attr += 1`).

 # `a[i] &&= val` -- read once, conditionally write once. Mirrors
 # compile_index_op_assign's per-receiver-type dispatch but routes
 # the new value through a C `if (cur)` guard instead of an
 # arithmetic op. The temp pattern keeps `a` and `i` evaluated
 # exactly once, matching CRuby's `a[i] = a[i] && val` evaluation
 # order.

 # `a[i] ||= val` -- read once, write only if current is falsy.
 # Same C-truthy vs Ruby-truthy gap as the LocalVariable/Global/
 # Class compound forms (numeric 0 is C-falsy; documented in
 # test/global_var_or_write.rb). For string-valued slots (NULL
 # falsy, anything else truthy) the C and Ruby semantics agree.

 # Emit the get-then-set body for `recv[k] ||= v` against a
 # poly_poly_hash receiver and return the C name of the sp_RbVal temp
 # holding the resulting value (existing on hit, freshly-stored rhs on
 # miss). Caller is responsible for using or discarding the temp.

 # Same shape as `compile_poly_poly_index_or_assign_to_temp` for
 # sym_poly_hash / str_poly_hash receivers — the keys are bare
 # `sp_sym` / `const char *` (not boxed sp_RbVal) but the value
 # slot is poly so the get-then-set still pivots on the
 # `tag == SP_TAG_NIL` miss probe.

 # Box the rhs of an `||=` whose lhs is a poly-element slot. For
 # ArrayNode literals — `[]`, `[nil, nil]`, … — promote to
 # poly_array (boxed) so the slot carries a uniformly poly shape;
 # subsequent chain levels can then inspect the value's cls_id
 # against `SP_BUILTIN_POLY_ARRAY` reliably. Without the promote,
 # `[]` would lower to `sp_box_int_array(sp_IntArray_new())`, the
 # next chain level's `cls_id == POLY_ARRAY` check would fail, and
 # the back-set would skip silently. Non-ArrayNode rhs falls back
 # to the existing `box_expr_to_poly` machinery.

 # `recv[i] ||= v` against a typed-array receiver whose element
 # slot is a boxed sp_RbVal — i.e. poly_array. The miss probe is
 # the same `tag == SP_TAG_NIL` pivot used by the *_poly_hash
 # forms, but indexed access has two extra wrinkles:
 #
 # - the key must be in-bounds before `_get` is safe to call
 # (sp_PolyArray_get reads `data[i]` without a length check),
 # - on miss the slot may be beyond `len`, so before `_set` we
 # pad the array with nil entries up to `i` inclusive.
 #
 # Mirrors CRuby's `arr[i] = v` auto-grow semantics for the gap.
 # Scalar-element typed arrays (int_array, float_array, …) need a
 # different probe (out-of-bounds check, no SP_TAG_NIL) and are
 # not handled here; the caller falls back to the existing stmt
 # path for those.

 # `<poly>[i] ||= v` where the recv is an sp_RbVal carrying — at
 # runtime — a poly_array. Common in chains like
 # `(@h[k] ||= [])[i] ||= ...`: spinel types `(@h[k] ||= [])` as
 # poly because the hash leaf type is poly, and the inner `||=`
 # sees a poly recv even though the value is concretely a
 # poly_array.
 #
 # The miss probe needs an extra `tag == SP_TAG_OBJ &&
 # cls_id == SP_BUILTIN_POLY_ARRAY` guard so the runtime falls
 # through cleanly when the poly carries a non-array shape. The
 # body otherwise mirrors `compile_typed_array_index_or_assign_to_temp`.

 # Return a C expression that evaluates to the inspected form of `val`
 # (a value of inferred Ruby type `at`), following Ruby's Object#inspect
 # contract. Returns "" when `at` has no inspect implementation yet, so
 # callers can fall back to their previous behaviour.

 # Kernel#p: for each argument, prints `arg.inspect` followed by a
 # newline. Uses `compile_inspect_for` for types that implement inspect;
 # falls back to puts-style output for types that don't yet (e.g.
 # user-defined classes, ranges, hashes).

 # Emit the puts-equivalent for a single arg (extracted for reuse from p).




  def get_block_param(nid, idx)
    blk = @nd_block[nid]
    if blk < 0
      return ""
    end
    params = @nd_parameters[blk]
    if params < 0
      return ""
    end
 # NumberedParametersNode ({ _1 + _2 }): params is the node itself,
 # and @nd_value holds the maximum (1 for _1, 2 for _2, etc.).
    if @nd_type[params] == "NumberedParametersNode"
      if idx < @nd_value[params]
        return "_" + (idx + 1).to_s
      end
      return ""
    end
    inner = @nd_parameters[params]
    if inner < 0
      return ""
    end
    reqs = parse_id_list(@nd_requireds[inner])
    if idx < reqs.length
      return @nd_name[reqs[idx]]
    end
    ""
  end







 # `redo` label-stack helpers. Each loop emitter wraps its body
 # between push_redo_label / emit_redo_label / pop_redo_label so
 # `redo` knows which label to jump to. Labels are unique per loop
 # body via @redo_label_counter.


 # Emit the C label that `redo` jumps to. C requires labels to be
 # followed by a statement; `;` lets a subsequent `}` stay valid
 # even if the body is empty.





 # Emit the loop-open lines for iterating over a receiver expression.
 # Supports range and all array-like types. After calling this helper,
 # the caller emits the block body and a closing '}'. idx_var holds
 # the loop counter (position or value for range); elem_var gets the
 # current element.

 # Element type of an iterable (for block param type inference).
  def iter_elem_type(recv_type)
    if recv_type == "range"
      return "int"
    end
    elem_type_of_array(recv_type)
  end










 # An empty `map {}` block yields nil per iteration in CRuby —
 # the result array's length still matches the receiver. Without
 # an explicit push the typed accumulator stays short and
 # downstream `.length` / `[i]` on the result is wrong. Push a
 # type-appropriate default (0 / 0.0 / "" / sp_box_nil) so
 # length is preserved across all map dispatches.












 # Replay all in-scope ensure bodies inline (innermost-first) ahead
 # of an early-exit `return`. Each ensure is popped from the stack
 # *before* its body is emitted, so a nested `return` inside the
 # ensure body sees only the *outer* ensures still active and
 # doesn't replay the same ensure recursively. Stack is restored
 # afterwards so the caller continues with the same view.
 #
 # @setjmp_depth is stashed to 0 during the replay because the
 # caller is responsible for emitting `sp_exc_top -= N` *once*
 # before the replays — a nested `return` inside an ensure body
 # would otherwise re-emit that decrement and over-pop the stack.

 # Emit the `sp_exc_top -= N;` that an early `return` needs in
 # order to leave sp_exc_top balanced. Called immediately before
 # `emit_ensure_replays` at every `return` emission site.










 # Tries the yield-method or instance_eval-trampoline dispatch
 # against a single class index. Returns 1 if dispatch fired (caller
 # should return immediately), 0 otherwise (caller falls through to
 # the next gate, e.g. parent class). Shared by the direct-class and
 # parent-class branches in compile_no_recv_call_expr.

 # Splice the statements of a block body in place with `self`
 # rebound to self_var (typed as cname). Saves and restores the
 # rebound-self ivars (@instance_eval_self_var / _type) so nested
 # splices compose. compile_no_recv_call_expr's instance_eval-self
 # branch reads these to dispatch receiverless calls inside the
 # splice against the rebound class. Reusable by future
 # rebind-and-splice features (e.g. instance_exec, tap-shape
 # trampolines).

 # Inlines a `recv.m { body }` call when `m` is an arity-0
 # instance_eval trampoline. The entire method body is the call
 # `instance_eval(&block)`, so we splice the block body in place
 # with self rebound to the receiver. Modeled on
 # compile_yield_method_call_stmt but simpler — the trampoline body
 # has no locals/params to remap.










 # ============================================================
 # Analysis IR: serializer and loader
 # ============================================================
 #
 # The IR captures everything `analyze_phase` populates so that a
 # codegen-only step can pick up where analysis stopped without
 # re-running the whole-program inference fixpoint. The format is
 # line-oriented:
 #
 # SPINEL-IR v1
 # <tag> <name> <encoded payload>
 #
 # Tags:
 # INT <ivar> <integer> scalar int ivar
 # SA <ivar> <pipe-joined> Array<String> (each element percent-encoded)
 # IA <ivar> <comma-joined> Array<Int>
 # T <node_id> <type> per-AST-node inferred type cache
 #
 # All string payloads are percent-encoded for space/newline/tab/
 # percent/pipe so the line-and-pipe split is unambiguous. The MVP
 # dumps the analysis-bearing instance variables directly. A
 # follow-up should replace this with entity records (M / C / CONST
 # / FFI_FUNC / …) — those are the real interface — but for now this
 # gets the bootstrap pipeline running end to end.

  def ir_escape(s)
    result = ""
    i = 0
    n = s.length
    while i < n
      ch = s[i]
      code = ch.bytes[0]
      if code == 32
        result = result + "%20"
      elsif code == 10
        result = result + "%0A"
      elsif code == 13
        result = result + "%0D"
      elsif code == 9
        result = result + "%09"
      elsif code == 37
        result = result + "%25"
      elsif code == 124
        result = result + "%7C"
      else
        result = result + ch
      end
      i = i + 1
    end
    result
  end



  def ir_join_strs(arr)
    result = ""
    i = 0
    while i < arr.length
      if i > 0
        result = result + "|"
      end
      result = result + ir_escape(arr[i])
      i = i + 1
    end
    result
  end


  def ir_join_ints(arr)
    result = ""
    i = 0
    while i < arr.length
      if i > 0
        result = result + ","
      end
      result = result + arr[i].to_s
      i = i + 1
    end
    result
  end


 # The ir_emit_* helpers return the appended-to buffer rather than
 # mutating in place. spinel's type inference doesn't (yet) handle
 # passing an sp_String* through a method-parameter slot the body
 # then `<<`-mutates — the param ends up typed `const char*` at the
 # callsite, which fails at the cc step. Returning a new string and
 # rebinding `buf = ir_emit_*(buf, …)` keeps every value typed as
 # `const char*` through the pipeline.
  def ir_emit_sa(buf, name, arr)
    buf + "SA " + name + " " + arr.length.to_s + " " + ir_join_strs(arr) + "\n"
  end

  def ir_emit_ia(buf, name, arr)
    buf + "IA " + name + " " + arr.length.to_s + " " + ir_join_ints(arr) + "\n"
  end

  def ir_emit_int(buf, name, val)
    buf + "INT " + name + " " + val.to_s + "\n"
  end

  def dump_analysis_buf
    buf = "SPINEL-IR v1\n"

 # Counters / scalars
    buf = ir_emit_int(buf, "@nd_count", @nd_count)
    buf = ir_emit_int(buf, "@root_id", @root_id)
    buf = ir_emit_int(buf, "@analysis_frozen", @analysis_frozen)
    buf = ir_emit_int(buf, "@ieval_counter", @ieval_counter)

 # Top-level method tables
    buf = ir_emit_sa(buf, "@meth_names", @meth_names)
    buf = ir_emit_sa(buf, "@meth_param_names", @meth_param_names)
    buf = ir_emit_sa(buf, "@meth_param_types", @meth_param_types)
    buf = ir_emit_sa(buf, "@meth_param_empty", @meth_param_empty)
    buf = ir_emit_sa(buf, "@meth_return_types", @meth_return_types)
    buf = ir_emit_ia(buf, "@meth_body_ids", @meth_body_ids)
    buf = ir_emit_sa(buf, "@meth_has_defaults", @meth_has_defaults)
    buf = ir_emit_ia(buf, "@meth_rest_index", @meth_rest_index)
    buf = ir_emit_ia(buf, "@meth_has_yield", @meth_has_yield)

 # Class tables
    buf = ir_emit_sa(buf, "@cls_names", @cls_names)
    buf = ir_emit_sa(buf, "@cls_parents", @cls_parents)
    buf = ir_emit_sa(buf, "@cls_includes", @cls_includes)
    buf = ir_emit_sa(buf, "@cls_ivar_names", @cls_ivar_names)
    buf = ir_emit_sa(buf, "@cls_ivar_types", @cls_ivar_types)
    buf = ir_emit_sa(buf, "@cls_ivar_init_definite", @cls_ivar_init_definite)
    buf = ir_emit_sa(buf, "@cls_ivar_observed_types", @cls_ivar_observed_types)
    buf = ir_emit_sa(buf, "@cls_meth_names", @cls_meth_names)
    buf = ir_emit_sa(buf, "@cls_meth_params", @cls_meth_params)
    buf = ir_emit_sa(buf, "@cls_meth_ptypes", @cls_meth_ptypes)
    buf = ir_emit_sa(buf, "@cls_meth_returns", @cls_meth_returns)
    buf = ir_emit_sa(buf, "@cls_meth_bodies", @cls_meth_bodies)
    buf = ir_emit_sa(buf, "@cls_meth_defaults", @cls_meth_defaults)
    buf = ir_emit_sa(buf, "@cls_meth_ptypes_empty", @cls_meth_ptypes_empty)
    buf = ir_emit_sa(buf, "@cls_attr_readers", @cls_attr_readers)
    buf = ir_emit_sa(buf, "@cls_attr_writers", @cls_attr_writers)
    buf = ir_emit_sa(buf, "@cls_cmeth_names", @cls_cmeth_names)
    buf = ir_emit_sa(buf, "@cls_cmeth_params", @cls_cmeth_params)
    buf = ir_emit_sa(buf, "@cls_cmeth_ptypes", @cls_cmeth_ptypes)
    buf = ir_emit_sa(buf, "@cls_cmeth_returns", @cls_cmeth_returns)
    buf = ir_emit_sa(buf, "@cls_cmeth_bodies", @cls_cmeth_bodies)
    buf = ir_emit_sa(buf, "@cls_cmeth_defaults", @cls_cmeth_defaults)
    buf = ir_emit_sa(buf, "@cls_cmeth_scope_names", @cls_cmeth_scope_names)
    buf = ir_emit_sa(buf, "@cls_cmeth_scope_types", @cls_cmeth_scope_types)
    buf = ir_emit_ia(buf, "@cls_is_value_type", @cls_is_value_type)
    buf = ir_emit_ia(buf, "@cls_is_sra", @cls_is_sra)
    buf = ir_emit_sa(buf, "@cls_meth_has_yield", @cls_meth_has_yield)
    buf = ir_emit_sa(buf, "@cls_method_adapters", @cls_method_adapters)

 # Constants / cvars / gvars
    buf = ir_emit_sa(buf, "@const_names", @const_names)
    buf = ir_emit_sa(buf, "@const_types", @const_types)
    buf = ir_emit_ia(buf, "@const_expr_ids", @const_expr_ids)
    buf = ir_emit_sa(buf, "@const_scope_names", @const_scope_names)
    buf = ir_emit_sa(buf, "@cvar_names", @cvar_names)
    buf = ir_emit_sa(buf, "@cvar_types", @cvar_types)
    buf = ir_emit_sa(buf, "@cvar_init_values", @cvar_init_values)
    buf = ir_emit_sa(buf, "@gvar_names", @gvar_names)
    buf = ir_emit_sa(buf, "@gvar_types", @gvar_types)

 # Modules
    buf = ir_emit_sa(buf, "@module_names", @module_names)
    buf = ir_emit_ia(buf, "@module_body_ids", @module_body_ids)
    buf = ir_emit_sa(buf, "@module_acc_keys", @module_acc_keys)
    buf = ir_emit_sa(buf, "@module_acc_consts", @module_acc_consts)

 # FFI
    buf = ir_emit_sa(buf, "@ffi_modules", @ffi_modules)
    buf = ir_emit_sa(buf, "@ffi_module_libs", @ffi_module_libs)
    buf = ir_emit_sa(buf, "@ffi_module_cflags", @ffi_module_cflags)
    buf = ir_emit_sa(buf, "@ffi_func_modules", @ffi_func_modules)
    buf = ir_emit_sa(buf, "@ffi_func_names", @ffi_func_names)
    buf = ir_emit_sa(buf, "@ffi_func_arg_types", @ffi_func_arg_types)
    buf = ir_emit_sa(buf, "@ffi_func_ret_types", @ffi_func_ret_types)
    buf = ir_emit_sa(buf, "@ffi_func_arg_specs", @ffi_func_arg_specs)
    buf = ir_emit_sa(buf, "@ffi_func_ret_specs", @ffi_func_ret_specs)
    buf = ir_emit_sa(buf, "@ffi_buf_modules", @ffi_buf_modules)
    buf = ir_emit_sa(buf, "@ffi_buf_names", @ffi_buf_names)
    buf = ir_emit_ia(buf, "@ffi_buf_sizes", @ffi_buf_sizes)
    buf = ir_emit_sa(buf, "@ffi_reader_modules", @ffi_reader_modules)
    buf = ir_emit_sa(buf, "@ffi_reader_names", @ffi_reader_names)
    buf = ir_emit_sa(buf, "@ffi_reader_kinds", @ffi_reader_kinds)
    buf = ir_emit_ia(buf, "@ffi_reader_offsets", @ffi_reader_offsets)

 # Regexp / dyn-regex / local-regex
    buf = ir_emit_sa(buf, "@regexp_patterns", @regexp_patterns)
    buf = ir_emit_sa(buf, "@regexp_flags", @regexp_flags)
    buf = ir_emit_ia(buf, "@dyn_regex_node_ids", @dyn_regex_node_ids)
    buf = ir_emit_sa(buf, "@dyn_regex_flags", @dyn_regex_flags)
    buf = ir_emit_sa(buf, "@local_regex_names", @local_regex_names)
    buf = ir_emit_ia(buf, "@local_regex_idx", @local_regex_idx)

 # Misc tables
    buf = ir_emit_sa(buf, "@open_class_names", @open_class_names)
    buf = ir_emit_sa(buf, "@method_ref_vars", @method_ref_vars)
    buf = ir_emit_sa(buf, "@method_ref_names", @method_ref_names)
    buf = ir_emit_sa(buf, "@galias_new", @galias_new)
    buf = ir_emit_sa(buf, "@galias_old", @galias_old)
    buf = ir_emit_ia(buf, "@undef_class_idx", @undef_class_idx)
    buf = ir_emit_sa(buf, "@undef_method", @undef_method)
    buf = ir_emit_sa(buf, "@sym_names", @sym_names)
    buf = ir_emit_sa(buf, "@tuple_types", @tuple_types)
    buf = ir_emit_sa(buf, "@poly_funcs", @poly_funcs)
    buf = ir_emit_sa(buf, "@poly_param_types", @poly_param_types)
    buf = ir_emit_ia(buf, "@ieval_class_idxs", @ieval_class_idxs)
    buf = ir_emit_ia(buf, "@ieval_body_ids", @ieval_body_ids)
    buf = ir_emit_ia(buf, "@pre_execution_blocks", @pre_execution_blocks)
    buf = ir_emit_ia(buf, "@post_execution_blocks", @post_execution_blocks)
    buf = ir_emit_sa(buf, "@toplevel_ivar_names", @toplevel_ivar_names)
    buf = ir_emit_sa(buf, "@toplevel_ivar_types", @toplevel_ivar_types)
    buf = ir_emit_sa(buf, "@lambda_var_ret_names", @lambda_var_ret_names)
    buf = ir_emit_sa(buf, "@lambda_var_ret_types", @lambda_var_ret_types)
    buf = ir_emit_sa(buf, "@multi_const_inits", @multi_const_inits)

 # Feature flags
    buf = ir_emit_int(buf, "@needs_gc", @needs_gc)
    buf = ir_emit_int(buf, "@needs_system", @needs_system)
    buf = ir_emit_int(buf, "@needs_int_array", @needs_int_array)
    buf = ir_emit_int(buf, "@needs_float_array", @needs_float_array)
    buf = ir_emit_int(buf, "@needs_str_array", @needs_str_array)
    buf = ir_emit_int(buf, "@needs_str_int_hash", @needs_str_int_hash)
    buf = ir_emit_int(buf, "@needs_str_str_hash", @needs_str_str_hash)
    buf = ir_emit_int(buf, "@needs_int_str_hash", @needs_int_str_hash)
    buf = ir_emit_int(buf, "@needs_sym_int_hash", @needs_sym_int_hash)
    buf = ir_emit_int(buf, "@needs_sym_str_hash", @needs_sym_str_hash)
    buf = ir_emit_int(buf, "@needs_sym_intern", @needs_sym_intern)
    buf = ir_emit_int(buf, "@needs_setjmp", @needs_setjmp)
    buf = ir_emit_int(buf, "@needs_mutable_str", @needs_mutable_str)
    buf = ir_emit_int(buf, "@needs_rb_value", @needs_rb_value)
    buf = ir_emit_int(buf, "@needs_regexp", @needs_regexp)
    buf = ir_emit_int(buf, "@needs_rand", @needs_rand)
    buf = ir_emit_int(buf, "@needs_stringio", @needs_stringio)
    buf = ir_emit_int(buf, "@needs_lambda", @needs_lambda)
    buf = ir_emit_int(buf, "@needs_fiber", @needs_fiber)
    buf = ir_emit_int(buf, "@needs_bigint", @needs_bigint)
 # @needs_* flags pre-initialized in `initialize` so spinel sees
 # them as struct fields when self-compiling spinel_analyze.rb.
    buf = ir_emit_int(buf, "@needs_poly_array", @needs_poly_array)
    buf = ir_emit_int(buf, "@needs_poly_poly_hash", @needs_poly_poly_hash)
    buf = ir_emit_int(buf, "@needs_str_poly_hash", @needs_str_poly_hash)
    buf = ir_emit_int(buf, "@needs_sym_poly_hash", @needs_sym_poly_hash)
    buf = ir_emit_int(buf, "@needs_ptr_array", @needs_ptr_array)
    buf = ir_emit_int(buf, "@needs_file_io", @needs_file_io)

 # Non-array string ivars (computed in analyze, consumed by emit)
    buf = buf + "STR @cls_cmeth_live " + ir_escape(@cls_cmeth_live) + "\n"
    buf = buf + "STR @cls_meth_live " + ir_escape(@cls_meth_live) + "\n"
    buf = buf + "STR @meth_blk_param_types " + ir_escape(@meth_blk_param_types.join("|")) + "\n"
    buf = buf + "STR @cls_cmeth_blk_param_types " + ir_escape(@cls_cmeth_blk_param_types.join("|")) + "\n"

 # Per-AST-node records (T / NM / NB / SN / ST) get accumulated
 # into a StrArray and joined once. Building them with `buf + ...`
 # in the loop is O(N^2): each iteration allocates a fresh
 # `len(buf) + delta` byte string. With dense T-record fills
 # (one per reachable node, ~150K on spinel_codegen.rb) that
 # quadratic blew up the spinel-compiled binary's heap to 60GB
 # before crashing.
    rec_buf = "".split(",")
    ni = 0
    while ni < @nd_count
      t = @nd_inferred_type[ni]
      if t != ""
        rec_buf.push("T " + ni.to_s + " " + ir_escape(t) + "\n")
      end
      ni = ni + 1
    end
    nm = 0
    while nm < @nd_count
      if @nd_name[nm].length >= 11
        if @nd_name[nm][0, 11] == "__sp_ieval_"
          rec_buf.push("NM " + nm.to_s + " " + ir_escape(@nd_name[nm]) + "\n")
          rec_buf.push("NB " + nm.to_s + " -1\n")
        end
      end
      nm = nm + 1
    end
    sd = 0
    while sd < @nd_count
      sn = @nd_scope_names[sd]
      if sn != ""
        rec_buf.push("SN " + sd.to_s + " " + ir_escape(sn) + "\n")
        rec_buf.push("ST " + sd.to_s + " " + ir_escape(@nd_scope_types[sd]) + "\n")
      end
      sd = sd + 1
    end
    buf = buf + rec_buf.join("")

    buf
  end

 # Restore the analysis state from a buffer produced by
 # dump_analysis_buf. Caller is responsible for having read the
 # parsed AST first (read_text_ast) so the @nd_inferred_type cache
 # has slots to populate.


 # Length-prefixed split: returns exactly `n` elements. Distinguishes
 # `[]` (n==0) from `[""]` (n==1, body=="") so empty string elements
 # round-trip correctly.






 # Pre-fill @nd_inferred_type for every reachable AST node, using
 # the same scope context emission would set up. Run after
 # analyze_phase has converged, before serialize, so the IR carries
 # a complete per-node type cache that codegen can read in O(1).
 #
 # Strategy: mirror infer_function_body_call_types / etc.'s scope
 # setup pattern, but call walk_and_cache instead of scan_new_calls.
 # walk_and_cache stores infer_type's result at every visited nid.
 # Block param scoping is not yet handled — references to block params
 # in cache may carry default "int" instead of the iterator-derived
 # type; codegen still falls back to infer_type for cache misses.

  def walk_and_cache(nid)
    if nid < 0
      return
    end
 # IfNode with `var.is_a?(C)` predicate narrows `var` for the then-arm.
 # Mirror scan_new_calls' narrow handling so cached types in the
 # then-body reflect the narrow.
    if @nd_type[nid] == "IfNode"
 # Note: scan_new_calls pushes type narrows here for fixpoint-side
 # param widening. We DON'T push narrows during walk_and_cache —
 # codegen never applies narrows at emit time (the C variable
 # stays whatever type it was declared as), so cached
 # LocalVariableReadNode values inside an `is_a?` then-arm need
 # to reflect the unnarrowed declared type.
      pred = @nd_predicate[nid]
      if pred >= 0
        walk_and_cache(pred)
      end
      then_body = @nd_body[nid]
      if then_body >= 0
        walk_and_cache(then_body)
      end
      sub = @nd_subsequent[nid]
      if sub >= 0
        walk_and_cache(sub)
      end
      else_body = @nd_else_clause[nid]
      if else_body >= 0
        walk_and_cache(else_body)
      end
      @nd_inferred_type[nid] = infer_type(nid)
      return
    end
 # Class/Module nodes: skip — handled separately at top-level loop
 # so we set @current_class_idx / lexical scope correctly.
    if @nd_type[nid] == "ClassNode" || @nd_type[nid] == "ModuleNode" || @nd_type[nid] == "SingletonClassNode"
      return
    end
 # DefNode: skip — its body is walked separately via @meth_body_ids
 # / @cls_meth_bodies / @cls_cmeth_bodies with proper param scope.
    if @nd_type[nid] == "DefNode"
      return
    end
 # RescueNode: register the bound exception var so infer_call_type
 # recognises `.message` / `.class` / etc. as string-returning methods
 # within the rescue body. compile_begin_with_rescue does the same
 # at emit time; we mirror it here.
    if @nd_type[nid] == "RescueNode"
      ref_re = @nd_reference[nid]
      bound_re = ""
      if ref_re >= 0
        bound_re = @nd_name[ref_re]
        @exc_var_names.push(bound_re)
 # Class is unknown at analyze time but find_exc_var_cls's
 # callers only check for non-empty (presence). Use a sentinel.
        @exc_var_cls_vars.push("?")
      end
      cs_re = []
      push_child_ids(nid, cs_re)
      blk_re = @nd_block[nid]
      k = 0
      while k < cs_re.length
        if cs_re[k] != blk_re
          walk_and_cache(cs_re[k])
        end
        k = k + 1
      end
      if bound_re != ""
        @exc_var_names.pop
        @exc_var_cls_vars.pop
      end
      @nd_inferred_type[nid] = infer_type(nid)
      return
    end
 # Post-order walk: visit children FIRST so their types are cached
 # by the time the parent's infer_type runs. With cache-hit short
 # circuit at the top of infer_type, the parent's compute is then
 # O(1) per direct child instead of O(subtree). This is what makes
 # the annotate pass tractable on large files.
    cs = []
    push_child_ids(nid, cs)
 # When this nid is a CallNode with a block, push a fresh scope
 # frame and declare the block params with iterator-derived types
 # before descending into @nd_block. Without this, a block param
 # that shadows an outer same-name local with a different type
 # would get the outer type cached at every reference inside the
 # block body — wrong.
 #
 # For proc / lambda / Fiber.new / Proc.new the block param's
 # actual type is determined dynamically by the caller (the value
 # passed to .call / .resume), and codegen's compile_lambda_def
 # hardcodes the param as int regardless. Don't descend into
 # those block bodies; codegen falls back to fresh infer_type
 # against its own scope at emit time.
    blk = @nd_block[nid]
    pushed_blk_scope = 0
    descend_blk = 1
    if blk >= 0 && @nd_type[nid] == "CallNode"
      mname_blk = @nd_name[nid]
      recv_blk = @nd_receiver[nid]
      if mname_blk == "proc" || mname_blk == "lambda"
        descend_blk = 0
      end
      if mname_blk == "new" && recv_blk >= 0
        cnk = @nd_name[recv_blk]
 # Proc.new / Lambda.new use compile_lambda_def's int-hardcoded
 # param. Fiber.new uses compile_fiber_new which declares the
 # param as poly — descending there is safe.
        if cnk == "Proc" || cnk == "Lambda"
          descend_blk = 0
        end
      end
      if descend_blk == 1
        push_scope
        pushed_blk_scope = 1
        bp_idx = 0
        while bp_idx < 8
          bpn = get_block_param(nid, bp_idx)
          if bpn == ""
            bp_idx = 8
          else
            if bpn != "_"
              declare_var(bpn, block_param_type_at(nid, bp_idx))
            end
            bp_idx = bp_idx + 1
          end
        end
      end
    end
    k = 0
    while k < cs.length
      if descend_blk == 0 && cs[k] == blk
 # Skip block body for poly-block dispatchers.
      else
        walk_and_cache(cs[k])
      end
      k = k + 1
    end
    if pushed_blk_scope == 1
      pop_scope
    end
 # LocalVariable*Read/Target/*Write nodes resolve through scope at
 # emit time. Codegen's compile_lambda_def hardcodes lambda params
 # as int regardless of analyze's type, narrows from is_a? aren't
 # applied in C, and auto-splat block params over poly receivers
 # diverge from analyze's elem-type guess. Leaving these uncached
 # lets codegen's infer_type fall back to find_var_type against
 # its own emit-time scope — that's the source of truth.
    nt = @nd_type[nid]
    skip_cache = 0
    if nt == "LocalVariableReadNode" || nt == "LocalVariableTargetNode" || nt == "LocalVariableAndWriteNode" || nt == "LocalVariableOrWriteNode" || nt == "LocalVariableOperatorWriteNode"
      skip_cache = 1
    end
 # Bare `new` inside an inherited class method body resolves to the
 # *calling* subclass at emit time, not the class that lexically
 # defined the method . Cache here would freeze the
 # type at the defining class's instance.
    if nt == "CallNode" && @nd_receiver[nid] < 0 && @nd_name[nid] == "new"
      skip_cache = 1
    end
 # `lambda.call(...)` — the return type comes from
 # @lambda_var_ret_types which is built at codegen time
 # multi-arg lambda). Caching at analyze would freeze the type as
 # "int" before scan_lambda_ret_types runs, and the outer `.to_s`
 # would emit sp_int_to_s instead of the bool ternary. Limited to
 # the literal `.call` shape (NOT `[]` — that's str_array indexing
 # in too many other places to skip safely).
    if nt == "CallNode" && @nd_name[nid] == "call"
      crv = @nd_receiver[nid]
      if crv >= 0 && @nd_type[crv] == "LocalVariableReadNode"
        skip_cache = 1
      end
    end
    if skip_cache == 0
      @nd_inferred_type[nid] = infer_type(nid)
    end
  end

 # Mirror emit_main / declare_method_locals' multi-pass scope
 # refinement (without the actual emit). After this, every local in
 # `stmts`' scope has its final emit-time type, so a subsequent
 # walk_and_cache caches correct values for nodes whose inference
 # touches scope.
 #
 # `do_lambda_upgrade` and `do_bigint_promote` mirror emit_main's
 # final passes (lambda-promotion and bigint-promotion). They're
 # off for method-body refinement (declare_method_locals does
 # neither) and on for top-level refinement.
  def refine_locals_multi_pass_full(stmts, lnames, ltypes, params, do_lambda_upgrade, do_bigint_promote)
 # Pre-pass: declare simple-literal locals into scope so the
 # subsequent scan_locals' infer_type can resolve cross-statement
 # references (e.g. `name = "ada"` then `{name:}` shorthand whose
 # value type depends on name's resolved type) on the first pass.
    pre_scan_simple_local_writes(stmts)
 # Pass 1: initial scan
    si = 0
    while si < stmts.length
      sid = stmts[si]
      if @nd_type[sid] != "DefNode" && @nd_type[sid] != "ClassNode" && @nd_type[sid] != "ModuleNode" && @nd_type[sid] != "ConstantWriteNode"
        scan_locals(sid, lnames, ltypes, params)
      end
      si = si + 1
    end
 # Constant initializer block params (only on top-level scope).
    if do_bigint_promote == 1
      scan_const_init_locals(lnames, ltypes, params)
    end
    j = 0
    while j < lnames.length
      declare_var(lnames[j], ltypes[j])
      j = j + 1
    end
 # Refinement passes — re-scan with declared scope; promote refined types.
    pass = 0
    while pass < 2
      ln = "".split(",")
      lt = "".split(",")
      si = 0
      while si < stmts.length
        sid = stmts[si]
        if @nd_type[sid] != "DefNode" && @nd_type[sid] != "ClassNode" && @nd_type[sid] != "ModuleNode" && @nd_type[sid] != "ConstantWriteNode"
          scan_locals(sid, ln, lt, params)
        end
        si = si + 1
      end
      j = 0
      while j < ln.length
        k = 0
        while k < lnames.length
          if lnames[k] == ln[j]
            merged_p = merge_refined_local_type(ltypes[k], lt[j])
            if merged_p != ""
              ltypes[k] = merged_p
              set_var_type(lnames[k], merged_p)
              if merged_p == "poly"
                @needs_rb_value = 1
              end
            end
          end
          k = k + 1
        end
        j = j + 1
      end
      pass = pass + 1
    end
 # Lambda upgrade: ints passed to lambda-param functions widen to lambda.
    if do_lambda_upgrade == 1
      j = 0
      while j < lnames.length
        if ltypes[j] == "int"
          si = 0
          while si < stmts.length
            sid = stmts[si]
            if @nd_type[sid] != "DefNode" && @nd_type[sid] != "ClassNode" && @nd_type[sid] != "ModuleNode" && @nd_type[sid] != "ConstantWriteNode"
              if param_used_as_lambda(lnames[j], sid) == 1
                ltypes[j] = "lambda"
                set_var_type(lnames[j], "lambda")
              end
            end
            si = si + 1
          end
        end
        j = j + 1
      end
    end
 # Bigint promotion: vars with `*=` / `x = x * y` in while loops.
    if do_bigint_promote == 1
      detect_bigint_vars(stmts, lnames, ltypes)
      j = 0
      while j < lnames.length
        if ltypes[j] == "bigint"
          set_var_type(lnames[j], "bigint")
        end
        j = j + 1
      end
    end
  end

 # Iterator-derived block param type at call_nid for param index
 # `pi` (0-based). Mirrors the corresponding dispatch in scan_locals
 # so walk_and_cache can declare block params in their own scope
 # frame before descending into the block body. Returns "int" for
 # methods we don't model — that's fine because codegen's
 # compile_block_iteration_stmt does its own scope setup with the
 # full type-aware logic at emit time, and any walk_and_cache cache
 # value for a block-param read will be re-checked against
 # find_var_type's emit-time scope on cache miss in infer_type;
 # only block-body nodes outside the param-read path care about
 # the cached value here.
  def block_param_type_at(call_nid, pi)
    mname = @nd_name[call_nid]
    recv = @nd_receiver[call_nid]
    recv_t = "int"
    if recv >= 0
      recv_t = infer_type(recv)
    end
    if mname == "each" || mname == "map" || mname == "flat_map" || mname == "filter" || mname == "select" || mname == "reject" || mname == "find" || mname == "detect" || mname == "find_index" || mname == "find_all" || mname == "count" || mname == "all?" || mname == "any?" || mname == "none?" || mname == "min_by" || mname == "max_by" || mname == "sort_by" || mname == "group_by" || mname == "partition" || mname == "uniq" || mname == "tally" || mname == "drop_while" || mname == "take_while" || mname == "filter_map"
 # Hash#each yields |k, v|. elem_type_of_array on a hash type
 # falls back to "int" for both, which then types `puts k + ":"`
 # inside the block as int-arithmetic and lowers to printf("%lld")
 # with a raw `+` between pointers. Pick the hash's key/value
 # part directly so block_param_type_at returns the right per-
 # position type. hash_key_part/hash_value_part return the
 # short ("str"/"sym"/"int") tags; expand to full type names.
      if is_hash_type(recv_t) == 1
        if pi == 0
          kp = hash_key_part(recv_t)
          return "string" if kp == "str"
          return "symbol" if kp == "sym"
          return "int" if kp == "int"
          return "poly"
        end
        vp = hash_value_part(recv_t)
        return "string" if vp == "str"
        return "int" if vp == "int"
        return "poly"
      end
      return elem_type_of_array(recv_t)
    end
    if mname == "each_with_index"
      if pi == 0
        return elem_type_of_array(recv_t)
      end
      return "int"
    end
    if mname == "each_with_object"
      if pi == 0
        return elem_type_of_array(recv_t)
      end
 # 2nd param is the seed/object — its type is the call's first arg.
      args = @nd_arguments[call_nid]
      if args >= 0
        aargs = get_args(args)
        if aargs.length > 0
          return infer_type(aargs[0])
        end
      end
      return "int"
    end
    if mname == "times" || mname == "upto" || mname == "downto" || mname == "step"
      return "int"
    end
    if mname == "reduce" || mname == "inject"
      if pi == 0
        args = @nd_arguments[call_nid]
        if args >= 0
          aargs = get_args(args)
          if aargs.length > 0
            return infer_type(aargs[0])
          end
        end
        return elem_type_of_array(recv_t)
      end
      return elem_type_of_array(recv_t)
    end
    if mname == "each_char"
      return "string"
    end
    if mname == "each_byte"
      return "int"
    end
    if mname == "each_line"
      return "string"
    end
    if mname == "each_pair"
 # Hash#each_pair: key, value. Mirror scan_locals.
      if recv_t == "str_int_hash"
        if pi == 0
          return "string"
        end
        return "int"
      end
      if recv_t == "str_str_hash"
        if pi == 0
          return "string"
        end
        return "string"
      end
      if recv_t == "sym_int_hash"
        if pi == 0
          return "symbol"
        end
        return "int"
      end
      if recv_t == "sym_str_hash"
        if pi == 0
          return "symbol"
        end
        return "string"
      end
      if recv_t == "sym_poly_hash"
        if pi == 0
          return "symbol"
        end
        return "poly"
      end
      if recv_t == "str_poly_hash"
        if pi == 0
          return "string"
        end
        return "poly"
      end
      return "int"
    end
    if mname == "each_key"
      if recv_t == "str_int_hash" || recv_t == "str_str_hash" || recv_t == "str_poly_hash"
        return "string"
      end
      if recv_t == "sym_int_hash" || recv_t == "sym_str_hash" || recv_t == "sym_poly_hash"
        return "symbol"
      end
      return "int"
    end
    if mname == "each_value"
      if recv_t == "str_int_hash" || recv_t == "sym_int_hash"
        return "int"
      end
      if recv_t == "str_str_hash" || recv_t == "sym_str_hash"
        return "string"
      end
      if recv_t == "sym_poly_hash" || recv_t == "str_poly_hash" || recv_t == "poly_poly_hash"
        return "poly"
      end
      return "int"
    end
    if mname == "zip"
      return elem_type_of_array(recv_t)
    end
    if mname == "sort"
      return elem_type_of_array(recv_t)
    end
    if mname == "scan"
 # String#scan with block: param is each match — string for the
 # /pattern without captures/ form, otherwise array of captures.
      return "string"
    end
    if mname == "tap" || mname == "then" || mname == "yield_self" || mname == "itself"
 # Block param type is the receiver's type.
      return recv_t
    end
    if mname == "cycle"
      return elem_type_of_array(recv_t)
    end
    if mname == "loop"
      return "int"
    end
    if mname == "each_slice" || mname == "each_cons"
 # Block param is sub-array of recv's element type — keep recv's
 # array type (slice of int_array is still int_array).
      return recv_t
    end
    if mname == "chunk_while" || mname == "slice_when"
      return elem_type_of_array(recv_t)
    end
 # Fiber.new / Proc.new / Lambda.new / proc / lambda / Thread.new
 # — the block param is whatever the caller resumes/calls with, so
 # it's poly (sp_RbVal) at the type system's perspective. Without
 # this dispatch the param infers as "int" and a body like
 # `{|x| x + 1}` would be cached as int-add even though x is
 # actually an unboxed sp_RbVal at runtime.
    if mname == "new"
      if recv >= 0 && @nd_type[recv] == "ConstantReadNode"
        cn = @nd_name[recv]
        if cn == "Fiber" || cn == "Proc"
          return "poly"
        end
      end
      if recv >= 0 && @nd_type[recv] == "ConstantPathNode"
 # ::Fiber.new / ::Proc.new
        cn2 = @nd_name[recv]
        if cn2 == "Fiber" || cn2 == "Proc"
          return "poly"
        end
      end
    end
    if mname == "proc" || mname == "lambda"
      return "poly"
    end
    "int"
  end

 # Mirror declare_method_locals' three-pass scan + lambda upgrade,
 # without emitting C. Computes the final (lnames, ltypes) the
 # codegen would otherwise compute itself.
  def refine_method_body_locals(bid, lnames, ltypes, params)
    scan_locals(bid, lnames, ltypes, params)
    j = 0
    while j < lnames.length
      declare_var(lnames[j], ltypes[j])
      j = j + 1
    end
 # Pass 2: re-scan with vars declared
    lnames2 = "".split(",")
    ltypes2 = "".split(",")
    scan_locals(bid, lnames2, ltypes2, params)
    j = 0
    while j < lnames2.length
      k = 0
      while k < lnames.length
        if lnames[k] == lnames2[j]
          merged = merge_refined_local_type(ltypes[k], ltypes2[j])
          if merged != ""
            ltypes[k] = merged
            set_var_type(lnames[k], merged)
            if merged == "poly"
              @needs_rb_value = 1
            end
          end
        end
        k = k + 1
      end
      j = j + 1
    end
 # Pass 3: re-scan with the merged types now reflected in scope.
 # Intermediate locals (e.g. `pp = pa[i]` where pa is itself a
 # body local) had their pass-1 type fall back to "int" because
 # pa wasn't declared yet on pass 1. Pass 2 pushed pp's correct
 # type into the fresh ln2/lt2 but pp's *global* scope entry
 # still carried the pass-1 stale type, so a downstream
 # `h[pp[1..]] = ap` resolved pp via infer_type → "int" and the
 # empty-hash promotion picked int_str_hash. The merge above
 # set pp's scope to the merged type; redo scan_locals so the
 # `[]=` arm now sees pp resolved to its real type and re-picks
 # the right hash variant. Mirror the merge rules from pass 2.
    lnames3 = "".split(",")
    ltypes3 = "".split(",")
    scan_locals(bid, lnames3, ltypes3, params)
    j = 0
    while j < lnames3.length
      k = 0
      while k < lnames.length
        if lnames[k] == lnames3[j]
          merged3 = merge_refined_local_type(ltypes[k], ltypes3[j])
          if merged3 != ""
            ltypes[k] = merged3
            set_var_type(lnames[k], merged3)
            if merged3 == "poly"
              @needs_rb_value = 1
            end
          end
        end
        k = k + 1
      end
      j = j + 1
    end
 # Pass 4: lambda upgrade
    j = 0
    while j < lnames.length
      if ltypes[j] == "int"
        if param_used_as_lambda(lnames[j], bid) == 1
          ltypes[j] = "lambda"
          set_var_type(lnames[j], "lambda")
        end
      end
      j = j + 1
    end
 # Pass 5: callee-arg-slot back-prop. For each hash-typed LV
 # forwarded to a callee whose matching slot has widened to a
 # poly variant (via narrow_param_hash_types_from_body_writes),
 # widen the caller's LV. Same shape as infer_param_type_from_
 # callee_slot but applied here at the LV layer so the refined
 # type sticks through subsequent refine_method_body_locals
 # invocations (return-type recompute, etc.). Surfaces in real-
 # blog's parse_request -> parse_form_into chain where the
 # caller's `params = {}` literal should widen alongside the
 # callee's body-widened `into : str_poly_hash`.
    saved_scope_p5 = @current_lexical_scope
 # Derive the lexical scope from @current_method_name (set by
 # the caller, typically precompute_all_scope_decls) so the
 # sibling-cmeth synth lookup `<Scope>_cls_<callee>` resolves.
    if @current_method_name != ""
      cls_marker_p5 = @current_method_name.index("_cls_")
      if cls_marker_p5 != nil && cls_marker_p5 >= 0
        @current_lexical_scope = @current_method_name[0, cls_marker_p5]
      end
    end
    j = 0
    while j < lnames.length
      if is_hash_type(ltypes[j]) == 1 && ltypes[j].include?("poly") == false
        obs_p5 = "".split(",")
        collect_param_callee_slots(bid, lnames[j], obs_p5)
        cur_kt_p5 = hash_key_part(ltypes[j])
        agreed_p5 = ""
        disagree_p5 = 0
        kk_p5 = 0
        while kk_p5 < obs_p5.length
          tab_p5 = obs_p5[kk_p5].index("\t")
          if tab_p5 >= 0
            cn_p5 = obs_p5[kk_p5][0, tab_p5]
            pos_p5 = obs_p5[kk_p5][tab_p5 + 1, obs_p5[kk_p5].length - tab_p5 - 1].to_i
            ct_p5 = callee_slot_type(cn_p5, pos_p5)
            if is_hash_type(ct_p5) == 1 && ct_p5.include?("poly") && hash_key_part(ct_p5) == cur_kt_p5
              if agreed_p5 == ""
                agreed_p5 = ct_p5
              elsif agreed_p5 != ct_p5
                disagree_p5 = 1
              end
            end
          end
          kk_p5 = kk_p5 + 1
        end
        if agreed_p5 != "" && disagree_p5 == 0
          ltypes[j] = agreed_p5
          set_var_type(lnames[j], agreed_p5)
        end
      end
      j = j + 1
    end
    @current_lexical_scope = saved_scope_p5
  end

 # Constant-initializer locals — block params introduced inside
 # const-init RHSes (e.g. `FRAME = [...].map { |n| ... }`) need
 # their `lv_<n>` decls in main's frame because the const inits
 # are compiled inline at main()'s top. Mirror codegen's old
 # scan_const_init_locals.
  def scan_const_init_locals(lnames, ltypes, empty_params)
    i = 0
    while i < @const_expr_ids.length
      if @const_expr_ids[i] >= 0
        scan_locals(@const_expr_ids[i], lnames, ltypes, empty_params)
      end
      i = i + 1
    end
    if @multi_const_inits != nil
      j = 0
      while j < @multi_const_inits.length
        mw_id = @multi_const_inits[j].split("|")[1].to_i
        rhs = @nd_expression[mw_id]
        if rhs >= 0
          scan_locals(rhs, lnames, ltypes, empty_params)
        end
        j = j + 1
      end
    end
  end

 # Walk every codegen-visible scope (top-level main, every method
 # body, every class instance method body, every class method body)
 # and persist the (lnames, ltypes) lists scan_locals + multi-pass
 # refinement would compute. Codegen reads these from IR to declare
 # locals without re-running scan_locals or any of its multi-pass
 # refinement / bigint / lambda-upgrade dependencies.
 # the caller's empty `{}` literal to match the widened param type.
  def narrow_param_hash_types_from_body_writes
 # Top-level methods.
    mi = 0
    while mi < @meth_names.length
      bid_h = @meth_body_ids[mi]
      if bid_h >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        changed = 0
        pk = 0
        while pk < pnames.length
          if pk < ptypes.length && is_hash_type(ptypes[pk]) == 1
            new_t = infer_param_hash_from_writes(bid_h, pnames[pk], ptypes[pk])
            if new_t != "" && new_t != ptypes[pk]
              ptypes[pk] = new_t
              changed = 1
            end
          end
          pk = pk + 1
        end
        if changed == 1
          @meth_param_types[mi] = ptypes.join(",")
        end
      end
      mi = mi + 1
    end
 # Class instance methods.
    ci = 0
    while ci < @cls_names.length
      all_params = @cls_meth_params[ci].split("|")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      bodies = @cls_meth_bodies[ci].split(";")
      cls_changed = 0
      mj = 0
      while mj < all_params.length
        bid_c = -1
        if mj < bodies.length
          bid_c = bodies[mj].to_i
        end
        if bid_c >= 0
          cm_pnames = all_params[mj].split(",")
          cm_ptypes = "".split(",")
          if mj < all_ptypes.length
            cm_ptypes = all_ptypes[mj].split(",")
          end
          m_changed = 0
          pk2 = 0
          while pk2 < cm_pnames.length
            if pk2 < cm_ptypes.length && is_hash_type(cm_ptypes[pk2]) == 1
              new_t = infer_param_hash_from_writes(bid_c, cm_pnames[pk2], cm_ptypes[pk2])
              if new_t != "" && new_t != cm_ptypes[pk2]
                cm_ptypes[pk2] = new_t
                m_changed = 1
              end
            end
            pk2 = pk2 + 1
          end
          if m_changed == 1
            all_ptypes[mj] = cm_ptypes.join(",")
            cls_changed = 1
          end
        end
        mj = mj + 1
      end
      if cls_changed == 1
        @cls_meth_ptypes[ci] = all_ptypes.join("|")
      end
      ci = ci + 1
    end
  end

 # hash-each block-arg widening for nested cmeth /
 # method calls. The in-pipeline scan_new_calls runs without
 # the iterator's k/v scope pushed, so a call site like
 # `Json.escape(k)` inside `h.each |k, v| { ... }` sees k as
 # untyped and the cmeth's param ends up at the int default.
 # The block-scope push approach (matz comment, option 2/3)
 # perturbed's symbolize_keys convergence; this pass
 # stays surgical -- for every hash-typed param p of a method,
 # find any `lv_p.each |k, v|` blocks in the body and walk the
 # block body for `<recv>.<m>(args)` where args reference k or
 # v. Widen the called method's param types from the hash's
 # key/value variant only at those specific sites, leaving
 # scan_new_calls and the wider iterative loop untouched.
  def widen_cmeths_via_hash_each_blocks
 # Top-level methods.
    mi = 0
    while mi < @meth_names.length
      bid_h = @meth_body_ids[mi]
      if bid_h >= 0
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        each_widen_walk(bid_h, pnames, ptypes)
      end
      mi = mi + 1
    end
 # Class instance methods.
    ci = 0
    while ci < @cls_names.length
      all_params = @cls_meth_params[ci].split("|")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      bodies = @cls_meth_bodies[ci].split(";")
      mj = 0
      while mj < all_params.length
        bid_c = -1
        if mj < bodies.length
          bid_c = bodies[mj].to_i
        end
        if bid_c >= 0
          cm_pnames = all_params[mj].split(",")
          cm_ptypes = "".split(",")
          if mj < all_ptypes.length
            cm_ptypes = all_ptypes[mj].split(",")
          end
          each_widen_walk(bid_c, cm_pnames, cm_ptypes)
        end
        mj = mj + 1
      end
      ci = ci + 1
    end
 # Class methods (cmeths).
    ci = 0
    while ci < @cls_names.length
      all_params = @cls_cmeth_params[ci].split("|")
      all_ptypes = @cls_cmeth_ptypes[ci].split("|")
      bodies = @cls_cmeth_bodies[ci].split(";")
      mj = 0
      while mj < all_params.length
        bid_c = -1
        if mj < bodies.length
          bid_c = bodies[mj].to_i
        end
        if bid_c >= 0
          cm_pnames = all_params[mj].split(",")
          cm_ptypes = "".split(",")
          if mj < all_ptypes.length
            cm_ptypes = all_ptypes[mj].split(",")
          end
          each_widen_walk(bid_c, cm_pnames, cm_ptypes)
        end
        mj = mj + 1
      end
      ci = ci + 1
    end
  end

 # Recurse through `nid` looking for `<local>.each |k, v| { body }`
 # where `<local>` matches one of the enclosing method's hash-typed
 # params. When found, walk the block body for nested call sites
 # whose args reference k or v and widen the called method's
 # param types accordingly.
  def each_widen_walk(nid, pnames, ptypes)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "CallNode" && @nd_name[nid] == "each" && @nd_block[nid] >= 0
      recv = @nd_receiver[nid]
      if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
        local_name = @nd_name[recv]
 # Locate the param's type in the enclosing method.
        local_t = ""
        pi = 0
        while pi < pnames.length
          if pnames[pi] == local_name
            if pi < ptypes.length
              local_t = ptypes[pi]
            end
            pi = pnames.length
          else
            pi = pi + 1
          end
        end
        if is_hash_type(local_t) == 1
          k_name = get_block_param(nid, 0)
          v_name = get_block_param(nid, 1)
          if k_name != "" && v_name != ""
            k_t = hash_key_type_from_variant(local_t)
            v_t = hash_leaf_type(local_t)
            blk = @nd_block[nid]
            block_body = @nd_body[blk]
            widen_callsites_referencing_kv(block_body, k_name, k_t, v_name, v_t)
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      each_widen_walk(cs[k], pnames, ptypes)
      k = k + 1
    end
  end

 # Walk the block body. For each CallNode whose args reference
 # `k_name` / `v_name`, widen the called method's param types
 # from the hash-derived k/v types. Covers <Class>.<cmeth>(...)
 # constant-recv shape (the canonical repro). Other shapes
 # (bare method, instance recv) stay handled by the existing
 # iterative loop.
  def widen_callsites_referencing_kv(nid, k_name, k_t, v_name, v_t)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode"
      mname_w = @nd_name[nid]
      recv_w = @nd_receiver[nid]
      args_id_w = @nd_arguments[nid]
      if recv_w >= 0 && @nd_type[recv_w] == "ConstantReadNode" && args_id_w >= 0
        cls_name_w = @nd_name[recv_w]
        ci_w = find_class_idx(cls_name_w)
        if ci_w >= 0
          cm_names_w = @cls_cmeth_names[ci_w].split(";")
          cmpall_w = @cls_cmeth_ptypes[ci_w].split("|")
          cmi_w = 0
          while cmi_w < cm_names_w.length
            if cm_names_w[cmi_w] == mname_w && cmi_w < cmpall_w.length
              cmpt_w = cmpall_w[cmi_w].split(",")
              arg_ids_w = get_args(args_id_w)
              ai_w = 0
              changed_w = 0
              while ai_w < arg_ids_w.length && ai_w < cmpt_w.length
                arg_t_w = arg_type_in_each_block(arg_ids_w[ai_w], k_name, k_t, v_name, v_t)
                if arg_t_w != ""
                  new_t_w = unify_call_types(cmpt_w[ai_w], arg_t_w, arg_ids_w[ai_w])
                  if new_t_w != cmpt_w[ai_w]
                    cmpt_w[ai_w] = new_t_w
                    changed_w = 1
                  end
                end
                ai_w = ai_w + 1
              end
              if changed_w == 1
                cmpall_w[cmi_w] = cmpt_w.join(",")
                @cls_cmeth_ptypes[ci_w] = cmpall_w.join("|")
                @cls_cmeth_ptypes_version = @cls_cmeth_ptypes_version + 1
              end
            end
            cmi_w = cmi_w + 1
          end
        end
      end
    end
    cs2 = []
    push_child_ids(nid, cs2)
    k = 0
    while k < cs2.length
      widen_callsites_referencing_kv(cs2[k], k_name, k_t, v_name, v_t)
      k = k + 1
    end
  end

 # Returns the type to use for `arg` in the context of an each
 # block where k/v have hash-derived types. Mirrors infer_type
 # for the LocalVariableReadNode case but pins k/v to their
 # hash-derived types instead of consulting the (empty) var-type
 # table.
  def arg_type_in_each_block(arg, k_name, k_t, v_name, v_t)
    if arg < 0
      return ""
    end
    if @nd_type[arg] == "LocalVariableReadNode"
      n = @nd_name[arg]
      if n == k_name
        return k_t
      end
      if n == v_name
        return v_t
      end
    end
    ""
  end

 # Hash key type from a variant name. "str_int_hash" -> "string",
 # "sym_int_hash" -> "symbol", etc. Mirrors hash_leaf_type's
 # value-side counterpart.
  def hash_key_type_from_variant(t)
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
    if t == "str_int_hash" || t == "str_str_hash" || t == "str_poly_hash"
      return "string"
    end
    if t == "sym_int_hash" || t == "sym_str_hash" || t == "sym_poly_hash"
      return "symbol"
    end
    if t == "int_str_hash"
      return "int"
    end
    if t == "poly_poly_hash"
      return "poly"
    end
    ""
  end

 # Walk the method body looking for `lv_<pname>[k] = v` writes
 # (CallNode `[]=` with recv = LocalVariableReadNode named pname).
 # Returns a more-specific hash type than `cur` based on the
 # observed key + value types, or "" if there's no widening needed
 # (or if the observed types are inconsistent).
  def infer_param_hash_from_writes(nid, pname, cur)
    if nid < 0
      return ""
    end
 # Track all observed value types via a single string accumulator.
    val_types = "".split(",")
    key_types = "".split(",")
    collect_param_hash_writes(nid, pname, val_types, key_types)
 # also harvest signals from `pname.each do |k, v|`
 # block bodies. Programs that read-only-iterate the hash never
 # hit the `[]=` collector above, leaving the param widened to
 # whatever poly-ish variant an earlier widening pinned it to.
    collect_param_each_block_signals(nid, pname, val_types, key_types)
    if val_types.length == 0
 # option B (Ori's "weaker fix"): when no concrete
 # signals at all and the current type is poly-ish AND the
 # body has an `each |k, v|` on the param, default to
 # str_str_hash. This is unsound in general (a sym-keyed
 # poly_poly_hash would mis-narrow), but covers the dominant
 # Rails/Tep shape -- string-keyed hashes flowing through a
 # cmeth body that just dispatches to a sibling-cmeth
 # formatter. The narrowing only fires when no caller has
 # established a non-string-keyed shape via the existing
 # call-site widening, so the fallback applies exactly when
 # the param's poly type is itself a fallback from missing
 # call-site signal rather than a deliberate widening.
      if (cur == "poly_poly_hash" || cur == "sym_poly_hash" || cur == "str_poly_hash") &&
         param_has_each_kv?(nid, pname) == 1
        @needs_str_str_hash = 1
        return "str_str_hash"
      end
      return ""
    end
 # Decide hash variant from the union of observed types. If any
 # observed value type doesn't fit `cur`'s value slot, widen.
    cur_kt = hash_key_part(cur)
    cur_vt = hash_value_part(cur)
    new_vt = unify_hash_value_types(val_types)
    new_kt = unify_hash_key_types(key_types, cur_kt)
    return compose_hash_type(new_kt, new_vt)
  end

 # walks `nid` and returns 1 if there is at least
 # one `pname.each do |k, v|` shape on the param (regardless of
 # what the block body does with k / v). Used by the option B
 # weak-default fallback in infer_param_hash_from_writes.
  def param_has_each_kv?(nid, pname)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "CallNode" && (@nd_name[nid] == "each" || @nd_name[nid] == "each_pair")
      r_each_kv = @nd_receiver[nid]
      if r_each_kv >= 0 && @nd_type[r_each_kv] == "LocalVariableReadNode" && @nd_name[r_each_kv] == pname
        blk_each_kv = @nd_block[nid]
        if blk_each_kv >= 0
 # 2-arity block?
          if get_block_param(nid, 1) != ""
            return 1
          end
        end
      end
    end
    cs_each_kv = []
    push_child_ids(nid, cs_each_kv)
    k_each_kv = 0
    while k_each_kv < cs_each_kv.length
      if param_has_each_kv?(cs_each_kv[k_each_kv], pname) == 1
        return 1
      end
      k_each_kv = k_each_kv + 1
    end
    0
  end

 # walk `nid` for `pname.each do |k, v|` block
 # expressions and harvest type signals from how `k` and `v`
 # participate in `+`-chains in the body. A chain whose
 # transitive leaves include both a string literal and a
 # reference to k_pname / v_pname is treated as evidence the
 # corresponding side is string-typed.
  def collect_param_each_block_signals(nid, pname, val_types, key_types)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "each"
      r = @nd_receiver[nid]
      if r >= 0 && @nd_type[r] == "LocalVariableReadNode" && @nd_name[r] == pname
        blk = @nd_block[nid]
        if blk >= 0
          k_pname = get_block_param(nid, 0)
          v_pname = get_block_param(nid, 1)
          body = @nd_body[blk]
          if body >= 0
            collect_each_block_concat_signals(body, k_pname, v_pname, val_types, key_types)
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_param_each_block_signals(cs[k], pname, val_types, key_types)
      k = k + 1
    end
  end

  def collect_each_block_concat_signals(nid, k_pname, v_pname, val_types, key_types)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "+"
      leaves = []
      collect_concat_chain_leaves(nid, leaves)
      has_str_lit = 0
      has_k = 0
      has_v = 0
      li = 0
      while li < leaves.length
        lt = @nd_type[leaves[li]]
        if lt == "StringNode" || lt == "InterpolatedStringNode"
          has_str_lit = 1
        end
        if lt == "LocalVariableReadNode"
          ln = @nd_name[leaves[li]]
          if ln == k_pname && k_pname != ""
            has_k = 1
          end
          if ln == v_pname && v_pname != ""
            has_v = 1
          end
        end
        li = li + 1
      end
      if has_str_lit == 1
        if has_k == 1 && not_in("string", key_types) == 1
          key_types.push("string")
        end
        if has_v == 1 && not_in("string", val_types) == 1
          val_types.push("string")
        end
      end
    end
    cs_b = []
    push_child_ids(nid, cs_b)
    k_b = 0
    while k_b < cs_b.length
      collect_each_block_concat_signals(cs_b[k_b], k_pname, v_pname, val_types, key_types)
      k_b = k_b + 1
    end
  end

  def collect_concat_chain_leaves(nid, leaves)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "+"
      r_chain = @nd_receiver[nid]
      if r_chain >= 0
        collect_concat_chain_leaves(r_chain, leaves)
      end
      args_id_chain = @nd_arguments[nid]
      if args_id_chain >= 0
        args_chain = get_args(args_id_chain)
        ai_chain = 0
        while ai_chain < args_chain.length
          collect_concat_chain_leaves(args_chain[ai_chain], leaves)
          ai_chain = ai_chain + 1
        end
      end
      return
    end
    leaves.push(nid)
  end

  def collect_param_hash_writes(nid, pname, val_types, key_types)
    if nid < 0
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "[]="
      r = @nd_receiver[nid]
      if r >= 0 && @nd_type[r] == "LocalVariableReadNode" && @nd_name[r] == pname
        args_id_h = @nd_arguments[nid]
        if args_id_h >= 0
          args = get_args(args_id_h)
          if args.length >= 2
            kt = infer_type(args[0])
            vt = infer_type(args[args.length - 1])
            if not_in(kt, key_types) == 1
              key_types.push(kt)
            end
            if not_in(vt, val_types) == 1
              val_types.push(vt)
            end
          end
        end
      end
    end
    cs = []
    push_child_ids(nid, cs)
    k = 0
    while k < cs.length
      collect_param_hash_writes(cs[k], pname, val_types, key_types)
      k = k + 1
    end
  end

  def hash_key_part(t)
    if t == "str_int_hash" || t == "str_str_hash" || t == "str_poly_hash"
      return "str"
    end
    if t == "sym_int_hash" || t == "sym_str_hash" || t == "sym_poly_hash"
      return "sym"
    end
    if t == "int_str_hash"
      return "int"
    end
    if t == "poly_poly_hash"
      return "poly"
    end
    "str"
  end

  def hash_value_part(t)
    if t == "str_int_hash" || t == "sym_int_hash"
      return "int"
    end
    if t == "str_str_hash" || t == "sym_str_hash" || t == "int_str_hash"
      return "str"
    end
    if t == "str_poly_hash" || t == "sym_poly_hash" || t == "poly_poly_hash"
      return "poly"
    end
    "int"
  end

  def unify_hash_value_types(vts)
    if vts.length == 0
      return "int"
    end
    if vts.length == 1
      v = vts[0]
      return "int" if v == "int" || v == "bool" || v == "nil"
      return "str" if v == "string"
      return "poly"
    end
 # Multiple distinct types -> poly.
    "poly"
  end

  def unify_hash_key_types(kts, cur_kt)
    if kts.length == 0
      return cur_kt
    end
    if kts.length == 1
      k = kts[0]
      return "str" if k == "string"
      return "sym" if k == "symbol"
      return cur_kt
    end
    "poly"
  end

 # helper: when positional arg `nid` is an empty `{}` and

  def compose_hash_type(kt, vt)
    if kt == "str" && vt == "int"
      @needs_str_int_hash = 1
      return "str_int_hash"
    end
    if kt == "str" && vt == "str"
      @needs_str_str_hash = 1
      return "str_str_hash"
    end
    if kt == "str" && vt == "poly"
      @needs_str_poly_hash = 1
      return "str_poly_hash"
    end
    if kt == "sym" && vt == "int"
      @needs_sym_int_hash = 1
      return "sym_int_hash"
    end
    if kt == "sym" && vt == "str"
      @needs_sym_str_hash = 1
      return "sym_str_hash"
    end
    if kt == "sym" && vt == "poly"
      @needs_sym_poly_hash = 1
      return "sym_poly_hash"
    end
    if kt == "int" && vt == "str"
      @needs_int_str_hash = 1
      return "int_str_hash"
    end
    @needs_poly_poly_hash = 1
    return "poly_poly_hash"
  end

  def precompute_all_scope_decls
 # ---- Top-level main ----
    push_scope
    stmts = get_body_stmts(@root_id)
    ml = "".split(",")
    mt = "".split(",")
    empty_p = "".split(",")
    refine_locals_multi_pass_full(stmts, ml, mt, empty_p, 1, 1)
    @nd_scope_names[@root_id] = ml.join("|")
    @nd_scope_types[@root_id] = mt.join("|")
    pop_scope

 # ---- Top-level method bodies ----
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        push_scope
        saved_method_name = @current_method_name
        @current_method_name = @meth_names[mi]
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        pk = 0
        while pk < pnames.length
          if pnames[pk] != ""
            declare_var(pnames[pk], ptypes[pk])
          end
          pk = pk + 1
        end
        ml = "".split(",")
        mt = "".split(",")
        refine_method_body_locals(bid, ml, mt, pnames)
        @nd_scope_names[bid] = ml.join("|")
        @nd_scope_types[bid] = mt.join("|")
        @current_method_name = saved_method_name
        pop_scope
      end
      mi = mi + 1
    end

 # ---- Class instance method bodies + class method bodies ----
    ci = 0
    while ci < @cls_names.length
      saved_ci = @current_class_idx
      @current_class_idx = ci
      bodies = @cls_meth_bodies[ci].split(";")
      bj = 0
      while bj < bodies.length
        bid = bodies[bj].to_i
        if bid >= 0
          push_scope
          pnames2 = cls_meth_pnames_get(ci, bj)
          ptypes2 = cls_meth_ptypes_get(ci, bj)
          pk = 0
          while pk < pnames2.length
            pt = "int"
            if pk < ptypes2.length
              pt = ptypes2[pk]
            end
            declare_var(pnames2[pk], pt)
            pk = pk + 1
          end
          ml2 = "".split(",")
          mt2 = "".split(",")
          refine_method_body_locals(bid, ml2, mt2, pnames2)
          @nd_scope_names[bid] = ml2.join("|")
          @nd_scope_types[bid] = mt2.join("|")
          pop_scope
        end
        bj = bj + 1
      end
      cm_bodies = @cls_cmeth_bodies[ci].split(";")
      cm_names = @cls_cmeth_names[ci].split(";")
      saved_meth = @current_method_name
 # Build per-(ci, cmj) scope tables alongside the per-bid
 # @nd_scope_names entry. Inherited cmeths share their body id
 # across subclasses, so the per-bid entry gets overwritten by
 # whichever subclass scans last (effectively last-class-wins,
 # leaving every other subclass with the wrong LV types — see
 # Comment.find returning sp_Article *). Per-(ci, cmj) keeps
 # each subclass's specialized result alive for codegen.
      cms_per_ci = "".split(",")
      cmt_per_ci = "".split(",")
      cbj = 0
      while cbj < cm_bodies.length
        cbid = cm_bodies[cbj].to_i
        scope_n_entry = ""
        scope_t_entry = ""
        if cbid >= 0
          if cbj < cm_names.length
            @current_method_name = @cls_names[ci] + "_cls_" + cm_names[cbj]
          end
          push_scope
          cpnames = cls_cmeth_pnames_get(ci, cbj)
          cptypes = cls_cmeth_ptypes_get(ci, cbj)
          cpk = 0
          while cpk < cpnames.length
            cpt = "int"
            if cpk < cptypes.length
              cpt = cptypes[cpk]
            end
            declare_var(cpnames[cpk], cpt)
            cpk = cpk + 1
          end
          cml = "".split(",")
          cmt = "".split(",")
          refine_method_body_locals(cbid, cml, cmt, cpnames)
          @nd_scope_names[cbid] = cml.join("|")
          @nd_scope_types[cbid] = cmt.join("|")
          scope_n_entry = cml.join("|")
          scope_t_entry = cmt.join("|")
          pop_scope
        end
        cms_per_ci.push(scope_n_entry)
        cmt_per_ci.push(scope_t_entry)
        cbj = cbj + 1
      end
      @cls_cmeth_scope_names[ci] = cms_per_ci.join(";")
      @cls_cmeth_scope_types[ci] = cmt_per_ci.join(";")
      @current_method_name = saved_meth
      @current_class_idx = saved_ci
      ci = ci + 1
    end

 # ---- Constant initializer RHSes ----
 # Top-level main loop skipped ConstantWriteNode; ClassNode-body
 # MultiWriteNode (`P, Q = expr_inside_class_body`) wasn't reached
 # at all (the class loop only covers method bodies). compile_main
 # eventually inlines all const inits into main(), so walk them
 # under main's scope so any CallNodes / array literals on the
 # RHS get cached.
    push_scope
    cei = 0
    while cei < @const_expr_ids.length
      ceid = @const_expr_ids[cei]
      if ceid >= 0
        walk_and_cache(ceid)
      end
      cei = cei + 1
    end
    if @multi_const_inits != nil
      mci = 0
      while mci < @multi_const_inits.length
        parts = @multi_const_inits[mci].split("|")
        scope_n = parts[0]
        mw_id = parts[1].to_i
        rhs = @nd_expression[mw_id]
        if rhs >= 0
 # Set lexical scope so resolve_const_read_name finds
 # constants under the enclosing class (e.g. `M1, M2 = ARR`
 # inside class D needs to resolve ARR as D_ARR).
          saved_lex = @current_lexical_scope
          @current_lexical_scope = scope_n
          walk_and_cache(rhs)
          @current_lexical_scope = saved_lex
        end
        mci = mci + 1
      end
    end
    pop_scope

 # ---- ieval (instance_eval-rewritten) bodies ----
    iv = 0
    while iv < @ieval_body_ids.length
      bid_iv = @ieval_body_ids[iv]
      if bid_iv >= 0
        saved_ci_iv = @current_class_idx
        @current_class_idx = @ieval_class_idxs[iv]
        push_scope
        walk_and_cache(bid_iv)
        pop_scope
        @current_class_idx = saved_ci_iv
      end
      iv = iv + 1
    end

 # ---- BlockNode / LambdaNode / ProcNode bodies ----
 # compile_fiber_new and compile_lambda_def use scan_locals on a
 # block body to find referenced names; codegen then splits those
 # into captures (in outer scope) vs true locals at emit time. The
 # scan output is purely syntactic, so we cache it here keyed by
 # the body bid. Params are the block's syntactic required names.
    bn = 0
    while bn < @nd_count
      tk = @nd_type[bn]
      if tk == "BlockNode" || tk == "LambdaNode" || tk == "ProcNode"
        body_bn = @nd_body[bn]
        if body_bn >= 0
 # Collect syntactic block param names
          bp_list = "".split(",")
          params_n = @nd_parameters[bn]
          if params_n >= 0
            inner_p = @nd_parameters[params_n]
            if inner_p >= 0
              reqs_p = parse_id_list(@nd_requireds[inner_p])
              kp = 0
              while kp < reqs_p.length
                bp_list.push(@nd_name[reqs_p[kp]])
                kp = kp + 1
              end
            else
              reqs_p = parse_id_list(@nd_requireds[params_n])
              kp = 0
              while kp < reqs_p.length
                bp_list.push(@nd_name[reqs_p[kp]])
                kp = kp + 1
              end
            end
          end
          bn_names = "".split(",")
          bn_types = "".split(",")
          scan_locals(body_bn, bn_names, bn_types, bp_list)
          @nd_scope_names[body_bn] = bn_names.join("|")
          @nd_scope_types[body_bn] = bn_types.join("|")
        end
      end
      bn = bn + 1
    end
  end

  def annotate_all_node_types
 # ---- Top-level (main) ----
    stmts = get_body_stmts(@root_id)
    push_scope
    empty_p = "".split(",")
    ml = "".split(",")
    mt = "".split(",")
    refine_locals_multi_pass_full(stmts, ml, mt, empty_p, 1, 1)
    si = 0
    while si < stmts.length
      walk_and_cache(stmts[si])
      si = si + 1
    end
    pop_scope

 # ---- Top-level method bodies ----
    mi = 0
    while mi < @meth_names.length
      bid = @meth_body_ids[mi]
      if bid >= 0
        push_scope
        saved_method_name = @current_method_name
        @current_method_name = @meth_names[mi]
        pnames = @meth_param_names[mi].split(",")
        ptypes = @meth_param_types[mi].split(",")
        pk = 0
        while pk < pnames.length
          if pnames[pk] != ""
            declare_var(pnames[pk], ptypes[pk])
          end
          pk = pk + 1
        end
        lnames = "".split(",")
        ltypes = "".split(",")
        refine_method_body_locals(bid, lnames, ltypes, pnames)
        walk_and_cache(bid)
        @current_method_name = saved_method_name
        pop_scope
      end
      mi = mi + 1
    end

 # ---- Class instance method bodies + class method bodies ----
    ci = 0
    while ci < @cls_names.length
      saved_ci = @current_class_idx
      @current_class_idx = ci
      bodies = @cls_meth_bodies[ci].split(";")
      bj = 0
      while bj < bodies.length
        bid = bodies[bj].to_i
        if bid >= 0
          push_scope
          pnames2 = cls_meth_pnames_get(ci, bj)
          ptypes2 = cls_meth_ptypes_get(ci, bj)
          pk = 0
          while pk < pnames2.length
            pt = "int"
            if pk < ptypes2.length
              pt = ptypes2[pk]
            end
            declare_var(pnames2[pk], pt)
            pk = pk + 1
          end
          ml2 = "".split(",")
          mt2 = "".split(",")
          refine_method_body_locals(bid, ml2, mt2, pnames2)
          walk_and_cache(bid)
          pop_scope
        end
        bj = bj + 1
      end
      cm_bodies = @cls_cmeth_bodies[ci].split(";")
      cm_names = @cls_cmeth_names[ci].split(";")
      saved_meth = @current_method_name
      cbj = 0
      while cbj < cm_bodies.length
        cbid = cm_bodies[cbj].to_i
        if cbid >= 0
          if cbj < cm_names.length
            @current_method_name = @cls_names[ci] + "_cls_" + cm_names[cbj]
          end
          push_scope
          cpnames = cls_cmeth_pnames_get(ci, cbj)
          cptypes = cls_cmeth_ptypes_get(ci, cbj)
          cpk = 0
          while cpk < cpnames.length
            cpt = "int"
            if cpk < cptypes.length
              cpt = cptypes[cpk]
            end
            declare_var(cpnames[cpk], cpt)
            cpk = cpk + 1
          end
          cml = "".split(",")
          cmt = "".split(",")
          refine_method_body_locals(cbid, cml, cmt, cpnames)
          walk_and_cache(cbid)
          pop_scope
        end
        cbj = cbj + 1
      end
      @current_method_name = saved_meth
      @current_class_idx = saved_ci
      ci = ci + 1
    end
  end
end

# ---- Main (analyze) ----
ast_file = ARGV[0]
ir_file = ARGV[1]
if ast_file == nil || ir_file == nil
  $stderr.puts "Usage: ruby spinel_analyze.rb ast.txt out.ir"
  exit(1)
end
data = File.read(ast_file)
compiler = Compiler.new
compiler.read_text_ast(data)
compiler.analyze_phase
File.write(ir_file, compiler.dump_analysis_buf)
